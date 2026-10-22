#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "archive_batch_store.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_sync, fail_rename, fail_close, fail_truncate, partial_write;
int __real_fsync(int);
int __wrap_fsync(int fd) {
  if (fail_sync) { fail_sync = 0; errno = EIO; return -1; }
  return __real_fsync(fd);
}
int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int a, const char *b, int c, const char *d) {
  if (fail_rename) { fail_rename = 0; errno = EIO; return -1; }
  return __real_renameat(a, b, c, d);
}
int __real_close(int);
int __wrap_close(int fd) {
  int rc = __real_close(fd);
  if (fail_close) { fail_close = 0; errno = EIO; return -1; }
  return rc;
}
int __real_ftruncate(int, off_t);
int __wrap_ftruncate(int fd, off_t size) {
  if (fail_truncate) { fail_truncate = 0; errno = EIO; return -1; }
  return __real_ftruncate(fd, size);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t n) {
  if (partial_write == 1) { partial_write = 2; return __real_write(fd, data, n < 7 ? n : 7); }
  if (partial_write == 2) { partial_write = 0; errno = EIO; return -1; }
  return __real_write(fd, data, n);
}
static bfab_item_t *items, *restored;
static bfab_job_t make_job(unsigned count, const char *id) {
  bfab_job_t j = {0}; strcpy(j.id, id); j.revision = 1;
  j.state = BFAB_PREPARED; j.items = items; j.count = count;
  memset(items, 0, BFAB_MAX_ITEMS * sizeof(*items));
  for (unsigned i = 0; i < count; i++) {
    snprintf(items[i].source, sizeof(items[i].source), "/data/archive-%u.zip", i);
    snprintf(items[i].destination, sizeof(items[i].destination), "/data/output-%u", i);
  }
  return j;
}
static bfab_event_t event(const bfab_job_t *j, unsigned kind, int result) {
  bfab_event_t e = {0}; strcpy(e.id, j->id); e.sequence = j->revision + 1;
  e.kind = kind; e.index = kind == BFAB_CANCEL || kind == BFAB_HALT ? UINT32_MAX : j->next;
  e.result = result; return e;
}
static void append(int dir, bfab_job_t *j, unsigned kind, int result) {
  bfab_event_t e = event(j, kind, result); assert(!bfab_store_append(dir, j, &e));
}
static void raw_tail(int dir, const char *id, const void *p, size_t n) {
  char name[80]; snprintf(name, sizeof(name), "%s.events", id);
  int fd = openat(dir, name, O_WRONLY | O_APPEND); assert(fd >= 0);
  assert(write(fd, p, n) == (ssize_t)n); assert(!close(fd));
}
static bfab_job_t load(int dir, unsigned expected_tail) {
  bfab_job_t j; unsigned torn = 0;
  assert(!bfab_store_load(dir, &j, restored, BFAB_MAX_ITEMS, &torn));
  assert(torn == expected_tail); return j;
}
int main(void) {
  char root[] = "/tmp/bfpilot-batch-store-r23-XXXXXX"; assert(mkdtemp(root));
  items = calloc(BFAB_MAX_ITEMS, sizeof(*items)); restored = calloc(BFAB_MAX_ITEMS, sizeof(*items));
  assert(items && restored);
  int dir = bfab_store_open(root); assert(dir >= 0);
  bfab_job_t j, out; unsigned torn = 0, groups = 0;
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) == -ENOENT);
  j = make_job(3, "initial"); assert(!bfab_store_publish(dir, &j));
  struct stat st; assert(!fstatat(dir, "manifest.bin", &st, AT_SYMLINK_NOFOLLOW));
  assert((st.st_mode & 0777) == 0600);
  assert(!fstatat(dir, "initial.events", &st, AT_SYMLINK_NOFOLLOW)); assert((st.st_mode & 0777) == 0600);
  out = load(dir, 0); assert(out.state == BFAB_PREPARED && out.revision == 1); groups++;
  /* Publication failure leaves the old manifest recoverable. */
  const char *ids[] = {"failed-sync", "failed-close", "failed-rename", "failed-write"};
  for (unsigned i = 0; i < 4; i++) {
    j = make_job(3, ids[i]);
    if (i == 0) fail_sync = 1;
    if (i == 1) fail_close = 1;
    if (i == 2) fail_rename = 1;
    if (i == 3) partial_write = 1;
    assert(bfab_store_publish(dir, &j) == -EIO);
    out = load(dir, 0); assert(!strcmp(out.id, "initial"));
    char name[80]; snprintf(name, sizeof(name), "%s.events", ids[i]);
    assert(fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
  }
  groups++;
  j = make_job(3, "initial");
  append(dir, &j, BFAB_BEGIN, 0); append(dir, &j, BFAB_FINISH, 0);
  append(dir, &j, BFAB_BEGIN, 0); append(dir, &j, BFAB_FINISH, EACCES);
  out = load(dir, 0); assert(out.next == 2 && out.failed == 1 && out.succeeded == 1);
  append(dir, &out, BFAB_CANCEL, 0); out = load(dir, 0);
  assert(out.state == BFAB_CANCELLED && restored[2].state == BFAB_PENDING); groups++;
  /* Every possible short tail, followed by an actual successful append. */
  unsigned char bytes[BFAB_EVENT_SIZE]; memset(bytes, 0xa5, sizeof(bytes));
  for (unsigned n = 1; n < BFAB_EVENT_SIZE; n++) {
    char id[40]; snprintf(id, sizeof(id), "tail-%u", n);
    j = make_job(2, id); assert(!bfab_store_publish(dir, &j)); append(dir, &j, BFAB_BEGIN, 0);
    raw_tail(dir, j.id, bytes, n); out = load(dir, n);
    assert(out.revision == 2 && restored[0].state == BFAB_ACTIVE);
    append(dir, &out, BFAB_FINISH, 0); out = load(dir, 0); assert(out.next == 1);
  }
  groups++;
  j = make_job(2, "partial-append"); assert(!bfab_store_publish(dir, &j));
  bfab_event_t e = event(&j, BFAB_BEGIN, 0); partial_write = 1;
  assert(bfab_store_append(dir, &j, &e) == -EIO && j.revision == 1 && items[0].state == BFAB_PENDING);
  out = load(dir, 7); assert(out.revision == 1); append(dir, &out, BFAB_BEGIN, 0); groups++;
  /* A complete append can exist despite a reported sync/close failure. The
     caller must stop; reloading, not retrying, establishes the next revision. */
  for (unsigned i = 0; i < 2; i++) {
    j = make_job(2, i ? "close-append" : "sync-append"); assert(!bfab_store_publish(dir, &j));
    e = event(&j, BFAB_BEGIN, 0); if (i) fail_close = 1; else fail_sync = 1;
    assert(bfab_store_append(dir, &j, &e) == -EIO && j.revision == 1);
    out = load(dir, 0); assert(out.revision == 2 && restored[0].state == BFAB_ACTIVE);
    append(dir, &out, BFAB_HALT, EIO); out = load(dir, 0); assert(out.state == BFAB_ERROR);
  }
  groups++;
  j = make_job(2, "truncate-failure"); assert(!bfab_store_publish(dir, &j));
  raw_tail(dir, j.id, bytes, 7); fail_truncate = 1;
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) == -EIO && !out.items);
  out = load(dir, 7); assert(out.revision == 1); groups++;
  j = make_job(2, "corrupt-complete"); assert(!bfab_store_publish(dir, &j));
  raw_tail(dir, j.id, bytes, sizeof(bytes));
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) < 0 && !out.items);
  assert(!fstatat(dir, "corrupt-complete.events", &st, 0) && st.st_size == BFAB_EVENT_SIZE); groups++;
  j = make_job(2, "foreign-event"); assert(!bfab_store_publish(dir, &j));
  e = event(&j, BFAB_BEGIN, 0); strcpy(e.id, "other"); assert(!bfab_event_encode(&e, bytes, sizeof(bytes)));
  raw_tail(dir, j.id, bytes, sizeof(bytes));
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) < 0); groups++;
  j = make_job(2, "missing-event-file"); assert(!bfab_store_publish(dir, &j));
  assert(!unlinkat(dir, "missing-event-file.events", 0));
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) == -EPROTO); groups++;
  /* Symlinks and public permissions fail before replay. */
  j = make_job(2, "symlink-event-file"); assert(!bfab_store_publish(dir, &j));
  assert(!unlinkat(dir, "symlink-event-file.events", 0));
  assert(!symlinkat("manifest.bin", dir, "symlink-event-file.events"));
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) < 0);
  assert(!unlinkat(dir, "symlink-event-file.events", 0));
  int fd = openat(dir, "symlink-event-file.events", O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
  assert(!fchmod(fd, 0644) && !close(fd));
  assert(bfab_store_load(dir, &out, restored, BFAB_MAX_ITEMS, &torn) == -EACCES); groups++;
  /* Real maximum manifest and all 4096 durable events, including final record. */
  j = make_job(BFAB_MAX_ITEMS, "capacity-2048"); assert(!bfab_store_publish(dir, &j));
  for (unsigned i = 0; i < j.count; i++) {
    append(dir, &j, BFAB_BEGIN, 0); append(dir, &j, BFAB_FINISH, i % 3 ? 0 : EIO);
  }
  out = load(dir, 0);
  assert(out.state == BFAB_FINISHED && out.next == 2048 && out.failed == 683 &&
         out.succeeded == 1365 && out.revision == 4097);
  assert(!fstatat(dir, "capacity-2048.events", &st, 0) && st.st_size == 409600);
  groups++;
  assert(!bfab_store_retire(dir, "initial"));
  assert(bfab_store_retire(dir, "../manifest.bin") < 0); groups++;
  assert(!close(dir)); assert(!chmod(root, 0755));
  assert(bfab_store_open(root) == -EACCES); assert(!chmod(root, 0700));
  DIR *scan = opendir(root); assert(scan);
  struct dirent *entry;
  while ((entry = readdir(scan))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    assert(!unlinkat(dirfd(scan), entry->d_name, 0));
  }
  assert(!closedir(scan) && !rmdir(root)); free(restored); free(items);
  printf("archive batch store: %u groups passed (99 tails, injected I/O faults, 2048 items/4096 durable events)\n", groups);
  return 0;
}
