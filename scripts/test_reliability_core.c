/* Same product parsers/traversal on Linux and PS5. diag_log_impl.inc is
 * generated verbatim from diag.c with a guarded allocator and inert locks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "pkg_file.h"
#include "tree_chmod.h"
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
#include "diag_log_impl.inc"

static FILE *report;
static int failures, checks;
#ifdef BFPILOT_FAULT_WRAPPERS
static int injected_fault;
int __real_bfpilot_fs_fstatat(int, const char *, struct stat *, int);
int __real_fchmod(int, mode_t);
struct dirent *__real_readdir(DIR *);
int __wrap_bfpilot_fs_fstatat(int fd, const char *name, struct stat *st, int flags) {
  int rc=__real_bfpilot_fs_fstatat(fd,name,st,flags);
  if(rc==0 && injected_fault==1)st->st_dev++;
  return rc;
}
int __wrap_fchmod(int fd, mode_t mode) {
  if(injected_fault==2){errno=EIO;return -1;}
  return __real_fchmod(fd,mode);
}
struct dirent *__wrap_readdir(DIR *dir) {
  if(injected_fault==3){errno=EIO;return NULL;}
  return __real_readdir(dir);
}
#endif
static void check(const char *name, int pass) {
  fprintf(report, "%s %s\n", pass ? "PASS" : "FAIL", name);
  checks++; if(!pass) failures++;
}
static int guard_ok(const char *buffer) {
  for(size_t i=0;i<32;i++) if((unsigned char)buffer[allocation_capacity+i]!=0xa5)return 0;
  return 1;
}
static void put_be32(unsigned char *p, unsigned value) {
  p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value;
}
static void package_case(const char *root, const char *json, const char *expected, const char *name) {
  size_t size=strlen(json), length=0xc0+size;
  unsigned char *data=calloc(1,length);
  memcpy(data,"\x7f" "CNT",4);
  put_be32(data+0x10,1);put_be32(data+0x18,0xa0);
  put_be32(data+0xa0,0x2000);put_be32(data+0xb0,0xc0);put_be32(data+0xb4,(unsigned)size);
  memcpy(data+0xc0,json,size);
  char path[1024];snprintf(path,sizeof(path),"%s/metadata.pkg",root);
  FILE *file=fopen(path,"wb");
  if(!file) {check(name,0);free(data);return;}
  int written=fwrite(data,1,length,file)==length;
  int closed=fclose(file)==0;
  bfpilot_pkg_details_t details;
  int rc=bfpilot_pkg_parse_details(path,&details);
  check(name,written && closed && rc==0 && !strcmp(details.title_name,expected));
  free(data);unlink(path);
}
static int cancel_now(void *context) {(void)context;return 1;}
int main(void) {
  char root[256];
#ifdef BFPILOT_NATIVE_TEST
#ifdef BFPILOT_FAULT_WRAPPERS
  snprintf(root,sizeof(root),"/data/test/bfpilot-repairs-faults-r1");
#else
  snprintf(root,sizeof(root),"/data/test/bfpilot-repairs-core-r1");
#endif
#else
  snprintf(root,sizeof(root),"/tmp/bfpilot-repairs-core-%ld",(long)getpid());
#endif
  if(mkdir(root,0700)!=0)return 2;
  char path[1024];
#ifdef BFPILOT_NATIVE_TEST
  snprintf(path,sizeof(path),"%s/results.txt",root);
  report=fopen(path,"wx");
#else
  report=stdout;
#endif
  if(!report)return 3;
  char *buffer=NULL;size_t length=0;
  check("empty JSON",bfpilot_diag_dump_logs(&buffer,&length,0,"json",NULL)==0 &&
        !strcmp(buffer,"{\"ok\":true,\"count\":0,\"logs\":[]}") && guard_ok(buffer));free(buffer);
  strcpy(g_log_ring[0].line,"info: start");strcpy(g_log_ring[1].line,"error: read");
  strcpy(g_log_ring[2].line,"info: end");g_log_ring_count=3;
  check("filtered actual count",bfpilot_diag_dump_logs(&buffer,&length,3,"json","error")==0 &&
        !strcmp(buffer,"{\"ok\":true,\"count\":1,\"logs\":[\"error: read\"]}") && guard_ok(buffer));free(buffer);
  check("tail before filter",bfpilot_diag_dump_logs(&buffer,&length,1,"json","error")==0 &&
        !strcmp(buffer,"{\"ok\":true,\"count\":0,\"logs\":[]}") && guard_ok(buffer));free(buffer);
  for(size_t i=0;i<512;i++) {memset(g_log_ring[i].line,1,1023);g_log_ring[i].line[1023]=0;}
  g_log_ring_count=512;g_log_ring_head=0;
  int rc=bfpilot_diag_dump_logs(&buffer,&length,512,"json",NULL);
  check("worst-case escaping allocation",rc==0 && guard_ok(buffer) && strlen(buffer)==length && length<allocation_capacity && length>3000000 && buffer[length-1]=='}');
  snprintf(path,sizeof(path),"%s/stress.json",root);
  FILE *stress=fopen(path,"wb");
  check("stress JSON written",stress && fwrite(buffer,1,length,stress)==length);
  if(stress)fclose(stress);free(buffer);
  const char mixed[]="quote\" slash\\ tab\t CR\r LF\n";
  bfpilot_diag_clear_logs();strcpy(g_log_ring[0].line,mixed);g_log_ring_count=1;
  check("all JSON escapes bounded",bfpilot_diag_dump_logs(&buffer,&length,1,"json",NULL)==0 && guard_ok(buffer) && strstr(buffer,"\\u000a") && strstr(buffer,"\\u000d") && strstr(buffer,"\\u0009"));free(buffer);
  package_case(root,"{\"titleName\":\"Caf\\u00e9\"}","Caf\xc3\xa9","BMP Unicode escape");
  package_case(root,"{\"titleName\":\"\\ud83d\\ude80\"}","\xf0\x9f\x9a\x80","surrogate pair");
  package_case(root,"{\"titleName\":\"a\\n\\t\\r\\b\\f\\/\\\\\\\"b\"}","a\n\t\r\b\f/\\\"b","simple JSON escapes");
  package_case(root,"{\"titleName\":\"Caf\xc3\xa9\"}","Caf\xc3\xa9","raw UTF8");
  package_case(root,"{\"titleName\":\"\\ud83d\"}","","lone high surrogate rejected");
  package_case(root,"{\"titleName\":\"\\ude80\"}","","lone low surrogate rejected");
  package_case(root,"{\"titleName\":\"\\u12\"}","","truncated Unicode rejected");
  package_case(root,"{\"titleName\":\"\\uZZZZ\"}","","invalid hex rejected");
  package_case(root,"{\"titleName\":\"bad\\q\"}","","invalid escape rejected");
  package_case(root,"{\"titleName\":\"bad\\u0000x\"}","","embedded NUL rejected");
  package_case(root,"{\"titleName\":\"unterminated}","","unterminated string rejected");
  package_case(root,"{\"localizedParameters\":{\"en-US\":{\"titleName\":\"nested\"}}}","nested","nested localized title");
  char dir[512], child[512], file[512], outside[512], link[512];
  snprintf(dir,sizeof(dir),"%s/tree",root);mkdir(dir,0755);
  snprintf(child,sizeof(child),"%s/tree/child",root);mkdir(child,0755);
  snprintf(file,sizeof(file),"%s/tree/child/data",root);
  FILE *data=fopen(file,"wb");if(data){fputs("data",data);fclose(data);}
  snprintf(outside,sizeof(outside),"%s/outside",root);data=fopen(outside,"wb");if(data)fclose(data);chmod(outside,0600);
  snprintf(link,sizeof(link),"%s/tree/escape",root);
  int linked=symlink(outside,link)==0;
  if(!linked)fprintf(report,"CAPABILITY symlink unavailable errno=%d\n",errno);
  check("ordinary recursive chmod",bfpilot_tree_chmod(dir,0700,NULL,NULL)==0);
  struct stat st;
  check("descendant permission changed",stat(file,&st)==0 && (st.st_mode&0777)==0700);
  if(linked)check("symlink target untouched",stat(outside,&st)==0 && (st.st_mode&0777)==0600);
  check("cancellation propagated",bfpilot_tree_chmod(dir,0755,cancel_now,NULL)==-ECANCELED);
  check("missing root error",bfpilot_tree_chmod("/no-such-bfpilot-test-root",0755,NULL,NULL)==-ENOENT);
  check("regular file root rejected",bfpilot_tree_chmod(file,0755,NULL,NULL)==-ENOTDIR);
#ifdef BFPILOT_FAULT_WRAPPERS
  injected_fault=1;
  check("foreign device rejected before mutation",bfpilot_tree_chmod(dir,0755,NULL,NULL)==-EXDEV);
  injected_fault=2;
  check("descendant chmod EIO propagated",bfpilot_tree_chmod(dir,0755,NULL,NULL)==-EIO);
  injected_fault=3;
  check("readdir EIO propagated",bfpilot_tree_chmod(dir,0755,NULL,NULL)==-EIO);
  injected_fault=0;
#endif
  check("postorder removes search bits safely",bfpilot_tree_chmod(dir,0600,NULL,NULL)==0);
  chmod(dir,0700);chmod(child,0700);unlink(file);unlink(link);unlink(outside);rmdir(child);rmdir(dir);
  fprintf(report,"SUMMARY checks=%d failures=%d root=%s\n",checks,failures,root);
#ifdef BFPILOT_NATIVE_TEST
  fclose(report);payload_exit(failures!=0);
#else
  fprintf(report,"STRESS %s/stress.json\n",root);
#endif
  return failures!=0;
}
