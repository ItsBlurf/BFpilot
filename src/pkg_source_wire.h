#pragma once
#include <stddef.h>
#include <stdint.h>
#define BFPILOT_PKG_SOURCE_HTTP_PORT 9042
#define BFPILOT_PKG_SOURCE_CONTROL_PORT 9043
#define BFPILOT_PKG_SOURCE_PREFIX "http://127.0.0.1:9042/source/"
#define BFPILOT_PKG_SOURCE_VERSION "BFPSRC/1"
#define BFPILOT_PKG_SOURCE_HEADER_SIZE 0x440U
#define BFPILOT_PKG_SOURCE_PATH_SIZE 1024U
#define BFPILOT_PKG_SOURCE_FRAME_MAX 4096U
#define BFPILOT_PKG_SOURCE_MAX_RECORDS 4096U
#define BFPILOT_PKG_SOURCE_MAX_STREAMS 4U
#define BFPILOT_PKG_SOURCE_REGISTER_MAX                                        \
  (32U + BFPILOT_PKG_SOURCE_HEADER_SIZE + BFPILOT_PKG_SOURCE_PATH_SIZE)
enum {
  BFPS_PING = 1,
  BFPS_PONG = 2,
  BFPS_REGISTER = 3,
  BFPS_REGISTERED = 4,
  BFPS_DROP = 5,
  BFPS_DROPPED = 6,
  BFPS_STATS = 7,
  BFPS_STATISTICS = 8,
  BFPS_STOP = 100,
  BFPS_STOPPED = 101,
  BFPS_ERROR = 65535
};
typedef struct bfpilot_pkg_source_stamp {
  uint64_t size, inode;
  int64_t mtime_sec;
  uint32_t mtime_nsec;
  unsigned char header[BFPILOT_PKG_SOURCE_HEADER_SIZE];
} bfpilot_pkg_source_stamp_t;
typedef struct bfpilot_pkg_source_range {
  uint64_t first, count;
  int partial;
} bfpilot_pkg_source_range_t;
static inline uint32_t bfps_get32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static inline uint64_t bfps_get64(const unsigned char *p) {
  return (uint64_t)bfps_get32(p) << 32 | bfps_get32(p + 4);
}
static inline void bfps_put32(unsigned char *p, uint32_t v) {
  for (int i = 3; i >= 0; i--) {
    p[i] = (unsigned char)v;
    v >>= 8;
  }
}
static inline void bfps_put64(unsigned char *p, uint64_t v) {
  bfps_put32(p, (uint32_t)(v >> 32));
  bfps_put32(p + 4, (uint32_t)v);
}
static inline int bfps_token_valid(const char *p, size_t n) {
  if (!p || n != 32)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
      return 0;
  return 1;
}
/* Shared with the small AppInst worker: only our exact loopback capability URI.
 */
static inline int bfps_uri_valid(const char *uri) {
  const char prefix[] = BFPILOT_PKG_SOURCE_PREFIX;
  size_t i = 0;
  if (!uri)
    return 0;
  for (; i < sizeof(prefix) - 1; i++)
    if (uri[i] != prefix[i])
      return 0;
  const char *p = uri + i;
  for (i = 0; i < 32; i++)
    if (!p[i] ||
        !((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
      return 0;
  return p[32] == '.' && p[33] == 'p' && p[34] == 'k' && p[35] == 'g' &&
         p[36] == 0;
}
int bfps_register_encode(const char *, const bfpilot_pkg_source_stamp_t *,
                         void *, size_t, size_t *);
int bfps_register_decode(const void *, size_t, char *, size_t,
                         bfpilot_pkg_source_stamp_t *);
int bfps_range_parse(const char *, uint64_t, bfpilot_pkg_source_range_t *);
int bfps_journal_encode(const char *, uint64_t, const char *,
                        const bfpilot_pkg_source_stamp_t *, void *, size_t,
                        size_t *);
int bfps_journal_decode(const void *, size_t, char[33], uint64_t *, char *,
                        size_t, bfpilot_pkg_source_stamp_t *);
int bfps_send_frame(int, unsigned, const void *, size_t);
int bfps_receive_frame(int, unsigned *, void *, size_t, size_t *);
