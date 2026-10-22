/* Independently authored MIT adapter. libsmb2 retains its LGPL terms. */
#include "smb.h"
#include "path_ops.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>

#ifndef BFPILOT_SMB_DIR
#define BFPILOT_SMB_DIR "/data/BFpilot/network"
#endif
#define RECORDS_MAX 16
typedef struct {char magic[16];uint32_t count,checksum;bfpilot_smb_record_t records[RECORDS_MAX];} record_file;
static pthread_mutex_t records_lock=PTHREAD_MUTEX_INITIALIZER;
/* libsmb2's active-context registry is global; serialize its mutations. */
static pthread_mutex_t context_lock=PTHREAD_MUTEX_INITIALIZER;
struct bfpilot_smb_session {struct smb2_context *smb;char relative[1024];};
static void scrub(void *data,size_t length){volatile unsigned char *p=data;while(length--)*p++=0;}
static int text_valid(const char *s,size_t capacity,int empty){
 size_t n=strnlen(s,capacity);if(n==capacity || (!empty&&!n))return 0;
 for(size_t i=0;i<n;){unsigned char c=s[i];if(c<32 || c==127)return 0;
  size_t count=bfpilot_utf8_sequence_length((const unsigned char *)s+i);if(!count)return 0;i+=count;}
 return 1;
}
static int id_valid(const char *id){
 if(strlen(id)!=16)return 0;
 for(size_t i=0;i<16;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return 0;
 return 1;
}
static int record_valid(const bfpilot_smb_record_t *r){
 if(!text_valid(r->id,sizeof(r->id),0)||!id_valid(r->id)||
    !text_valid(r->label,sizeof(r->label),0)||!text_valid(r->server,sizeof(r->server),0)||
    !text_valid(r->user,sizeof(r->user),1)||!text_valid(r->domain,sizeof(r->domain),1)||
    !text_valid(r->password,sizeof(r->password),1)||r->encrypt>1 ||
    !(r->dialect==0||r->dialect==2||r->dialect==3))return 0;
 if(strpbrk(r->server,"/\\@?#; \t"))return 0;
 return 1;
}
static uint32_t checksum(const record_file *data){
 const unsigned char *p=(const unsigned char *)data->records;uint32_t hash=2166136261u;
 for(size_t i=0;i<sizeof(data->records);i++)hash=(hash^p[i])*16777619u;
 return (hash^data->count)*16777619u;
}
static int read_records(record_file *data){
 memset(data,0,sizeof(*data));memcpy(data->magic,"BFPILOT_SMB_V1",14);
 int fd=open(BFPILOT_SMB_DIR "/connections.bin",O_RDONLY|O_NOFOLLOW);
 if(fd<0)return errno==ENOENT?0:-errno;
 struct stat st;int rc=0;
 if(fstat(fd,&st)!=0)rc=-errno;
 else if(!S_ISREG(st.st_mode)||st.st_size!=sizeof(*data))rc=-EINVAL;
 size_t done=0;
 while(!rc&&done<sizeof(*data)){
  ssize_t got=read(fd,(char *)data+done,sizeof(*data)-done);
  if(got<0&&errno==EINTR)continue;
  if(got<=0){rc=got<0?-errno:-EIO;break;}done+=got;
 }
 if(close(fd)!=0&&!rc)rc=-errno;
 if(!rc&&(memcmp(data->magic,"BFPILOT_SMB_V1",14)||data->count>RECORDS_MAX||data->checksum!=checksum(data)))rc=-EINVAL;
 for(unsigned i=0;!rc&&i<data->count;i++){
  if(!record_valid(data->records+i))rc=-EINVAL;
  for(unsigned j=0;j<i;j++)if(!strcmp(data->records[i].id,data->records[j].id))rc=-EINVAL;
 }
 return rc;
}
static int private_directory(void){
 struct stat st;
 if(mkdir(BFPILOT_SMB_DIR,0700)!=0&&errno!=EEXIST)return -errno;
 if(lstat(BFPILOT_SMB_DIR,&st)!=0)return -errno;
 if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -ENOTDIR;
 if(chmod(BFPILOT_SMB_DIR,0700)!=0)return -errno;
 return 0;
}
static int write_records(record_file *data){
 int rc=private_directory();if(rc)return rc;
 data->checksum=checksum(data);
 int fd=open(BFPILOT_SMB_DIR "/connections.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
 if(fd<0)return -errno;
 if(fchmod(fd,0600)!=0)rc=-errno;
 size_t done=0;
 while(!rc&&done<sizeof(*data)){
  ssize_t n=write(fd,(char *)data+done,sizeof(*data)-done);
  if(n<0&&errno==EINTR)continue;
  if(n<=0){rc=n<0?-errno:-EIO;break;}done+=n;
 }
 if(!rc&&fsync(fd)!=0)rc=-errno;
 if(close(fd)!=0&&!rc)rc=-errno;
 if(!rc&&rename(BFPILOT_SMB_DIR "/connections.tmp",BFPILOT_SMB_DIR "/connections.bin")!=0)rc=-errno;
 if(rc)unlink(BFPILOT_SMB_DIR "/connections.tmp");
 return rc;
}
int bfpilot_smb_records(int (*entry)(const bfpilot_smb_record_t *,void *),void *context){
 record_file *data=calloc(1,sizeof(*data));if(!data)return -ENOMEM;
 pthread_mutex_lock(&records_lock);int rc=read_records(data);pthread_mutex_unlock(&records_lock);
 for(unsigned i=0;!rc&&i<data->count;i++){
  scrub(data->records[i].password,sizeof(data->records[i].password));
  rc=entry(data->records+i,context);
 }
 scrub(data,sizeof(*data));free(data);return rc;
}
int bfpilot_smb_save(bfpilot_smb_record_t *record,int replace_password){
 record_file *data=calloc(1,sizeof(*data));if(!data)return -ENOMEM;
 pthread_mutex_lock(&records_lock);int rc=read_records(data);unsigned index=0;
 if(!rc&&record->id[0]&&!id_valid(record->id))rc=-EINVAL;
 if(!rc&&record->id[0]){
  while(index<data->count&&strcmp(record->id,data->records[index].id))index++;
  if(index==data->count)rc=-ENOENT;
 } else if(!rc){
  if(data->count==RECORDS_MAX)rc=-ENOSPC;
  else {
   int random=open("/dev/urandom",O_RDONLY);unsigned char bytes[8];ssize_t n=random<0?-1:read(random,bytes,8);
   if(random>=0)close(random);
   if(n!=8)rc=-EIO;
   else {for(unsigned i=0;i<8;i++)snprintf(record->id+i*2,3,"%02x",bytes[i]);index=data->count++;}
  }
 }
 if(!rc){
  if(!replace_password)memcpy(record->password,data->records[index].password,sizeof(record->password));
  if(!record_valid(record))rc=-EINVAL;
  else {data->records[index]=*record;rc=write_records(data);}
 }
 pthread_mutex_unlock(&records_lock);scrub(data,sizeof(*data));free(data);return rc;
}
int bfpilot_smb_remove(const char *id){
 if(!id_valid(id))return -EINVAL;
 record_file *data=calloc(1,sizeof(*data));if(!data)return -ENOMEM;
 pthread_mutex_lock(&records_lock);int rc=read_records(data);unsigned i=0;
 while(!rc&&i<data->count&&strcmp(id,data->records[i].id))i++;
 if(!rc&&i==data->count)rc=-ENOENT;
 if(!rc){memmove(data->records+i,data->records+i+1,(data->count-i-1)*sizeof(*data->records));memset(data->records+--data->count,0,sizeof(*data->records));rc=write_records(data);}
 pthread_mutex_unlock(&records_lock);scrub(data,sizeof(*data));free(data);return rc;
}
int bfpilot_smb_path(const char *path){return path&&(!strcmp(path,"/smb")||!strncmp(path,"/smb/",5));}
static int split_path(const char *path,char *id,char *share,char *relative){
 if(!bfpilot_smb_path(path)||strlen(path)>=1024||!text_valid(path,1024,0)||strchr(path,'\\'))return -EINVAL;
 id[0]=share[0]=relative[0]=0;
 if(!strcmp(path,"/smb")||!strcmp(path,"/smb/"))return 0;
 const char *part=path+5,*slash=strchr(part,'/');size_t length=slash?(size_t)(slash-part):strlen(part);
 if(length!=16)return -EINVAL;memcpy(id,part,length);id[length]=0;if(!id_valid(id))return -EINVAL;
 if(!slash||!slash[1])return 0;
 part=slash+1;slash=strchr(part,'/');length=slash?(size_t)(slash-part):strlen(part);
 if(!length||length>=256)return -ENAMETOOLONG;
 memcpy(share,part,length);share[length]=0;
 if(!strcmp(share,".")||!strcmp(share,".."))return -EINVAL;
 if(slash)snprintf(relative,1024,"%s",slash+1);
 const char *p=relative;
 while(*p){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);
  if(!n||n>255||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return -EINVAL;
  if(!end)break;p=end+1;
 }
 return 0;
}
static int get_record(const char *id,bfpilot_smb_record_t *record){
 record_file *data=calloc(1,sizeof(*data));if(!data)return -ENOMEM;
 pthread_mutex_lock(&records_lock);int rc=read_records(data);pthread_mutex_unlock(&records_lock);
 unsigned i=0;while(!rc&&i<data->count&&strcmp(data->records[i].id,id))i++;
 if(!rc&&i==data->count)rc=-ENOENT;
 if(!rc)*record=data->records[i];scrub(data,sizeof(*data));free(data);return rc;
}
static int connect_record(const char *id,const char *share,bfpilot_smb_session_t **out){
 bfpilot_smb_record_t record={0};int rc=get_record(id,&record);if(rc)return rc;
 bfpilot_smb_session_t *session=calloc(1,sizeof(*session));if(!session){scrub(&record,sizeof(record));return -ENOMEM;}
 pthread_mutex_lock(&context_lock);session->smb=smb2_init_context();pthread_mutex_unlock(&context_lock);
 if(!session->smb){scrub(&record,sizeof(record));free(session);return -ENOMEM;}
 struct smb2_context *s=session->smb;
 smb2_set_user(s,record.user);smb2_set_domain(s,record.domain);smb2_set_password(s,record.password);
 smb2_set_timeout(s,10);smb2_set_version(s,(enum smb2_negotiate_version)record.dialect);
 smb2_set_security_mode(s,SMB2_NEGOTIATE_SIGNING_ENABLED|SMB2_NEGOTIATE_SIGNING_REQUIRED);
 smb2_set_sign(s,!record.encrypt);smb2_set_seal(s,record.encrypt);
 rc=smb2_connect_share(s,record.server,share,NULL);scrub(&record,sizeof(record));
 if(rc){pthread_mutex_lock(&context_lock);smb2_destroy_context(s);pthread_mutex_unlock(&context_lock);free(session);return rc;}
 *out=session;return 0;
}
int bfpilot_smb_connect(const char *path,bfpilot_smb_session_t **out){
 *out=NULL;char id[24],share[256],relative[1024];int rc=split_path(path,id,share,relative);
 if(rc)return rc;if(!share[0])return -EINVAL;
 rc=connect_record(id,share,out);if(!rc)snprintf((*out)->relative,sizeof((*out)->relative),"%s",relative);return rc;
}
void bfpilot_smb_disconnect(bfpilot_smb_session_t *session){
 if(!session)return;(void)smb2_disconnect_share(session->smb);
 pthread_mutex_lock(&context_lock);smb2_destroy_context(session->smb);pthread_mutex_unlock(&context_lock);free(session);
}
const char *bfpilot_smb_relative(bfpilot_smb_session_t *session){return session->relative;}
void *bfpilot_smb_context(bfpilot_smb_session_t *session){return session->smb;}
struct enumeration {int done,rc;unsigned count;int (*entry)(const bfpilot_smb_entry_t *,void *);void *context;};
static void shares_done(struct smb2_context *s,int status,void *data,void *context){
 struct enumeration *result=context;result->rc=status;
 if(!status&&data){struct srvsvc_NetrShareEnum_rep *rep=data;
  if(rep->ses.ShareInfo.Level1.EntriesRead>4096)result->rc=-E2BIG;
  for(uint32_t i=0;!result->rc&&i<rep->ses.ShareInfo.Level1.EntriesRead;i++){
   struct srvsvc_SHARE_INFO_1 *share=rep->ses.ShareInfo.Level1.Buffer->share_info_1+i;
   if((share->type&3)!=SHARE_TYPE_DISKTREE)continue;
   const char *name=share->netname.utf8;if(!name||!text_valid(name,256,0)||strpbrk(name,"/\\")){result->rc=-EINVAL;break;}
   bfpilot_smb_entry_t entry={.name=name,.directory=1};result->rc=result->entry(&entry,result->context);
  }smb2_free_data(s,data);
 }result->done=1;
}
struct root_entries {int (*entry)(const bfpilot_smb_entry_t *,void *);void *context;};
static int record_entry(const bfpilot_smb_record_t *record,void *context){
 struct root_entries *root=context;bfpilot_smb_entry_t entry={.name=record->id,.display_name=record->label,.directory=1};return root->entry(&entry,root->context);
}
int bfpilot_smb_list(const char *path,int (*entry)(const bfpilot_smb_entry_t *,void *),void *context){
 char id[24],share[256],relative[1024];int rc=split_path(path,id,share,relative);if(rc)return rc;
 if(!id[0]){struct root_entries root={entry,context};return bfpilot_smb_records(record_entry,&root);}
 bfpilot_smb_session_t *session=NULL;rc=connect_record(id,share[0]?share:"IPC$",&session);if(rc)return rc;
 struct smb2_context *s=session->smb;
 if(!share[0]){
  struct enumeration result={.entry=entry,.context=context};rc=smb2_share_enum_async(s,SHARE_INFO_1,shares_done,&result);
  for(unsigned i=0;!rc&&!result.done&&i<120;i++){
   struct pollfd ready={smb2_get_fd(s),smb2_which_events(s),0};
   if(poll(&ready,1,100)<0){if(errno==EINTR)continue;rc=-errno;break;}
   if(smb2_service(s,ready.revents)<0){rc=-EIO;break;}
  }if(!rc)rc=result.done?result.rc:-ETIMEDOUT;
 } else {
  struct smb2dir *dir=smb2_opendir(s,relative);if(!dir)rc=smb2_get_nterror(s)?-nterror_to_errno(smb2_get_nterror(s)):-EIO;
  else {struct smb2dirent *child;unsigned count=0;
   while(!rc&&(child=smb2_readdir(s,dir))){
    if(!strcmp(child->name,".")||!strcmp(child->name,".."))continue;
    if(++count>100000||!text_valid(child->name,256,0)||strpbrk(child->name,"/\\")){rc=-E2BIG;break;}
    bfpilot_smb_entry_t item={.name=child->name,.size=child->st.smb2_size,.mtime=child->st.smb2_mtime,.inode=child->st.smb2_ino,.directory=child->st.smb2_type==SMB2_TYPE_DIRECTORY,.link=child->st.smb2_type==SMB2_TYPE_LINK};
    rc=entry(&item,context);
   }smb2_closedir(s,dir);
  }
 }bfpilot_smb_disconnect(session);return rc;
}
