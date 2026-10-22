/* Independent, embedded SMB package source. No AppInst/Sony identity changes. */
#include "pkg_source_http.h"
#include "pkg_source_store.h"
#include "diag.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ps5/payload.h>
static pthread_mutex_t admission = PTHREAD_MUTEX_INITIALIZER;
static unsigned active_http, active_control;
static int stopping;
/* Do not link the main process's web/logging globals into this small service. */
void bfpilot_log(const char *format, ...) {
  va_list args; va_start(args, format);
  vfprintf(stderr, format, args); fputc('\n', stderr); va_end(args);
}
static void send_error(int fd, int rc) {
  unsigned char encoded[4]; bfps_put32(encoded, (uint32_t)(int32_t)rc);
  (void)bfps_send_frame(fd, BFPS_ERROR, encoded, sizeof(encoded));
}
static void control_serve(int fd) {
  unsigned char data[BFPILOT_PKG_SOURCE_FRAME_MAX];
  unsigned type; size_t size;
  int rc = bfps_receive_frame(fd, &type, data, sizeof(data), &size);
  if (rc) { send_error(fd, rc); return; }
  if (type == BFPS_PING && !size) {
    (void)bfps_send_frame(fd, BFPS_PONG, BFPILOT_PKG_SOURCE_VERSION, sizeof(BFPILOT_PKG_SOURCE_VERSION) - 1);
  } else if (type == BFPS_REGISTER) {
    char path[BFPILOT_PKG_SOURCE_PATH_SIZE], token[33];
    bfpilot_pkg_source_stamp_t stamp;
    rc = bfps_register_decode(data, size, path, sizeof(path), &stamp);
    if (!rc) rc = bfps_store_register(path, &stamp, token);
    if (rc) send_error(fd, rc);
    else (void)bfps_send_frame(fd, BFPS_REGISTERED, token, 32);
    /* If reply delivery fails the committed capability is retained. */
  } else if (type == BFPS_DROP && size == 32 && bfps_token_valid((char *)data, size)) {
    char token[33]; memcpy(token, data, 32); token[32] = 0;
    rc = bfps_store_drop(token);
    if (rc) send_error(fd, rc);
    else (void)bfps_send_frame(fd, BFPS_DROPPED, NULL, 0);
  } else if ((type == BFPS_STATS || type == BFPS_STOP) && !size) {
    unsigned records, refs;
    /* Admission lock orders STOP against both new connections and REGISTER.
     * Only this control thread can be present when a stop is accepted. */
    pthread_mutex_lock(&admission);
    bfps_store_stats(&records, &refs);
    if (type == BFPS_STOP) {
      if (records || refs || active_http || active_control != 1) rc = -EBUSY;
      else stopping = 1;
    }
    pthread_mutex_unlock(&admission);
    if (rc) send_error(fd, rc);
    else if (type == BFPS_STOP) (void)bfps_send_frame(fd, BFPS_STOPPED, NULL, 0);
    else {
      unsigned char encoded[8]; bfps_put32(encoded, records); bfps_put32(encoded + 4, refs);
      (void)bfps_send_frame(fd, BFPS_STATISTICS, encoded, sizeof(encoded));
    }
  } else send_error(fd, -EPROTO);
}
typedef struct connection { int fd, control; } connection_t;
static void *serve(void *arg) {
  connection_t connection = *(connection_t *)arg; free(arg);
  if (connection.control) control_serve(connection.fd);
  else bfps_http_serve(connection.fd);
  close(connection.fd);
  pthread_mutex_lock(&admission);
  if (connection.control) active_control--; else active_http--;
  pthread_mutex_unlock(&admission);
  return NULL;
}
static int listener(unsigned port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  int reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) { int rc = -errno; close(fd); return rc; }
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 8)) { int rc = -errno; close(fd); return rc; }
  return fd;
}
static void accept_connection(int server, int control) {
  int fd = accept(server, NULL, NULL);
  if (fd < 0) return;
  struct timeval timeout = {.tv_sec = control ? 10 : 60, .tv_usec = 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) { close(fd); return; }
  pthread_mutex_lock(&admission);
  if (stopping || (control ? active_control >= 2 : active_http >= BFPILOT_PKG_SOURCE_MAX_STREAMS)) {
    pthread_mutex_unlock(&admission);
    if (control) send_error(fd, -EBUSY);
    else {
      const char busy[] = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
      (void)send(fd, busy, sizeof(busy) - 1, 0);
    }
    close(fd); return;
  }
  if (control) active_control++; else active_http++;
  pthread_mutex_unlock(&admission);
  connection_t *connection = malloc(sizeof(*connection));
  int rc = ENOMEM;
  if (connection) {
    connection->fd = fd; connection->control = control;
    pthread_t thread; rc = pthread_create(&thread, NULL, serve, connection);
    if (!rc) { (void)pthread_detach(thread); return; }
    free(connection);
  }
  close(fd);
  pthread_mutex_lock(&admission);
  if (control) active_control--; else active_http--;
  pthread_mutex_unlock(&admission);
  bfpilot_log("pkg source: connection thread failed rc=%d", rc);
}
int main(void) {
  (void)syscall(SYS_thr_set_name, -1, "bf-pkg-source.elf");
  signal(SIGPIPE, SIG_IGN);
  int guard = open("/system_tmp/bfpilot-pkg-source.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
  if (guard < 0 || flock(guard, LOCK_EX | LOCK_NB)) {
    if (guard >= 0) close(guard);
    payload_exit(1); return 1;
  }
  int http = listener(BFPILOT_PKG_SOURCE_HTTP_PORT);
  int control = http < 0 ? http : listener(BFPILOT_PKG_SOURCE_CONTROL_PORT);
  int rc = control < 0 ? control : bfps_store_init();
  if (!rc) {
    struct pollfd sockets[2] = {{.fd = http, .events = POLLIN}, {.fd = control, .events = POLLIN}};
    time_t last_expire = 0;
    for (;;) {
      pthread_mutex_lock(&admission); int done = stopping && !active_control && !active_http; pthread_mutex_unlock(&admission);
      if (done) break;
      int ready = poll(sockets, 2, 250);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) { rc = -errno; break; }
      if (sockets[1].revents & POLLIN) accept_connection(control, 1);
      if (sockets[0].revents & POLLIN) accept_connection(http, 0);
      if (sockets[0].revents & (POLLERR | POLLHUP | POLLNVAL) || sockets[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { rc = -EIO; break; }
      time_t now = time(NULL);
      if (now >= last_expire && now - last_expire >= 60) { bfps_store_expire(); last_expire = now; }
    }
  }
  if (rc) bfpilot_log("pkg source: server exit rc=%d", rc);
  if (http >= 0) close(http);
  if (control >= 0) close(control);
  close(guard);
  payload_exit(rc ? 1 : 0); return rc ? 1 : 0;
}
