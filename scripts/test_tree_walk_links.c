#define _GNU_SOURCE
#include "../src/tree_walk.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct counts { int files, dirs, links, sequence, last_dir; off_t bytes; };
static int collect(const char *path,const char *relative,const struct stat *st,void *ctx) {
  (void)path;(void)relative;struct counts *c=ctx;c->sequence++;
  if(S_ISDIR(st->st_mode)){c->dirs++;c->last_dir=c->sequence;}
  if(S_ISLNK(st->st_mode))c->links++;
  if(S_ISREG(st->st_mode)){c->files++;c->bytes+=st->st_size;}
  return 0;
}
static int cancel(void *ctx){(void)ctx;return 1;}
static int fd_count(void){DIR *dir=opendir("/proc/self/fd");if(!dir)return -1;int n=0;while(readdir(dir))n++;closedir(dir);return n;}
int main(void){
  char root[]="/tmp/bfpilot-links-XXXXXX",tree[512],file[1024],link[1024];
  if(!mkdtemp(root))return 2;
  snprintf(tree,sizeof(tree),"%s/tree",root);mkdir(tree,0700);
  snprintf(file,sizeof(file),"%s/file",tree);int fd=open(file,O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0)return 3;
  if(write(fd,"x",1)!=1 || close(fd)!=0)return 4;
  snprintf(link,sizeof(link),"%s/loop",tree);if(symlink(".",link)!=0)return 5;
  snprintf(link,sizeof(link),"%s/outside",tree);if(symlink("/proc",link)!=0)return 6;
  struct stat st;lstat(tree,&st);int failures=0,baseline=fd_count();struct counts c={0};
  int rc=bfpilot_tree_walk(tree,st.st_dev,collect,NULL,&c);
  int pass=rc==0 && c.files==1 && c.bytes==1 && c.links==2 && c.dirs==1;
  printf("%s symbolic cycles and external target are metadata only\n",pass?"PASS":"FAIL");failures+=!pass;
  c=(struct counts){0};rc=bfpilot_tree_walk_post(tree,st.st_dev,collect,NULL,&c);
  pass=rc==0 && c.last_dir==c.sequence && c.sequence==4;
  printf("%s directory callback is postorder\n",pass?"PASS":"FAIL");failures+=!pass;
  c=(struct counts){0};rc=bfpilot_tree_walk(tree,st.st_dev,collect,cancel,&c);
  pass=rc==-ECANCELED && c.sequence==0;
  printf("%s cancellation before first entry\n",pass?"PASS":"FAIL");failures+=!pass;
  char deep[1024];snprintf(deep,sizeof(deep),"%s/deep",root);mkdir(deep,0700);
  for(int i=0;i<65;i++){strcat(deep,"/d");mkdir(deep,0700);}
  char top[512];snprintf(top,sizeof(top),"%s/deep",root);lstat(top,&st);c=(struct counts){0};
  rc=bfpilot_tree_walk(top,st.st_dev,collect,NULL,&c);
  pass=rc==-ELOOP && fd_count()==baseline;
  printf("%s excessive depth rejected and all directory handles closed\n",pass?"PASS":"FAIL");failures+=!pass;
  for(int i=0;i<66;i++){rmdir(deep);char *slash=strrchr(deep,'/');if(slash)*slash=0;}
  unlink(file);snprintf(link,sizeof(link),"%s/loop",tree);unlink(link);snprintf(link,sizeof(link),"%s/outside",tree);unlink(link);rmdir(tree);rmdir(root);
  printf("SUMMARY failures=%d\n",failures);return failures!=0;
}
