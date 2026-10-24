/* Client for the separately linked WFM 7-Zip Helper (LGPL).
 * The service uses a private local socket. See NOTICE.md for source/licenses. */
#include "full_archive_client.h"
#include "diag.h"
#include "payload_launch.h"
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef BFPILOT_FULL_ARCHIVE_SOCKET
#ifdef __PROSPERO__
#define BFPILOT_FULL_ARCHIVE_SOCKET "/system_tmp/bfpilot-7zip.sock"
#else
#define BFPILOT_FULL_ARCHIVE_SOCKET "/tmp/bfpilot-7zip.sock"
#endif
#endif
#ifndef BFPILOT_FULL_ARCHIVE_LOCK
#ifdef __PROSPERO__
#define BFPILOT_FULL_ARCHIVE_LOCK "/system_tmp/bfpilot-7zip.lock"
#else
#define BFPILOT_FULL_ARCHIVE_LOCK "/tmp/bfpilot-7zip.lock"
#endif
#endif
#define FULL_VERSION "BF7Z/1 26.03-2-bf2"
#define FULL_MAX_PAYLOAD (1024U * 1024U)

#ifdef BFPILOT_FULL_ARCHIVE_EMBED
__asm__(".section .rodata\n.global bfpilot_full_archive_elf\n"
        ".align 16\nbfpilot_full_archive_elf:\n"
        ".incbin \"build/internal/bfpilot-full-archive.elf\"\n"
        ".global bfpilot_full_archive_elf_end\nbfpilot_full_archive_elf_end:\n"
        ".previous\n");
extern const unsigned char bfpilot_full_archive_elf[], bfpilot_full_archive_elf_end[];
#endif

static atomic_uint sequence;
typedef struct {
  int fd, cancel_sent;
  uint64_t id;
  long activity_ms, cancel_ms;
  const bfpilot_full_archive_callbacks_t *callbacks;
} connection_t;
typedef struct { unsigned type; uint64_t id; size_t size; } frame_t;
static long now_ms(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
  return (long)ts.tv_sec*1000L+ts.tv_nsec/1000000L;
}
static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p)<<32 | get32(p+4); }
static void put32(unsigned char *p,uint32_t value) {
  for(int i=3;i>=0;i--){p[i]=(unsigned char)value;value>>=8;}
}
static void put64(unsigned char *p,uint64_t value) { put32(p,(uint32_t)(value>>32));put32(p+4,(uint32_t)value); }
static uint64_t next_id(void) {
  return ((uint64_t)(unsigned long)now_ms()<<24) ^ ((uint64_t)(unsigned)getpid()<<8) ^
         (uint64_t)(atomic_fetch_add(&sequence,1)+1);
}
static int write_all(int fd,const void *buffer,size_t size) {
  const unsigned char *p=buffer;
  long deadline=now_ms()+3000;
  while(size) {
    struct pollfd ready={fd,POLLOUT,0};
    int event=poll(&ready,1,100);
    if(event<0 && errno==EINTR)continue;
    if(event<0)return -errno;
    if(now_ms()>deadline)return -ETIMEDOUT;
    if(event==0)continue;
    if(!(ready.revents&POLLOUT))return -EPIPE;
    ssize_t n=send(fd,p,size,0);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)return n<0?-errno:-EIO;
    p+=n;size-=(size_t)n;
  }
  return 0;
}
static int send_frame(connection_t *c,unsigned type,const void *payload,size_t size) {
  if(type>65535 || size>FULL_MAX_PAYLOAD)return -EMSGSIZE;
  unsigned char header[20]={ 'B','7','H','P',0,0,0,0 };
  header[4]=(unsigned char)(type>>8);header[5]=(unsigned char)type;
  put32(header+8,(uint32_t)size);put64(header+12,c->id);
  int rc=write_all(c->fd,header,sizeof(header));
  return rc || !size ? rc : write_all(c->fd,payload,size);
}
static int read_all(connection_t *c,void *buffer,size_t size) {
  unsigned char *p=buffer;
  while(size) {
    if(c->callbacks && c->callbacks->cancelled &&
       c->callbacks->cancelled(c->callbacks->context) && !c->cancel_sent) {
      int rc=send_frame(c,11,NULL,0);
      if(rc)return rc;
      c->cancel_sent=1;c->cancel_ms=now_ms();
    }
    long now=now_ms();
    if((c->cancel_sent && now-c->cancel_ms>30000) ||
       now-c->activity_ms>(c->callbacks?120000:3000))return -ETIMEDOUT;
    struct pollfd ready={c->fd,POLLIN,0};
    int event=poll(&ready,1,100);
    if(event<0 && errno==EINTR)continue;
    if(event<0)return -errno;
    if(event==0)continue;
    if(!(ready.revents&POLLIN))return -ECONNRESET;
    ssize_t n=recv(c->fd,p,size,0);
    if(n<0 && errno==EINTR)continue;
    if(n<=0)return n<0?-errno:-ECONNRESET;
    p+=n;size-=(size_t)n;c->activity_ms=now_ms();
  }
  return 0;
}
static int receive_frame(connection_t *c,frame_t *frame,unsigned char *payload,size_t capacity) {
  unsigned char header[20];int rc=read_all(c,header,sizeof(header));
  if(rc)return rc;
  if(memcmp(header,"B7HP",4) || header[6] || header[7])return -EPROTO;
  frame->type=(unsigned)header[4]<<8 | header[5];
  frame->id=get64(header+12);frame->size=get32(header+8);
  if(frame->id!=c->id || frame->size>FULL_MAX_PAYLOAD || frame->size>=capacity)return -EPROTO;
  rc=read_all(c,payload,frame->size);
  if(!rc)payload[frame->size]=0;
  return rc;
}
static int connect_service(connection_t *c) {
  memset(c,0,sizeof(*c));c->fd=-1;
  int fd=socket(AF_UNIX,SOCK_STREAM,0);
  if(fd<0)return -errno;
  struct sockaddr_un address;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
#ifdef __PROSPERO__
  address.sun_len=sizeof(address);
#endif
  if(strlen(BFPILOT_FULL_ARCHIVE_SOCKET)>=sizeof(address.sun_path)){close(fd);return -ENAMETOOLONG;}
  snprintf(address.sun_path,sizeof(address.sun_path),"%s",BFPILOT_FULL_ARCHIVE_SOCKET);
  struct timeval timeout={.tv_sec=3,.tv_usec=0};
  (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  if(connect(fd,(struct sockaddr *)&address,sizeof(address))!=0){int rc=-errno;close(fd);return rc;}
  c->fd=fd;c->id=next_id();c->activity_ms=now_ms();return 0;
}
static int ping_service(void) {
  connection_t c;int rc=connect_service(&c);if(rc)return rc;
  unsigned char payload[128];frame_t frame;
  rc=send_frame(&c,1,NULL,0);
  if(!rc)rc=receive_frame(&c,&frame,payload,sizeof(payload));
  if(!rc && (frame.type!=2 || strcmp((char *)payload,FULL_VERSION)))rc=-EPROTO;
  close(c.fd);return rc;
}
int bfpilot_full_archive_probe(void) { return ping_service(); }
int bfpilot_full_archive_ack(uint64_t task_id) {
  if(!task_id)return -EINVAL;
  connection_t c;int rc=connect_service(&c);if(rc)return rc;
  c.id=task_id;
  unsigned char payload[1024];frame_t frame;
  rc=send_frame(&c,15,NULL,0);
  if(!rc)rc=receive_frame(&c,&frame,payload,sizeof(payload));
  if(!rc && (frame.type!=24 || frame.size!=0))rc=-EPROTO;
  close(c.fd);return rc;
}
static int ensure_service(void) {
  int rc=ping_service();
  if(!rc)return 0;
  if(rc!=-ENOENT && rc!=-ECONNREFUSED)return rc;
#ifdef BFPILOT_FULL_ARCHIVE_EMBED
  rc=bfpilot_launch_payload_memory(bfpilot_full_archive_elf,
      (size_t)(bfpilot_full_archive_elf_end-bfpilot_full_archive_elf),"BFpilot archive service");
  if(rc)return rc;
  for(unsigned attempt=0;attempt<100;attempt++) {
    usleep(100000);rc=ping_service();
    if(!rc)return 0;
    if(rc!=-ENOENT && rc!=-ECONNREFUSED)return rc;
  }
  return -ETIMEDOUT;
#else
  return rc;
#endif
}
int bfpilot_full_archive_stop(void) {
  connection_t c;int rc=connect_service(&c);
  if(rc==-ENOENT || rc==-ECONNREFUSED)return 0;
  if(rc)return rc;
  unsigned char payload[1024];frame_t frame;
  rc=send_frame(&c,100,NULL,0);
  if(!rc)rc=receive_frame(&c,&frame,payload,sizeof(payload));
  if(!rc && frame.type!=101)rc=frame.type==25?-EBUSY:-EPROTO;
  close(c.fd);return rc;
}
static int engine_error(const unsigned char *payload,size_t length,char *error,size_t capacity) {
  if(length<8)return -EPROTO;
  size_t code_len=get32(payload),message_len=get32(payload+4);
  if(code_len>length-8 || message_len!=length-8-code_len ||
     memchr(payload+8,0,code_len+message_len))return -EPROTO;
  const char *code=(const char *)payload+8,*message=code+code_len;
  if(error && capacity)snprintf(error,capacity,"%.*s: %.*s",(int)code_len,code,(int)message_len,message);
#define CODE(text) (code_len==sizeof(text)-1 && !memcmp(code,text,sizeof(text)-1))
  if(CODE("canceled"))return -ECANCELED;
  if(CODE("archive_no_space"))return -ENOSPC;
  if(CODE("archive_bad_password") || CODE("archive_password_required"))return -EACCES;
  if(CODE("archive_missing_volume"))return -ENOENT;
  if(CODE("task_not_found"))return -ENOENT;
  if(CODE("archive_unsupported_format"))return -ENOTSUP;
  if(CODE("busy") || CODE("retained_tasks") || CODE("stopping"))return -EBUSY;
  return -EIO;
#undef CODE
}
static int wait_result(connection_t *c,unsigned char *payload,
    const char *source,const char *destination,
    const bfpilot_full_archive_callbacks_t *callbacks,char *error,size_t error_size,
    int *quiescent,int accepted,int need_snapshot) {
  frame_t frame;
  int rc=0;
  while(!rc) {
    rc=receive_frame(c,&frame,payload,FULL_MAX_PAYLOAD+1);
    if(rc)break;
    if(need_snapshot && frame.type!=26 && frame.type!=25){rc=-EPROTO;break;}
    if(frame.type==26) {
      if(!need_snapshot || frame.size<52 || get32(payload+4)!=1){rc=-EPROTO;break;}
      unsigned state=get32(payload);
      size_t current_len=get32(payload+32),code_len=get32(payload+36),message_len=get32(payload+40);
      if(state<1 || state>4 || current_len>frame.size-52 ||
         code_len>frame.size-52-current_len || message_len>frame.size-52-current_len-code_len){rc=-EPROTO;break;}
      size_t offset=44+current_len+code_len+message_len;
      size_t sl=get32(payload+offset),dl=get32(payload+offset+4);offset+=8;
      if(sl>frame.size-offset || dl!=frame.size-offset-sl || sl!=strlen(source) || dl!=strlen(destination) ||
         memcmp(payload+offset,source,sl) || memcmp(payload+offset+sl,destination,dl) ||
         memchr(payload+44,0,current_len+code_len+message_len)){rc=-EPROTO;break;}
      need_snapshot=0;accepted=1;
      if(callbacks && callbacks->accepted)callbacks->accepted(callbacks->context);
      if(callbacks && callbacks->progress)callbacks->progress(callbacks->context,get64(payload+24),get64(payload+16));
      if(callbacks && callbacks->current) {
        unsigned char saved=payload[44+current_len];payload[44+current_len]=0;
        callbacks->current(callbacks->context,(const char *)payload+44);payload[44+current_len]=saved;
      }
      if(state==1)continue;
      if(state==2){frame.type=24;frame.size=0;}
      else {
        memmove(payload+8,payload+44+current_len,code_len+message_len);
        put32(payload,(uint32_t)code_len);put32(payload+4,(uint32_t)message_len);
        frame.type=25;frame.size=8+code_len+message_len;
      }
    }
    if(frame.type==20 && frame.size==0){
      accepted=1;
      if(callbacks && callbacks->accepted)callbacks->accepted(callbacks->context);
      continue;
    }
    if(frame.type==21 && frame.size==16) {
      if(callbacks && callbacks->progress)callbacks->progress(callbacks->context,get64(payload),get64(payload+8));
      continue;
    }
    if(frame.type==22 && frame.size>=4 && get32(payload)==frame.size-4 && !memchr(payload+4,0,frame.size-4)) {
      if(callbacks && callbacks->current)callbacks->current(callbacks->context,(const char *)payload+4);
      continue;
    }
    if((frame.type==24 && frame.size==0) || frame.type==25) {
      rc=frame.type==24?0:engine_error(payload,frame.size,error,error_size);
      if(rc==-EPROTO)break;
      if(quiescent)*quiescent=1;
      break;
    }
    rc=-EPROTO;
  }
  return rc;
}

static int service_lock_free(void) {
  int fd=open(BFPILOT_FULL_ARCHIVE_LOCK,O_RDWR|O_NOFOLLOW);
  if(fd<0)return 0;
  struct stat st;
  int quiet=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && flock(fd,LOCK_EX|LOCK_NB)==0;
  close(fd);
  return quiet;
}
int bfpilot_full_archive_resume(uint64_t task_id,const char *source,const char *destination,
    const bfpilot_full_archive_callbacks_t *callbacks,char *error,size_t error_size,int *quiescent) {
  if(quiescent)*quiescent=0;
  if(!task_id || !source || !destination)return -EINVAL;
  int rc=ping_service();
  if(rc) {
    if(quiescent && service_lock_free())*quiescent=1;
    return rc;
  }
  connection_t c;rc=connect_service(&c);if(rc)return rc;
  c.id=task_id;c.callbacks=callbacks;
  unsigned char *payload=malloc(FULL_MAX_PAYLOAD+1);
  if(!payload){close(c.fd);return -ENOMEM;}
  rc=send_frame(&c,14,NULL,0);
  if(!rc)rc=wait_result(&c,payload,source,destination,callbacks,error,error_size,quiescent,1,1);
  if(quiescent && !*quiescent && service_lock_free())*quiescent=1;
  close(c.fd);free(payload);return rc;
}

int bfpilot_full_archive_extract(const char *source,const char *destination,
    const char *password,const bfpilot_full_archive_callbacks_t *callbacks,
    char *error,size_t error_size,int *quiescent) {
  if(error && error_size)error[0]=0;
  if(quiescent)*quiescent=1;
  if(!source || !destination || source[0]!='/' || destination[0]!='/')return -EINVAL;
  if(!password)password="";
  size_t sl=strlen(source),dl=strlen(destination),pl=strlen(password);
  if(sl>1023 || dl>1023 || pl>511)return -ENAMETOOLONG;
  int rc=ensure_service();
  if(rc){if(error && error_size)snprintf(error,error_size,"archive service unavailable: %s",strerror(-rc));return rc;}
  connection_t c;rc=connect_service(&c);if(rc)return rc;
  c.callbacks=callbacks;
  bfpilot_log("full archive service request id=%llu source=%s stage=%s",
               (unsigned long long)c.id,source,destination);
  if(callbacks && callbacks->started) {
    rc=callbacks->started(callbacks->context,c.id);
    if(rc){close(c.fd);return rc;}
  }
  size_t length=24+pl+sl+dl;
  unsigned char *request=calloc(1,length);
  unsigned char *payload=malloc(FULL_MAX_PAYLOAD+1);
  if(!request || !payload){free(request);free(payload);close(c.fd);return -ENOMEM;}
  put32(request+4,(uint32_t)pl);put32(request+8,1);
  memcpy(request+16,password,pl);
  put32(request+16+pl,(uint32_t)sl);put32(request+20+pl,(uint32_t)dl);
  memcpy(request+24+pl,source,sl);memcpy(request+24+pl+sl,destination,dl);
  /* No overwrite extension: output is an owned, initially empty private tree. */
  if(quiescent)*quiescent=0; /* A partial send/response can be ambiguous. */
  rc=send_frame(&c,10,request,length);
  memset(request,0,length);free(request);
  if(!rc)rc=wait_result(&c,payload,source,destination,callbacks,error,error_size,quiescent,0,0);
  if(quiescent && !*quiescent) {
    uint64_t task_id=c.id;
    close(c.fd);c.fd=-1;
    bfpilot_log("full archive service reconnect id=%llu rc=%d",(unsigned long long)task_id,rc);
    rc=bfpilot_full_archive_resume(task_id,source,destination,callbacks,error,error_size,quiescent);
  }
  if(quiescent && !*quiescent) {
    /* Retain its stage. The service retains task identity for later attach or
     * cancellation; an uncertain connection must not trigger filesystem cleanup. */
    if(c.fd>=0)(void)send_frame(&c,11,NULL,0);
    if(error && error_size)snprintf(error,error_size,"archive service connection uncertain; retain private stage (%s)",strerror(rc<0?-rc:EIO));
  }
  if(c.fd>=0)close(c.fd);free(payload);
  return rc;
}
