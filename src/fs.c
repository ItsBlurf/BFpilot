/*
 * BFpilot - direct file and directory serving.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "fs.h"
#include "mime.h"
#include "websrv.h"


#define STREAM_BUF_SIZE (64 * 1024)


typedef struct dynbuf {
  char  *data;
  size_t len;
  size_t cap;
} dynbuf_t;


static int
buf_grow(dynbuf_t *b, size_t add) {
  if(b->len + add + 1 <= b->cap) return 0;
  size_t next = b->cap ? b->cap : 4096;
  while(next < b->len + add + 1) next *= 2;
  char *p = realloc(b->data, next);
  if(!p) return -1;
  b->data = p;
  b->cap = next;
  return 0;
}


static int
buf_append(dynbuf_t *b, const char *s) {
  size_t n = strlen(s);
  if(buf_grow(b, n) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return 0;
}


static int
buf_appendf(dynbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if(n < 0) {
    va_end(ap);
    return -1;
  }
  if(buf_grow(b, (size_t)n) != 0) {
    va_end(ap);
    return -1;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 0;
}


static int
buf_json_string(dynbuf_t *b, const char *s) {
  if(buf_append(b, "\"") != 0) return -1;
  for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch(*p) {
    case '\\': if(buf_append(b, "\\\\") != 0) return -1; break;
    case '"':  if(buf_append(b, "\\\"") != 0) return -1; break;
    case '\n': if(buf_append(b, "\\n") != 0) return -1; break;
    case '\r': if(buf_append(b, "\\r") != 0) return -1; break;
    case '\t': if(buf_append(b, "\\t") != 0) return -1; break;
    default:
      if(*p < 0x20) {
        if(buf_appendf(b, "\\u%04x", *p) != 0) return -1;
      } else {
        if(buf_grow(b, 1) != 0) return -1;
        b->data[b->len++] = (char)*p;
        b->data[b->len] = 0;
      }
      break;
    }
  }
  return buf_append(b, "\"");
}


static int
buf_html(dynbuf_t *b, const char *s) {
  for(const char *p = s ? s : ""; *p; p++) {
    switch(*p) {
    case '&': if(buf_append(b, "&amp;") != 0) return -1; break;
    case '<': if(buf_append(b, "&lt;") != 0) return -1; break;
    case '>': if(buf_append(b, "&gt;") != 0) return -1; break;
    case '"': if(buf_append(b, "&quot;") != 0) return -1; break;
    default:
      if(buf_grow(b, 1) != 0) return -1;
      b->data[b->len++] = *p;
      b->data[b->len] = 0;
      break;
    }
  }
  return 0;
}


static char
mode_char(const struct stat *st, dev_t parent_dev) {
  if(S_ISDIR(st->st_mode) && st->st_dev != parent_dev) return 'm';
  if(S_ISDIR(st->st_mode)) return 'd';
  if(S_ISBLK(st->st_mode)) return 'b';
  if(S_ISCHR(st->st_mode)) return 'c';
  if(S_ISLNK(st->st_mode)) return 'l';
  if(S_ISFIFO(st->st_mode)) return 'p';
  if(S_ISSOCK(st->st_mode)) return 's';
  return '-';
}


static int
path_is_safe(const char *path) {
  return path && path[0] == '/' && !strstr(path, "..");
}


static void
normalize_path(char *path) {
  char *src = path;
  char *dst = path;
  while(*src) {
    *dst = *src;
    if(*src == '/') {
      while(src[1] == '/') src++;
    }
    dst++;
    src++;
  }
  *dst = 0;

  size_t len = strlen(path);
  if(len > 1 && path[len - 1] == '/') path[len - 1] = 0;
}


static int
dir_json_request(const http_request_t *req, const char *path, DIR *dir,
                 dev_t parent_dev) {
  dynbuf_t b = {0};
  struct dirent *entry;
  int first = 1;

  if(buf_append(&b, "[") != 0) goto fail;
  while((entry = readdir(dir))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

    struct stat st;
    if(fstatat(dirfd(dir), entry->d_name, &st, 0) != 0) continue;

    if(!first && buf_append(&b, ",") != 0) goto fail;
    first = 0;
    if(buf_append(&b, "{\"name\":") != 0) goto fail;
    if(buf_json_string(&b, entry->d_name) != 0) goto fail;
    if(buf_appendf(&b, ",\"mode\":\"%c\",\"mtime\":%ld,\"size\":%ld}",
                   mode_char(&st, parent_dev), (long)st.st_mtime,
                   (long)st.st_size) != 0) goto fail;
  }
  if(buf_append(&b, "]") != 0) goto fail;

  int rc = websrv_send(req->fd, 200, "application/json", b.data, b.len);
  free(b.data);
  return rc;

fail:
  free(b.data);
  return websrv_send_error_json(req->fd, 500, "out of memory");
}


static int
dir_html_request(const http_request_t *req, const char *path, DIR *dir) {
  dynbuf_t b = {0};
  struct dirent *entry;

  if(buf_append(&b, "<!doctype html><html><head><title>Index of ") != 0) goto fail;
  if(buf_html(&b, path) != 0) goto fail;
  if(buf_append(&b, "</title></head><body><h1>Index of ") != 0) goto fail;
  if(buf_html(&b, path) != 0) goto fail;
  if(buf_append(&b, "</h1><ul>") != 0) goto fail;

  while((entry = readdir(dir))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    if(buf_append(&b, "<li><a href=\"/fs") != 0) goto fail;
    if(buf_html(&b, path) != 0) goto fail;
    if(strcmp(path, "/") && buf_append(&b, "/") != 0) goto fail;
    if(buf_html(&b, entry->d_name) != 0) goto fail;
    if(buf_append(&b, "\">") != 0) goto fail;
    if(buf_html(&b, entry->d_name) != 0) goto fail;
    if(buf_append(&b, "</a></li>") != 0) goto fail;
  }
  if(buf_append(&b, "</ul></body></html>") != 0) goto fail;

  int rc = websrv_send(req->fd, 200, "text/html", b.data, b.len);
  free(b.data);
  return rc;

fail:
  free(b.data);
  return websrv_send_error_json(req->fd, 500, "out of memory");
}


static int
dir_request(const http_request_t *req, const char *path) {
  char fmt[32];
  struct stat st;

  DIR *dir = opendir(path);
  if(!dir) return websrv_send_error_json(req->fd, 404, strerror(errno));

  if(stat(path, &st) != 0) {
    closedir(dir);
    return websrv_send_error_json(req->fd, 404, strerror(errno));
  }

  int json = websrv_get_query_arg(req, "fmt", fmt, sizeof(fmt)) &&
             !strcmp(fmt, "json");
  int rc = json ? dir_json_request(req, path, dir, st.st_dev)
                : dir_html_request(req, path, dir);
  closedir(dir);
  return rc;
}


static int
file_request(const http_request_t *req, const char *path) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return websrv_send_error_json(req->fd, 404, strerror(errno));

  if(fstat(fd, &st) != 0) {
    close(fd);
    return websrv_send_error_json(req->fd, 404, strerror(errno));
  }

  const char *mime = mime_get_type(path);
  if(websrv_send_headers(req->fd, 200, mime ? mime : "application/octet-stream",
                         (size_t)st.st_size, NULL) != 0) {
    close(fd);
    return -1;
  }

  int rc = 0;
  off_t offset = 0;
  size_t remaining = (size_t)st.st_size;

  while(remaining > 0) {
    off_t sent = 0;
    if(sendfile(fd, req->fd, offset, remaining, NULL, &sent, 0) < 0) {
      if(errno == EINTR || errno == EAGAIN) {
        if(sent > 0) {
          offset += sent;
          remaining -= sent;
        }
        continue;
      }
      rc = -1;
      break;
    }
    if(sent == 0) break;
    offset += sent;
    remaining -= sent;
  }
  close(fd);
  return rc;
}


int
fs_request(const http_request_t *req, const char *url) {
  char path[PATH_MAX];
  struct stat st;

  if(!strcmp(url, "/fs")) {
    snprintf(path, sizeof(path), "/");
  } else {
    snprintf(path, sizeof(path), "%s", url + 3);
  }
  normalize_path(path);

  if(!path_is_safe(path)) {
    return websrv_send_error_json(req->fd, 400, "unsafe path");
  }

  if(stat(path, &st) != 0) {
    return websrv_send_error_json(req->fd, 404, strerror(errno));
  }

  if(S_ISDIR(st.st_mode)) return dir_request(req, path);
  return file_request(req, path);
}


uint8_t *
fs_readfile(const char *path, size_t *size) {
  uint8_t *buf = NULL;
  FILE *file = fopen(path, "rb");
  if(!file) return NULL;

  if(fseek(file, 0, SEEK_END) != 0) goto fail;
  long len = ftell(file);
  if(len < 0) goto fail;
  if(fseek(file, 0, SEEK_SET) != 0) goto fail;

  buf = malloc((size_t)len);
  if(!buf) goto fail;

  if(fread(buf, 1, (size_t)len, file) != (size_t)len) goto fail;
  fclose(file);
  if(size) *size = (size_t)len;
  return buf;

fail:
  free(buf);
  fclose(file);
  return NULL;
}
