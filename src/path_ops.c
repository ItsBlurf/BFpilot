/*
 * Path resolve + chmod helpers.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "path_ops.h"

size_t
bfpilot_utf8_sequence_length(const unsigned char *s) {
  uint32_t codepoint;
  size_t length;
  if(!s || !s[0]) return 0;
  if(s[0] < 0x80) return 1;
  if((s[0] & 0xe0) == 0xc0) {
    codepoint = s[0] & 0x1f;
    length = 2;
  } else if((s[0] & 0xf0) == 0xe0) {
    codepoint = s[0] & 0x0f;
    length = 3;
  } else if((s[0] & 0xf8) == 0xf0) {
    codepoint = s[0] & 0x07;
    length = 4;
  } else {
    return 0;
  }
  for(size_t i = 1; i < length; i++) {
    if(!s[i] || (s[i] & 0xc0) != 0x80) return 0;
    codepoint = (codepoint << 6) | (s[i] & 0x3f);
  }
  if((length == 2 && codepoint < 0x80) ||
     (length == 3 && codepoint < 0x800) ||
     (length == 4 && (codepoint < 0x10000 || codepoint > 0x10ffff)) ||
     (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0;
  return length;
}

int
bfpilot_path_decode_byte_tokens(char *path) {
  unsigned char *read = (unsigned char *)path;
  unsigned char *write = (unsigned char *)path;
  int changed = 0;
  if(!path) return 0;
  while(*read) {
    size_t length = bfpilot_utf8_sequence_length(read);
    if(length == 4) {
      uint32_t codepoint = ((uint32_t)(read[0] & 0x07) << 18) |
                           ((uint32_t)(read[1] & 0x3f) << 12) |
                           ((uint32_t)(read[2] & 0x3f) << 6) |
                           (uint32_t)(read[3] & 0x3f);
      if(codepoint >= 0xf0080 && codepoint <= 0xf00ff) {
        *write++ = (unsigned char)(codepoint - 0xf0000);
        read += 4;
        changed = 1;
        continue;
      }
    }
    if(length == 0) length = 1;
    memmove(write, read, length);
    write += length;
    read += length;
  }
  *write = 0;
  return changed;
}

/* Host unit tests on MinGW lack lstat; PS5 SDK has full POSIX. */
#if defined(_WIN32) && !defined(__ORBIS__) && !defined(__PROSPERO__)
#define bfpilot_lstat stat
#else
#define bfpilot_lstat lstat
#endif

int
bfpilot_path_is_safe(const char *path) {
  const unsigned char *p;

  if(!path || path[0] != '/') return 0;
  p = (const unsigned char *)path;
  while(*p) {
    const unsigned char *start;
    size_t length;

    if(*p < 0x20 || *p == 0x7f) return 0;
    while(*p == '/') p++;
    start = p;
    while(*p && *p != '/') {
      if(*p < 0x20 || *p == 0x7f) return 0;
      p++;
    }
    length = (size_t)(p - start);
    if(length == 2 && start[0] == '.' && start[1] == '.') return 0;
  }
  return 1;
}

int
bfpilot_parse_mode_octal(const char *s, mode_t *out_mode) {
  mode_t m = 0;
  int digits = 0;
  if(!s || !*s || !out_mode) return -1;
  while(*s) {
    if(*s < '0' || *s > '7') return -1;
    m = (m << 3) | (mode_t)(*s - '0');
    digits++;
    s++;
    if(digits > 4) return -1;
  }
  if(digits == 0) return -1;
  *out_mode = m & 07777;
  return 0;
}

int
bfpilot_path_chmod(const char *path, mode_t mode) {
  struct stat st;
  if(!bfpilot_path_is_safe(path)) return -EINVAL;
  if(bfpilot_lstat(path, &st) != 0) return -errno;
  if(chmod(path, mode) != 0) return -errno;
  return 0;
}

int
bfpilot_path_resolve_ci(const char *in, char *out, size_t out_sz) {
  char work[1024];
  char cur[1024];
  char *save = NULL;
  char *seg;
  size_t n;

  if(!in || !out || out_sz < 2) return -EINVAL;
  if(!bfpilot_path_is_safe(in)) return -EINVAL;

  n = strlen(in);
  if(n >= sizeof(work)) return -ENAMETOOLONG;
  memcpy(work, in, n + 1);

  /* Exact path first */
  {
    struct stat st;
    if(bfpilot_lstat(in, &st) == 0) {
      if(strlen(in) >= out_sz) return -ENAMETOOLONG;
      memcpy(out, in, strlen(in) + 1);
      return 0;
    }
  }

  cur[0] = '/';
  cur[1] = 0;

  /* Skip leading slash for strtok */
  seg = strtok_r(work, "/", &save);
  if(!seg) {
    if(out_sz < 2) return -ENAMETOOLONG;
    out[0] = '/';
    out[1] = 0;
    return 0;
  }

  while(seg) {
    DIR *d = opendir(cur[1] ? cur : "/");
    struct dirent *ent;
    int found = 0;
    char next[1024];

    if(!d) return -errno;

    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(strcasecmp(ent->d_name, seg) == 0) {
        int written;
        if(cur[1] == 0)
          written = snprintf(next, sizeof(next), "/%s", ent->d_name);
        else
          written = snprintf(next, sizeof(next), "%s/%s", cur, ent->d_name);
        if(written < 0 || (size_t)written >= sizeof(next)) {
          closedir(d);
          return -ENAMETOOLONG;
        }
        found = 1;
        break;
      }
    }
    closedir(d);

    if(!found) return -ENOENT;
    if(strlen(next) >= sizeof(cur)) return -ENAMETOOLONG;
    memcpy(cur, next, strlen(next) + 1);
    seg = strtok_r(NULL, "/", &save);
  }

  if(strlen(cur) >= out_sz) return -ENAMETOOLONG;
  memcpy(out, cur, strlen(cur) + 1);
  return 0;
}
