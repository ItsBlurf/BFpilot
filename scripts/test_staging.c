/* Host exercises a private test volume root; PS5 additionally exercises actual
 * /data and USB0 selection without creating an incomplete homebrew title. */
#include <stdio.h>
static FILE *report;
#define BFPILOT_STAGE_TRACE(...) do { if(report) { fprintf(report,"TRACE "); fprintf(report,__VA_ARGS__); fflush(report); } } while(0)
#include "../src/staging.c"
#include "../src/tree_chmod.h"
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static int sync_fault;
int __real_fsync(int);
int __wrap_fsync(int fd) { if(sync_fault){errno=EIO;return -1;}return __real_fsync(fd); }
static int failures;
static void check(const char *name,int pass) { fprintf(report,"%s %s\n",pass?"PASS":"FAIL",name);fflush(report);failures+=!pass; }
static int put(const char *path) { int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0400);if(fd<0)return -1;int rc=write(fd,"staged bytes",12)==12 && fsync(fd)==0 && close(fd)==0;return rc?0:-1; }
int main(void) {
 char root[256],result[300],tree[512],file[1024],stage[1024],base[1024];
#ifdef BFPILOT_NATIVE_TEST
 snprintf(root,sizeof(root),"/data/test/bfpilot-staging-controls-r5d");
#else
 snprintf(root,sizeof(root),"/tmp/bfpilot-staging-%ld",(long)getpid());
#endif
 if(mkdir(root,0700)!=0)return 2;
 snprintf(result,sizeof(result),"%s/results.txt",root);
#ifdef BFPILOT_NATIVE_TEST
 report=fopen(result,"wx");
#else
 report=stdout;
#endif
 if(!report)return 3;
 snprintf(tree,sizeof(tree),"%s/tree",root);if(mkdir(tree,0700)!=0)return 4;
 snprintf(file,sizeof(file),"%s/readonly",tree);if(put(file)!=0)return 5;
 struct stat st;lstat(tree,&st);
 int rc=stage_at(root,&st,"copy",tree,stage,sizeof(stage));
 check("private stage outside source subtree",rc==0 && !contains(tree,stage) && strstr(stage,"/.bfpilot-staging/copy-")!=NULL);
 if(rc!=0)return 6;
 char first[1024];snprintf(first,sizeof(first),"%s",stage);
 rc=stage_at(root,&st,"copy",tree,stage,sizeof(stage));
 fprintf(report,"DETAIL existing-stage rc=%d errno=%d\n",rc,errno);
 check("unique stage identity",rc==0 && strcmp(first,stage));
 struct stat foreign=st;foreign.st_dev++;
 rc=stage_at(root,&foreign,"copy",NULL,stage,sizeof(stage));
 check("foreign device refused",rc!=0 && errno==EXDEV);
 rc=stage_at(root,&st,"copy",root,stage,sizeof(stage));
 check("source containing every available stage refused",rc!=0 && errno==EINVAL);
 rc=stage_at(root,&st,"copy",NULL,stage,8);
 fprintf(report,"DETAIL short-buffer rc=%d errno=%d\n",rc,errno);
 check("small output buffer refused",rc!=0 && errno==ENAMETOOLONG);
 rc=bfpilot_stage_path(root,"../bad",stage,sizeof(stage));
 check("invalid stage kind refused",rc!=0 && errno==EINVAL);
 snprintf(base,sizeof(base),"%s/.bfpilot-staging",root);
#ifndef BFPILOT_NATIVE_TEST
 char held[1024];snprintf(held,sizeof(held),"%s/held-stage",root);
 rename(base,held);symlink(tree,base);
 rc=stage_at(root,&st,"copy",NULL,stage,sizeof(stage));
 check("symlink staging directory never followed",rc!=0 && (errno==ELOOP || errno==ENOTDIR));
 unlink(base);rename(held,base);
#endif
 rc=bfpilot_tree_finalize(tree,0777,NULL,NULL);lstat(file,&st);
 check("checked final sync and permissions",rc==0 && (st.st_mode&0777)==0777);
 sync_fault=1;rc=bfpilot_tree_finalize(tree,0777,NULL,NULL);sync_fault=0;
 check("finalization propagates fsync EIO",rc==-EIO);
#ifdef BFPILOT_NATIVE_TEST
 const char *parents[]={"/data/homebrew","/mnt/usb0"};
 for(int i=0;i<2;i++) {
  struct stat parent;
  if(stat(parents[i],&parent)!=0)return 7;
  rc=bfpilot_stage_path(parents[i],"extract",stage,sizeof(stage));
  fprintf(report,"DETAIL native parent=%s rc=%d errno=%d\n",parents[i],rc,errno);
  char name[128];snprintf(name,sizeof(name),"native same-device isolated stage %s",parents[i]);
  int fd=rc==0?open(stage,O_WRONLY|O_CREAT|O_EXCL,0600):-1;
  struct stat output={0};if(fd>=0){fstat(fd,&output);close(fd);}
  check(name,rc==0 && fd>=0 && output.st_dev==parent.st_dev && !contains("/data/homebrew",stage) && !contains("/mnt/usb0/homebrew",stage));
  if(fd>=0)unlink(stage);
  fprintf(report,"LOCATION %s => %s\n",parents[i],stage);
 }
#endif
 unlink(file);rmdir(tree);rmdir(base);
 fprintf(report,"SUMMARY failures=%d\n",failures);
#ifdef BFPILOT_NATIVE_TEST
 fclose(report);payload_exit(failures!=0);
#else
 rmdir(root);
#endif
 return failures!=0;
}
