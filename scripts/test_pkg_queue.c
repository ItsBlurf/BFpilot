/* Execute the production queue with a deterministic Sony boundary. This tests
 * batching/status/order, not licensed package acceptance or installation. */
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "../src/paths.h"
static char test_root[256], status_path[300], status_tmp[300], log_path[300];
#undef BFPILOT_DATA_DIR
#undef BFPILOT_PKG_STATUS_PATH
#undef BFPILOT_PKG_STATUS_TMP
#undef BFPILOT_PKG_LOG_PATH
#define BFPILOT_DATA_DIR test_root
#define BFPILOT_PKG_STATUS_PATH status_path
#define BFPILOT_PKG_STATUS_TMP status_tmp
#define BFPILOT_PKG_LOG_PATH log_path
#include "../src/pkg_install.c"
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static atomic_int calls;
static int reject, wrong_order;
void bfpilot_log(const char *fmt, ...) { (void)fmt; }
void bfpilot_notify(const char *title, const char *message) { (void)title; (void)message; }
int bfpilot_pkg_file_validate(const char *path, bfpilot_pkg_file_info_t *info, char *error, size_t cap) {
  (void)error; (void)cap; memset(info, 0, sizeof(*info)); info->size = 4096;
  snprintf(info->content_id, sizeof(info->content_id), "TEST-%s", base_name(path));
  return 0;
}
int bfpilot_pkg_file_is_system(const bfpilot_pkg_file_info_t *info) { (void)info; return 0; }
int bfpilot_appinst_install_pkg(const char *path, const char *name, bfpilot_appinst_result_t *result) {
  (void)name; memset(result, 0, sizeof(*result));
  int index = atomic_fetch_add(&calls, 1), actual = -1;
  sscanf(base_name(path), "part-%d.pkg", &actual);
  if(actual != index) wrong_order = 1;
  usleep(2000);
  result->used_embedded_worker = 1;
  return index == reject ? -EIO : 0;
}
int main(void) {
#ifdef BFPILOT_NATIVE_TEST
  snprintf(test_root, sizeof(test_root), "/data/test/bfpilot-pkg-queue-r3");
#else
  snprintf(test_root, sizeof(test_root), "/tmp/bfpilot-queue-%ld", (long)getpid());
#endif
  if(mkdir(test_root, 0700) != 0) return 2;
  snprintf(status_path,sizeof(status_path),"%s/status.json",test_root);
  snprintf(status_tmp,sizeof(status_tmp),"%s/status.tmp",test_root);
  snprintf(log_path,sizeof(log_path),"%s/queue.log",test_root);
  char report_path[300];snprintf(report_path,sizeof(report_path),"%s/results.txt",test_root);
#ifdef BFPILOT_NATIVE_TEST
  FILE *out=fopen(report_path,"wx");
#else
  FILE *out=stdout;
#endif
  if(!out)return 3;
  char names[21][100], error[256]; const char *paths[21];
  for(int i=0;i<21;i++){snprintf(names[i],sizeof(names[i]),"/data/test/part-%d.pkg",i);paths[i]=names[i];}
  int failures=0;
  for(int run=0;run<2;run++) {
    reject=run==0?-1:12;wrong_order=0;atomic_store(&calls,0);
    int rc=bfpilot_pkg_install_start(paths,21,error,sizeof(error));
    int busy=bfpilot_pkg_install_start(paths,21,error,sizeof(error));
    for(int wait=0;wait<10000 && bfpilot_pkg_is_busy();wait++)usleep(1000);
    char status[4096]={0};FILE *file=fopen(status_path,"rb");if(file){fread(status,1,sizeof(status)-1,file);fclose(file);}
    const char *state=run==0?"\"state\":\"accepted\"":"\"state\":\"completed_with_errors\"";
    const char *counts=run==0?"\"submitted\":21,\"completed\":0,\"failed\":0":"\"submitted\":20,\"completed\":0,\"failed\":1";
    int pass=rc==0 && busy==-EBUSY && !bfpilot_pkg_is_busy() && atomic_load(&calls)==21 && !wrong_order && strstr(status,state) && strstr(status,counts) && strstr(status,"\"busy\":false") && strstr(status,"\"packageCount\":21");
    fprintf(out,"%s 21-item %s queue preserves order/counts, busy gate and submission semantics\n",pass?"PASS":"FAIL",run==0?"accepted":"mixed-result");
    if(!pass){failures++;fprintf(out,"DETAIL rc=%d busy=%d calls=%d status=%s\n",rc,busy,atomic_load(&calls),status);}
  }
  paths[20]=paths[0];
  int rc=bfpilot_pkg_install_start(paths,21,error,sizeof(error));
  int pass=rc==-EINVAL && !bfpilot_pkg_is_busy();
  fprintf(out,"%s duplicate queue rejected without acquiring a lasting busy slot\nSUMMARY failures=%d\n",pass?"PASS":"FAIL",failures+!pass);
  failures+=!pass;
#ifdef BFPILOT_NATIVE_TEST
  fclose(out);payload_exit(failures!=0);
#else
  unlink(status_path);unlink(log_path);rmdir(test_root);
#endif
  return failures!=0;
}
