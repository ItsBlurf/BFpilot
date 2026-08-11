/*
 * Send a local payload file to elfldr on 127.0.0.1:9021.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "diag.h"
#include "notify.h"
#include "payload_launch.h"
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

int
bfpilot_launch_payload_path(const char *path) {
  struct stat st;
  int fd = -1;
  int sock = -1;
  int err = 0;
  char buf[8192];
  ssize_t nread;
  size_t total = 0;

  if(!path_ok(path) || !is_payload_name(path)) {
    bfpilot_log("launch: rejected path=%s", path ? path : "(null)");
    return -EINVAL;
  }

  if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    bfpilot_log("launch: not a regular file path=%s errno=%d", path, errno);
    return -errno;
  }

  if(st.st_size <= 0 || st.st_size > (off_t)(512LL * 1024 * 1024)) {
    bfpilot_log("launch: bad size path=%s size=%lld", path,
                (long long)st.st_size);
    return -EFBIG;
  }

  fd = open(path, O_RDONLY);
  if(fd < 0) {
    bfpilot_log("launch: open failed path=%s errno=%d", path, errno);
    return -errno;
  }

  sock = connect_elfldr();
  if(sock < 0) {
    err = sock;
    bfpilot_log("launch: connect elfldr:%d failed errno=%d",
                BFPILOT_ELFLDR_PORT, -sock);
    close(fd);
    return err;
  }

  while((nread = read(fd, buf, sizeof(buf))) > 0) {
    size_t off = 0;
    while(off < (size_t)nread) {
      ssize_t sent = send(sock, buf + off, (size_t)nread - off, 0);
      if(sent <= 0) {
        err = sent < 0 ? -errno : -EIO;
        bfpilot_log("launch: send failed path=%s errno=%d", path, errno);
        close(sock);
        close(fd);
        return err;
      }
      off += (size_t)sent;
      total += (size_t)sent;
    }
  }

  if(nread < 0) {
    err = -errno;
    bfpilot_log("launch: read failed path=%s errno=%d", path, errno);
    close(sock);
    close(fd);
    return err;
  }

  (void)shutdown(sock, SHUT_WR);
  close(sock);
  close(fd);
  bfpilot_log("launch: sent %zu bytes to elfldr path=%s", total, path);
  return 0;
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
    if(rc == -EINVAL || rc == -EFBIG) {
      return websrv_send_error_json(req->fd, 400, "not a loadable payload");
    }
    if(rc == -ENOENT) {
      return websrv_send_error_json(req->fd, 404, "file not found");
    }
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
