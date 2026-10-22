#include "tree_walk.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WALK_DEPTH 64U
#define WALK_DIRS 262144U
#define WALK_PATH 1024U
typedef struct { dev_t dev; ino_t ino; int used; } identity_t;
typedef struct { identity_t *slots; size_t count, capacity; } visited_t;
typedef struct { DIR *dir; struct stat st; char path[WALK_PATH]; } frame_t;

static size_t hash_id(dev_t dev, ino_t ino, size_t capacity) {
  return (size_t)((uint64_t)dev * UINT64_C(0x9e3779b97f4a7c15) ^
                  (uint64_t)ino * UINT64_C(0xbf58476d1ce4e5b9)) & (capacity - 1);
}
static int visit(visited_t *set, const struct stat *st) {
  if(set->count >= WALK_DIRS) return -E2BIG;
  if(!set->capacity || set->count * 2 >= set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 128;
    identity_t *slots = calloc(capacity, sizeof(*slots));
    if(!slots) return -ENOMEM;
    for(size_t i = 0; i < set->capacity; i++) {
      if(!set->slots[i].used) continue;
      size_t pos = hash_id(set->slots[i].dev, set->slots[i].ino, capacity);
      while(slots[pos].used) pos = (pos + 1) & (capacity - 1);
      slots[pos] = set->slots[i];
    }
    free(set->slots); set->slots = slots; set->capacity = capacity;
  }
  size_t pos = hash_id(st->st_dev, st->st_ino, set->capacity);
  while(set->slots[pos].used) {
    if(set->slots[pos].dev == st->st_dev && set->slots[pos].ino == st->st_ino)
      return -ELOOP;
    pos = (pos + 1) & (set->capacity - 1);
  }
  set->slots[pos] = (identity_t){st->st_dev, st->st_ino, 1};
  set->count++;
  return 0;
}
static int open_directory(int parent, const char *name, const struct stat *st,
                          DIR **result) {
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if(fd < 0) return -errno;
  struct stat opened;
  int rc = fstat(fd, &opened);
  if(rc != 0 || opened.st_dev != st->st_dev || opened.st_ino != st->st_ino) {
    int error = rc != 0 ? errno : EAGAIN; close(fd); return -error;
  }
  *result = fdopendir(fd);
  if(!*result) { int error = errno; close(fd); return -error; }
  return 0;
}
static int walk(const char *path, dev_t device,
    int (*entry)(const char *, const char *, const struct stat *, void *),
    int (*cancelled)(void *), void *context, int postorder) {
  if(!path || !*path || !entry) return -EINVAL;
  size_t root_len = strlen(path);
  if(root_len >= WALK_PATH) return -ENAMETOOLONG;
  struct stat st;
  if(lstat(path, &st) != 0) return -errno;
  if(st.st_dev != device) return -EXDEV;
  if(cancelled && cancelled(context)) return -ECANCELED;
  if(!S_ISDIR(st.st_mode)) return entry(path, "", &st, context);
  frame_t *frames = calloc(WALK_DEPTH + 1, sizeof(*frames));
  if(!frames) return -ENOMEM;
  visited_t visited = {0};
  int rc = open_directory(AT_FDCWD, path, &st, &frames[0].dir);
  size_t depth = 0;
  if(rc == 0) rc = visit(&visited, &st);
  if(rc == 0 && !postorder) rc = entry(path, "", &st, context);
  frames[0].st = st;
  memcpy(frames[0].path, path, root_len + 1);
  /* Normalize trailing separators for relative names without modifying input. */
  while(root_len > 1 && frames[0].path[root_len - 1] == '/')
    frames[0].path[--root_len] = 0;
  char *child = malloc(WALK_PATH);
  if(!child && rc == 0) rc = -ENOMEM;
  while(rc == 0) {
    if(cancelled && cancelled(context)) { rc = -ECANCELED; break; }
    DIR *dir = frames[depth].dir;
    errno = 0;
    struct dirent *ent = readdir(dir);
    if(!ent) {
      if(errno) { rc = -errno; break; }
      frames[depth].dir = NULL;
      if(closedir(dir) != 0) { rc = -errno; break; }
      if(postorder) {
        const char *relative = frames[depth].path + root_len;
        if(*relative == '/') relative++;
        rc = entry(frames[depth].path, relative, &frames[depth].st, context);
        if(rc != 0) break;
      }
      if(depth == 0) break;
      depth--; continue;
    }
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    int n = snprintf(child, WALK_PATH, "%s%s%s", frames[depth].path,
                     !strcmp(frames[depth].path, "/") ? "" : "/", ent->d_name);
    if(n < 0 || n >= (int)WALK_PATH) { rc = -ENAMETOOLONG; break; }
    if(fstatat(dirfd(dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      rc = -errno; break;
    }
    if(st.st_dev != device) { rc = -EXDEV; break; }
    DIR *next = NULL;
    if(S_ISDIR(st.st_mode)) {
      if(depth == WALK_DEPTH) { rc = -ELOOP; break; }
      rc = visit(&visited, &st);
      if(rc == 0) rc = open_directory(dirfd(dir), ent->d_name, &st, &next);
      if(rc != 0) break;
    }
    const char *relative = child + root_len;
    if(*relative == '/') relative++;
    if(!next || !postorder) rc = entry(child, relative, &st, context);
    if(rc != 0) { if(next) closedir(next); break; }
    if(next) {
      frames[++depth].dir = next;
      frames[depth].st = st;
      memcpy(frames[depth].path, child, (size_t)n + 1);
    }
  }
  for(size_t i = 0; i <= WALK_DEPTH; i++)
    if(frames[i].dir && closedir(frames[i].dir) != 0 && rc == 0) rc = -errno;
  free(child); free(frames); free(visited.slots);
  return rc;
}
int bfpilot_tree_walk(const char *path, dev_t device,
    int (*entry)(const char *, const char *, const struct stat *, void *),
    int (*cancelled)(void *), void *context) {
  return walk(path, device, entry, cancelled, context, 0);
}
int bfpilot_tree_walk_post(const char *path, dev_t device,
    int (*entry)(const char *, const char *, const struct stat *, void *),
    int (*cancelled)(void *), void *context) {
  return walk(path, device, entry, cancelled, context, 1);
}
