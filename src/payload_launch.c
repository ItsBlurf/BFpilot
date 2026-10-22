/*
 * Send a selected local/SMB payload file to elfldr on 127.0.0.1:9021.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "diag.h"
#include "notify.h"
#include "payload_launch.h"
#include "paths.h"
#include "remote_io.h"
#include "smb.h"
#include "staging.h"
#include "websrv.h"

static int
path_ok(const char *path) {
  if(!path || path[0] != '/') return 0;
  if(strstr(path, "/../") || strstr(path, "/..\\")) return 0;
  size_t n = strlen(path);
  if(n >= 2 && path[n - 1] == '.' && path[n - 2] == '.') return 0;
  return 1;
}

static int
is_payload_name(const char *path) {
  const char *dot = strrchr(path, '.');
  if(!dot || !dot[1]) return 0;
  if(!strcasecmp(dot, ".elf") || !strcasecmp(dot, ".bin") ||
     !strcasecmp(dot, ".js")) {
    return 1;
  }
  return 0;
}

static int
connect_elfldr(void) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock < 0) return -errno;

  struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
  (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  (void)setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(BFPILOT_ELFLDR_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int error = errno;
    close(sock);
    return -error;
  }
  return sock;
}

int
bfpilot_launch_payload_memory(const void *data, size_t size,
                              const char *label) {
  if(!data || size < 4 || size > (size_t)(64U * 1024U * 1024U)) {
    return -EINVAL;
  }
  const unsigned char *bytes = data;
  if(bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
     bytes[3] != 'F') return -ENOEXEC;

  int sock = connect_elfldr();
  if(sock < 0) return sock;
  size_t offset = 0;
  while(offset < size) {
    size_t chunk = size - offset > 16384 ? 16384 : size - offset;
    ssize_t sent = send(sock, bytes + offset, chunk, 0);
    if(sent < 0 && errno == EINTR) continue;
    if(sent <= 0) {
      int rc = sent < 0 ? -errno : -EIO;
      close(sock);
      return rc;
    }
    offset += (size_t)sent;
  }
  (void)shutdown(sock, SHUT_WR);
  close(sock);
  bfpilot_log("launch: sent embedded %s (%zu bytes) to elfldr",
              label ? label : "payload", size);
  return 0;
}

/* Complete and verify the remote read before connecting to the executable
 * loader. RAM stays bounded; the unlinked private fd owns the stable snapshot. */
static int remote_payload_snapshot(const char *path) {
  bfpilot_io_t *endpoint=NULL;bfpilot_io_file_t *source=NULL;
  struct stat before,after;int fd=-1,owned=0;
  char stage[1024]={0};unsigned char *buffer=NULL;
  int rc=bfpilot_io_endpoint(path,&endpoint);
  if(!rc)rc=bfpilot_io_open(endpoint,path,O_RDONLY,0,&source);
  if(!rc)rc=bfpilot_io_fstat(source,&before);
  if(!rc && (before.st_size<=0 || before.st_size>512LL*1024*1024))rc=-EFBIG;
  if(!rc && bfpilot_stage_path(BFPILOT_DATA_DIR,"launch",stage,sizeof(stage)))rc=-errno;
  if(!rc) {
    bfpilot_io_t *local=NULL;bfpilot_io_space_t space;
    rc=bfpilot_io_endpoint(BFPILOT_DATA_DIR,&local);
    if(!rc)rc=bfpilot_io_space(local,BFPILOT_DATA_DIR,&space);
    if(!rc && (uint64_t)before.st_size>space.available)rc=-ENOSPC;
    bfpilot_io_end(local);
  }
  if(!rc) {
    fd=open(stage,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)rc=-errno;else owned=1;
  }
  if(!rc) { buffer=malloc(1024*1024);if(!buffer)rc=-ENOMEM; }
  off_t total=0;unsigned char magic[4];size_t magic_size=0;
  int elf=!strcasecmp(strrchr(path,'.'),".elf");
  while(!rc && total<before.st_size) {
    size_t want=before.st_size-total>1024*1024?1024*1024:(size_t)(before.st_size-total);
    ssize_t got=bfpilot_io_read(source,buffer,want);
    if(got<=0){rc=got<0?(int)got:-EIO;break;}
    if(elf && magic_size<sizeof(magic)) {
      size_t take=(size_t)got<sizeof(magic)-magic_size?(size_t)got:sizeof(magic)-magic_size;
      memcpy(magic+magic_size,buffer,take);magic_size+=take;
      if(magic_size==sizeof(magic) && memcmp(magic,"\177ELF",4)){rc=-ENOEXEC;break;}
    }
    size_t used=0;
    while(used<(size_t)got) {
      ssize_t written=write(fd,buffer+used,(size_t)got-used);
      if(written<0 && errno==EINTR)continue;
      if(written<=0){rc=written<0?-errno:-EIO;break;}
      used+=(size_t)written;
    }
    total+=got;
  }
  if(!rc && elf && magic_size<sizeof(magic))rc=-ENOEXEC;
  if(!rc) {
    unsigned char extra;ssize_t got=bfpilot_io_read(source,&extra,1);
    if(got)rc=got<0?(int)got:-EAGAIN;
  }
  if(!rc)rc=bfpilot_io_fstat(source,&after);
  if(!rc && (before.st_ino!=after.st_ino || before.st_size!=after.st_size ||
      before.st_mtim.tv_sec!=after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec))rc=-EAGAIN;
  if(source){int closed=bfpilot_io_close(source);if(!rc)rc=closed;}
  bfpilot_io_end(endpoint);free(buffer);
  if(!rc && fsync(fd))rc=-errno;
  if(owned && unlink(stage)) {
    if(!rc)rc=-errno;
    bfpilot_log("launch: private snapshot cleanup failed path=%s errno=%d",stage,errno);
  }
  if(!rc && lseek(fd,0,SEEK_SET)<0)rc=-errno;
  if(rc){if(fd>=0)close(fd);return rc;}
  return fd;
}

static pthread_mutex_t remote_launch_mutex=PTHREAD_MUTEX_INITIALIZER;

int
bfpilot_launch_payload_path(const char *path) {
  struct stat st;
  int fd = -1;
  int sock = -1;
  int err = 0;
  char buf[8192];
  ssize_t nread;
  size_t total = 0;
  int remote=bfpilot_smb_path(path),locked=0;

  if(!path_ok(path) || !is_payload_name(path)) {
    bfpilot_log("launch: rejected path=%s", path ? path : "(null)");
    return -EINVAL;
  }

  if(remote) {
    if(pthread_mutex_trylock(&remote_launch_mutex))return -EBUSY;
    locked=1;fd=remote_payload_snapshot(path);
    if(fd<0){pthread_mutex_unlock(&remote_launch_mutex);return fd;}
  } else fd=open(path,O_RDONLY);
  if(fd<0)return -errno;
  if(fstat(fd,&st)) {err=-errno;goto done;}
  if(!S_ISREG(st.st_mode)) {err=-ENOEXEC;goto done;}

  if(st.st_size <= 0 || st.st_size > (off_t)(512LL * 1024 * 1024)) {
    bfpilot_log("launch: bad size path=%s size=%lld", path,
                (long long)st.st_size);
    err=-EFBIG;goto done;
  }

  sock = connect_elfldr();
  if(sock < 0) {
    err = sock;
    bfpilot_log("launch: connect elfldr:%d failed errno=%d",
                BFPILOT_ELFLDR_PORT, -sock);
    goto done;
  }

  for(;;) {
    nread=read(fd,buf,sizeof(buf));
    if(nread<0 && errno==EINTR)continue;
    if(nread<=0)break;
    if((unsigned long long)nread>(unsigned long long)st.st_size-total) {
      err=-EAGAIN;goto done;
    }
    size_t off = 0;
    while(off < (size_t)nread) {
      ssize_t sent = send(sock, buf + off, (size_t)nread - off, 0);
      if(sent<0 && errno==EINTR)continue;
      if(sent <= 0) {
        err = sent < 0 ? -errno : -EIO;
        bfpilot_log("launch: send failed path=%s errno=%d", path, errno);
        goto done;
      }
      off += (size_t)sent;
      total += (size_t)sent;
    }
  }

  if(nread < 0) {
    err = -errno;
    bfpilot_log("launch: read failed path=%s errno=%d", path, errno);
    goto done;
  }
  if(total!=(size_t)st.st_size){err=-EAGAIN;goto done;}

  (void)shutdown(sock, SHUT_WR);
  bfpilot_log("launch: sent %zu bytes to elfldr path=%s", total, path);
done:
  if(sock>=0)close(sock);
  if(fd>=0 && close(fd) && !err)err=-errno;
  if(locked)pthread_mutex_unlock(&remote_launch_mutex);
  return err;
}

int
bfpilot_launch_request(const http_request_t *req) {
  char path[1024];
  char body[512];
  int rc;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path))) {
    return websrv_send_error_json(req->fd, 400, "missing path");
  }

  rc = bfpilot_launch_payload_path(path);
  if(rc != 0) {
    if(rc == -ECONNREFUSED || rc == -ENETUNREACH || rc == -ETIMEDOUT) {
      return websrv_send_error_json(
          req->fd, 503, "elfldr not reachable on 127.0.0.1:9021");
    }
    if(rc == -EINVAL || rc == -EFBIG || rc == -ENOEXEC) {
      return websrv_send_error_json(req->fd, 400, "not a loadable payload");
    }
    if(rc == -ENOENT) {
      return websrv_send_error_json(req->fd, 404, "file not found");
    }
    if(rc == -EBUSY)return websrv_send_error_json(req->fd,409,"another SMB payload load is running");
    if(rc == -EAGAIN)return websrv_send_error_json(req->fd,409,"payload source changed while reading; retry from a stable file");
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"launch failed\",\"errno\":%d}", -rc);
    return websrv_send_text(req->fd, 500, "application/json", body);
  }

  {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char note[160];
    snprintf(note, sizeof(note), "sent %s to elfldr", base);
    bfpilot_notify("BFpilot", note);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"port\":%d}", BFPILOT_ELFLDR_PORT);
  }
  return websrv_send_text(req->fd, 200, "application/json", body);
}
