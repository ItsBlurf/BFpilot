/* Minimal, no-key PS4/PS5 PKG container validation. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "path_ops.h"
#include "pkg_file.h"
#ifdef BFPILOT_ENABLE_REMOTE_PKG
#include "pkg_remote.h"
#include "smb.h"
#endif

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
#ifdef BFPILOT_ENABLE_REMOTE_PKG_INSTALL
  if(bfpilot_smb_path(path))return bfpilot_pkg_remote_validate(path,info,error,error_size);
#endif
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

#define PKG_HEADER_SIZE 0xa0U
#define PKG_ENTRY_SIZE 0x20U
#define PKG_ENTRY_PARAM_SFO 0x1000U
#define PKG_ENTRY_ICON0_PNG 0x1200U
#define PKG_ENTRY_PARAM_JSON 0x2000U
#define PKG_ENTRY_MAX 65536U
#define PKG_PARAM_MAX (4U * 1024U * 1024U)
#define PKG_ICON_MAX (32U * 1024U * 1024U)
#define SFO_MAGIC 0x46535000U
#define SFO_ENTRY_SIZE 0x10U
#define SFO_ENTRY_MAX 256U
#define PKG_MAGIC_LIH 0x7f4c4948U

static inline uint16_t
read_le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t
read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t
read_le64(const unsigned char *p) {
  return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static inline int
range_valid(uint64_t offset, uint64_t size, uint64_t total_size) {
  return offset <= total_size && size <= (total_size - offset);
}

static int
read_at(int fd, void *buffer, size_t size, uint64_t offset) {
  unsigned char *p = (unsigned char *)buffer;
  size_t done = 0;
  while(done < size) {
#if defined(_WIN32)
    if(_lseeki64(fd, (int64_t)(offset + done), SEEK_SET) == -1) return -1;
    int n = read(fd, p + done, (unsigned int)(size - done));
#else
    ssize_t n = pread(fd, p + done, size - done, (off_t)(offset + done));
#endif
    if(n <= 0) {
      if(n < 0 && errno == EINTR) continue;
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static void
add_field(bfpilot_pkg_details_t *details, const char *name, const char *value) {
  if(!details || !name || !value || details->field_count >= BFPILOT_PKG_MAX_FIELDS) return;
  size_t idx = details->field_count++;
  strncpy(details->fields[idx].name, name, sizeof(details->fields[idx].name) - 1);
  details->fields[idx].name[sizeof(details->fields[idx].name) - 1] = 0;
  strncpy(details->fields[idx].value, value, sizeof(details->fields[idx].value) - 1);
  details->fields[idx].value[sizeof(details->fields[idx].value) - 1] = 0;
}

static void
parse_sfo(const unsigned char *sfo, size_t size, bfpilot_pkg_details_t *details) {
  if(!sfo || size < 20 || read_le32(sfo) != SFO_MAGIC) return;
  uint32_t key_offset = read_le32(sfo + 8);
  uint32_t value_offset = read_le32(sfo + 12);
  uint32_t count = read_le32(sfo + 16);
  if(count > SFO_ENTRY_MAX || (uint64_t)20 + (uint64_t)count * SFO_ENTRY_SIZE > size ||
     key_offset >= size || value_offset >= size) return;

  for(uint32_t i = 0; i < count; i++) {
    const unsigned char *entry = sfo + 20 + (size_t)i * SFO_ENTRY_SIZE;
    uint16_t name_offset = read_le16(entry);
    unsigned int type = entry[3];
    uint32_t val_size = read_le32(entry + 4);
    uint32_t data_offset = read_le32(entry + 12);
    uint64_t key_pos = (uint64_t)key_offset + name_offset;
    uint64_t val_pos = (uint64_t)value_offset + data_offset;
    if(key_pos >= size || val_pos > size) continue;

    const unsigned char *key_end = memchr(sfo + key_pos, 0, size - (size_t)key_pos);
    if(!key_end) continue;
    const char *key_name = (const char *)sfo + key_pos;

    char val_buf[256];
    val_buf[0] = 0;

    if(type == 2) {
      if(val_size && range_valid(val_pos, val_size, size)) {
        size_t len = strnlen((const char *)sfo + val_pos, val_size);
        if(len >= sizeof(val_buf)) len = sizeof(val_buf) - 1;
        memcpy(val_buf, sfo + val_pos, len);
        val_buf[len] = 0;
      }
    } else if(type == 4 && range_valid(val_pos, 4, size)) {
      uint32_t num = read_le32(sfo + val_pos);
      if(!strcmp(key_name, "SYSTEM_VER")) {
        snprintf(val_buf, sizeof(val_buf), "%x.%02x",
                 (unsigned)(num >> 24) & 0xff, (unsigned)(num >> 16) & 0xff);
      } else {
        snprintf(val_buf, sizeof(val_buf), "%u", (unsigned)num);
      }
    }
    if(!val_buf[0]) continue;

    add_field(details, key_name, val_buf);

    if(!strcmp(key_name, "TITLE") && !details->title_name[0]) {
      strncpy(details->title_name, val_buf, sizeof(details->title_name) - 1);
    } else if(!strcmp(key_name, "TITLE_ID") && !details->title_id[0]) {
      strncpy(details->title_id, val_buf, sizeof(details->title_id) - 1);
    } else if(!strcmp(key_name, "APP_VER") && !details->app_ver[0]) {
      strncpy(details->app_ver, val_buf, sizeof(details->app_ver) - 1);
    } else if(!strcmp(key_name, "SYSTEM_VER") && !details->min_fw[0]) {
      strncpy(details->min_fw, val_buf, sizeof(details->min_fw) - 1);
    } else if(!strcmp(key_name, "CATEGORY") && !details->category[0]) {
      strncpy(details->category, val_buf, sizeof(details->category) - 1);
    } else if(!strcmp(key_name, "CONTENT_ID") && !details->content_id[0]) {
      strncpy(details->content_id, val_buf, sizeof(details->content_id) - 1);
    }
  }
}

static int
json_hex4(const char **cursor, const char *end, unsigned *value) {
  if(end - *cursor < 4) return -1;
  unsigned result = 0;
  for(int i = 0; i < 4; i++) {
    unsigned char c = (unsigned char)*(*cursor)++;
    unsigned digit;
    if(c >= '0' && c <= '9') digit = c - '0';
    else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return -1;
    result = (result << 4) | digit;
  }
  *value = result;
  return 0;
}

/* Decode a bounded string, validating even a truncated output's remaining
 * input. UTF-8 sequences and surrogate pairs are never split in the result. */
static int
json_read_string(const char **cursor, const char *end, char *out, size_t size) {
  const char *p = *cursor;
  if(!size || p >= end || *p++ != '"') return -1;
  size_t used = 0;
  int truncated = 0;
  out[0] = 0;
  while(p < end) {
    unsigned char c = (unsigned char)*p++;
    if(c == '"') { out[used] = 0; *cursor = p; return 0; }
    if(c < 0x20) break;
    unsigned char bytes[5] = {0};
    size_t count = 1;
    bytes[0] = c;
    if(c == '\\') {
      if(p >= end) break;
      c = (unsigned char)*p++;
      switch(c) {
      case '"': case '\\': case '/': bytes[0] = c; break;
      case 'b': bytes[0] = '\b'; break;
      case 'f': bytes[0] = '\f'; break;
      case 'n': bytes[0] = '\n'; break;
      case 'r': bytes[0] = '\r'; break;
      case 't': bytes[0] = '\t'; break;
      case 'u': {
        unsigned cp = 0;
        if(json_hex4(&p, end, &cp) != 0 || cp == 0) goto invalid;
        if(cp >= 0xd800 && cp <= 0xdbff) {
          if(end - p < 6 || p[0] != '\\' || p[1] != 'u') goto invalid;
          p += 2;
          unsigned low = 0;
          if(json_hex4(&p, end, &low) != 0 || low < 0xdc00 || low > 0xdfff) goto invalid;
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if(cp >= 0xdc00 && cp <= 0xdfff) goto invalid;
        if(cp < 0x80) bytes[0] = (unsigned char)cp;
        else if(cp < 0x800) {
          count = 2; bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63);
        } else if(cp < 0x10000) {
          count = 3; bytes[0] = 0xe0 | (cp >> 12);
          bytes[1] = 0x80 | ((cp >> 6) & 63); bytes[2] = 0x80 | (cp & 63);
        } else {
          count = 4; bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63);
          bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63);
        }
        break;
      }
      default: goto invalid;
      }
    } else if(c >= 0x80) {
      const char *start = p - 1;
      size_t available = (size_t)(end - start);
      memcpy(bytes, start, available < 4 ? available : 4);
      count = bfpilot_utf8_sequence_length(bytes);
      if(count == 0 || count > available) goto invalid;
      p = start + count;
    }
    if(!truncated && count < size - used) {
      memcpy(out + used, bytes, count);
      used += count;
    } else truncated = 1;
  }
invalid:
  out[0] = 0;
  return -1;
}

static void
json_extract_val(const char *json, size_t json_len, const char *key, char *out, size_t out_size) {
  if(!out || !out_size) return;
  out[0] = 0;
  if(!json || !key) return;
  const char *p = json, *end = json + json_len;
  while(p < end) {
    if(*p != '"') { p++; continue; }
    char name[256];
    if(json_read_string(&p, end, name, sizeof(name)) != 0) return;
    const char *value = p;
    while(value < end && isspace((unsigned char)*value)) value++;
    if(value == end || *value != ':' || strcmp(name, key)) continue;
    value++;
    while(value < end && isspace((unsigned char)*value)) value++;
    if(value == end) return;
    if(*value == '"') {
      (void)json_read_string(&value, end, out, out_size);
    } else if(*value != '{' && *value != '[' && *value != '}' && *value != ']') {
      size_t used = 0;
      while(value < end && *value != ',' && *value != '}' && *value != ']' &&
            !isspace((unsigned char)*value)) {
        if(used + 1 < out_size) out[used++] = *value;
        value++;
      }
      out[used] = 0;
    }
    return;
  }
}

static void
parse_param_json(const char *json, size_t size, bfpilot_pkg_details_t *details) {
  if(!json || !size) return;
  char buf[256];

  json_extract_val(json, size, "titleId", buf, sizeof(buf));
  if(buf[0]) {
    strncpy(details->title_id, buf, sizeof(details->title_id) - 1);
    add_field(details, "titleId", buf);
  }

  json_extract_val(json, size, "contentId", buf, sizeof(buf));
  if(buf[0]) {
    strncpy(details->content_id, buf, sizeof(details->content_id) - 1);
    add_field(details, "contentId", buf);
  }

  json_extract_val(json, size, "contentVersion", buf, sizeof(buf));
  if(buf[0]) {
    strncpy(details->app_ver, buf, sizeof(details->app_ver) - 1);
    add_field(details, "contentVersion", buf);
  }

  json_extract_val(json, size, "titleName", buf, sizeof(buf));
  if(buf[0]) {
    strncpy(details->title_name, buf, sizeof(details->title_name) - 1);
    add_field(details, "titleName", buf);
  }

  json_extract_val(json, size, "requiredSystemSoftwareVersion", buf, sizeof(buf));
  if(buf[0]) {
    unsigned long long num = strtoull(buf, NULL, 0);
    if(num > 0) {
      snprintf(details->min_fw, sizeof(details->min_fw), "%llx.%02llx",
               (num >> 24) & 0xff, (num >> 16) & 0xff);
    } else {
      strncpy(details->min_fw, buf, sizeof(details->min_fw) - 1);
    }
    add_field(details, "requiredSystemSoftwareVersion", buf);
  }

  json_extract_val(json, size, "applicationCategoryType", buf, sizeof(buf));
  if(buf[0]) {
    unsigned int cat = (unsigned int)strtoul(buf, NULL, 0);
    if(cat == 66048) strncpy(details->category, "Web App (Fullscreen)", sizeof(details->category) - 1);
    else if(cat == 65536) strncpy(details->category, "Web Shortcut", sizeof(details->category) - 1);
    else if(cat == 0) strncpy(details->category, "Game / App", sizeof(details->category) - 1);
    else snprintf(details->category, sizeof(details->category), "%u", cat);
    add_field(details, "applicationCategoryType", buf);
  }
}

static int pkg_reader_read(const bfpilot_pkg_reader_t *reader, void *buffer,
                            size_t size, uint64_t offset) {
  if(!reader || !reader->read_at || !range_valid(offset,size,reader->size))return -EINVAL;
  return reader->read_at(reader->context,buffer,size,offset);
}

int bfpilot_pkg_validate_reader(const bfpilot_pkg_reader_t *reader,
                                bfpilot_pkg_file_info_t *info) {
  unsigned char header[PKG_PROBE_SIZE];
  if(!info)return -EINVAL;
  int rc=pkg_reader_read(reader,header,sizeof(header),0);
  return rc?rc:bfpilot_pkg_header_validate(header,sizeof(header),reader->size,info);
}

int
bfpilot_pkg_parse_reader(const bfpilot_pkg_reader_t *reader, bfpilot_pkg_details_t *details) {
  if(!reader || !reader->read_at || !details)return -EINVAL;
  memset(details,0,sizeof(*details));
  uint64_t file_size=reader->size;
  unsigned char header[PKG_HEADER_SIZE];
  if(file_size < PKG_HEADER_SIZE || pkg_reader_read(reader, header, sizeof(header), 0) != 0) {
    return -EINVAL;
  }

  details->size = file_size;
  uint32_t magic = read_be32(header);
  uint64_t container_offset = 0;

  if(magic == PKG_MAGIC_FIH) {
    container_offset = read_le64(header + 0x58);
  } else if(magic == PKG_MAGIC_LIH) {
    container_offset = read_le64(header + 0x30);
  } else if(magic != PKG_MAGIC_CNT) {
    return -EINVAL;
  }

  if(container_offset > 0) {
    if(!range_valid(container_offset, sizeof(header), file_size) ||
       pkg_reader_read(reader, header, sizeof(header), container_offset) != 0 ||
       read_be32(header) != PKG_MAGIC_CNT) {
      return -EINVAL;
    }
  }
  uint64_t container_size = file_size - container_offset;

  memcpy(details->content_id, header + 0x40, 36);
  details->content_id[36] = 0;
  details->content_type = read_be32(header + 0x74);
  details->content_flags = read_be32(header + 0x78);
  extract_title_id(details->content_id, details->title_id, sizeof(details->title_id));

  uint32_t entry_count = read_be32(header + 0x10);
  uint32_t table_offset = read_be32(header + 0x18);
  if(!entry_count || entry_count > PKG_ENTRY_MAX) return -EINVAL;
  size_t table_size = (size_t)entry_count * PKG_ENTRY_SIZE;
  if(!range_valid(table_offset, table_size, container_size)) return -EINVAL;

  unsigned char *table = malloc(table_size);
  if(!table) return -ENOMEM;
  if(pkg_reader_read(reader, table, table_size, container_offset + table_offset) != 0) {
    free(table);
    return -EIO;
  }

  uint64_t param_offset = 0;
  uint32_t param_size = 0;
  int is_param_json = 0;

  for(uint32_t i = 0; i < entry_count; i++) {
    const unsigned char *entry = table + (size_t)i * PKG_ENTRY_SIZE;
    uint32_t id = read_be32(entry);
    uint32_t offset = read_be32(entry + 0x10);
    uint32_t size = read_be32(entry + 0x14);

    if(!range_valid(offset, size, container_size)) continue;
    if(id == PKG_ENTRY_PARAM_SFO && size && size <= PKG_PARAM_MAX) {
      param_offset = container_offset + offset;
      param_size = size;
      is_param_json = 0;
    } else if(id == PKG_ENTRY_PARAM_JSON && size && size <= PKG_PARAM_MAX) {
      param_offset = container_offset + offset;
      param_size = size;
      is_param_json = 1;
    } else if(id == PKG_ENTRY_ICON0_PNG && size && size <= PKG_ICON_MAX) {
      details->icon_offset = container_offset + offset;
      details->icon_size = size;
    }
  }
  free(table);

  if(details->icon_size >= 8) {
    unsigned char png_magic[8];
    if(pkg_reader_read(reader, png_magic, sizeof(png_magic), details->icon_offset))return -EIO;
    if(memcmp(png_magic, "\x89PNG\r\n\x1a\n", 8) == 0) {
      details->has_icon = 1;
    }
  }

  if(param_size > 0) {
    unsigned char *param_buf = malloc(param_size + 1);
    if(!param_buf)return -ENOMEM;
    {
      if(pkg_reader_read(reader, param_buf, param_size, param_offset) == 0) {
        param_buf[param_size] = 0;
        if(is_param_json) {
          strncpy(details->platform, "PS5", sizeof(details->platform) - 1);
          parse_param_json((const char *)param_buf, param_size, details);
        } else {
          strncpy(details->platform, "PS4", sizeof(details->platform) - 1);
          parse_sfo(param_buf, param_size, details);
        }
      } else {free(param_buf);return -EIO;}
      free(param_buf);
    }
  }

  if(!details->platform[0]) {
    strncpy(details->platform, magic == PKG_MAGIC_CNT ? "PS4" : "PS5", sizeof(details->platform) - 1);
  }
  return 0;
}

static int pkg_fd_read(void *context,void *buffer,size_t size,uint64_t offset) {
  return read_at((int)(intptr_t)context,buffer,size,offset)==0?0:-(errno?errno:EIO);
}

int bfpilot_pkg_icon_reader(const bfpilot_pkg_reader_t *reader,
                             void **icon_buf,size_t *icon_size) {
  if(!icon_buf || !icon_size)return -EINVAL;
  *icon_buf=NULL;*icon_size=0;
  bfpilot_pkg_details_t details;int rc=bfpilot_pkg_parse_reader(reader,&details);
  if(rc || !details.has_icon || details.icon_size<8 || details.icon_size>PKG_ICON_MAX)
    return rc?rc:-ENOENT;
  void *buffer=malloc(details.icon_size);if(!buffer)return -ENOMEM;
  rc=pkg_reader_read(reader,buffer,details.icon_size,details.icon_offset);
  if(rc){free(buffer);return rc;}
  *icon_buf=buffer;*icon_size=details.icon_size;return 0;
}

int
bfpilot_pkg_parse_details(const char *path, bfpilot_pkg_details_t *details) {
#ifdef BFPILOT_ENABLE_REMOTE_PKG
  if(bfpilot_smb_path(path))return bfpilot_pkg_remote_details(path,details);
#endif
  if(!path || !details || !bfpilot_path_is_safe(path)) return -EINVAL;
  memset(details, 0, sizeof(*details));

  struct stat st;
  if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)PKG_HEADER_SIZE) {
    return -EINVAL;
  }

  int flags = O_RDONLY;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int fd = open(path, flags);
  if(fd < 0) return -errno;

  bfpilot_pkg_reader_t reader={(void *)(intptr_t)fd,(uint64_t)st.st_size,pkg_fd_read};
  int rc = bfpilot_pkg_parse_reader(&reader,details);
  close(fd);
  return rc;
}

int
bfpilot_pkg_read_icon(const char *path, void **icon_buf, size_t *icon_size) {
#ifdef BFPILOT_ENABLE_REMOTE_PKG
  if(bfpilot_smb_path(path))return bfpilot_pkg_remote_icon(path,icon_buf,icon_size);
#endif
  if(!path || !icon_buf || !icon_size || !bfpilot_path_is_safe(path)) return -EINVAL;
  *icon_buf = NULL;
  *icon_size = 0;

  struct stat st;
  if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)PKG_HEADER_SIZE) {
    return -EINVAL;
  }

  int flags = O_RDONLY;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int fd = open(path, flags);
  if(fd < 0) return -errno;

  bfpilot_pkg_reader_t reader={(void *)(intptr_t)fd,(uint64_t)st.st_size,pkg_fd_read};
  int rc=bfpilot_pkg_icon_reader(&reader,icon_buf,icon_size);
  close(fd);
  return rc;
}

