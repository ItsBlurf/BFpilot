/* Minimal, no-key PS4/PS5 PKG container validation. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "path_ops.h"
#include "pkg_file.h"

#if defined(_WIN32)
#define lstat stat
#ifndef ESTALE
#define ESTALE EIO
#endif
#endif

#define PKG_PROBE_SIZE 0x440U
#define PKG_CONTENT_ID_OFFSET 0x40U
#define PKG_CONTENT_ID_SIZE 0x30U
#define PKG_CONTENT_TYPE_OFFSET 0x74U
#define PKG_CONTENT_FLAGS_OFFSET 0x78U
#define PKG_DECLARED_SIZE_OFFSET 0x430U
#define PKG_MAGIC_CNT 0x7f434e54U
#define PKG_MAGIC_FIH 0x7f464948U

static uint32_t
read_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t
read_be64(const unsigned char *p) {
  uint64_t value = 0;
  for(unsigned int i = 0; i < 8; i++) value = (value << 8) | p[i];
  return value;
}

static int
read_exact(int fd, void *data, size_t size) {
  unsigned char *cursor = data;
  while(size > 0) {
    ssize_t got = read(fd, cursor, size);
    if(got < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(got == 0) {
      errno = EIO;
      return -1;
    }
    cursor += got;
    size -= (size_t)got;
  }
  return 0;
}

static int
content_id_valid(const char *content_id) {
  size_t length = strlen(content_id);
  if(length < 16 || length > PKG_CONTENT_ID_SIZE) return 0;
  int dash = 0;
  int underscore = 0;
  for(size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)content_id[i];
    if(c == '-') dash = 1;
    else if(c == '_') underscore = 1;
    else if(!isalnum(c)) return 0;
  }
  return dash && underscore;
}

static int
extract_title_id(const char *content_id, char *out, size_t out_size) {
  const char *dash = strchr(content_id, '-');
  const char *underscore = dash ? strchr(dash + 1, '_') : NULL;
  if(!dash || !underscore || underscore <= dash + 1) return -1;
  size_t length = (size_t)(underscore - (dash + 1));
  if(length + 1 > out_size || length < 5) return -1;
  for(size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)dash[1 + i];
    if(!isalnum(c)) return -1;
  }
  memcpy(out, dash + 1, length);
  out[length] = 0;
  return 0;
}

int
bfpilot_pkg_header_validate(const unsigned char *header, size_t header_size,
                            uint64_t file_size,
                            bfpilot_pkg_file_info_t *info) {
  if(!header || header_size < PKG_PROBE_SIZE || !info) return -EINVAL;
  memset(info, 0, sizeof(*info));
  uint32_t magic = read_be32(header);
  if(magic == PKG_MAGIC_FIH) {
    if(file_size < PKG_PROBE_SIZE) return -EBADMSG;
    info->size = file_size;
    info->format = BFPILOT_PKG_FORMAT_FIH;
    return 0;
  }
  if(magic != PKG_MAGIC_CNT) return -EBADMSG;
  uint64_t declared_size = read_be64(header + PKG_DECLARED_SIZE_OFFSET);
  if(declared_size != file_size || declared_size < PKG_PROBE_SIZE) {
    return -EBADMSG;
  }

  size_t content_length = 0;
  while(content_length < PKG_CONTENT_ID_SIZE &&
        header[PKG_CONTENT_ID_OFFSET + content_length] != 0) {
    unsigned char c = header[PKG_CONTENT_ID_OFFSET + content_length];
    if(c < 0x20 || c > 0x7e) return -EBADMSG;
    info->content_id[content_length++] = (char)c;
  }
  info->content_id[content_length] = 0;
  if(!content_id_valid(info->content_id) ||
     extract_title_id(info->content_id, info->title_id,
                      sizeof(info->title_id)) != 0) return -EBADMSG;
  info->size = declared_size;
  info->format = BFPILOT_PKG_FORMAT_CNT;
  info->content_type = read_be32(header + PKG_CONTENT_TYPE_OFFSET);
  info->content_flags = read_be32(header + PKG_CONTENT_FLAGS_OFFSET);
  return 0;
}

int
bfpilot_pkg_file_is_system(const bfpilot_pkg_file_info_t *info) {
  return info && !strncmp(info->title_id, "NPXS", 4);
}

int
bfpilot_pkg_file_validate(const char *path, bfpilot_pkg_file_info_t *info,
                          char *error, size_t error_size) {
  unsigned char header[PKG_PROBE_SIZE];
  struct stat before;
  struct stat opened;
  int flags = O_RDONLY;
  int fd = -1;
  int saved_errno = 0;

  if(error && error_size) error[0] = 0;
  if(!path || !info || !bfpilot_path_is_safe(path)) {
    saved_errno = EINVAL;
    goto fail;
  }
  memset(info, 0, sizeof(*info));
  if(lstat(path, &before) != 0) {
    saved_errno = errno;
    goto fail;
  }
  if(!S_ISREG(before.st_mode) || before.st_size < (off_t)PKG_PROBE_SIZE) {
    saved_errno = EINVAL;
    goto fail;
  }
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open(path, flags);
  if(fd < 0) {
    saved_errno = errno;
    goto fail;
  }
  if(fstat(fd, &opened) != 0) {
    saved_errno = errno;
    goto fail;
  }
  if(!S_ISREG(opened.st_mode) || opened.st_dev != before.st_dev ||
     opened.st_ino != before.st_ino || opened.st_size != before.st_size) {
    saved_errno = ESTALE;
    goto fail;
  }
  if(read_exact(fd, header, sizeof(header)) != 0) {
    saved_errno = errno;
    goto fail;
  }
  if(close(fd) != 0) {
    fd = -1;
    saved_errno = errno;
    goto fail;
  }
  fd = -1;

  int header_rc = bfpilot_pkg_header_validate(
      header, sizeof(header), (uint64_t)opened.st_size, info);
  if(header_rc != 0) {
    saved_errno = -header_rc;
    goto fail;
  }
  return 0;

fail:
  if(fd >= 0) close(fd);
  if(!saved_errno) saved_errno = EINVAL;
  if(error && error_size) {
    snprintf(error, error_size, "invalid PKG %s: %s", path ? path : "",
             strerror(saved_errno));
  }
  return -saved_errno;
}
