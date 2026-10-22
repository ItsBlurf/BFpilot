#include "pkg_source_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef BFPILOT_PKG_SOURCE_EMBED
#include "payload_launch.h"
__asm__(".section .rodata\n.global bfpilot_pkg_source_elf\n"
        ".align 16\nbfpilot_pkg_source_elf:\n"
        ".incbin \"build/internal/bf-pkg-source.elf\"\n"
        ".global bfpilot_pkg_source_elf_end\nbfpilot_pkg_source_elf_end:\n"
        ".previous\n");
extern const unsigned char bfpilot_pkg_source_elf[], bfpilot_pkg_source_elf_end[];
#endif
static pthread_mutex_t startup_lock = PTHREAD_MUTEX_INITIALIZER;
static int request(unsigned type, const void *input, size_t input_size,
                   unsigned expected, void *output, size_t capacity, size_t *size) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
  int rc = 0;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) rc = -errno;
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(BFPILOT_PKG_SOURCE_CONTROL_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (!rc && connect(fd, (struct sockaddr *)&address, sizeof(address))) rc = -errno;
  if (!rc) rc = bfps_send_frame(fd, type, input, input_size);
  unsigned reply = 0; unsigned char data[BFPILOT_PKG_SOURCE_FRAME_MAX]; size_t length = 0;
  if (!rc) rc = bfps_receive_frame(fd, &reply, data, sizeof(data), &length);
  if (!rc && reply == BFPS_ERROR) {
    if (length != 4 || (int32_t)bfps_get32(data) >= 0) rc = -EPROTO;
    else rc = (int32_t)bfps_get32(data);
  } else if (!rc && (reply != expected || length > capacity)) rc = -EPROTO;
  if (!rc) {
    if (length) memcpy(output, data, length);
    *size = length;
  }
  if (close(fd) && !rc) rc = -errno;
  return rc;
}
int bfpilot_pkg_source_ping(void) {
  char version[32]; size_t size;
  int rc = request(BFPS_PING, NULL, 0, BFPS_PONG, version, sizeof(version), &size);
  return rc ? rc : size == sizeof(BFPILOT_PKG_SOURCE_VERSION) - 1 && !memcmp(version, BFPILOT_PKG_SOURCE_VERSION, size) ? 0 : -EPROTO;
}
int bfpilot_pkg_source_ensure(void) {
  pthread_mutex_lock(&startup_lock);
  int rc = bfpilot_pkg_source_ping();
#ifdef BFPILOT_PKG_SOURCE_EMBED
  /* Only a refused connection proves absence. An occupied/unknown endpoint
   * must never be stopped or overwritten by a guessed replacement. */
  if (rc == -ECONNREFUSED) {
    rc = bfpilot_launch_payload_memory(bfpilot_pkg_source_elf,
         (size_t)(bfpilot_pkg_source_elf_end - bfpilot_pkg_source_elf), "BFpilot package source");
    if (!rc) for (unsigned attempt = 0; attempt < 50; attempt++) {
      usleep(100000); rc = bfpilot_pkg_source_ping();
      if (rc != -ECONNREFUSED) break;
    }
  }
#endif
  pthread_mutex_unlock(&startup_lock);
  return rc;
}
int bfpilot_pkg_source_register(const char *path, const bfpilot_pkg_source_stamp_t *stamp, char *uri, size_t capacity) {
  if (!uri || capacity < sizeof(BFPILOT_PKG_SOURCE_PREFIX) + 32 + 4) return -EINVAL;
  uri[0] = 0;
  unsigned char encoded[BFPILOT_PKG_SOURCE_REGISTER_MAX]; size_t size, reply_size;
  int rc = bfps_register_encode(path, stamp, encoded, sizeof(encoded), &size);
  if (!rc) rc = bfpilot_pkg_source_ensure();
  char token[33];
  if (!rc) rc = request(BFPS_REGISTER, encoded, size, BFPS_REGISTERED, token, 32, &reply_size);
  if (!rc && !bfps_token_valid(token, reply_size)) rc = -EPROTO;
  if (!rc) { token[32] = 0; snprintf(uri, capacity, BFPILOT_PKG_SOURCE_PREFIX "%s.pkg", token); }
  return rc;
}
int bfpilot_pkg_source_drop(const char *uri) {
  if (!bfps_uri_valid(uri)) return -EINVAL;
  size_t size;
  return request(BFPS_DROP, uri + sizeof(BFPILOT_PKG_SOURCE_PREFIX) - 1, 32, BFPS_DROPPED, NULL, 0, &size);
}
int bfpilot_pkg_source_stats(unsigned *records, unsigned *streams) {
  unsigned char data[8]; size_t size;
  int rc = request(BFPS_STATS, NULL, 0, BFPS_STATISTICS, data, sizeof(data), &size);
  if (!rc && size != sizeof(data)) rc = -EPROTO;
  if (!rc) { if (records) *records = bfps_get32(data); if (streams) *streams = bfps_get32(data + 4); }
  return rc;
}
int bfpilot_pkg_source_stop(void) {
  size_t size;
  return request(BFPS_STOP, NULL, 0, BFPS_STOPPED, NULL, 0, &size);
}
