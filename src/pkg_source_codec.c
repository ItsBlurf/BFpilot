/* Portable bounded source registration, byte ranges and corruption detection.
 */
#include "path_ops.h"
#include "pkg_file.h"
#include "pkg_source_wire.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
static int source_path_valid(const char *path) {
  if (!path || strncmp(path, "/smb/", 5) ||
      strlen(path) >= BFPILOT_PKG_SOURCE_PATH_SIZE ||
      !bfpilot_path_is_safe(path))
    return 0;
  size_t length = strlen(path);
  if (length < 5 + 16 + 1 + 1 + 1 + 5 || path[21] != '/')
    return 0;
  for (size_t i = 5; i < 21; i++)
    if (!((path[i] >= '0' && path[i] <= '9') ||
          (path[i] >= 'a' && path[i] <= 'f')))
      return 0;
  const unsigned char *p = (const unsigned char *)path;
  for (size_t i = 0; i < length;) {
    if (p[i] < 32 || p[i] == 127 || p[i] == '\\')
      return 0;
    if (p[i] < 128) {
      i++;
      continue;
    }
    size_t need = p[i] >= 0xc2 && p[i] <= 0xdf   ? 2
                  : p[i] >= 0xe0 && p[i] <= 0xef ? 3
                  : p[i] >= 0xf0 && p[i] <= 0xf4 ? 4
                                                 : 0;
    if (!need || need > length - i)
      return 0;
    size_t n = bfpilot_utf8_sequence_length(p + i);
    if (n != need)
      return 0;
    i += n;
  }
  const char *segment = path + 1;
  for (const char *at = segment;; at++)
    if (!*at || *at == '/') {
      size_t n = (size_t)(at - segment);
      if (!n || (n == 1 && segment[0] == '.'))
        return 0;
      if (!*at)
        break;
      segment = at + 1;
    }
  const char *dot = strrchr(path, '.');
  return dot && strlen(dot) == 4 && tolower((unsigned char)dot[1]) == 'p' &&
         tolower((unsigned char)dot[2]) == 'k' &&
         tolower((unsigned char)dot[3]) == 'g';
}
static int stamp_valid(const bfpilot_pkg_source_stamp_t *stamp) {
  if (!stamp || stamp->size > INT64_MAX || stamp->mtime_nsec >= 1000000000U)
    return 0;
  bfpilot_pkg_file_info_t info;
  return !bfpilot_pkg_header_validate(stamp->header, sizeof(stamp->header),
                                      stamp->size, &info) &&
         !bfpilot_pkg_file_is_system(&info);
}
int bfps_register_encode(const char *path,
                         const bfpilot_pkg_source_stamp_t *stamp, void *buffer,
                         size_t capacity, size_t *size) {
  if (!buffer || !size || !source_path_valid(path) || !stamp_valid(stamp))
    return -EINVAL;
  size_t n = strlen(path), total = 32 + sizeof(stamp->header) + n;
  if (total > capacity)
    return -EMSGSIZE;
  unsigned char *out = buffer;
  bfps_put64(out, stamp->size);
  bfps_put64(out + 8, stamp->inode);
  bfps_put64(out + 16, (uint64_t)stamp->mtime_sec);
  bfps_put32(out + 24, stamp->mtime_nsec);
  bfps_put32(out + 28, (uint32_t)n);
  memcpy(out + 32, stamp->header, sizeof(stamp->header));
  memcpy(out + 32 + sizeof(stamp->header), path, n);
  *size = total;
  return 0;
}
int bfps_register_decode(const void *buffer, size_t size, char *path,
                         size_t capacity, bfpilot_pkg_source_stamp_t *stamp) {
  if (!buffer || !path || !stamp || size < 32 + BFPILOT_PKG_SOURCE_HEADER_SIZE)
    return -EPROTO;
  const unsigned char *in = buffer;
  size_t n = bfps_get32(in + 28);
  if (n >= capacity || n >= BFPILOT_PKG_SOURCE_PATH_SIZE ||
      n != size - 32 - BFPILOT_PKG_SOURCE_HEADER_SIZE ||
      memchr(in + 32 + BFPILOT_PKG_SOURCE_HEADER_SIZE, 0, n))
    return -EPROTO;
  memcpy(path, in + 32 + BFPILOT_PKG_SOURCE_HEADER_SIZE, n);
  path[n] = 0;
  memset(stamp, 0, sizeof(*stamp));
  stamp->size = bfps_get64(in);
  stamp->inode = bfps_get64(in + 8);
  stamp->mtime_sec = (int64_t)bfps_get64(in + 16);
  stamp->mtime_nsec = bfps_get32(in + 24);
  memcpy(stamp->header, in + 32, sizeof(stamp->header));
  return source_path_valid(path) && stamp_valid(stamp) ? 0 : -EINVAL;
}
static int decimal(const char **text, uint64_t *number) {
  const char *p = *text;
  if (*p < '0' || *p > '9')
    return -ERANGE;
  uint64_t value = 0;
  do {
    unsigned n = (unsigned)(*p - '0');
    if (value > (UINT64_MAX - n) / 10)
      return -ERANGE;
    value = value * 10 + n;
    p++;
  } while (*p >= '0' && *p <= '9');
  *number = value;
  *text = p;
  return 0;
}
int bfps_range_parse(const char *text, uint64_t size,
                     bfpilot_pkg_source_range_t *out) {
  if (!out)
    return -EINVAL;
  out->first = 0;
  out->count = size;
  out->partial = 0;
  if (!text)
    return 0;
  if (strncmp(text, "bytes=", 6) || !size)
    return -ERANGE;
  const char *p = text + 6;
  uint64_t first, last;
  if (*p == '-') {
    p++;
    if (decimal(&p, &last) || *p || !last)
      return -ERANGE;
    out->count = last < size ? last : size;
    out->first = size - out->count;
  } else {
    if (decimal(&p, &first) || *p++ != '-' || first >= size)
      return -ERANGE;
    if (!*p)
      last = size - 1;
    else {
      if (decimal(&p, &last) || *p || last < first)
        return -ERANGE;
      if (last >= size)
        last = size - 1;
    }
    out->first = first;
    out->count = last - first + 1;
  }
  out->partial = 1;
  return 0;
}
static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
int bfps_journal_encode(const char *token, uint64_t seen, const char *path,
                        const bfpilot_pkg_source_stamp_t *stamp, void *buffer,
                        size_t capacity, size_t *size) {
  if (!buffer || !size || !token || strlen(token) != 32 ||
      !bfps_token_valid(token, 32) || capacity < 56)
    return -EINVAL;
  unsigned char *out = buffer;
  size_t n;
  int rc = bfps_register_encode(path, stamp, out + 52, capacity - 56, &n);
  if (rc)
    return rc;
  memcpy(out, "BFSRC1\0\0", 8);
  bfps_put64(out + 8, seen);
  bfps_put32(out + 16, (uint32_t)n);
  memcpy(out + 20, token, 32);
  bfps_put32(out + 52 + n, crc32(out, 52 + n));
  *size = 56 + n;
  return 0;
}
int bfps_journal_decode(const void *buffer, size_t size, char token[33],
                        uint64_t *seen, char *path, size_t capacity,
                        bfpilot_pkg_source_stamp_t *stamp) {
  if (!buffer || !token || !seen || size < 56)
    return -EPROTO;
  const unsigned char *in = buffer;
  size_t n = bfps_get32(in + 16);
  if (n != size - 56 || memcmp(in, "BFSRC1\0\0", 8) ||
      crc32(in, size - 4) != bfps_get32(in + size - 4) ||
      !bfps_token_valid((const char *)in + 20, 32))
    return -EPROTO;
  int rc = bfps_register_decode(in + 52, n, path, capacity, stamp);
  if (rc)
    return rc;
  memcpy(token, in + 20, 32);
  token[32] = 0;
  *seen = bfps_get64(in + 8);
  return 0;
}
