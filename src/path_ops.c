/*
 * Path resolve + chmod helpers.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "path_ops.h"

/* Host unit tests on MinGW lack lstat; PS5 SDK has full POSIX. */
#if defined(_WIN32) && !defined(__ORBIS__) && !defined(__PROSPERO__)
#define bfpilot_lstat stat
#else
#define bfpilot_lstat lstat
#endif

int
bfpilot_path_is_safe(const char *path) {
  if(!path || !*path) return 0;
  if(path[0] != '/') return 0;
  if(strstr(path, "..")) return 0;
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
        if(cur[1] == 0)
          snprintf(next, sizeof(next), "/%s", ent->d_name);
        else
          snprintf(next, sizeof(next), "%s/%s", cur, ent->d_name);
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
