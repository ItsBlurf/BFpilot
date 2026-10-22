#include "fs_at.h"
#include "tree_chmod.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHMOD_MAX_DEPTH 64U
#define CHMOD_MAX_DIRS 262144U

typedef struct { dev_t dev; ino_t ino; int used; } identity_t;
typedef struct { identity_t *slots; size_t count, capacity; } visited_t;

static size_t
identity_hash(dev_t dev, ino_t ino, size_t capacity) {
  uint64_t hash = (uint64_t)dev * UINT64_C(0x9e3779b97f4a7c15) ^
                  (uint64_t)ino * UINT64_C(0xbf58476d1ce4e5b9);
  return (size_t)hash & (capacity - 1);
}

static int
visit(visited_t *set, const struct stat *st) {
  if(!set->capacity || set->count * 2 >= set->capacity) {
    if(set->count >= CHMOD_MAX_DIRS) return -E2BIG;
    size_t capacity = set->capacity ? set->capacity * 2 : 128;
    identity_t *slots = calloc(capacity, sizeof(*slots));
    if(!slots) return -ENOMEM;
    for(size_t i = 0; i < set->capacity; i++) {
      if(!set->slots[i].used) continue;
      size_t pos = identity_hash(set->slots[i].dev, set->slots[i].ino, capacity);
      while(slots[pos].used) pos = (pos + 1) & (capacity - 1);
      slots[pos] = set->slots[i];
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;
  }
  size_t pos = identity_hash(st->st_dev, st->st_ino, set->capacity);
  while(set->slots[pos].used) {
    if(set->slots[pos].dev == st->st_dev && set->slots[pos].ino == st->st_ino) return 0;
    pos = (pos + 1) & (set->capacity - 1);
  }
  set->slots[pos] = (identity_t){st->st_dev, st->st_ino, 1};
  set->count++;
  return 1;
}

static int
walk_chmod(const char *path, mode_t mode,
            int (*cancelled)(void *), void *context, int sync_files,
            bfpilot_tree_totals_t *totals) {
  if(totals) memset(totals, 0, sizeof(*totals));
  struct stat root;
  if(!path || !*path) return -EINVAL;
  if(lstat(path, &root) != 0) return -errno;
  if(!S_ISDIR(root.st_mode)) return -ENOTDIR;
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if(fd < 0) return -errno;
  struct stat opened;
  if(fstat(fd, &opened) != 0) { int error = errno; close(fd); return -error; }
  if(opened.st_dev != root.st_dev || opened.st_ino != root.st_ino) {
    close(fd); return -EAGAIN;
  }
  DIR *first = fdopendir(fd);
  if(!first) { int error = errno; close(fd); return -error; }
  /* Heap frames avoid both unbounded recursion and path-sized stack frames. */
  DIR **frames = calloc(CHMOD_MAX_DEPTH + 1, sizeof(*frames));
  if(!frames) { closedir(first); return -ENOMEM; }
  visited_t visited = {0};
  int rc = visit(&visited, &root);
  if(rc < 0) { closedir(first); free(frames); free(visited.slots); return rc; }
  size_t depth = 0;
  frames[0] = first;
  rc = 0;
  for(;;) {
    if(cancelled && cancelled(context)) { rc = -ECANCELED; break; }
    DIR *dir = frames[depth];
    errno = 0;
    struct dirent *entry = readdir(dir);
    if(!entry) {
      if(errno) { rc = -errno; break; }
      /* Post-order preserves access when removing directory search bits. */
      if(fchmod(dirfd(dir), mode) != 0) { rc = -errno; break; }
      frames[depth] = NULL;
      if(closedir(dir) != 0) { rc = -errno; break; }
      if(depth == 0) break;
      depth--;
      continue;
    }
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat child;
    if(bfpilot_fs_fstatat(dirfd(dir), entry->d_name, &child, AT_SYMLINK_NOFOLLOW) != 0) {
      rc = -errno; break;
    }
    if(S_ISLNK(child.st_mode)) continue;
    if(child.st_dev != root.st_dev) { rc = -EXDEV; break; }
    if(!S_ISDIR(child.st_mode) && !S_ISREG(child.st_mode)) { rc = -ENOTSUP; break; }
    if(S_ISDIR(child.st_mode) && depth == CHMOD_MAX_DEPTH) { rc = -ELOOP; break; }
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
    if(S_ISDIR(child.st_mode)) flags |= O_DIRECTORY;
    fd = bfpilot_fs_openat(dirfd(dir), entry->d_name, flags, 0);
    if(fd < 0) { rc = -errno; break; }
    if(fstat(fd, &opened) != 0) { rc = -errno; close(fd); break; }
    if(opened.st_dev != root.st_dev || opened.st_ino != child.st_ino ||
       (opened.st_mode & S_IFMT) != (child.st_mode & S_IFMT)) {
      close(fd); rc = -EAGAIN; break;
    }
    if(S_ISDIR(child.st_mode)) {
      int fresh = visit(&visited, &child);
      if(fresh <= 0) {
        close(fd);
        if(fresh < 0) { rc = fresh; break; }
        continue;
      }
      DIR *next = fdopendir(fd);
      if(!next) { rc = -errno; close(fd); break; }
      frames[++depth] = next;
    } else {
      if(fchmod(fd, mode) != 0) rc = -errno;
      if(rc == 0 && sync_files && fsync(fd) != 0) rc = -errno;
      if(rc == 0 && totals) {
        unsigned long long bytes = opened.st_size > 0 ? (unsigned long long)opened.st_size : 0;
        if(totals->files == ULLONG_MAX || bytes > ULLONG_MAX - totals->bytes)
          rc = -EOVERFLOW;
        else { totals->files++; totals->bytes += bytes; }
      }
      if(close(fd) != 0 && rc == 0) rc = -errno;
      if(rc != 0) break;
    }
  }
  for(size_t i = 0; i <= CHMOD_MAX_DEPTH; i++) {
    if(frames[i] && closedir(frames[i]) != 0 && rc == 0) rc = -errno;
  }
  free(frames);
  free(visited.slots);
  return rc;
}

int bfpilot_tree_chmod(const char *path, mode_t mode,
                       int (*cancelled)(void *), void *context) {
  return walk_chmod(path, mode, cancelled, context, 0, NULL);
}
int bfpilot_tree_finalize(const char *path, mode_t mode,
                          int (*cancelled)(void *), void *context) {
  return walk_chmod(path, mode, cancelled, context, 1, NULL);
}
int bfpilot_tree_finalize_stats(const char *path, mode_t mode,
    int (*cancelled)(void *), void *context, bfpilot_tree_totals_t *totals) {
  return walk_chmod(path, mode, cancelled, context, 1, totals);
}
