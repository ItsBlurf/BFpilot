#include "../src/fs_at.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static FILE *out;static int failures;
static void check(const char *name,int pass){fprintf(out,"%s %s\n",pass?"PASS":"FAIL",name);failures+=!pass;}
int main(void){
 char root[256];
#ifdef BFPILOT_NATIVE_TEST
 snprintf(root,sizeof(root),"/data/test/bfpilot-at-controls-r5");
#else
 snprintf(root,sizeof(root),"/tmp/bfpilot-at-%ld",(long)getpid());
#endif
 if(mkdir(root,0700)!=0)return 2;
 char path[512];snprintf(path,sizeof(path),"%s/results.txt",root);
#ifdef BFPILOT_NATIVE_TEST
 out=fopen(path,"wx");
#else
 out=stdout;
#endif
 if(!out)return 3;
 int dir=open(root,O_RDONLY|O_DIRECTORY),fd;
 if(dir<0)return 4;
 errno=EIO;int rc=bfpilot_fs_mkdirat(dir,"child",0700);
 check("mkdirat success",rc==0);
 errno=EIO;rc=bfpilot_fs_mkdirat(dir,"child",0700);
 check("mkdirat existing becomes -1 and EEXIST",rc==-1 && errno==EEXIST);
 errno=EIO;fd=bfpilot_fs_openat(dir,"missing",O_RDONLY,0);
 check("openat missing never returns errno as a descriptor",fd==-1 && errno==ENOENT);
 errno=0;fd=bfpilot_fs_openat(-10000,"child",O_RDONLY,0);
 check("openat bad descriptor becomes EBADF",fd==-1 && errno==EBADF);
 fd=bfpilot_fs_openat(dir,"new",O_WRONLY|O_CREAT|O_EXCL,0600);
 check("openat mode argument creates real regular file",fd>=0);
 if(fd>=0){write(fd,"ok",2);close(fd);}
 errno=0;fd=bfpilot_fs_openat(dir,"new",O_WRONLY|O_CREAT|O_EXCL,0600);
 check("openat O_EXCL collision is failure",fd==-1 && errno==EEXIST);
 struct stat st={0};errno=EIO;rc=bfpilot_fs_fstatat(dir,"new",&st,AT_SYMLINK_NOFOLLOW);
 check("fstatat success and exact size",rc==0 && S_ISREG(st.st_mode) && st.st_size==2);
 errno=EIO;rc=bfpilot_fs_fstatat(dir,"missing",&st,AT_SYMLINK_NOFOLLOW);
 check("fstatat missing sets ENOENT",rc==-1 && errno==ENOENT);
 errno=0;rc=bfpilot_fs_fstatat(-10000,"new",&st,AT_SYMLINK_NOFOLLOW);
 check("fstatat invalid descriptor sets EBADF",rc==-1 && errno==EBADF);
 errno=0;rc=bfpilot_fs_mkdirat(-10000,"new",0700);
 check("mkdirat invalid descriptor sets EBADF",rc==-1 && errno==EBADF);
 close(dir);snprintf(path,sizeof(path),"%s/new",root);unlink(path);snprintf(path,sizeof(path),"%s/child",root);rmdir(path);
 fprintf(out,"SUMMARY failures=%d\n",failures);
#ifdef BFPILOT_NATIVE_TEST
 fclose(out);payload_exit(failures!=0);
#else
 rmdir(root);
#endif
 return failures!=0;
}
