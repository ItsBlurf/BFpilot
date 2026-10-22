/* Run the production walker over real host directories; only the endpoint
 * boundary is a small POSIX test adapter. No SMB behavior is simulated here. */
#define _GNU_SOURCE
#include "../src/remote_io.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct bfpilot_io {
  int dummy;
};
struct bfpilot_io_dir {
  DIR *dir;
  int cycle_sent;
};
static char root[1024];
static int opened, mode, entries, dirs, files, last_root;
int bfpilot_io_lstat(bfpilot_io_t *io, const char *path, struct stat *st) {
  (void)io;
  return lstat(path, st) ? -errno : 0;
}
int bfpilot_io_opendir(bfpilot_io_t *io, const char *path,
                       bfpilot_io_dir_t **out) {
  (void)io;
  *out = calloc(1, sizeof(**out));
  if (!*out)
    return -ENOMEM;
  (*out)->dir = opendir(path);
  if (!(*out)->dir) {
    int rc = -errno;
    free(*out);
    *out = NULL;
    return rc;
  }
  opened++;
  return 0;
}
int bfpilot_io_closedir(bfpilot_io_dir_t *dir) {
  int rc = closedir(dir->dir) ? -errno : 0;
  opened--;
  free(dir);
  return rc;
}
int bfpilot_io_readdir(bfpilot_io_dir_t *dir, char *name, size_t capacity,
                       struct stat *st) {
  if (mode == 2)
    return -EIO;
  if (mode == 3 && !dir->cycle_sent) {
    dir->cycle_sent = 1;
    snprintf(name, capacity, "cycle");
    return lstat(root, st) ? -errno : 1;
  }
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir->dir);
    if (!entry)
      return errno ? -errno : 0;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (strlen(entry->d_name) >= capacity)
      return -ENAMETOOLONG;
    snprintf(name, capacity, "%s", entry->d_name);
    return fstatat(dirfd(dir->dir), name, st, AT_SYMLINK_NOFOLLOW) ? -errno : 1;
  }
}
static int visit(const char *path, const char *relative, const struct stat *st,
                 void *context) {
  (void)context;
  entries++;
  dirs += S_ISDIR(st->st_mode);
  files += S_ISREG(st->st_mode);
  last_root = !strcmp(path, root);
  if (!*relative && strcmp(path, root))
    return -EINVAL;
  return 0;
}
static int cancel(void *ctx) {
  (void)ctx;
  return mode == 1 && entries >= 10;
}
static void reset(int next) {
  entries = dirs = files = last_root = 0;
  mode = next;
}
static int failures;
static void check(const char *name, int pass) {
  printf("%s %s\n", pass ? "PASS" : "FAIL", name);
  failures += !pass;
}
int main(void) {
  snprintf(root, sizeof(root), "/tmp/bfpilot-remote-walk-XXXXXX");
  if (!mkdtemp(root))
    return 2;
  char path[1024];
  snprintf(path, sizeof(path), "%s", root);
  for (int i = 0; i < 64; i++) {
    strcat(path, "/d");
    if (mkdir(path, 0700))
      return 3;
  }
  strcat(path, "/file");
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 4;
  if(write(fd, "x", 1)!=1)return 5;
  close(fd);
  struct bfpilot_io endpoint = {0};
  reset(0);
  int rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("deepest allowed directory plus file fits frames",
        rc == 0 && dirs == 65 && files == 1 && !opened);
  reset(0);
  rc = bfpilot_io_walk(&endpoint, root, 1, visit, cancel, NULL);
  check("postorder root last and all handles closed",
        rc == 0 && last_root && dirs == 65 && files == 1 && !opened);
  char *tail = strrchr(path, '/');
  *tail = 0;
  strcat(path, "/d");
  mkdir(path, 0700);
  reset(0);
  rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("excessive directory depth refused", rc == -ELOOP && !opened);
  rmdir(path);
  *strrchr(path, '/') = 0;
  reset(1);
  rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("cancellation closes complete active stack",
        rc == -ECANCELED && entries == 10 && !opened);
  reset(2);
  rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("enumeration error aborts and closes stack", rc == -EIO && !opened);
  reset(3);
  rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("repeated directory identity refused before open",
        rc == -ELOOP && !opened);
  char link[1024];
  snprintf(link, sizeof(link), "%s/link", root);
  if(symlink(root, link)!=0)return 6;
  reset(0);
  rc = bfpilot_io_walk(&endpoint, root, 0, visit, cancel, NULL);
  check("advertised symlink not followed", rc == -ENOTSUP && !opened);
  unlink(link);
  char file[1024];
  snprintf(file, sizeof(file), "%s/file", path);
  unlink(file);
  for (int i = 0; i < 64; i++) {
    rmdir(path);
    *strrchr(path, '/') = 0;
  }
  rmdir(root);
  printf("SUMMARY failures=%d\n", failures);
  return failures != 0;
}
