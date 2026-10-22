#include "archive_batch_store.h"
#include "fs_at.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANIFEST "manifest.bin"
static int exact_io(int fd, void *buffer, size_t size, int writing) {
  unsigned char *p = buffer;
  while (size) {
    ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 ? -errno : -EIO;
    p += n; size -= (size_t)n;
  }
  return 0;
}
static int checked_close(int fd, int result) {
  if (close(fd) && !result) return -errno;
  return result;
}
static int private_file(int fd, struct stat *st, size_t limit) {
  if (fstat(fd, st)) return -errno;
  if (!S_ISREG(st->st_mode) || (st->st_mode & 077) ||
      (st->st_uid != geteuid() && st->st_uid != 0) || st->st_size < 0 ||
      (uint64_t)st->st_size > limit) return -EACCES;
  return 0;
}
/* Reuse the public codec's exact ID validation instead of trusting filenames. */
static int event_name(const char *id, char name[BFAB_ID_SIZE + 8]) {
  if (!id) return -EINVAL;
  bfab_event_t e = {0};
  size_t n = 0;
  while (n < BFAB_ID_SIZE && id[n]) n++;
  if (n == BFAB_ID_SIZE) return -EINVAL;
  memcpy(e.id, id, n);
  e.sequence = 1; e.kind = BFAB_CANCEL; e.index = UINT32_MAX;
  unsigned char data[BFAB_EVENT_SIZE];
  if (bfab_event_encode(&e, data, sizeof(data))) return -EINVAL;
  snprintf(name, BFAB_ID_SIZE + 8, "%s.events", id);
  return 0;
}
int bfab_store_open(const char *path) {
  if (!path) return -EINVAL;
  if (mkdir(path, 0700) && errno != EEXIST) return -errno;
  struct stat before, after;
  if (lstat(path, &before)) return -errno;
  if (!S_ISDIR(before.st_mode) || (before.st_mode & 077) ||
      (before.st_uid != geteuid() && before.st_uid != 0)) return -EACCES;
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0) return -errno;
  int rc = fstat(fd, &after) ? -errno : 0;
  if (!rc && (before.st_ino != after.st_ino || before.st_dev != after.st_dev)) rc = -EIO;
  if (rc) { (void)close(fd); return rc; }
  return fd;
}
int bfab_store_publish(int dir, const bfab_job_t *j) {
  if (bfab_validate(j) || j->state != BFAB_PREPARED || j->revision != 1)
    return -EINVAL;
  unsigned char *data = malloc(BFAB_MANIFEST_MAX);
  if (!data) return -ENOMEM;
  size_t size = 0;
  int rc = bfab_manifest_encode(j, data, BFAB_MANIFEST_MAX, &size);
  char name[BFAB_ID_SIZE + 8], temporary[BFAB_ID_SIZE + 16];
  if (!rc) rc = event_name(j->id, name);
  int journal = -1;
  if (!rc) {
    journal = bfpilot_fs_openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (journal < 0) rc = -errno;
    else { rc = fsync(journal) ? -errno : 0; rc = checked_close(journal, rc); }
  }
  if (rc) {
    if (journal >= 0) (void)bfpilot_fs_unlinkat(dir, name, 0);
    free(data); return rc;
  }
  snprintf(temporary, sizeof(temporary), ".manifest-%s.tmp", j->id);
  int fd = bfpilot_fs_openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) rc = -errno;
  else {
    rc = exact_io(fd, data, size, 1);
    if (!rc && fsync(fd)) rc = -errno;
    rc = checked_close(fd, rc);
    if (!rc && bfpilot_fs_renameat(dir, temporary, dir, MANIFEST)) rc = -errno;
    if (rc) (void)bfpilot_fs_unlinkat(dir, temporary, 0);
  }
  if (rc) (void)bfpilot_fs_unlinkat(dir, name, 0);
  free(data);
  return rc;
}
int bfab_store_load(int dir, bfab_job_t *out, bfab_item_t *items,
                    size_t capacity, unsigned *torn_bytes) {
  if (!out || !torn_bytes) return -EINVAL;
  memset(out, 0, sizeof(*out)); *torn_bytes = 0;
  int fd = bfpilot_fs_openat(dir, MANIFEST, O_RDONLY | O_NOFOLLOW, 0);
  if (fd < 0) return -errno;
  struct stat st;
  int rc = private_file(fd, &st, BFAB_MANIFEST_MAX);
  unsigned char *data = NULL;
  if (!rc) {
    data = malloc((size_t)st.st_size + 1);
    if (!data) rc = -ENOMEM;
    else rc = exact_io(fd, data, (size_t)st.st_size, 0);
  }
  rc = checked_close(fd, rc);
  bfab_job_t j = {0};
  if (!rc) rc = bfab_manifest_decode(data, (size_t)st.st_size, &j, items, capacity);
  free(data);
  if (!rc && (j.state != BFAB_PREPARED || j.revision != 1)) rc = -EPROTO;
  if (rc) return rc;
  char name[BFAB_ID_SIZE + 8];
  if ((rc = event_name(j.id, name))) return rc;
  fd = bfpilot_fs_openat(dir, name, O_RDWR | O_NOFOLLOW, 0);
  if (fd < 0) return errno == ENOENT ? -EPROTO : -errno;
  rc = private_file(fd, &st, (2U * j.count + 2U) * BFAB_EVENT_SIZE);
  unsigned tail = 0;
  if (!rc) {
    size_t records = (size_t)st.st_size / BFAB_EVENT_SIZE;
    tail = (unsigned)((size_t)st.st_size % BFAB_EVENT_SIZE);
    for (size_t i = 0; !rc && i < records; i++) {
      unsigned char record[BFAB_EVENT_SIZE]; bfab_event_t event;
      rc = exact_io(fd, record, sizeof(record), 0);
      if (!rc) rc = bfab_event_decode(record, sizeof(record), &event);
      if (!rc) rc = bfab_event_apply(&j, &event);
    }
    if (!rc && tail) {
      if (ftruncate(fd, st.st_size - tail)) rc = -errno;
      if (!rc && fsync(fd)) rc = -errno;
    }
  }
  rc = checked_close(fd, rc);
  if (rc) return rc;
  *torn_bytes = tail; *out = j;
  return 0;
}
int bfab_store_append(int dir, bfab_job_t *j, const bfab_event_t *event) {
  int rc = bfab_event_check(j, event);
  if (rc) return rc;
  if (j->revision > 2U * j->count + 2U) return -E2BIG;
  unsigned char record[BFAB_EVENT_SIZE];
  if ((rc = bfab_event_encode(event, record, sizeof(record)))) return rc;
  char name[BFAB_ID_SIZE + 8];
  if ((rc = event_name(j->id, name))) return rc;
  int fd = bfpilot_fs_openat(dir, name, O_WRONLY | O_APPEND | O_NOFOLLOW, 0);
  if (fd < 0) return -errno;
  struct stat st;
  rc = private_file(fd, &st, (2U * j->count + 2U) * BFAB_EVENT_SIZE);
  if (!rc && (uint64_t)st.st_size != (j->revision - 1) * BFAB_EVENT_SIZE) rc = -EPROTO;
  if (!rc) rc = exact_io(fd, record, sizeof(record), 1);
  if (!rc && fsync(fd)) rc = -errno;
  rc = checked_close(fd, rc);
  if (!rc) rc = bfab_event_apply(j, event);
  return rc;
}
int bfab_store_retire(int dir, const char *id) {
  char name[BFAB_ID_SIZE + 8];
  int rc = event_name(id, name);
  if (rc) return rc;
  if (bfpilot_fs_unlinkat(dir, name, 0) && errno != ENOENT) return -errno;
  return 0;
}
