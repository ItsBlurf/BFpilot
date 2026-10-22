#include "../src/full_archive_client.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/payload.h>
static FILE *report;static int failures,progress_calls,current_calls;
static void check(const char *name,int pass){fprintf(report,"%s %s\n",pass?"PASS":"FAIL",name);fflush(report);failures+=!pass;}
static void progress(void *ctx,uint64_t done,uint64_t total){(void)ctx;(void)done;(void)total;progress_calls++;}
static void current(void *ctx,const char *path){(void)ctx;if(path && *path)current_calls++;}
void bfpilot_log(const char *format,...){(void)format;}
int main(void){
 signal(SIGPIPE,SIG_IGN);
 const char *root="/data/test/bfpilot-full-engine-controls-r1";
 if(mkdir(root,0700)!=0)return 2;
 report=fopen("/data/test/bfpilot-full-engine-controls-r1/results.txt","wx");if(!report)return 3;
 check("private helper version handshake",bfpilot_full_archive_probe()==0);
 bfpilot_full_archive_callbacks_t callbacks={.progress=progress,.current=current};
 const char *names[]={"readonly.zip","fixture.tar.gz","aes.zip"};
 for(int i=0;i<3;i++){
  char source[512],destination[512],error[1024]={0};int quiet=0;
  snprintf(source,sizeof(source),"/data/test/bfpilot-engine-fixtures-r1/%s",names[i]);
  snprintf(destination,sizeof(destination),"%s/out-%d",root,i);
  progress_calls=0;current_calls=0;
  int rc=bfpilot_full_archive_extract(source,destination,i==2?"bfpilot-test-password":"",&callbacks,error,sizeof(error),&quiet);
  char name[160];snprintf(name,sizeof(name),"full engine extraction %s",names[i]);
  check(name,rc==0 && quiet && progress_calls>0 && current_calls>0);
  fprintf(report,"DETAIL rc=%d quiet=%d progress=%d current=%d error=%s\n",rc,quiet,progress_calls,current_calls,error);
  char path[1024];snprintf(path,sizeof(path),"%s/game/eboot.bin",destination);
  FILE *file=fopen(path,"rb");unsigned char bytes[5000];size_t n=file?fread(bytes,1,sizeof(bytes),file):0;if(file)fclose(file);
  const char *unit="BFpilot execute-permission fixture\n";int exact=n==strlen(unit)*128;
  for(size_t k=0;k<n;k++)if(bytes[k]!=(unsigned char)unit[k%strlen(unit)])exact=0;
  snprintf(name,sizeof(name),"full engine exact output %s",names[i]);check(name,exact);
 }
 check("owned idle helper normal shutdown",bfpilot_full_archive_stop()==0);
 for(int attempt=0;attempt<50 && bfpilot_full_archive_probe()==0;attempt++)usleep(100000);
 int rc=bfpilot_full_archive_probe();check("owned helper listener exits",rc==-ENOENT || rc==-ECONNREFUSED);
 fprintf(report,"SUMMARY failures=%d\n",failures);fclose(report);payload_exit(failures!=0);return failures!=0;
}
