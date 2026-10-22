#include "../src/url_codec.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
static FILE *out;static int failures,checks;
static void check(const char *name,int pass){fprintf(out,"%s %s\n",pass?"PASS":"FAIL",name);checks++;failures+=!pass;}
int main(void){
#ifdef BFPILOT_NATIVE_TEST
 out=fopen("/data/test/bfpilot-url-controls-r6.txt","wx");
#else
 out=stdout;
#endif
 if(!out)return 2;
 char value[1024],query[16384],raw[4097];
 strcpy(query,"path=/data/");strcpy(value,"/data/");
 for(int i=0;i<300;i++){strcat(query,"%E6%BC%A2");strcat(value,"漢");}
 char decoded[1024];int rc=bfpilot_form_arg(query,"path",decoded,sizeof(decoded));
 check("UTF-8 encoded value over 1024 bytes retained",rc==1 && !strcmp(decoded,value));
 strcpy(query,"path=/data/");
 for(int i=0;i<900;i++)strcat(query,"%F3%B0%82%80");
 rc=bfpilot_form_arg(query,"path",decoded,sizeof(decoded));
 int exact=rc==1 && strlen(decoded)==906 && !memcmp(decoded,"/data/",6);
 for(int i=6;i<906;i++)if((unsigned char)decoded[i]!=0x80)exact=0;
 check("long private byte-token paths decode before capacity check",exact);
 rc=bfpilot_form_arg("path=/first&path=/second","path",decoded,sizeof(decoded));
 check("first complete scalar field chosen",rc==1 && !strcmp(decoded,"/first"));
 rc=bfpilot_form_arg("filename=a%2Bb+c%26d%3De","filename",decoded,sizeof(decoded));
 check("literal plus, space, ampersand and equal roundtrip",rc==1 && !strcmp(decoded,"a+b c&d=e"));
 const char *invalid[]={"%","%0","%GG","x%00y","a%2","%0Z","a%FF%"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  strcpy(decoded,"sentinel");errno=0;rc=bfpilot_url_decode(decoded,sizeof(decoded),invalid[i],strlen(invalid[i]));
  char name[80];snprintf(name,sizeof(name),"invalid escape/NUL %u clears output",i);check(name,rc==-1 && errno==EINVAL && !decoded[0]);
 }
 memset(raw,'a',1023);raw[1023]=0;
 rc=bfpilot_url_decode(decoded,sizeof(decoded),raw,1023);check("maximum decoded length accepted",rc==0 && strlen(decoded)==1023);
 raw[1023]='a';raw[1024]=0;errno=0;
 rc=bfpilot_url_decode(decoded,sizeof(decoded),raw,1024);check("one-byte overflow rejected without partial output",rc==-1 && errno==ENAMETOOLONG && !decoded[0]);
 check("empty value fits terminator only",bfpilot_url_decode(decoded,1,"",0)==0 && !decoded[0]);
 strcpy(raw,"a%2Fb");rc=bfpilot_url_decode(raw,sizeof(raw),raw,strlen(raw));check("in-place decode safe",rc==0 && !strcmp(raw,"a/b"));
 rc=bfpilot_form_arg("path=/data/%FF","path",decoded,sizeof(decoded));check("raw filename byte preserved",rc==1 && !memcmp(decoded,"/data/",6) && (unsigned char)decoded[6]==255 && !decoded[7]);
 char embedded[3]={'a',0,'b'};rc=bfpilot_url_decode(decoded,sizeof(decoded),embedded,sizeof(embedded));check("literal NUL rejected",rc==-1 && !decoded[0]);
 fprintf(out,"SUMMARY checks=%d failures=%d\n",checks,failures);
#ifdef BFPILOT_NATIVE_TEST
 fclose(out);payload_exit(failures!=0);
#endif
 return failures!=0;
}
