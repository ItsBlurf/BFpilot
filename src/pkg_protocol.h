#ifndef BFPILOT_PKG_PROTOCOL_H
#define BFPILOT_PKG_PROTOCOL_H
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>

/* TCP preserves bytes, not send boundaries. Never dispatch a partial command,
 * an embedded NUL, or a truncated path as an installation request. The caller
 * configures socket timeouts and closes after exactly one command. */
static int bfpilot_pkg_read_request(int fd, char *buffer, size_t capacity) {
  if(capacity < 2) return -EINVAL;
  size_t used = 0;
  for(;;) {
    if(used == capacity - 1) return -EMSGSIZE;
    ssize_t n = recv(fd, buffer + used, capacity - 1 - used, 0);
    if(n < 0) { if(errno == EINTR) continue; return -errno; }
    if(n == 0) return -EPROTO;
    size_t end = used + (size_t)n;
    for(size_t i = used; i < end; i++) {
      if(buffer[i] == 0) return -EPROTO;
      if(buffer[i] == '\n') {
        if(i > 0 && buffer[i - 1] == '\r') i--;
        buffer[i] = 0;
        return 0;
      }
    }
    used = end;
  }
}
#endif
