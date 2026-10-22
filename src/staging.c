#include "fs_at.h"
#include "staging.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef BFPILOT_STAGE_TRACE
#define BFPILOT_STAGE_TRACE(...) ((void)0)
#endif

static atomic_uint stage_sequence;

static int contains(const char *parent, const char *path) {
  size_t n = strlen(parent);
  return !strcmp(parent, "/") ||
      (!strncmp(parent, path, n) && (!path[n] || path[n] == '/'));
}

static int volume_matches(const char *path, dev_t device) {
  struct stat st;
  if(stat(path, &st) == 0) return st.st_dev == device;
  if(errno == EIO || errno == ESTALE || errno == EBADF || errno == EFAULT)
    return -1;
  return 0;
}

static int copy_path(char *out, size_t size, const char *path) {
  int n = snprintf(out, size, "%s", path);
  if(n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
  return 0;
}

static int child_directory(int parent, const char *name, dev_t device) {
  int mkdir_rc = bfpilot_fs_mkdirat(parent, name, 0700);
  BFPILOT_STAGE_TRACE("mkdirat parent=%d name=%s rc=%d errno=%d\n",parent,name,mkdir_rc,errno);
  if(mkdir_rc != 0 && errno != EEXIST) return -1;
  int fd = bfpilot_fs_openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
  BFPILOT_STAGE_TRACE("openat parent=%d name=%s fd=%d errno=%d\n",parent,name,fd,errno);
  if(fd < 0) return -1;
  struct stat st;
  int rc = fstat(fd, &st);
  BFPILOT_STAGE_TRACE("fstat child fd=%d rc=%d dev=%lu expected=%lu errno=%d\n",fd,rc,(unsigned long)st.st_dev,(unsigned long)device,errno);
  if(rc != 0 || st.st_dev != device) {
    int error = rc != 0 ? errno : EXDEV;
    close(fd); errno = error; return -1;
  }
  return fd;
}

static int stage_at(const char *root, const struct stat *destination,
                     const char *kind, const char *avoid, char *out, size_t size) {
  /* A nested game mount is not a suitable staging volume. Do not fall back to
   * putting partially written executables in a watched homebrew tree. */
  const char *homebrew = strstr(root, "/homebrew");
  if(homebrew && (!homebrew[9] || homebrew[9] == '/')) {
    errno = EXDEV; return -1;
  }
  char *canonical = realpath(root, NULL);
  BFPILOT_STAGE_TRACE("realpath root=%s canonical=%s errno=%d\n",root,canonical?canonical:"null",errno);
  if(!canonical) return -1;
  int application_base = !strcmp(root, "/data");
  char candidate[1024];
  int n = snprintf(candidate, sizeof(candidate), "%s%s%s", canonical,
      !strcmp(canonical, "/") ? "" : "/",
      application_base ? "BFpilot/staging" : ".bfpilot-staging");
  if(n < 0 || (size_t)n >= sizeof(candidate)) {
    free(canonical); errno = ENAMETOOLONG; return -1;
  }
  if(avoid && contains(avoid, candidate)) {
    /* Backing up BFpilot's own data must not copy its growing private stage. */
    application_base = 0;
    n = snprintf(candidate, sizeof(candidate), "%s%s.bfpilot-staging", canonical,
                 !strcmp(canonical, "/") ? "" : "/");
    if(n < 0 || (size_t)n >= sizeof(candidate) ||
       contains(avoid, candidate)) {
      free(canonical); errno = EINVAL; return -1;
    }
  }
  int fd = open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  BFPILOT_STAGE_TRACE("open root=%s fd=%d errno=%d\n",canonical,fd,errno);
  free(canonical);
  if(fd < 0) return -1;
  struct stat st;
  int rc = fstat(fd, &st);
  BFPILOT_STAGE_TRACE("fstat root fd=%d rc=%d dev=%lu expected=%lu errno=%d\n",fd,rc,(unsigned long)st.st_dev,(unsigned long)destination->st_dev,errno);
  if(rc != 0 || st.st_dev != destination->st_dev) {
    int error = rc != 0 ? errno : EXDEV;
    close(fd); errno = error; return -1;
  }
  char base[1024];
  if(application_base) {
    int app = child_directory(fd, "BFpilot", destination->st_dev);
    int error = errno;
    close(fd);
    if(app < 0) { errno = error; return -1; }
    fd = child_directory(app, "staging", destination->st_dev);
    error = errno;
    close(app);
    if(fd < 0) { errno = error; return -1; }
    snprintf(base, sizeof(base), "/data/BFpilot/staging");
  } else {
    int next = child_directory(fd, ".bfpilot-staging", destination->st_dev);
    int error = errno;
    close(fd);
    if(next < 0) { errno = error; return -1; }
    fd = next;
    int n = snprintf(base, sizeof(base), "%s%s.bfpilot-staging", root,
                     !strcmp(root, "/") ? "" : "/");
    if(n < 0 || (size_t)n >= sizeof(base)) {
      close(fd); errno = ENAMETOOLONG; return -1;
    }
  }
  struct stat named;
  rc = fstat(fd, &st);
  if(rc == 0) rc = lstat(base, &named);
  BFPILOT_STAGE_TRACE("verify base=%s rc=%d opened=%lu/%lu named=%lu/%lu errno=%d\n",base,rc,(unsigned long)st.st_dev,(unsigned long)st.st_ino,(unsigned long)named.st_dev,(unsigned long)named.st_ino,errno);
  if(rc != 0 || !S_ISDIR(named.st_mode) || named.st_dev != st.st_dev ||
     named.st_ino != st.st_ino || named.st_dev != destination->st_dev) {
    int error = rc != 0 ? errno : EAGAIN;
    close(fd); errno = error; return -1;
  }
  if(close(fd) != 0) return -1;
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  unsigned sequence = atomic_fetch_add(&stage_sequence, 1);
  n = snprintf(out, size, "%s/%s-%ld-%ld-%ld-%u", base, kind,
                   (long)getpid(), (long)now.tv_sec, (long)now.tv_nsec, sequence);
  if(n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
  return 0;
}

static int stage_select(const char *parent, const char *kind, const char *avoid,
                         char *out, size_t size) {
  if(!parent || parent[0] != '/' || !kind || !*kind || strlen(kind) > 16 || !out || !size) {
    errno = EINVAL; return -1;
  }
  for(const char *p = kind; *p; p++) {
    if((*p < 'a' || *p > 'z') && *p != '-') { errno = EINVAL; return -1; }
  }
  struct stat destination, volume;
  if(stat(parent, &destination) != 0) return -1;
  if(!S_ISDIR(destination.st_mode)) { errno = ENOTDIR; return -1; }
  int match = volume_matches("/data", destination.st_dev);
  if(match < 0) return -1;
  if(match)
    return stage_at("/data", &destination, kind, avoid, out, size);
  for(int i = 0; i < 8; i++) {
    char root[32];
    snprintf(root, sizeof(root), "/mnt/usb%d", i);
    match = volume_matches(root, destination.st_dev);
    if(match < 0) return -1;
    if(match)
      return stage_at(root, &destination, kind, avoid, out, size);
    snprintf(root, sizeof(root), "/mnt/ext%d", i);
    match = volume_matches(root, destination.st_dev);
    if(match < 0) return -1;
    if(match)
      return stage_at(root, &destination, kind, avoid, out, size);
  }
  /* Other writable filesystems retain support. Ascend by stat identity to the
   * volume root without enumerating it or crossing its filesystem boundary. */
  char *canonical = realpath(parent, NULL);
  if(!canonical) return -1;
  char root[1024], ancestor[1024];
  int rc = copy_path(root, sizeof(root), canonical);
  free(canonical);
  if(rc != 0) return -1;
  while(strcmp(root, "/")) {
    if(copy_path(ancestor, sizeof(ancestor), root) != 0) return -1;
    char *slash = strrchr(ancestor, '/');
    if(slash == ancestor) slash[1] = 0;
    else *slash = 0;
    if(stat(ancestor, &volume) != 0) return -1;
    if(volume.st_dev != destination.st_dev) break;
    if(copy_path(root, sizeof(root), ancestor) != 0) return -1;
  }
  return stage_at(root, &destination, kind, avoid, out, size);
}
int bfpilot_stage_path_avoiding(const char *parent, const char *kind,
                               const char *source, char *out, size_t size) {
  char *canonical = source ? realpath(source, NULL) : NULL;
  if(source && !canonical) return -1;
  int rc = stage_select(parent, kind, canonical, out, size);
  int error = errno;
  free(canonical);
  errno = error;
  return rc;
}
int bfpilot_stage_path(const char *parent, const char *kind, char *out, size_t size) {
  return bfpilot_stage_path_avoiding(parent, kind, NULL, out, size);
}
