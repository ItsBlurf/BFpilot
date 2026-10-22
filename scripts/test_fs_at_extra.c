/* Native controls for the additional archive engine at-syscalls. */
#include "../src/fs_at.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/payload.h>
static FILE *report;static int failures;
static void check(const char *name,int pass){fprintf(report,"%s %s\n",pass?"PASS":"FAIL",name);fflush(report);failures+=!pass;}
void bfpilot_log(const char *format,...){(void)format;}
int main(void){
 const char *root="/data/test/bfpilot-at-extra-r8";
 if(mkdir(root,0700))return 2;
 char result[256];snprintf(result,sizeof(result),"%s/results.txt",root);
 report=fopen(result,"wx");if(!report)return 3;
 int dir=open(root,O_RDONLY|O_DIRECTORY),fd=bfpilot_fs_openat(dir,"source",O_CREAT|O_EXCL|O_WRONLY,0600);
 check("owned input created",dir>=0 && fd>=0);if(fd>=0)close(fd);
 errno=EDOM;int rc=bfpilot_fs_renameat(dir,"missing",dir,"target");
 check("renameat missing returns minus one and ENOENT",rc==-1 && errno==ENOENT);
 errno=EDOM;rc=bfpilot_fs_renameat(-1,"source",dir,"target");
 check("renameat invalid fd returns EBADF",rc==-1 && errno==EBADF);
 errno=EDOM;rc=bfpilot_fs_renameat(dir,"source",dir,"target");
 struct stat st;check("renameat success preserves valid file",rc==0 && bfpilot_fs_fstatat(dir,"target",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(st.st_mode));
 errno=EDOM;rc=bfpilot_fs_unlinkat(dir,"missing",0);
 check("unlinkat missing returns minus one and ENOENT",rc==-1 && errno==ENOENT);
 errno=EDOM;rc=bfpilot_fs_unlinkat(-1,"target",0);
 check("unlinkat invalid fd returns EBADF",rc==-1 && errno==EBADF);
 check("unlinkat removes owned file",bfpilot_fs_unlinkat(dir,"target",0)==0);
 check("mkdirat owned directory",bfpilot_fs_mkdirat(dir,"directory",0700)==0);
 errno=EDOM;rc=bfpilot_fs_unlinkat(dir,"directory",0);
 check("unlinkat directory requires AT_REMOVEDIR",rc==-1 && (errno==EPERM || errno==EISDIR));
 check("unlinkat AT_REMOVEDIR removes directory",bfpilot_fs_unlinkat(dir,"directory",AT_REMOVEDIR)==0);
 close(dir);fprintf(report,"SUMMARY failures=%d\n",failures);fclose(report);
 payload_exit(failures!=0);return failures!=0;
}
