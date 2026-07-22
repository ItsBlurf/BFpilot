/*
 * BFpilot - direct file and directory serving.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "fs.h"
#include "mime.h"
#include "websrv.h"


/* Download path buffer — larger sequential reads reduce syscall/PFS overhead */
#ifndef BFPILOT_STREAM_BUF_SIZE
#define BFPILOT_STREAM_BUF_SIZE (2 * 1024 * 1024)
#endif
#define STREAM_BUF_SIZE BFPILOT_STREAM_BUF_SIZE
#define BFPILOT_IO_ALIGNMENT 0x4000U


static pthread_mutex_t g_download_lock = PTHREAD_MUTEX_INITIALIZER;
static bfpilot_download_stats_t g_download_stats;


static long
monotonic_ms(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}


static void *
alloc_io_buffer(size_t size, int *aligned_out) {
  void *buffer = NULL;
  if(aligned_out) *aligned_out = 0;
  if(posix_memalign(&buffer, BFPILOT_IO_ALIGNMENT, size) == 0) {
    if(aligned_out) *aligned_out = 1;
    return buffer;
  }
  return malloc(size);
}


void
bfpilot_fs_get_download_stats(bfpilot_download_stats_t *out) {
  if(!out) return;
  pthread_mutex_lock(&g_download_lock);
  *out = g_download_stats;
  pthread_mutex_unlock(&g_download_lock);
}


static void
record_download(const char *path, const struct stat *st, size_t bytes,
                long elapsed_ms, long read_ms, long send_ms,
                int buffer_aligned, int error_code) {
  pthread_mutex_lock(&g_download_lock);
  g_download_stats.elapsed_ms = elapsed_ms;
  g_download_stats.read_ms = read_ms;
  g_download_stats.send_ms = send_ms;
  g_download_stats.bytes = (unsigned long)bytes;
  g_download_stats.source_dev = st ? (unsigned long)st->st_dev : 0;
  g_download_stats.buffer_size = (unsigned int)STREAM_BUF_SIZE;
  g_download_stats.buffer_aligned = buffer_aligned;
  g_download_stats.error_code = error_code;
  snprintf(g_download_stats.path, sizeof(g_download_stats.path), "%s",
           path ? path : "");
  pthread_mutex_unlock(&g_download_lock);
}


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

  if(buf_grow(&b, 64U * 1024U) != 0 || buf_append(&b, "[") != 0) goto fail;
  while((entry = readdir(dir))) {
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

    struct stat st;
    if(fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      continue;
    }

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

  if(fstat(dirfd(dir), &st) != 0) {
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

  if(!S_ISREG(st.st_mode) || st.st_size < 0 ||
     (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
    close(fd);
    return websrv_send_error_json(req->fd, 404, "not a regular file");
  }

  size_t file_size = (size_t)st.st_size;
  off_t offset = 0;
  size_t content_size = file_size;

  /* Support ?tail=N to serve only the last N bytes of the file (for log viewing) */
  char tail_str[32];
  if(websrv_get_query_arg(req, "tail", tail_str, sizeof(tail_str))) {
    size_t tail_bytes = (size_t)strtoull(tail_str, NULL, 10);
    if(tail_bytes > 0 && tail_bytes < file_size) {
      offset = (off_t)(file_size - tail_bytes);
      content_size = tail_bytes;
    }
  }

  /* Seek to offset if tailing */
  if(offset > 0 && lseek(fd, offset, SEEK_SET) < 0) {
    close(fd);
    return websrv_send_error_json(req->fd, 500, strerror(errno));
  }

  const char *mime = mime_get_type(path);
  if(websrv_send_headers(req->fd, 200, mime ? mime : "application/octet-stream",
                         content_size, NULL) != 0) {
    close(fd);
    return -1;
  }

  int buffer_aligned = 0;
  char *buf = alloc_io_buffer(STREAM_BUF_SIZE, &buffer_aligned);
  if(!buf) {
    close(fd);
    return -1;
  }


#ifdef POSIX_FADV_SEQUENTIAL
  (void)posix_fadvise(fd, offset, (off_t)content_size,
                      POSIX_FADV_SEQUENTIAL);
#endif

  int rc = 0;
  int saved_errno = 0;
  size_t sent_bytes = 0;
  size_t remaining = content_size;
  long started_ms = monotonic_ms();
  long read_ms = 0;
  long send_ms = 0;
  while(remaining > 0) {
    size_t to_read = remaining < STREAM_BUF_SIZE ? remaining : STREAM_BUF_SIZE;
    long read_started = monotonic_ms();
    ssize_t n = read(fd, buf, to_read);
    read_ms += monotonic_ms() - read_started;
    if(n < 0) {
      if(errno == EINTR) continue;
      saved_errno = errno;
      rc = -1;
      break;
    }
    if(n == 0) {
      saved_errno = EIO;
      errno = saved_errno;
      rc = -1;
      break;
    }
    long send_started = monotonic_ms();
    if(websrv_write_all(req->fd, buf, (size_t)n) != 0) {
      saved_errno = errno ? errno : EPIPE;
      rc = -1;
      break;
    }
    send_ms += monotonic_ms() - send_started;
    remaining -= (size_t)n;
    sent_bytes += (size_t)n;
  }

  free(buf);
  if(close(fd) != 0 && rc == 0) {
    saved_errno = errno;
    rc = -1;
  }
  long ended_ms = monotonic_ms();
  long elapsed_ms = ended_ms > started_ms ? ended_ms - started_ms : 0;
  record_download(path, &st, sent_bytes, elapsed_ms, read_ms, send_ms,
                  buffer_aligned,
                  rc == 0 ? 0 : (saved_errno ? saved_errno : EIO));
  if(rc != 0) errno = saved_errno ? saved_errno : EIO;
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
