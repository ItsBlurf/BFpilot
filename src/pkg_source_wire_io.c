#include "pkg_source_wire.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
static int exact(int fd, void *buffer, size_t size, int sending) {
  unsigned char *p = buffer;
  size_t done = 0;
  while (done < size) {
    ssize_t n = sending ? send(fd, p + done, size - done, 0)
                        : recv(fd, p + done, size - done, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return n < 0 ? -errno : -EPROTO;
    done += (size_t)n;
  }
  return 0;
}
int bfps_send_frame(int fd, unsigned type, const void *payload, size_t size) {
  if (type > 65535 || size > BFPILOT_PKG_SOURCE_FRAME_MAX || (!payload && size))
    return -EINVAL;
  unsigned char header[12] = {'B', 'F', 'S', '1', 0, 0, 0, 0};
  header[4] = (unsigned char)(type >> 8);
  header[5] = (unsigned char)type;
  bfps_put32(header + 8, (uint32_t)size);
  int rc = exact(fd, header, sizeof(header), 1);
  return rc || !size ? rc : exact(fd, (void *)payload, size, 1);
}
int bfps_receive_frame(int fd, unsigned *type, void *payload, size_t capacity,
                       size_t *size) {
  if (!type || !size)
    return -EINVAL;
  unsigned char header[12];
  int rc = exact(fd, header, sizeof(header), 0);
  if (rc)
    return rc;
  if (memcmp(header, "BFS1", 4) || header[6] || header[7])
    return -EPROTO;
  size_t n = bfps_get32(header + 8);
  if (n > BFPILOT_PKG_SOURCE_FRAME_MAX || n > capacity || (!payload && n))
    return -EMSGSIZE;
  rc = n ? exact(fd, payload, n, 0) : 0;
  if (!rc) {
    *size = n;
    *type = (unsigned)header[4] << 8 | header[5];
  }
  return rc;
}
