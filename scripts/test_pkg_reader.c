#include "pkg_file.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {unsigned char bytes[0x3000];size_t used,read_bytes,calls;uint64_t fail_at;} memory_t;
static void be32(unsigned char *p,uint32_t n){for(int i=3;i>=0;i--){p[i]=(unsigned char)n;n>>=8;}}
static void be64(unsigned char *p,uint64_t n){for(int i=7;i>=0;i--){p[i]=(unsigned char)n;n>>=8;}}
static int read_memory(void *context,void *buffer,size_t size,uint64_t offset){
 memory_t *m=context;m->calls++;
 if(offset==m->fail_at)return -ECONNRESET;
 if(offset>m->used || size>m->used-offset)return -EIO;
 memcpy(buffer,m->bytes+(size_t)offset,size);m->read_bytes+=size;return 0;
}
static void fixture(memory_t *m,uint64_t size,unsigned base){
 memset(m,0,sizeof(*m));m->used=sizeof(m->bytes);m->fail_at=UINT64_MAX;
 unsigned char *h=m->bytes+base;memcpy(h,"\177CNT",4);
 const char *id="EP0000-PPSA99991_00-OWNEDPKGTEST0000";memcpy(h+0x40,id,strlen(id));
 be32(h+0x10,2);be32(h+0x18,0x500);be64(h+0x430,size-base);
 const char *param="{\"titleId\":\"PPSA99991\",\"titleName\":\"Reader \\u4e2d\\u6587\",\"contentVersion\":\"01.23\"}";
 unsigned char *entry=h+0x500;be32(entry,0x2000);be32(entry+0x10,0x600);be32(entry+0x14,(uint32_t)strlen(param));memcpy(h+0x600,param,strlen(param));
 entry+=0x20;be32(entry,0x1200);be32(entry+0x10,0x900);be32(entry+0x14,16);memcpy(h+0x900,"\211PNG\r\n\032\nownedPNG",16);
 if(base){memcpy(m->bytes,"\177FIH",4);m->bytes[0x59]=(unsigned char)(base>>8);}
}
int main(void){
 memory_t memory;bfpilot_pkg_details_t details;bfpilot_pkg_file_info_t info;void *icon;size_t icon_size;
 fixture(&memory,0x2000,0);bfpilot_pkg_reader_t reader={&memory,0x2000,read_memory};
 assert(!bfpilot_pkg_validate_reader(&reader,&info) && info.size==0x2000 && !strcmp(info.title_id,"PPSA99991"));
 assert(!bfpilot_pkg_parse_reader(&reader,&details) && !strcmp(details.title_name,"Reader \344\270\255\346\226\207") && !strcmp(details.platform,"PS5"));
 assert(!bfpilot_pkg_icon_reader(&reader,&icon,&icon_size) && icon_size==16 && !memcmp(icon,memory.bytes+0x900,16));free(icon);
 assert(memory.read_bytes<4096);
 for(unsigned i=0;i<3;i++){
  fixture(&memory,0x2000,0);memory.fail_at=i==0?0x500:i==1?0x600:0x900;
  assert(bfpilot_pkg_parse_reader(&reader,&details)<0);
 }
 fixture(&memory,0x2000,0);reader.size=0x500;assert(bfpilot_pkg_parse_reader(&reader,&details)<0);
 reader.size=0x2000;be32(memory.bytes+0x10,65537);assert(bfpilot_pkg_parse_reader(&reader,&details)<0);
 fixture(&memory,0x2000,0);be32(memory.bytes+0x18,0xfffffff0U);assert(bfpilot_pkg_parse_reader(&reader,&details)<0);
 fixture(&memory,UINT64_C(1)<<40,0);reader.size=UINT64_C(1)<<40;
 assert(!bfpilot_pkg_validate_reader(&reader,&info) && info.size==(UINT64_C(1)<<40));
 assert(!bfpilot_pkg_parse_reader(&reader,&details) && details.has_icon && memory.read_bytes<4096);
 fixture(&memory,0x3000,0x1000);reader.size=0x3000;
 assert(!bfpilot_pkg_parse_reader(&reader,&details) && details.icon_offset==0x1900);
 assert(!bfpilot_pkg_icon_reader(&reader,&icon,&icon_size) && icon_size==16 && !memcmp(icon,memory.bytes+0x1900,16));free(icon);
 assert(bfpilot_pkg_parse_reader(NULL,&details)<0 && bfpilot_pkg_icon_reader(NULL,&icon,&icon_size)<0);
 puts("PKG reader metadata/icon, faults, truncation, bounds, large virtual source and FIH controls pass");return 0;
}
