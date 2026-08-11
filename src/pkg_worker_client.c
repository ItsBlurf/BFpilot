/* Client for the private PKG worker embedded in bfpilot.elf. */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "appinst_direct.h"
#include "diag.h"
#include "payload_launch.h"
#include "pkg_file.h"

#define PKG_WORKER_PORT 9041
#define PKG_WORKER_PROTOCOL "BFPKG/1"
#define PKG_WORKER_INIT_PENDING (-0xDEAD)

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                             \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"           \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const unsigned char name[];                                          \
  extern const size_t name##_size

INCASSET(bfpilot_pkg_worker_elf, "build/internal/bfpilot-pkg-worker.elf");

static void
result_init(bfpilot_appinst_result_t *result) {
  memset(result, 0, sizeof(*result));
  result->result = BFPILOT_DIAG_SKIPPED;
  result->authid_result = BFPILOT_DIAG_SKIPPED;
  result->init_result = BFPILOT_DIAG_SKIPPED;
  result->primary_result = BFPILOT_DIAG_SKIPPED;
  result->fallback_result = BFPILOT_DIAG_SKIPPED;
  result->restore_authid_result = BFPILOT_DIAG_SKIPPED;
}

static int
worker_request(const char *request, char *response, size_t response_size) {
  if(!request || !response || response_size < 2) return -EINVAL;
  response[0] = 0;
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock < 0) return -errno;
  struct timeval timeout = {.tv_sec = 20, .tv_usec = 0};
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(PKG_WORKER_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(connect(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
    int rc = -errno;
    close(sock);
    return rc;
  }

  char line[1100];
  int length = snprintf(line, sizeof(line), "%s\n", request);
  if(length < 0 || (size_t)length >= sizeof(line)) {
    close(sock);
    return -ENAMETOOLONG;
  }
  size_t offset = 0;
  while(offset < (size_t)length) {
    ssize_t sent = send(sock, line + offset, (size_t)length - offset, 0);
    if(sent < 0 && errno == EINTR) continue;
    if(sent <= 0) {
      int rc = sent < 0 ? -errno : -EIO;
      close(sock);
      return rc;
    }
    offset += (size_t)sent;
  }

  size_t used = 0;
  while(used + 1 < response_size) {
    ssize_t received = recv(sock, response + used,
                            response_size - used - 1, 0);
    if(received < 0 && errno == EINTR) continue;
    if(received < 0) {
      int rc = -errno;
      close(sock);
      return rc;
    }
    if(received == 0) break;
    used += (size_t)received;
  }
  response[used] = 0;
  close(sock);
  return used ? 0 : -EIO;
}

static int
worker_ping(bfpilot_appinst_result_t *result) {
  char response[192];
  int rc = worker_request("PING", response, sizeof(response));
  if(rc != 0) return rc;
  int major = 0;
  unsigned int init = 0;
  int identity = -1;
  if(sscanf(response, "READY " PKG_WORKER_PROTOCOL
                     " fw=%d init=0x%x identity=%d",
            &major, &init, &identity) != 3) {
    bfpilot_log("pkg worker: unexpected handshake: %s", response);
    return -EPROTO;
  }
  result->firmware_major = major;
  result->init_result = (int32_t)init;
  result->authid_result = identity;
  result->used_embedded_worker = 1;
  if(identity != 0) return -ENOTSUP;
  if((int32_t)init == PKG_WORKER_INIT_PENDING) return -EAGAIN;
  if((int32_t)init != 0) return (int32_t)init;
  return 0;
}

static int
worker_is_absent(int rc) {
  return rc == -ECONNREFUSED || rc == -ECONNRESET || rc == -EPIPE ||
         rc == -ETIMEDOUT || rc == -EHOSTUNREACH || rc == -ENETUNREACH;
}

static int
wait_worker_ready(bfpilot_appinst_result_t *result, int attempts) {
  int rc = -ETIMEDOUT;
  for(int attempt = 0; attempt < attempts; attempt++) {
    usleep(100000);
    rc = worker_ping(result);
    if(rc == 0) return 0;
    if(rc != -EAGAIN && !worker_is_absent(rc)) return rc;
  }
  return rc;
}

static int
launch_worker(bfpilot_appinst_result_t *result) {
  int rc = bfpilot_launch_payload_memory(bfpilot_pkg_worker_elf,
                                         bfpilot_pkg_worker_elf_size,
                                         "BFpilot PKG worker");
  if(rc != 0) return rc;
  return wait_worker_ready(result, 150);
}

int
bfpilot_pkg_worker_stop(void) {
  char response[64];
  int rc = worker_request("STOP", response, sizeof(response));
  if(worker_is_absent(rc)) return 0;
  if(rc != 0) return rc;
  return !strncmp(response, "OK STOP", 7) ? 0 : -EPROTO;
}

static int
ensure_worker(bfpilot_appinst_result_t *result) {
  int rc = worker_ping(result);
  if(rc == 0) return 0;

  if(rc == -EAGAIN) {
    rc = wait_worker_ready(result, 100);
    if(rc == 0) return 0;
  }

  if(worker_is_absent(rc)) return launch_worker(result);

  /* A reachable worker with a failed init/identity or stale protocol must not
     poison every later install. Retire it once, then start the embedded copy. */
  int stop_rc = bfpilot_pkg_worker_stop();
  if(stop_rc != 0) return rc;
  for(int attempt = 0; attempt < 50; attempt++) {
    usleep(100000);
    int ping_rc = worker_ping(result);
    if(worker_is_absent(ping_rc)) return launch_worker(result);
  }
  return -EBUSY;
}

int
bfpilot_appinst_install_pkg(const char *path, const char *content_name,
                            bfpilot_appinst_result_t *result) {
  (void)content_name;
  if(!path || !path[0] || !result) return -EINVAL;
  result_init(result);

  bfpilot_pkg_file_info_t info;
  char validation_error[256];
  int rc = bfpilot_pkg_file_validate(path, &info, validation_error,
                                     sizeof(validation_error));
  if(rc != 0) return rc;
  if(bfpilot_pkg_file_is_system(&info)) return -ENOTSUP;

  rc = ensure_worker(result);
  if(rc == 0) {
    char request[1050];
    char response[192];
    int length = snprintf(request, sizeof(request), "INSTALL %s", path);
    if(length < 0 || (size_t)length >= sizeof(request)) {
      rc = -ENAMETOOLONG;
    } else {
      rc = worker_request(request, response, sizeof(response));
      if(rc == 0 && !strncmp(response, "OK INSTALL", 10)) {
        const char *content_id = response + 10;
        while(*content_id == ' ') content_id++;
        snprintf(result->content_id, sizeof(result->content_id), "%s",
                 content_id);
        rc = 0;
      } else if(rc == 0) {
        unsigned int sony_rc = 0;
        if(sscanf(response, "ERR INSTALL 0x%x", &sony_rc) == 1 ||
           sscanf(response, "ERR INIT 0x%x", &sony_rc) == 1) {
          rc = (int32_t)sony_rc;
        } else {
          rc = -EPROTO;
        }
        bfpilot_log("pkg worker: install response=%s", response);
      }
    }
  }
  result->primary_result = rc;
  result->result = rc;
  bfpilot_log("pkg worker: path=%s fw=%d identity=%d init=0x%08x "
              "rc=0x%08x cid=%s",
              path, result->firmware_major, result->authid_result,
              result->init_result, rc, result->content_id);
  return rc;
}
