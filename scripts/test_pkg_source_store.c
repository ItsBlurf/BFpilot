/* Actual private journal lifecycle, modeled clock and injected syscall errors. */
#define SOURCE_DIRECTORY "/tmp/bfpilot-pkg-source-store-r21"
#define time modeled_time
#include "../src/pkg_source_store.c"
#undef time
#include <assert.h>
#include <sys/random.h>
static time_t clock_value = 1700000000;
time_t modeled_time(time_t *out) { if(out)*out=clock_value;return clock_value; }
int sysctl(const int *mib, unsigned n, void *out, size_t *size, const void *in, size_t in_size) {
  (void)mib;(void)n;(void)in;(void)in_size;
  ssize_t got=getrandom(out,*size,0);if(got<0)return -1;*size=(size_t)got;return 0;
}
void bfpilot_log(const char *format, ...) { (void)format; }
static int fail_sync, fail_rename;
int __real_fsync(int);
int __wrap_fsync(int fd) { if(fail_sync){fail_sync=0;errno=EIO;return -1;}return __real_fsync(fd); }
int __real_renameat(int,const char *,int,const char *);
int __wrap_renameat(int a,const char *b,int c,const char *d) { if(fail_rename){fail_rename=0;errno=EIO;return -1;}return __real_renameat(a,b,c,d); }
static void empty(void) { unsigned records,refs;bfps_store_stats(&records,&refs);assert(!records&&!refs); }
static void restart(void) { assert(close(directory)==0);directory=-1;memset(entries,0,sizeof(entries));assert(!bfps_store_init()); }
int main(void) {
  assert(mkdir(SOURCE_DIRECTORY,0700)==0);
  assert(!bfps_store_init());empty();
  bfpilot_pkg_source_stamp_t stamp={0};stamp.size=8192;stamp.inode=12;stamp.mtime_sec=123;memcpy(stamp.header,"\177CNT",4);
  memcpy(stamp.header+0x40,"EP0000-PPSA99991_00-OWNEDPKGTEST0000",32);bfps_put64(stamp.header+0x430,8192);
  const char path[]="/smb/0123456789abcdef/share/owned.pkg";
  char token[33];
  fail_sync=1;assert(bfps_store_register(path,&stamp,token)==-EIO);empty();
  fail_rename=1;assert(bfps_store_register(path,&stamp,token)==-EIO);empty();
  assert(!bfps_store_register(path,&stamp,token));
  char name[40];snprintf(name,sizeof(name),"%s.src",token);struct stat st;
  assert(!fstatat(directory,name,&st,AT_SYMLINK_NOFOLLOW));assert((st.st_mode&0777)==0600);
  bfps_source_handle_t handle;assert(!bfps_store_acquire(token,&handle));
  assert(!bfps_store_drop(token));assert(bfps_store_acquire(token,&handle)==-ENOENT);
  unsigned records,refs;bfps_store_stats(&records,&refs);assert(records==1&&refs==1);
  bfps_store_release(&handle);empty();
  assert(!bfps_store_register(path,&stamp,token));restart();
  assert(!bfps_store_acquire(token,&handle));assert(!strcmp(handle.path,path));assert(!memcmp(&handle.stamp,&stamp,sizeof(stamp)));
  clock_value+=SOURCE_IDLE_SECONDS+1;bfps_store_expire();bfps_store_stats(&records,&refs);assert(records==1&&refs==1);
  bfps_store_touch(&handle);bfps_store_release(&handle);restart();
  assert(!bfps_store_acquire(token,&handle));bfps_store_release(&handle);
  clock_value-=1000000;bfps_store_expire();assert(!bfps_store_acquire(token,&handle));bfps_store_release(&handle);
  clock_value+=1000000+SOURCE_IDLE_SECONDS+1;bfps_store_expire();empty();
  for(unsigned i=0;i<BFPILOT_PKG_SOURCE_MAX_RECORDS;i++)assert(!bfps_store_register(path,&stamp,token));
  assert(bfps_store_register(path,&stamp,token)==-ENOSPC);
  bfps_store_stats(&records,&refs);assert(records==4096&&!refs);restart();
  bfps_store_stats(&records,&refs);assert(records==4096&&!refs);
  for(size_t i=0;i<BFPILOT_PKG_SOURCE_MAX_RECORDS;i++)if(entries[i].used){char own[33];memcpy(own,entries[i].token,33);assert(!bfps_store_drop(own));}
  empty();assert(close(directory)==0);assert(!rmdir(SOURCE_DIRECTORY));
  puts("journal fsync/rename errors, private modes, active retirement, restart, TTL/rollback/touch and real4096 capacity pass");return 0;
}
