#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "archive_merge.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int sync_fault, rename_fault, publication_fault, close_fault;
int __real_fsync(int);
int __wrap_fsync(int fd) {
  if (sync_fault) {
    char link[64], name[2048];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, name, sizeof(name) - 1);
    if (n >= 0)
      name[n] = 0;
    if (n >= 0 && strstr(name, ".merge.tmp")) {
      sync_fault = 0;
      errno = EIO;
      return -1;
    }
  }
  return __real_fsync(fd);
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *a, const char *b) {
  if (rename_fault && strstr(a, ".merge.tmp")) {
    rename_fault = 0;
    errno = EIO;
    return -1;
  }
  return __real_rename(a, b);
}
int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int a, const char *b, int c, const char *d) {
  if (publication_fault && !strcmp(b, "replace.txt")) {
    publication_fault = 0;
    errno = EIO;
    return -1;
  }
  return __real_renameat(a, b, c, d);
}
int __real_close(int);
int __wrap_close(int fd) {
  int rc = __real_close(fd);
  if (close_fault) {
    close_fault = 0;
    errno = EIO;
    return -1;
  }
  return rc;
}
typedef struct {
  char root[1024], stage[1024], out[1024], manifest[1024];
} fixture_t;
typedef struct {
  unsigned files, cut;
  int cancel, close_after_first;
  const char *protect;
} control_t;
static char owned[1024];
static unsigned case_number, groups;
static void path(char *out, const char *base, const char *name) {
  size_t a = strlen(base), b = strlen(name);
  assert(a + b + 2 <= 1024);
  memcpy(out, base, a);
  out[a] = '/';
  memcpy(out + a + 1, name, b + 1);
}
static fixture_t fixture(void) {
  fixture_t f;
  char name[32], private[1024];
  snprintf(name, sizeof(name), "case-%u", ++case_number);
  path(f.root, owned, name);
  assert(!mkdir(f.root, 0700));
  path(private, f.root, "private");
  assert(!mkdir(private, 0700));
  path(f.stage, private, "stage");
  assert(!mkdir(f.stage, 0700));
  path(f.out, f.root, "out");
  assert(!mkdir(f.out, 0700));
  assert(strlen(f.stage) + 7 < sizeof(f.manifest));
  strcpy(f.manifest, f.stage);
  strcat(f.manifest, ".merge");
  return f;
}
static int missing(const char *p) {
  struct stat st;
  return lstat(p, &st) && errno == ENOENT;
}
static void put(const char *base, const char *name, const char *text) {
  char p[1024];
  path(p, base, name);
  int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
  assert(fd >= 0);
  size_t n = strlen(text);
  assert(write(fd, text, n) == (ssize_t)n && !fsync(fd) && !close(fd));
}
static void expect(const char *base, const char *name, const char *text) {
  char p[1024], bytes[1024];
  path(p, base, name);
  int fd = open(p, O_RDONLY | O_NOFOLLOW);
  assert(fd >= 0);
  ssize_t n = read(fd, bytes, sizeof(bytes));
  assert(n == (ssize_t)strlen(text) && !memcmp(bytes, text, (size_t)n) &&
         !close(fd));
}
static void inputs(fixture_t *f) {
  char p[1024];
  path(p, f->stage, "dir");
  assert(!mkdir(p, 0700));
  put(p, "new.txt", "complete nested new data");
  put(f->stage, "replace.txt", "complete replacement");
  put(f->stage, " fresh .txt ", "spaces preserved");
  put(f->out, "replace.txt", "complete original");
  put(f->out, "keep.txt", "unrelated original");
  path(p, f->out, "dir");
  assert(!mkdir(p, 0700));
  put(p, "keep.txt", "unrelated nested original");
}
static void expected(fixture_t *f) {
  char p[1024];
  expect(f->out, "replace.txt", "complete replacement");
  expect(f->out, " fresh .txt ", "spaces preserved");
  expect(f->out, "keep.txt", "unrelated original");
  path(p, f->out, "dir");
  expect(p, "new.txt", "complete nested new data");
  expect(p, "keep.txt", "unrelated nested original");
  struct stat st;
  assert(!lstat(p, &st) && (st.st_mode & 0777) == 0700);
}
static int cancel(void *opaque) { return ((control_t *)opaque)->cancel; }
static int protect(void *opaque, const char *destination,
                   const struct stat *st) {
  (void)st;
  control_t *c = opaque;
  return c->protect && strstr(destination, c->protect) ? -EPERM : 0;
}
static void progress(void *opaque, const char *relative, uint32_t done,
                     uint32_t total) {
  (void)done;
  (void)total;
  control_t *c = opaque;
  if (strstr(relative, ".txt")) {
    c->files++;
    if (c->cut && c->files == c->cut)
      c->cancel = 1;
    if (c->close_after_first && c->files == 1)
      close_fault = 1;
  }
}
static int merge(fixture_t *f, control_t *c, const char *id,
                 bfpilot_merge_result_t *r) {
  bfpilot_merge_options_t o = {c, cancel, protect, progress};
  return bfpilot_archive_merge(f->stage, f->out, id, f->manifest, &o, r);
}
static void complete(fixture_t *f, control_t *c, const char *id) {
  bfpilot_merge_result_t r;
  assert(!merge(f, c, id, &r));
  expected(f);
  assert(r.files == 3 && r.entries == 4 && r.published == 4 &&
         missing(f->stage) && !missing(f->manifest));
  assert(!bfpilot_archive_merge_forget(f->stage, f->out, id, f->manifest) &&
         missing(f->manifest));
}
static int erase_entry(const char *p, const struct stat *st, int type,
                       struct FTW *walk) {
  (void)st;
  (void)type;
  (void)walk;
  return remove(p);
}
static void erase(const char *p) {
  assert(!nftw(p, erase_entry, 16, FTW_DEPTH | FTW_PHYS));
}
int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  strcpy(owned, "/tmp/bfpilot-merge-r24-XXXXXX");
  assert(mkdtemp(owned));
  printf("Owned merge fixture: %s\n", owned);
  fixture_t f = fixture();
  inputs(&f);
  control_t c = {0};
  bfpilot_merge_result_t r;
  assert(!merge(&f, &c, "fresh", &r));
  expected(&f);
  assert(r.files == 3 && r.entries == 4 && missing(f.stage));
  assert(!merge(&f, &c, "fresh", &r));
  expected(&f);
  assert(r.published == 4);
  assert(!bfpilot_archive_merge_forget(f.stage, f.out, "fresh", f.manifest));
  groups++;
  for (unsigned cut = 1; cut <= 2; cut++) {
    f = fixture();
    inputs(&f);
    c = (control_t){.cut = cut};
    assert(merge(&f, &c, "cut-point", &r) == -ECANCELED && r.published > 0 &&
           !missing(f.manifest));
    assert(bfpilot_archive_merge_forget(f.stage, f.out, "cut-point",
                                        f.manifest) == -EBUSY);
    c = (control_t){0};
    complete(&f, &c, "cut-point");
  }
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.cancel = 1};
  assert(merge(&f, &c, "cancel-before", &r) == -ECANCELED);
  expect(f.out, "replace.txt", "complete original");
  assert(missing(f.manifest));
  groups++;
  /* All type/protection conflicts fail before a single publication. */
  f = fixture();
  inputs(&f);
  char p[1024], q[1024];
  path(p, f.out, "replace.txt");
  assert(!unlink(p) && !mkdir(p, 0700));
  put(p, "keep.txt", "old directory content");
  c = (control_t){0};
  assert(merge(&f, &c, "file-over-dir", &r) == -EISDIR);
  expect(p, "keep.txt", "old directory content");
  path(q, f.out, " fresh .txt ");
  assert(missing(q) && missing(f.manifest));
  groups++;
  f = fixture();
  inputs(&f);
  path(p, f.out, "dir");
  erase(p);
  put(f.out, "dir", "old regular file");
  assert(merge(&f, &c, "dir-over-file", &r) == -EEXIST);
  expect(f.out, "replace.txt", "complete original");
  expect(f.out, "dir", "old regular file");
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.protect = "replace.txt"};
  assert(merge(&f, &c, "protected", &r) == -EPERM);
  expect(f.out, "replace.txt", "complete original");
  assert(missing(f.manifest));
  groups++;
  /* Follow no symlink referents: replace the old link name, preserve its file.
   */
  f = fixture();
  inputs(&f);
  put(f.root, "referent.txt", "outside destination original");
  path(p, f.out, "replace.txt");
  assert(!unlink(p));
  path(q, f.root, "referent.txt");
  assert(!symlink(q, p));
  path(p, f.stage, "staged-link");
  assert(!symlink(q, p));
  c = (control_t){0};
  assert(!merge(&f, &c, "links", &r));
  expected(&f);
  expect(f.root, "referent.txt", "outside destination original");
  path(p, f.out, "staged-link");
  char target[1024];
  ssize_t n = readlink(p, target, sizeof(target));
  assert(n == (ssize_t)strlen(q) && !memcmp(target, q, (size_t)n));
  assert(!bfpilot_archive_merge_forget(f.stage, f.out, "links", f.manifest));
  groups++;
  f = fixture();
  inputs(&f);
  path(p, f.out, "dir");
  erase(p);
  assert(!symlink(f.root, p));
  c = (control_t){0};
  assert(merge(&f, &c, "parent-link", &r) < 0);
  expect(f.out, "replace.txt", "complete original");
  groups++;
  /* Active private stage overlap is rejected even when extracting into a root.
   */
  f = fixture();
  inputs(&f);
  path(p, f.stage, "private");
  assert(!mkdir(p, 0700));
  put(p, "forbidden.txt", "private collision");
  assert(bfpilot_archive_merge(f.stage, f.root, "overlap", f.manifest, NULL,
                               &r) == -EINVAL);
  assert(missing(f.manifest));
  groups++;
  for (unsigned fault = 0; fault < 2; fault++) {
    f = fixture();
    inputs(&f);
    c = (control_t){0};
    if (fault)
      rename_fault = 1;
    else
      sync_fault = 1;
    assert(merge(&f, &c, "manifest-io", &r) == -EIO);
    expect(f.out, "replace.txt", "complete original");
    path(p, f.out, " fresh .txt ");
    assert(missing(p) && missing(f.manifest));
    complete(&f, &c, "manifest-io");
  }
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){0};
  publication_fault = 1;
  assert(merge(&f, &c, "rename-io", &r) == -EIO);
  expect(f.out, "replace.txt", "complete original");
  complete(&f, &c, "rename-io");
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.close_after_first = 1};
  assert(merge(&f, &c, "close-io", &r) == -EIO);
  c = (control_t){0};
  complete(&f, &c, "close-io");
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.cut = 1};
  assert(merge(&f, &c, "bound-id", &r) == -ECANCELED);
  c = (control_t){0};
  assert(merge(&f, &c, "different-id", &r) == -EAGAIN);
  complete(&f, &c, "bound-id");
  groups++;
  /* Complete corruption/truncation never triggers a blind manifest rebuild. */
  for (unsigned corruption = 0; corruption < 3; corruption++) {
    f = fixture();
    inputs(&f);
    c = (control_t){.cut = 1};
    assert(merge(&f, &c, "corruption", &r) == -ECANCELED);
    int fd = open(f.manifest, O_RDWR | O_NOFOLLOW);
    assert(fd >= 0);
    struct stat st;
    assert(!fstat(fd, &st));
    if (corruption == 0) {
      unsigned char byte;
      assert(pread(fd, &byte, 1, st.st_size - 1) == 1);
      byte ^= 0x80;
      assert(pwrite(fd, &byte, 1, st.st_size - 1) == 1);
    } else if (corruption == 1)
      assert(!ftruncate(fd, 100));
    else {
      unsigned char byte;
      assert(pread(fd, &byte, 1, 2244 + 64) == 1);
      byte ^= 0x80;
      assert(pwrite(fd, &byte, 1, 2244 + 64) == 1);
    }
    assert(!fsync(fd) && !close(fd));
    c = (control_t){0};
    assert(merge(&f, &c, "corruption", &r) < 0);
    assert(!missing(f.manifest) && !missing(f.stage));
  }
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.cut = 1};
  assert(merge(&f, &c, "missing-stage-file", &r) == -ECANCELED);
  const char *names[] = {"replace.txt", " fresh .txt ", "dir/new.txt"};
  unsigned lost = 3;
  for (unsigned i = 0; i < 3; i++) {
    path(p, f.stage, names[i]);
    if (!missing(p)) {
      lost = i;
      break;
    }
  }
  assert(lost < 3 && !unlink(p));
  path(q, f.out, names[lost]);
  int fd = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0 && write(fd, "old", 3) == 3 && !close(fd));
  c = (control_t){0};
  assert(merge(&f, &c, "missing-stage-file", &r) == -ENOENT);
  expect(f.out, names[lost], "old");
  groups++;
  f = fixture();
  inputs(&f);
  c = (control_t){.cut = 1};
  assert(merge(&f, &c, "root-identity", &r) == -ECANCELED);
  path(p, f.root, "previous-out");
  assert(!rename(f.out, p) && !mkdir(f.out, 0700));
  c = (control_t){0};
  assert(merge(&f, &c, "root-identity", &r) == -EAGAIN);
  groups++;
  /* A real broad tree, plus complete stage-absent replay, uses bounded frames.
   */
  f = fixture();
  put(f.out, "sentinel", "keep broad-tree unrelated data");
  for (unsigned i = 0; i < 2500; i++) {
    char name[40], text[80];
    snprintf(name, sizeof(name), "entry-%u.txt", i);
    snprintf(text, sizeof(text), "complete owned file %u", i);
    put(f.stage, name, text);
  }
  c = (control_t){0};
  assert(!merge(&f, &c, "breadth-2500", &r) && r.files == 2500 &&
         r.entries == 2500);
  for (unsigned i = 0; i < 2500; i++) {
    char name[40], text[80];
    snprintf(name, sizeof(name), "entry-%u.txt", i);
    snprintf(text, sizeof(text), "complete owned file %u", i);
    expect(f.out, name, text);
  }
  expect(f.out, "sentinel", "keep broad-tree unrelated data");
  assert(!merge(&f, &c, "breadth-2500", &r));
  assert(!bfpilot_archive_merge_forget(f.stage, f.out, "breadth-2500",
                                       f.manifest));
  groups++;
  /* A different real filesystem is rejected, not retried as a copy. */
  struct stat tmp, shm;
  if (!stat(owned, &tmp) && !stat("/dev/shm", &shm) &&
      tmp.st_dev != shm.st_dev) {
    f = fixture();
    inputs(&f);
    char other[] = "/dev/shm/bfpilot-merge-r24-XXXXXX";
    assert(mkdtemp(other));
    put(other, "sentinel", "other filesystem original");
    assert(bfpilot_archive_merge(f.stage, other, "cross-device", f.manifest,
                                 NULL, &r) == -EXDEV);
    expect(other, "sentinel", "other filesystem original");
    erase(other);
    groups++;
  }
  erase(owned);
  printf("archive merge: %u production filesystem control groups passed (2500 "
         "entries, partial-publication recovery, fault injection)\n",
         groups);
  return 0;
}
