/* Actual transfer/traversal code on an actual filesystem, including >84k files.
 * Fault wrappers are confined to this test ELF, never production. */
#include "../src/transfer.c"
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static int fault, reads;
int __real_bfpilot_fs_fstatat(int, const char *, struct stat *, int);
struct dirent *__real_readdir(DIR *);
int __wrap_bfpilot_fs_fstatat(int fd, const char *name, struct stat *st, int flags) {
  if(fault == 1) { errno = EIO; return -1; }
  int rc = __real_bfpilot_fs_fstatat(fd, name, st, flags);
  if(rc == 0 && fault == 2) st->st_dev++;
  return rc;
}
struct dirent *__wrap_readdir(DIR *dir) {
  if(fault == 3) { errno = ESTALE; return NULL; }
  if(fault == 4 && ++reads == 50) atomic_store(&g_job.cancel, 1);
  return __real_readdir(dir);
}
void bfpilot_log(const char *fmt, ...) { (void)fmt; }
void bfpilot_search_mark_stale(const char *reason) { (void)reason; }
static int failed;
static FILE *report;
static void check(const char *name, int pass) {
  fprintf(report, "%s %s\n", pass ? "PASS" : "FAIL", name);
  fflush(report);
  if(!pass) failed++;
}
struct count { unsigned long files, dirs, bytes; };
static int count_entry(const char *path, const char *rel, const struct stat *st, void *ctx) {
  (void)path; (void)rel;
  struct count *count = ctx;
  if(S_ISDIR(st->st_mode)) count->dirs++;
  else if(S_ISREG(st->st_mode)) { count->files++; count->bytes += st->st_size; }
  return 0;
}
int main(void) {
  char root[256], source[512], destination[512], path[1024], result[512];
#ifdef BFPILOT_NATIVE_TEST
  snprintf(root, sizeof(root), "/data/test/bfpilot-tree-controls-r3");
  const int files = 1200;
#else
  snprintf(root, sizeof(root), "/tmp/bfpilot-tree-%ld", (long)getpid());
  const int files = 85000;
#endif
  if(mkdir(root, 0700) != 0) return 2;
  snprintf(source, sizeof(source), "%s/source", root);
  snprintf(destination, sizeof(destination), "%s/copy", root);
  snprintf(result, sizeof(result), "%s/results.txt", root);
#ifdef BFPILOT_NATIVE_TEST
  report = fopen(result, "wx");
#else
  report = stdout;
#endif
  if(!report || mkdir(source, 0700) != 0) return 3;
  for(int i = 0; i < files; i++) {
    snprintf(path, sizeof(path), "%s/%08d", source, i);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd < 0 || write(fd, &i, sizeof(i)) != sizeof(i) || close(fd) != 0) return 4;
  }
  /* A deep path uses heap directory frames rather than path-sized recursion. */
  snprintf(path, sizeof(path), "%s", source);
  for(int i = 0; i < 48; i++) { strcat(path, "/d"); if(mkdir(path, 0700) != 0) return 5; }
  struct stat st; if(lstat(source, &st) != 0) return 6;
  struct count count = {0};
  long start = monotonic_ms();
  int rc = bfpilot_tree_walk(source, st.st_dev, count_entry, NULL, &count);
  fprintf(report, "MEASURE source_walk_ms=%ld files=%lu dirs=%lu\n", monotonic_ms()-start, count.files, count.dirs);
  check("real large/deep tree counted exactly", rc == 0 && count.files == (unsigned)files && count.dirs == 49 && count.bytes == (unsigned)files * sizeof(int));
  atomic_store(&g_job.total_known, 0);
  start = monotonic_ms();
  rc = copy_recursive(source, destination, st.st_dev);
  fprintf(report, "MEASURE durable_copy_ms=%ld files=%d\n", monotonic_ms()-start, files);
  check("single-pass copy counts every completed file", rc == 0 && atomic_load(&g_job.done_files) == files && atomic_load(&g_job.total_files) == files && atomic_load(&g_job.copied_bytes) == (long)files * (long)sizeof(int));
  check("unfinished traversal never advertises a known total", !atomic_load(&g_job.total_known));
  int exact = 1;
  for(int i = 0; i < files && exact; i++) {
    snprintf(path, sizeof(path), "%s/%08d", destination, i);
    int value = -1, fd = open(path, O_RDONLY);
    exact = fd >= 0 && read(fd, &value, sizeof(value)) == sizeof(value) && value == i;
    if(fd >= 0) close(fd);
  }
  check("every copied file byte checked", exact);
  check("iterative cleanup handles complete deep tree", delete_recursive_force(destination) == 0 && lstat(destination, &st) != 0 && errno == ENOENT);
  lstat(source, &st);
  const int errors[] = {0, EIO, EXDEV, ESTALE};
  for(fault = 1; fault <= 3; fault++) {
    count = (struct count){0};
    rc = bfpilot_tree_walk(source, st.st_dev, count_entry, NULL, &count);
    char name[100]; snprintf(name, sizeof(name), "scan propagates injected fault %d", fault);
    check(name, rc == -errors[fault]);
  }
  fault = 4; reads = 0;
  rc = copy_recursive(source, destination, st.st_dev);
  check("copy cancellation stops during real traversal", rc != 0 && errno == ECANCELED);
  fault = 0;
  check("cancelled private stage can be removed despite cancel flag", delete_recursive_force(destination) == 0);
  atomic_store(&g_job.cancel, 0);
  /* Move worker must rename the entire tree without visiting a single child. */
  char target[512]; snprintf(target, sizeof(target), "%s/moved", root);
  mkdir(target, 0700);
  struct copy_arg *arg = calloc(1, sizeof(*arg));
  snprintf(arg->src, sizeof(arg->src), "%s", source);
  snprintf(arg->dst, sizeof(arg->dst), "%s", target); arg->is_move = 1;
  job_begin("move"); fault = 3; /* Any readdir means an unwanted prescan. */
  copy_worker(arg);
  fault = 0;
  snprintf(destination, sizeof(destination), "%s/source", target);
  check("same-device move does not prescan children", !g_job.error[0] && lstat(source, &st) != 0 && lstat(destination, &st) == 0);
  check("final iterative cleanup", delete_recursive_force(target) == 0);
  fprintf(report, "SUMMARY failures=%d files=%d\n", failed, files);
#ifdef BFPILOT_NATIVE_TEST
  fclose(report); payload_exit(failed != 0);
#else
  rmdir(root);
#endif
  return failed != 0;
}
