#include "../src/full_archive_client.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int progress_calls,current_calls,cancel_test,admitted;
static uint64_t task_id;
void bfpilot_log(const char *format,...){(void)format;}
static int cancelled(void *context){(void)context;return cancel_test && progress_calls>0;}
static void progress(void *context,uint64_t done,uint64_t total){(void)context;(void)done;(void)total;progress_calls++;}
static void current(void *context,const char *path){(void)context;if(path && *path)current_calls++;}
static int started(void *context,uint64_t id){(void)context;task_id=id;return 0;}
static void accepted(void *context){(void)context;admitted=1;}
int main(int argc,char **argv){
 signal(SIGPIPE,SIG_IGN);if(argc!=2)return 2;cancel_test=!strcmp(argv[1],"cancel");
 bfpilot_full_archive_callbacks_t callbacks={.started=started,.accepted=accepted,.cancelled=cancelled,.progress=progress,.current=current};
 char error[1024]={0};int quiet=-1;
 int rc=bfpilot_full_archive_extract("/owned/input.zip","/owned/private-stage","test-password",&callbacks,error,sizeof(error),&quiet);
 printf("rc=%d quiet=%d progress=%d current=%d error=%s\n",rc,quiet,progress_calls,current_calls,error);
 // The protocol function must return before publication can be acknowledged.
 if(admitted && quiet){int ack=bfpilot_full_archive_ack(task_id);printf("ack=%d\n",ack);if(ack)return 3;}
 return 0;
}
