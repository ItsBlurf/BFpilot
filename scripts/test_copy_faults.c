/* Linker-wrapped I/O affects only this test executable, never the main ELF. */
#include "../src/transfer.c"
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static int fault, writes;
int __real_posix_fallocate(int, off_t, off_t);
int __real_fsync(int);
ssize_t __real_write(int, const void *, size_t);
int __wrap_posix_fallocate(int fd, off_t offset, off_t size) {
  if(fault==1)return ENOSPC;
  if(fault==3)return EOPNOTSUPP;
  return __real_posix_fallocate(fd,offset,size);
}
int __wrap_fsync(int fd) {
  if(fault==2){errno=EIO;return -1;}
  return __real_fsync(fd);
}
ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
  writes++;
  if(fault==4)return 0;
  return __real_write(fd,buffer,size);
}
void bfpilot_log(const char *format, ...) {(void)format;}
int main(void) {
  char root[256];
#ifdef BFPILOT_NATIVE_TEST
  snprintf(root,sizeof(root),"/data/test/bfpilot-copy-controls-r1");
#else
  snprintf(root,sizeof(root),"/tmp/bfpilot-copy-controls-%ld",(long)getpid());
#endif
  if(mkdir(root,0700)!=0)return 2;
  char source[512],destination[512],result_path[512];
  snprintf(source,sizeof(source),"%s/input",root);
  snprintf(destination,sizeof(destination),"%s/output",root);
  snprintf(result_path,sizeof(result_path),"%s/results.txt",root);
#ifdef BFPILOT_NATIVE_TEST
  FILE *report=fopen(result_path,"wx");
#else
  FILE *report=stdout;
#endif
  if(!report)return 3;
  unsigned char pattern[4096];for(size_t i=0;i<sizeof(pattern);i++)pattern[i]=(unsigned char)i;
  FILE *file=fopen(source,"wb");if(!file)return 4;
  fwrite(pattern,1,sizeof(pattern),file);fclose(file);
  struct stat expected; if(lstat(source,&expected)!=0)return 5;
  const char *names[]={"normal final sync","preallocation ENOSPC aborts before write","final fsync EIO discards staging","unsupported preallocation falls back","zero write becomes space failure"};
  int failures=0;
  for(fault=0;fault<=4;fault++) {
    writes=0;g_job.error[0]=0;atomic_store(&g_job.cancel,0);errno=0;
    int rc=copy_file(source,destination,&expected),saved=errno;
    int success=(fault==0 || fault==3),pass=0;
    struct stat st;
    if(success) {
      unsigned char actual[4096];file=fopen(destination,"rb");
      pass=rc==0 && file && fread(actual,1,sizeof(actual),file)==sizeof(actual) && !memcmp(actual,pattern,sizeof(actual));
      if(file)fclose(file);
    } else {
      pass=rc!=0 && saved==(fault==2?EIO:ENOSPC) && lstat(destination,&st)!=0 && errno==ENOENT && (fault!=1 || writes==0);
    }
    fprintf(report,"%s %s rc=%d errno=%d writes=%d\n",pass?"PASS":"FAIL",names[fault],rc,saved,writes);
    if(!pass)failures++;
    unlink(destination);
  }
  fault=0;unlink(source);
  fprintf(report,"SUMMARY failures=%d\n",failures);
#ifdef BFPILOT_NATIVE_TEST
  fclose(report);payload_exit(failures!=0);
#endif
  return failures!=0;
}
