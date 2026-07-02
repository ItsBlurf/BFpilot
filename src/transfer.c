/*
 * BFpilot - file-manager copy/move/delete primitives and API.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "diag.h"
#include "search.h"
#include "transfer.h"
#include "websrv.h"


#ifndef BFPILOT_TRANSFER_BUF_SIZE
#define BFPILOT_TRANSFER_BUF_SIZE (1024 * 1024)
#endif

#define COPY_BUF_SIZE   BFPILOT_TRANSFER_BUF_SIZE
#define UPLOAD_BUF_SIZE BFPILOT_TRANSFER_BUF_SIZE
#define UPLOAD_PREALLOC_MIN_SIZE (64UL * 1024UL * 1024UL)
#define BFPILOT_SPACE_MARGIN_BYTES (256ULL * 1024ULL * 1024ULL)
#define BFPILOT_DATA_ROOT "/data"
#define BFPILOT_DATA_DIR "/data/BFpilot"
#define BFPILOT_SHORTCUTS_PATH "/data/BFpilot/shortcuts.txt"
#define BFPILOT_SHORTCUTS_TMP "/data/BFpilot/shortcuts.tmp"
#define BFPILOT_MAX_SHORTCUTS 32

#ifndef BFPILOT_ENABLE_INTEGRATED_ARCHIVE
#define BFPILOT_ENABLE_INTEGRATED_ARCHIVE 0
#endif

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
#define BFPILOT_ARCHIVE_DIR "/data/BFpilot/archive-integrated"
#else
#define BFPILOT_ARCHIVE_DIR "/data/BFpilot/archive"
#endif
#define BFPILOT_ARCHIVE_JOB BFPILOT_ARCHIVE_DIR "/job.ini"
#define BFPILOT_ARCHIVE_JOB_TMP BFPILOT_ARCHIVE_DIR "/job.tmp"
#define BFPILOT_ARCHIVE_STATUS BFPILOT_ARCHIVE_DIR "/status.json"
#define BFPILOT_ARCHIVE_STATUS_TMP BFPILOT_ARCHIVE_DIR "/status.tmp"
#define BFPILOT_ARCHIVE_MAX_THREADS 8U
#define BFPILOT_ARCHIVE_DEFAULT_NICE 4
#define BFPILOT_ARCHIVE_MIN_NICE -20
#define BFPILOT_ARCHIVE_MAX_NICE 20

extern int posix_fallocate(int fd, off_t offset, off_t len);

typedef struct json_buf {
  char  *data;
  size_t len;
  size_t cap;
} json_buf_t;


static int
json_grow(json_buf_t *b, size_t add) {
  if(b->len + add + 1 <= b->cap) return 0;
  size_t next = b->cap ? b->cap : 1024;
  while(next < b->len + add + 1) next *= 2;
  char *p = realloc(b->data, next);
  if(!p) return -1;
  b->data = p;
  b->cap = next;
  return 0;
}


static int
json_append(json_buf_t *b, const char *s) {
  size_t n = strlen(s);
  if(json_grow(b, n) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return 0;
}


static int
json_appendf(json_buf_t *b, const char *fmt, ...) {
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
  if(json_grow(b, (size_t)n) != 0) {
    va_end(ap);
    return -1;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 0;
}


static int
json_string(json_buf_t *b, const char *s) {
  if(json_append(b, "\"") != 0) return -1;
  for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch(*p) {
    case '\\': if(json_append(b, "\\\\") != 0) return -1; break;
    case '"':  if(json_append(b, "\\\"") != 0) return -1; break;
    case '\b': if(json_append(b, "\\b") != 0) return -1; break;
    case '\f': if(json_append(b, "\\f") != 0) return -1; break;
    case '\n': if(json_append(b, "\\n") != 0) return -1; break;
    case '\r': if(json_append(b, "\\r") != 0) return -1; break;
    case '\t': if(json_append(b, "\\t") != 0) return -1; break;
    default:
      if(*p < 0x20) {
        if(json_appendf(b, "\\u%04x", *p) != 0) return -1;
      } else {
        if(json_grow(b, 1) != 0) return -1;
        b->data[b->len++] = (char)*p;
        b->data[b->len] = 0;
      }
      break;
    }
  }
  return json_append(b, "\"");
}


static int
serve_owned(const http_request_t *req, int status, char *data, size_t size) {
  int rc = websrv_send(req->fd, status, "application/json", data, size);
  free(data);
  return rc;
}


static int
serve_error(const http_request_t *req, int status, const char *msg) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":false,\"error\":") != 0 ||
     json_string(&b, msg ? msg : "error") != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, status, b.data, b.len);
}


static int write_all_fd(int fd, const void *data, size_t size);
static void drain_body(int fd, size_t already_read, size_t content_size);


static int
path_is_safe(const char *p) {
  if(!p || !*p) return 0;
  if(p[0] != '/') return 0;
  if(strstr(p, "..")) return 0;
  return 1;
}


static int
mkdirs(const char *path) {
  char buf[1024];
  size_t n = strlen(path);
  if(n >= sizeof(buf)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(buf, path, n + 1);
  for(size_t i = 1; i <= n; i++) {
    if(buf[i] == '/' || buf[i] == 0) {
      char saved = buf[i];
      buf[i] = 0;
      if(mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
      buf[i] = saved;
    }
  }
  return 0;
}


static void
ensure_bfpilot_dir(void) {
  (void)mkdir(BFPILOT_DATA_ROOT, 0755);
  (void)mkdir(BFPILOT_DATA_DIR, 0755);
}


static void
join_path(char *out, size_t out_sz, const char *dir, const char *name) {
  size_t n = strlen(dir);
  snprintf(out, out_sz, "%s%s%s", dir, (n > 1 && dir[n - 1] != '/') ? "/" : "",
           name);
}


static const char *
path_basename(const char *path) {
  const char *base = strrchr(path ? path : "", '/');
  return base && base[1] ? base + 1 : path;
}


static const char *
safe_basename_label(const char *path) {
  const char *base = path_basename(path);
  return base && base[0] ? base : path;
}


static int
path_is_same_or_child(const char *base, const char *path) {
  size_t n = strlen(base);
  while(n > 1 && base[n - 1] == '/') n--;
  return strncmp(base, path, n) == 0 && (path[n] == 0 || path[n] == '/');
}


static int
path_has_dotdot_segment(const char *path) {
  const char *p = path ? path : "";
  while(*p) {
    while(*p == '/') p++;
    const char *start = p;
    while(*p && *p != '/') p++;
    if((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.') {
      return 1;
    }
  }
  return 0;
}


static int
path_starts_with_root(const char *path, const char *root) {
  size_t n = strlen(root);
  return strncmp(path, root, n) == 0 && (path[n] == 0 || path[n] == '/');
}


static int
archive_path_allowed(const char *path) {
  if(!path_is_safe(path) || path_has_dotdot_segment(path)) return 0;
  if(path_starts_with_root(path, "/data")) return 1;
  for(int i = 0; i < 8; i++) {
    char root[32];
    snprintf(root, sizeof(root), "/mnt/usb%d", i);
    if(path_starts_with_root(path, root)) return 1;
    snprintf(root, sizeof(root), "/mnt/ext%d", i);
    if(path_starts_with_root(path, root)) return 1;
  }
  return 0;
}


static int
config_value_safe(const char *value, int allow_empty) {
  if(!value || (!allow_empty && !*value)) return 0;
  for(const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if(*p < 0x20 || *p == 0x7f) return 0;
  }
  return 1;
}


struct job_state {
  pthread_mutex_t lock;
  atomic_int      busy;
  atomic_int      cancel;
  atomic_long     total_bytes;
  atomic_long     copied_bytes;
  atomic_int      total_files;
  atomic_int      done_files;
  atomic_int      failed_files;
  atomic_long     elapsed_ms;
  atomic_long     read_bytes;
  atomic_long     written_bytes;
  atomic_int      last_errno;
  char            current[512];
  char            source[512];
  char            destination[512];
  char            verb[16];
  char            error[256];
  unsigned long   source_dev;
  unsigned long   destination_dev;
  time_t          started_at;
  time_t          ended_at;
  long            started_ms;
};


static struct job_state g_job = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

struct upload_state {
  pthread_mutex_t lock;
  long            elapsed_ms;
  unsigned long   bytes;
  unsigned long   destination_dev;
  int             error_code;
  char            path[512];
};

static struct upload_state g_upload = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

static atomic_int g_archive_busy = 0;


int
transfer_archive_busy(void) {
  return atomic_load(&g_archive_busy) != 0;
}


static long
monotonic_ms(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void
job_set_current(const char *path) {
  pthread_mutex_lock(&g_job.lock);
  snprintf(g_job.current, sizeof(g_job.current), "%s", path ? path : "");
  pthread_mutex_unlock(&g_job.lock);
}


static void
job_set_error(const char *fmt, ...) {
  pthread_mutex_lock(&g_job.lock);
  if(!g_job.error[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_job.error, sizeof(g_job.error), fmt, ap);
    va_end(ap);
  }
  pthread_mutex_unlock(&g_job.lock);
}


static int
job_cancelled(void) {
  return atomic_load(&g_job.cancel);
}


static int
job_begin(const char *verb) {
  int expected = 0;
  if(!atomic_compare_exchange_strong(&g_job.busy, &expected, 1)) return 0;
  pthread_mutex_lock(&g_job.lock);
  atomic_store(&g_job.cancel, 0);
  atomic_store(&g_job.total_bytes, 0);
  atomic_store(&g_job.copied_bytes, 0);
  atomic_store(&g_job.total_files, 0);
  atomic_store(&g_job.done_files, 0);
  atomic_store(&g_job.failed_files, 0);
  atomic_store(&g_job.elapsed_ms, 0);
  atomic_store(&g_job.read_bytes, 0);
  atomic_store(&g_job.written_bytes, 0);
  atomic_store(&g_job.last_errno, 0);
  g_job.current[0] = 0;
  g_job.source[0] = 0;
  g_job.destination[0] = 0;
  g_job.error[0] = 0;
  g_job.source_dev = 0;
  g_job.destination_dev = 0;
  snprintf(g_job.verb, sizeof(g_job.verb), "%s", verb);
  g_job.started_at = time(NULL);
  g_job.ended_at = 0;
  g_job.started_ms = monotonic_ms();
  pthread_mutex_unlock(&g_job.lock);
  return 1;
}


static void
job_end(int rc, const char *err) {
  int saved_errno = errno;
  pthread_mutex_lock(&g_job.lock);
  g_job.ended_at = time(NULL);
  long ended_ms = monotonic_ms();
  atomic_store(&g_job.elapsed_ms,
               ended_ms > g_job.started_ms ? ended_ms - g_job.started_ms : 0);
  atomic_store(&g_job.last_errno, rc == 0 ? 0 : saved_errno);
  if(rc != 0 && err && !g_job.error[0]) {
    snprintf(g_job.error, sizeof(g_job.error), "%s", err);
  }
  pthread_mutex_unlock(&g_job.lock);
  long elapsed_ms = atomic_load(&g_job.elapsed_ms);
  long written = atomic_load(&g_job.written_bytes);
  double mbps = elapsed_ms > 0 ?
      ((double)written * 1000.0 / (double)elapsed_ms) / (1024.0 * 1024.0) : 0.0;
  bfpilot_log("transfer job end verb=%s rc=%d errno=%d bytes_read=%ld "
              "bytes_written=%ld elapsed_ms=%ld average_mbps=%.2f "
              "src_dev=%lu dst_dev=%lu current=%s error=%s",
              g_job.verb, rc, atomic_load(&g_job.last_errno),
              atomic_load(&g_job.read_bytes), written, elapsed_ms, mbps,
              g_job.source_dev, g_job.destination_dev, g_job.current,
              err ? err : "");
  atomic_store(&g_job.busy, 0);
}


static void
size_walker(const char *path, long *items, long *bytes, int count_dirs) {
  if(job_cancelled()) return;

  struct stat st;
  if(lstat(path, &st) != 0) return;
  if(S_ISDIR(st.st_mode)) {
    if(count_dirs) (*items)++;
    if(count_dirs && st.st_size > 0) *bytes += (long)st.st_size;
    DIR *d = opendir(path);
    if(!d) return;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) continue;
      join_path(child, sizeof(child), path, ent->d_name);
      size_walker(child, items, bytes, count_dirs);
      if(job_cancelled()) break;
    }
    closedir(d);
  } else if(S_ISREG(st.st_mode)) {
    (*items)++;
    *bytes += (long)st.st_size;
  } else if(count_dirs) {
    (*items)++;
    if(st.st_size > 0) *bytes += (long)st.st_size;
  }
}


static int
copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if(in < 0) {
    job_set_error("open(%s): %s", src, strerror(errno));
    return -1;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0777);
  if(out < 0) {
    job_set_error("open(%s): %s", dst, strerror(errno));
    close(in);
    return -1;
  }

  void *buf = malloc(COPY_BUF_SIZE);
  if(!buf) {
    close(in);
    close(out);
    unlink(dst);
    return -1;
  }

  int rc = 0;
  job_set_current(src);
  while(!job_cancelled()) {
    ssize_t r = read(in, buf, COPY_BUF_SIZE);
    if(r < 0) {
      if(errno == EINTR) continue;
      job_set_error("read(%s): %s", src, strerror(errno));
      rc = -1;
      break;
    }
    if(r == 0) break;
    atomic_fetch_add(&g_job.read_bytes, (long)r);
    char *p = buf;
    ssize_t left = r;
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w < 0) {
        if(errno == EINTR) continue;
        job_set_error("write(%s): %s", dst, strerror(errno));
        rc = -1;
        break;
      }
      if(w == 0) {
        errno = EIO;
        rc = -1;
        break;
      }
      p += w;
      left -= w;
      atomic_fetch_add(&g_job.copied_bytes, (long)w);
      atomic_fetch_add(&g_job.written_bytes, (long)w);
    }
    if(rc != 0) break;
  }

  if(job_cancelled()) {
    errno = ECANCELED;
    rc = -1;
  }

  free(buf);
  close(in);
  if(close(out) != 0 && rc == 0) {
    job_set_error("close(%s): %s", dst, strerror(errno));
    rc = -1;
  }
  if(rc != 0) unlink(dst);
  return rc;
}


static int
copy_recursive(const char *src, const char *dst) {
  if(job_cancelled()) {
    errno = ECANCELED;
    return -1;
  }
  struct stat st;
  if(lstat(src, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    if(mkdir(dst, 0777) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char s2[1024], d2[1024];
      join_path(s2, sizeof(s2), src, ent->d_name);
      join_path(d2, sizeof(d2), dst, ent->d_name);
      if(copy_recursive(s2, d2) != 0) {
        rc = -1;
        atomic_fetch_add(&g_job.failed_files, 1);
        if(job_cancelled()) break;
      }
    }
    closedir(d);
    return rc;
  }

  if(S_ISREG(st.st_mode)) {
    int rc = copy_file(src, dst);
    if(rc == 0) atomic_fetch_add(&g_job.done_files, 1);
    return rc;
  }

  return 0;
}


static int
delete_recursive(const char *path, int count_progress) {
  if(job_cancelled()) {
    errno = ECANCELED;
    return -1;
  }
  struct stat st;
  if(lstat(path, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      join_path(child, sizeof(child), path, ent->d_name);
      if(delete_recursive(child, count_progress) != 0) {
        rc = -1;
        atomic_fetch_add(&g_job.failed_files, 1);
        if(job_cancelled()) break;
      }
    }
    closedir(d);
    if(rc == 0) {
      job_set_current(path);
      if(rmdir(path) != 0) return -1;
      if(count_progress) {
        if(st.st_size > 0) {
          atomic_fetch_add(&g_job.copied_bytes, (long)st.st_size);
        }
        atomic_fetch_add(&g_job.done_files, 1);
      }
    }
    return rc;
  }

  job_set_current(path);
  if(unlink(path) != 0) return -1;
  if(count_progress) {
    if(st.st_size > 0) {
      atomic_fetch_add(&g_job.copied_bytes, (long)st.st_size);
    }
    atomic_fetch_add(&g_job.done_files, 1);
  }
  return 0;
}


static int
delete_recursive_force(const char *path) {
  struct stat st;
  if(lstat(path, &st) != 0) {
    return errno == ENOENT ? 0 : -1;
  }

  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
        errno = ENAMETOOLONG;
        rc = -1;
        break;
      }
      join_path(child, sizeof(child), path, ent->d_name);
      if(delete_recursive_force(child) != 0) {
        rc = -1;
        break;
      }
    }
    closedir(d);
    if(rc != 0) return -1;
    return rmdir(path);
  }

  return unlink(path);
}


struct copy_arg {
  char src[1024];
  char dst[1024];
  int  is_move;
};


static void *
copy_worker(void *arg) {
  struct copy_arg *a = arg;
  long files = 0, bytes = 0;
  struct stat src_st, dst_st, final_st;
  char final_dst[1024];
  char staging_dst[1024];

  bfpilot_log("transfer worker start verb=%s src=%s dst=%s",
              a->is_move ? "move" : "copy", a->src, a->dst);
  if(lstat(a->src, &src_st) != 0) {
    job_end(-1, "source not found");
    free(a);
    return NULL;
  }
  pthread_mutex_lock(&g_job.lock);
  snprintf(g_job.source, sizeof(g_job.source), "%s", a->src);
  snprintf(g_job.destination, sizeof(g_job.destination), "%s", a->dst);
  g_job.source_dev = (unsigned long)src_st.st_dev;
  pthread_mutex_unlock(&g_job.lock);

  job_set_current("Scanning source");
  size_walker(a->src, &files, &bytes, 0);
  if(job_cancelled()) {
    job_end(-1, "cancelled");
    free(a);
    return NULL;
  }
  atomic_store(&g_job.total_files, (int)files);
  atomic_store(&g_job.total_bytes, bytes);

  if(stat(a->dst, &dst_st) == 0) {
    if(!S_ISDIR(dst_st.st_mode)) {
      job_end(-1, "target exists and is not a folder");
      free(a);
      return NULL;
    }
  } else {
    if(errno != ENOENT) {
      char err[160];
      snprintf(err, sizeof(err), "target: %s", strerror(errno));
      job_end(-1, err);
      free(a);
      return NULL;
    }
    if(mkdirs(a->dst) != 0) {
      char err[160];
      snprintf(err, sizeof(err), "mkdir target: %s", strerror(errno));
      job_end(-1, err);
      free(a);
      return NULL;
    }
  }
  if(stat(a->dst, &dst_st) == 0) {
    pthread_mutex_lock(&g_job.lock);
    g_job.destination_dev = (unsigned long)dst_st.st_dev;
    pthread_mutex_unlock(&g_job.lock);
  }

  const char *base = path_basename(a->src);
  if(strlen(a->dst) + strlen(base) + 2 >= sizeof(final_dst)) {
    job_end(-1, "target path too long");
    free(a);
    return NULL;
  }
  join_path(final_dst, sizeof(final_dst), a->dst, base);

  if(lstat(final_dst, &final_st) == 0) {
    job_end(-1, "destination already exists");
    free(a);
    return NULL;
  }
  if(errno != ENOENT) {
    char err[160];
    snprintf(err, sizeof(err), "destination: %s", strerror(errno));
    job_end(-1, err);
    free(a);
    return NULL;
  }

  int n = snprintf(staging_dst, sizeof(staging_dst), "%s.bfpilot-part-%ld-%ld",
                   final_dst, (long)getpid(), (long)time(NULL));
  if(n < 0 || (size_t)n >= sizeof(staging_dst)) {
    job_end(-1, "staging path too long");
    free(a);
    return NULL;
  }

  if(!strcmp(a->src, final_dst)) {
    job_end(-1, "source and destination are the same");
    free(a);
    return NULL;
  }
  if(S_ISDIR(src_st.st_mode) &&
     path_is_same_or_child(a->src, final_dst)) {
    job_end(-1, "refusing to place a folder inside itself");
    free(a);
    return NULL;
  }

  if(a->is_move) {
    job_set_current(a->src);
    if(job_cancelled()) {
      job_end(-1, "cancelled");
      free(a);
      return NULL;
    }
    if(rename(a->src, final_dst) == 0) {
      atomic_store(&g_job.done_files, (int)(files > 0 ? files : 1));
      atomic_store(&g_job.copied_bytes, bytes);
      bfpilot_search_mark_stale(a->is_move ? "move" : "copy");
      job_end(0, NULL);
      free(a);
      return NULL;
    }
    if(errno != EXDEV) {
      char err[160];
      snprintf(err, sizeof(err), "rename: %s", strerror(errno));
      job_end(-1, err);
      free(a);
      return NULL;
    }
    bfpilot_log("transfer move cross-device fallback src=%s staging=%s final=%s",
                a->src, staging_dst, final_dst);
  }

  int rc = copy_recursive(a->src, staging_dst);
  if(rc != 0) {
    char err[160];
    snprintf(err, sizeof(err), "copy: %s",
             job_cancelled() ? "cancelled" : strerror(errno));
    (void)delete_recursive_force(staging_dst);
    job_end(-1, err);
    free(a);
    return NULL;
  }
  if(job_cancelled()) {
    (void)delete_recursive_force(staging_dst);
    job_end(-1, "cancelled");
    free(a);
    return NULL;
  }
  job_set_current("Finalizing destination");
  if(rename(staging_dst, final_dst) != 0) {
    char err[160];
    snprintf(err, sizeof(err), "finalize: %s", strerror(errno));
    (void)delete_recursive_force(staging_dst);
    job_end(-1, err);
    free(a);
    return NULL;
  }
  bfpilot_log("transfer finalized staging=%s final=%s", staging_dst, final_dst);
  if(a->is_move && delete_recursive(a->src, 0) != 0) {
    char err[160];
    snprintf(err, sizeof(err), "post-move cleanup: %s",
             job_cancelled() ? "cancelled" : strerror(errno));
    job_end(-1, err);
    free(a);
    return NULL;
  }
  if(a->is_move) {
    bfpilot_log("transfer source cleanup complete src=%s", a->src);
  }

  bfpilot_search_mark_stale(a->is_move ? "move" : "copy");
  job_end(0, NULL);
  free(a);
  return NULL;
}


struct delete_arg {
  char path[1024];
};


static void *
delete_worker(void *arg) {
  struct delete_arg *a = arg;
  long items = 0, bytes = 0;
  job_set_current("Scanning folder");
  size_walker(a->path, &items, &bytes, 1);
  if(job_cancelled()) {
    job_end(-1, "cancelled");
    free(a);
    return NULL;
  }
  atomic_store(&g_job.total_files, (int)(items > 0 ? items : 1));
  atomic_store(&g_job.total_bytes, bytes);

  if(delete_recursive(a->path, 1) != 0) {
    char err[160];
    snprintf(err, sizeof(err), "delete: %s",
             job_cancelled() ? "cancelled" : strerror(errno));
    job_end(-1, err);
    free(a);
    return NULL;
  }
  bfpilot_search_mark_stale("delete");
  job_end(0, NULL);
  free(a);
  return NULL;
}


static int
list_request(const http_request_t *req) {
  char path[1024];
  char fast[32];
  int fast_mode = websrv_get_query_arg(req, "fast", fast, sizeof(fast)) &&
                  strcmp(fast, "0") != 0;
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }

  DIR *d = opendir(path);
  if(!d) return serve_error(req, 404, strerror(errno));

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b, ",\"fast\":%s,\"entries\":[",
                  fast_mode ? "true" : "false") != 0) {
    closedir(d);
    free(b.data);
    return -1;
  }

  int first = 1;
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    char full[1024];
    join_path(full, sizeof(full), path, ent->d_name);
    struct stat st;
    if(lstat(full, &st) != 0) continue;
    if(!first && json_append(&b, ",") != 0) break;
    first = 0;
    if(json_append(&b, "{\"name\":") != 0 ||
       json_string(&b, ent->d_name) != 0 ||
       json_appendf(&b, ",\"dir\":%s",
                    S_ISDIR(st.st_mode) ? "true" : "false") != 0) break;
    if(!fast_mode &&
       json_appendf(&b, ",\"size\":%ld,\"mtime\":%ld",
                    (long)st.st_size, (long)st.st_mtime) != 0) break;
    if(json_append(&b, "}") != 0) break;
  }
  closedir(d);
  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


typedef struct fs_total_info {
  int ok;
  unsigned long long total;
  unsigned long long free_bytes;
  unsigned long long available;
  unsigned long long reserved;
  unsigned long long used;
  unsigned long long usable_total;
  unsigned long long usable_used;
  double used_pct;
  double usable_used_pct;
} fs_total_info_t;

static void
fs_totals(const char *path, fs_total_info_t *info);


static int
stat_request(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }
  struct stat st;
  if(lstat(path, &st) != 0) return serve_error(req, 404, strerror(errno));

  json_buf_t b = {0};
  fs_total_info_t fs = {0};
  fs_totals(path, &fs);
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b,
                  ",\"dir\":%s,\"size\":%ld,\"mtime\":%ld,\"dev\":%lu,"
                  "\"statvfs\":%s,\"totalBytes\":%llu,\"freeBytes\":%llu,"
                  "\"availableBytes\":%llu,\"reservedBytes\":%llu,"
                  "\"usedBytes\":%llu,\"usedPercent\":%.2f,"
                  "\"usableTotalBytes\":%llu,\"usableUsedBytes\":%llu,"
                  "\"usableUsedPercent\":%.2f}",
                  S_ISDIR(st.st_mode) ? "true" : "false",
                  (long)st.st_size, (long)st.st_mtime,
                  (unsigned long)st.st_dev, fs.ok ? "true" : "false",
                  fs.total, fs.free_bytes, fs.available, fs.reserved,
                  fs.used, fs.used_pct, fs.usable_total, fs.usable_used,
                  fs.usable_used_pct) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


typedef struct du_state {
  unsigned long long bytes;
  long               entries;
  long               files;
  long               dirs;
  dev_t              root_dev;
} du_state_t;


static void
du_walk(const char *path, du_state_t *du) {
  struct stat st;
  if(lstat(path, &st) != 0) return;

  du->entries++;
  if(st.st_size > 0) du->bytes += (unsigned long long)st.st_size;

  if(!S_ISDIR(st.st_mode)) {
    du->files++;
    return;
  }

  du->dirs++;
  if(st.st_dev != du->root_dev) return;

  DIR *dir = opendir(path);
  if(!dir) return;

  struct dirent *ent;
  while((ent = readdir(dir))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    char child[1024];
    if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
      continue;
    }
    join_path(child, sizeof(child), path, ent->d_name);
    du_walk(child, du);
  }

  closedir(dir);
}


static int
du_request(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }

  struct stat st;
  if(lstat(path, &st) != 0) return serve_error(req, 404, strerror(errno));

  du_state_t du = {0};
  du.root_dev = st.st_dev;
  du_walk(path, &du);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b,
                  ",\"size\":%llu,\"entries\":%ld,\"files\":%ld,"
                  "\"dirs\":%ld,\"truncated\":false}",
                  du.bytes, du.entries, du.files, du.dirs) != 0) {
    free(b.data);
    return -1;
  }

  return serve_owned(req, 200, b.data, b.len);
}


typedef struct shortcut_entry {
  char label[64];
  char path[512];
} shortcut_entry_t;


static void
sanitize_shortcut_label(char *out, size_t out_size, const char *label,
                        const char *path) {
  const char *src = label && label[0] ? label : safe_basename_label(path);
  size_t pos = 0;
  if(out_size == 0) return;
  for(const unsigned char *p = (const unsigned char *)src;
      *p && pos + 1 < out_size; p++) {
    if(*p < 0x20 || *p == '\t') {
      if(pos > 0 && out[pos - 1] != ' ') out[pos++] = ' ';
    } else {
      out[pos++] = (char)*p;
    }
  }
  while(pos > 0 && out[pos - 1] == ' ') pos--;
  out[pos] = 0;
  if(!out[0]) snprintf(out, out_size, "%s", safe_basename_label(path));
}


static int
read_shortcuts(shortcut_entry_t *items, int max_items) {
  FILE *file = fopen(BFPILOT_SHORTCUTS_PATH, "r");
  if(!file) return 0;

  int count = 0;
  char line[700];
  while(count < max_items && fgets(line, sizeof(line), file)) {
    char *nl = strpbrk(line, "\r\n");
    if(nl) *nl = 0;
    char *tab = strchr(line, '\t');
    if(!tab) continue;
    *tab++ = 0;
    if(!path_is_safe(tab)) continue;
    sanitize_shortcut_label(items[count].label, sizeof(items[count].label),
                            line, tab);
    snprintf(items[count].path, sizeof(items[count].path), "%s", tab);
    count++;
  }
  fclose(file);
  return count;
}


static int
write_shortcuts(const shortcut_entry_t *items, int count) {
  ensure_bfpilot_dir();
  FILE *file = fopen(BFPILOT_SHORTCUTS_TMP, "w");
  if(!file) return -1;
  for(int i = 0; i < count; i++) {
    if(fprintf(file, "%s\t%s\n", items[i].label, items[i].path) < 0) {
      fclose(file);
      return -1;
    }
  }
  if(fclose(file) != 0) return -1;
  if(rename(BFPILOT_SHORTCUTS_TMP, BFPILOT_SHORTCUTS_PATH) != 0) return -1;
  return 0;
}


static void
fs_totals(const char *path, fs_total_info_t *info) {
  struct statvfs vfs;
  memset(info, 0, sizeof(*info));
  if(statvfs(path, &vfs) != 0) return;

  unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  info->total = (unsigned long long)vfs.f_blocks * block;
  info->free_bytes = (unsigned long long)vfs.f_bfree * block;
  info->available = (unsigned long long)vfs.f_bavail * block;
  info->reserved = info->free_bytes > info->available
                       ? info->free_bytes - info->available
                       : 0;
  info->used = info->total > info->free_bytes
                   ? info->total - info->free_bytes
                   : 0;
  info->usable_total = info->total > info->reserved
                           ? info->total - info->reserved
                           : info->total;
  info->usable_used = info->usable_total > info->available
                          ? info->usable_total - info->available
                          : 0;
  info->used_pct = info->total > 0
                       ? ((double)info->used * 100.0) / (double)info->total
                       : 0.0;
  info->usable_used_pct =
      info->usable_total > 0
          ? ((double)info->usable_used * 100.0) / (double)info->usable_total
          : 0.0;
  info->ok = 1;
}


static int
append_fs_place(json_buf_t *b, int *first, const char *label,
                const char *path, const char *kind, int custom,
                int mounted) {
  struct stat st;
  if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;

  if(!*first && json_append(b, ",") != 0) return -1;
  *first = 0;

  fs_total_info_t fs = {0};
  fs_totals(path, &fs);

  if(json_append(b, "{\"label\":") != 0 ||
     json_string(b, label && label[0] ? label : safe_basename_label(path)) != 0 ||
     json_append(b, ",\"path\":") != 0 ||
     json_string(b, path) != 0 ||
     json_append(b, ",\"kind\":") != 0 ||
     json_string(b, kind ? kind : "path") != 0 ||
     json_appendf(b,
                  ",\"custom\":%s,\"mounted\":%s,\"dev\":%lu,"
                  "\"statvfs\":%s,\"totalBytes\":%llu,\"freeBytes\":%llu,"
                  "\"availableBytes\":%llu,\"reservedBytes\":%llu,"
                  "\"usedBytes\":%llu,\"usedPercent\":%.2f,"
                  "\"usableTotalBytes\":%llu,\"usableUsedBytes\":%llu,"
                  "\"usableUsedPercent\":%.2f,"
                  "\"writable\":null,\"writeProbe\":\"skipped-read-only\"}",
                  custom ? "true" : "false", mounted ? "true" : "false",
                  (unsigned long)st.st_dev, fs.ok ? "true" : "false",
                  fs.total, fs.free_bytes, fs.available, fs.reserved,
                  fs.used, fs.used_pct, fs.usable_total, fs.usable_used,
                  fs.usable_used_pct) != 0) {
    return -1;
  }
  return 0;
}


static int
append_mount_candidate(json_buf_t *b, int *first, const char *path,
                       const char *kind, dev_t mnt_dev) {
  struct stat st;
  if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;

  if(st.st_dev == mnt_dev) {
    return 0;
  }

  char label[64];
  snprintf(label, sizeof(label), "%s", path + strlen("/mnt/"));
  for(char *p = label; *p; p++) *p = (char)toupper((unsigned char)*p);
  return append_fs_place(b, first, label, path, kind, 0, 1);
}


static int
places_request(const http_request_t *req) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"places\":[") != 0) return -1;
  int first = 1;

  if(append_fs_place(&b, &first, "Root", "/", "root", 0, 1) != 0 ||
     append_fs_place(&b, &first, "Homebrew", "/data/homebrew", "homebrew", 0, 1) != 0 ||
     append_fs_place(&b, &first, "Mounts", "/mnt", "mounts", 0, 1) != 0 ||
     append_fs_place(&b, &first, "User", "/user", "user", 0, 1) != 0 ||
     append_fs_place(&b, &first, "Data", "/data", "internal", 0, 1) != 0) {
    free(b.data);
    return -1;
  }

  struct stat mnt_st;
  dev_t mnt_dev = 0;
  if(stat("/mnt", &mnt_st) == 0) mnt_dev = mnt_st.st_dev;

  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    if(append_mount_candidate(&b, &first, path, "usb", mnt_dev) != 0) {
      free(b.data);
      return -1;
    }
  }
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/ext%d", i);
    if(append_mount_candidate(&b, &first, path, "ext", mnt_dev) != 0) {
      free(b.data);
      return -1;
    }
  }

  shortcut_entry_t shortcuts[BFPILOT_MAX_SHORTCUTS];
  int shortcut_count = read_shortcuts(shortcuts, BFPILOT_MAX_SHORTCUTS);
  for(int i = 0; i < shortcut_count; i++) {
    if(append_fs_place(&b, &first, shortcuts[i].label, shortcuts[i].path,
                       "custom", 1, 1) != 0) {
      free(b.data);
      return -1;
    }
  }

  if(json_append(&b, "],\"mounts\":[") != 0) {
    free(b.data);
    return -1;
  }

  int mount_first = 1;
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    if(append_mount_candidate(&b, &mount_first, path, "usb", mnt_dev) != 0) {
      free(b.data);
      return -1;
    }
  }
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/ext%d", i);
    if(append_mount_candidate(&b, &mount_first, path, "ext", mnt_dev) != 0) {
      free(b.data);
      return -1;
    }
  }

  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
shortcut_add_request(const http_request_t *req) {
  char label[128], path[512];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad shortcut path");
  }
  (void)websrv_get_query_arg(req, "label", label, sizeof(label));

  struct stat st;
  if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return serve_error(req, 404, "shortcut target is not a folder");
  }

  shortcut_entry_t shortcuts[BFPILOT_MAX_SHORTCUTS];
  int count = read_shortcuts(shortcuts, BFPILOT_MAX_SHORTCUTS);
  int index = -1;
  for(int i = 0; i < count; i++) {
    if(!strcmp(shortcuts[i].path, path)) {
      index = i;
      break;
    }
  }
  if(index < 0) {
    if(count >= BFPILOT_MAX_SHORTCUTS) {
      return serve_error(req, 409, "shortcut limit reached");
    }
    index = count++;
  }
  sanitize_shortcut_label(shortcuts[index].label,
                          sizeof(shortcuts[index].label), label, path);
  snprintf(shortcuts[index].path, sizeof(shortcuts[index].path), "%s", path);

  if(write_shortcuts(shortcuts, count) != 0) {
    return serve_error(req, 500, strerror(errno));
  }
  bfpilot_log("shortcut add label=%s path=%s", shortcuts[index].label, path);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"label\":") != 0 ||
     json_string(&b, shortcuts[index].label) != 0 ||
     json_append(&b, ",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
shortcut_delete_request(const http_request_t *req) {
  char path[512];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad shortcut path");
  }

  shortcut_entry_t shortcuts[BFPILOT_MAX_SHORTCUTS];
  int count = read_shortcuts(shortcuts, BFPILOT_MAX_SHORTCUTS);
  int out = 0;
  int removed = 0;
  for(int i = 0; i < count; i++) {
    if(!strcmp(shortcuts[i].path, path)) {
      removed = 1;
      continue;
    }
    if(out != i) shortcuts[out] = shortcuts[i];
    out++;
  }
  if(!removed) return serve_error(req, 404, "shortcut not found");
  if(write_shortcuts(shortcuts, out) != 0) {
    return serve_error(req, 500, strerror(errno));
  }
  bfpilot_log("shortcut delete path=%s", path);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
shortcut_rename_request(const http_request_t *req) {
  char label[128], path[512];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad shortcut path");
  }
  if(!websrv_get_query_arg(req, "label", label, sizeof(label))) {
    return serve_error(req, 400, "missing shortcut label");
  }

  shortcut_entry_t shortcuts[BFPILOT_MAX_SHORTCUTS];
  int count = read_shortcuts(shortcuts, BFPILOT_MAX_SHORTCUTS);
  int index = -1;
  for(int i = 0; i < count; i++) {
    if(!strcmp(shortcuts[i].path, path)) {
      index = i;
      break;
    }
  }
  if(index < 0) return serve_error(req, 404, "shortcut not found");

  sanitize_shortcut_label(shortcuts[index].label,
                          sizeof(shortcuts[index].label), label, path);
  if(write_shortcuts(shortcuts, count) != 0) {
    return serve_error(req, 500, strerror(errno));
  }
  bfpilot_log("shortcut rename label=%s path=%s", shortcuts[index].label, path);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"label\":") != 0 ||
     json_string(&b, shortcuts[index].label) != 0 ||
     json_append(&b, ",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
spawn_copy_or_move(const http_request_t *req, int is_move) {
  char src[1024], dst[1024];
  if(!websrv_get_query_arg(req, "src", src, sizeof(src)) ||
     !websrv_get_query_arg(req, "dst", dst, sizeof(dst)) ||
     !path_is_safe(src) || !path_is_safe(dst)) {
    return serve_error(req, 400, "bad src/dst");
  }
  if(!strcmp(src, dst)) return serve_error(req, 400, "src == dst");
  if(!job_begin(is_move ? "move" : "copy")) {
    return serve_error(req, 409, "another file-manager job is already running");
  }
  bfpilot_log("transfer job begin verb=%s src=%s dst=%s",
              is_move ? "move" : "copy", src, dst);

  struct copy_arg *a = calloc(1, sizeof(*a));
  if(!a) {
    job_end(-1, "alloc");
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->src, sizeof(a->src), "%s", src);
  snprintf(a->dst, sizeof(a->dst), "%s", dst);
  a->is_move = is_move;

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, copy_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    free(a);
    job_end(-1, "pthread_create");
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_appendf(&b, "{\"ok\":true,\"verb\":\"%s\",\"src\":",
                  is_move ? "move" : "copy") != 0 ||
     json_string(&b, src) != 0 ||
     json_append(&b, ",\"dst\":") != 0 ||
     json_string(&b, dst) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
delete_handler(const http_request_t *req) {
  char path[1024], recursive[32];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad path");
  }
  if(!strcmp(path, "/") || !strcmp(path, "/data") ||
     !strcmp(path, "/system_data") || !strcmp(path, "/user")) {
    return serve_error(req, 403, "refusing to delete root path");
  }

  int has_recursive = websrv_get_query_arg(req, "recursive", recursive,
                                           sizeof(recursive));
  struct stat st;
  if(lstat(path, &st) != 0) return serve_error(req, 404, strerror(errno));
  if(S_ISDIR(st.st_mode) && (!has_recursive || !strcmp(recursive, "0"))) {
    return serve_error(req, 400, "directory delete needs recursive=1");
  }
  if(!job_begin("delete")) {
    return serve_error(req, 409, "another file-manager job is already running");
  }

  struct delete_arg *a = calloc(1, sizeof(*a));
  if(!a) {
    job_end(-1, "alloc");
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->path, sizeof(a->path), "%s", path);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, delete_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    free(a);
    job_end(-1, "pthread_create");
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"verb\":\"delete\",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
mkdir_handler(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad path");
  }
  if(mkdirs(path) != 0) return serve_error(req, 500, strerror(errno));
  bfpilot_search_mark_stale("mkdir");

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
rename_handler(const http_request_t *req) {
  char src[1024], dst[1024];
  if(!websrv_get_query_arg(req, "src", src, sizeof(src)) ||
     !websrv_get_query_arg(req, "dst", dst, sizeof(dst)) ||
     !path_is_safe(src) || !path_is_safe(dst)) {
    return serve_error(req, 400, "bad src/dst");
  }
  if(rename(src, dst) != 0) return serve_error(req, 500, strerror(errno));
  bfpilot_search_mark_stale("rename");

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"src\":") != 0 ||
     json_string(&b, src) != 0 ||
     json_append(&b, ",\"dst\":") != 0 ||
     json_string(&b, dst) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
status_handler(const http_request_t *req) {
  int busy = atomic_load(&g_job.busy);
  long tb = atomic_load(&g_job.total_bytes);
  long cb = atomic_load(&g_job.copied_bytes);
  int tf = atomic_load(&g_job.total_files);
  int df = atomic_load(&g_job.done_files);
  int ff = atomic_load(&g_job.failed_files);
  long elapsed_ms = atomic_load(&g_job.elapsed_ms);
  long read_bytes = atomic_load(&g_job.read_bytes);
  long written_bytes = atomic_load(&g_job.written_bytes);
  int last_errno = atomic_load(&g_job.last_errno);
  int cancel = atomic_load(&g_job.cancel);
  char verb[16], current[512], source[512], destination[512], err[256];
  unsigned long source_dev, destination_dev;
  long started_ms;
  time_t started_at;
  time_t ended_at;

  pthread_mutex_lock(&g_job.lock);
  snprintf(verb, sizeof(verb), "%s", g_job.verb);
  snprintf(current, sizeof(current), "%s", g_job.current);
  snprintf(source, sizeof(source), "%s", g_job.source);
  snprintf(destination, sizeof(destination), "%s", g_job.destination);
  snprintf(err, sizeof(err), "%s", g_job.error);
  source_dev = g_job.source_dev;
  destination_dev = g_job.destination_dev;
  started_ms = g_job.started_ms;
  started_at = g_job.started_at;
  ended_at = g_job.ended_at;
  pthread_mutex_unlock(&g_job.lock);

  time_t now = time(NULL);
  time_t ref_at = busy ? now : (ended_at ? ended_at : now);
  long elapsed = started_at > 0 && ref_at > started_at
                     ? (long)(ref_at - started_at)
                     : 0;
  if(busy) {
    long now_ms = monotonic_ms();
    elapsed_ms = now_ms > started_ms ? now_ms - started_ms : 0;
  }
  double speed_bps = elapsed_ms > 0 && cb > 0
                         ? ((double)cb * 1000.0) / (double)elapsed_ms
                         : 0.0;
  long speed = speed_bps > 0.0 ? (long)speed_bps : 0;
  double avg_mbps = speed_bps / (1024.0 * 1024.0);
  long eta = busy && speed > 0 && tb > cb
                 ? (long)(((double)(tb - cb) / speed_bps) + 0.999)
                 : 0;

  json_buf_t b = {0};
  if(json_appendf(&b,
      "{\"ok\":true,\"busy\":%s,\"cancelling\":%s,\"verb\":",
      busy ? "true" : "false", cancel ? "true" : "false") != 0 ||
     json_string(&b, verb) != 0 ||
     json_append(&b, ",\"current\":") != 0 ||
     json_string(&b, current) != 0 ||
     json_append(&b, ",\"error\":") != 0 ||
     json_string(&b, err) != 0 ||
     json_append(&b, ",\"source\":") != 0 ||
     json_string(&b, source) != 0 ||
     json_append(&b, ",\"destination\":") != 0 ||
     json_string(&b, destination) != 0 ||
     json_appendf(&b,
      ",\"totalBytes\":%ld,\"copiedBytes\":%ld,"
      "\"totalFiles\":%d,\"doneFiles\":%d,\"failedFiles\":%d,"
      "\"startedAt\":%ld,\"endedAt\":%ld,"
      "\"elapsedSeconds\":%ld,\"elapsedMs\":%ld,"
      "\"bytesRead\":%ld,\"bytesWritten\":%ld,"
      "\"sourceDev\":%lu,\"destinationDev\":%lu,\"errno\":%d,"
      "\"speedBytesPerSec\":%ld,\"averageMBps\":%.2f,"
      "\"etaSeconds\":%ld}",
      tb, cb, tf, df, ff, (long)started_at, (long)ended_at,
      elapsed, elapsed_ms, read_bytes, written_bytes, source_dev,
      destination_dev, last_errno, speed, avg_mbps, eta) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}

static int
transfer_stats_handler(const http_request_t *req) {
  long elapsed_ms;
  unsigned long bytes, destination_dev;
  int error_code;
  char path[512];

  pthread_mutex_lock(&g_upload.lock);
  elapsed_ms = g_upload.elapsed_ms;
  bytes = g_upload.bytes;
  destination_dev = g_upload.destination_dev;
  error_code = g_upload.error_code;
  snprintf(path, sizeof(path), "%s", g_upload.path);
  pthread_mutex_unlock(&g_upload.lock);

  json_buf_t b = {0};
  double mbps = elapsed_ms > 0 ?
      ((double)bytes * 1000.0 / (double)elapsed_ms) / (1024.0 * 1024.0) : 0.0;
  if(json_append(&b, "{\"ok\":true,\"bufferSize\":") != 0 ||
     json_appendf(&b, "%u,\"lastUpload\":{\"path\":",
                  (unsigned int)UPLOAD_BUF_SIZE) != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b,
                  ",\"bytes\":%lu,\"elapsedMs\":%ld,\"averageMBps\":%.2f,"
                  "\"destinationDev\":%lu,\"errno\":%d}}",
                  bytes, elapsed_ms, mbps, destination_dev, error_code) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
read_request_body(const http_request_t *req, const char *initial_data,
                  size_t initial_size, size_t content_size,
                  size_t max_size, char **out) {
  if(content_size > max_size) {
    drain_body(req->fd, initial_size, content_size);
    return -1;
  }
  char *body = calloc(1, content_size + 1);
  if(!body) {
    drain_body(req->fd, initial_size, content_size);
    return -1;
  }
  if(initial_size > content_size) initial_size = content_size;
  if(initial_size > 0) memcpy(body, initial_data, initial_size);

  size_t used = initial_size;
  while(used < content_size) {
    ssize_t n = recv(req->fd, body + used, content_size - used, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      free(body);
      return -1;
    }
    if(n == 0) {
      free(body);
      return -1;
    }
    used += (size_t)n;
  }
  body[content_size] = 0;
  *out = body;
  return 0;
}


static int
form_get_arg(const char *body, const char *name, char *out, size_t out_size) {
  const char *p = body;
  char key[128];
  char value[2048];

  if(out_size > 0) out[0] = 0;
  while(p && *p) {
    const char *amp = strchr(p, '&');
    size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', pair_len);
    size_t key_len = eq ? (size_t)(eq - p) : pair_len;
    size_t val_len = eq ? pair_len - key_len - 1 : 0;

    if(key_len >= sizeof(key)) key_len = sizeof(key) - 1;
    memcpy(key, p, key_len);
    key[key_len] = 0;
    websrv_url_decode(key, sizeof(key), key);

    if(!strcmp(key, name)) {
      if(val_len >= sizeof(value)) val_len = sizeof(value) - 1;
      if(eq) memcpy(value, eq + 1, val_len);
      value[val_len] = 0;
      websrv_url_decode(out, out_size, value);
      return 1;
    }

    if(!amp) break;
    p = amp + 1;
  }
  return 0;
}


static int
write_text_atomic(const char *tmp_path, const char *final_path,
                  const char *data, size_t size, mode_t mode) {
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if(fd < 0) return -1;
  int rc = write_all_fd(fd, data, size);
  if(close(fd) != 0 && rc == 0) rc = -1;
  if(rc != 0) {
    unlink(tmp_path);
    return -1;
  }
  if(rename(tmp_path, final_path) != 0) {
    unlink(tmp_path);
    return -1;
  }
  return 0;
}


static unsigned
archive_normalize_threads_text(const char *threads) {
  if(!threads || !*threads) return 0;
  for(const unsigned char *p = (const unsigned char *)threads; *p; p++) {
    if(*p < '0' || *p > '9') return 0;
  }
  unsigned long value = strtoul(threads, NULL, 10);
  if(value > BFPILOT_ARCHIVE_MAX_THREADS) value = BFPILOT_ARCHIVE_MAX_THREADS;
  return (unsigned)value;
}

static int
archive_normalize_nice_text(const char *nice) {
  if(!nice || !*nice) return BFPILOT_ARCHIVE_DEFAULT_NICE;
  char *end = NULL;
  errno = 0;
  long value = strtol(nice, &end, 10);
  if(errno != 0 || end == nice || (end && *end)) {
    return BFPILOT_ARCHIVE_DEFAULT_NICE;
  }
  if(value < BFPILOT_ARCHIVE_MIN_NICE) value = BFPILOT_ARCHIVE_MIN_NICE;
  if(value > BFPILOT_ARCHIVE_MAX_NICE) value = BFPILOT_ARCHIVE_MAX_NICE;
  return (int)value;
}


static int
archive_bool_text_true(const char *value) {
  if(!value) return 0;
  return !strcmp(value, "1") ||
         !strcmp(value, "true") ||
         !strcmp(value, "yes") ||
         !strcmp(value, "on");
}


static int
archive_write_status_prepared(const char *src, const char *dst,
                              unsigned threads, int priority_nice,
                              int cleanup_partial) {
  json_buf_t b = {0};
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  const char *worker = "bfpilot-integrated-archive";
  const char *current = "starting integrated archive extractor";
  const char *requires_injection = "false";
#else
  const char *worker = "bfpilot-archive-worker";
  const char *current = "inject bfpilot-archive-worker.elf";
  const char *requires_injection = "true";
#endif
  if(json_append(&b,
                 "{\"ok\":true,\"worker\":\"") != 0 ||
     json_append(&b, worker) != 0 ||
     json_append(&b,
                 "\",\"state\":\"prepared\",\"archiveType\":\"pending\","
                 "\"source\":") != 0 ||
     json_string(&b, src) != 0 ||
     json_append(&b, ",\"destination\":") != 0 ||
     json_string(&b, dst) != 0 ||
     json_append(&b, ",\"stage\":\"\",\"current\":") != 0 ||
     json_string(&b, current) != 0 ||
     json_append(&b,
                 ",\"error\":\"\",\"percent\":0,\"totalBytes\":0,"
                 "\"bytesWritten\":0,\"filesDone\":0,\"totalFiles\":0,"
                 "\"elapsedMs\":0,\"averageMBps\":0,\"errno\":0,"
                 "\"threads\":") != 0 ||
     json_appendf(&b, "%u", threads) != 0 ||
     json_append(&b, ",\"threadMode\":\"") != 0 ||
     json_append(&b, threads == 0 ? "auto" : "manual") != 0 ||
     json_appendf(&b, "\",\"priorityNice\":%d", priority_nice) != 0 ||
     json_append(&b, ",\"cleanupPartial\":") != 0 ||
     json_append(&b, cleanup_partial ? "true" : "false") != 0 ||
     json_append(&b, ","
                 "\"requiresInjection\":") != 0 ||
     json_append(&b, requires_injection) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  int rc = write_text_atomic(BFPILOT_ARCHIVE_STATUS_TMP,
                             BFPILOT_ARCHIVE_STATUS, b.data, b.len, 0666);
  free(b.data);
  return rc;
}


static int
archive_status_handler(const http_request_t *req) {
  const char *idle_body =
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
      "{\"ok\":true,\"worker\":\"bfpilot-integrated-archive\","
      "\"state\":\"idle\",\"requiresInjection\":false}";
#else
      "{\"ok\":true,\"worker\":\"bfpilot-archive-worker\","
      "\"state\":\"idle\",\"requiresInjection\":true}";
#endif
  int fd = open(BFPILOT_ARCHIVE_STATUS, O_RDONLY);
  if(fd < 0) {
    return websrv_send(req->fd, 200, "application/json",
                       idle_body, strlen(idle_body));
  }
  char *buf = malloc(32768);
  if(!buf) {
    close(fd);
    return serve_error(req, 500, "out of memory");
  }
  ssize_t n = read(fd, buf, 32767);
  int saved = errno;
  close(fd);
  if(n < 0) {
    free(buf);
    errno = saved;
    return serve_error(req, 500, strerror(errno));
  }
  if(n == 0) {
    free(buf);
    return websrv_send(req->fd, 200, "application/json",
                       idle_body, strlen(idle_body));
  }
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  buf[n] = 0;
  if(!strstr(buf, "\"ok\":") || !strstr(buf, "\"state\":")) {
    free(buf);
    return websrv_send(req->fd, 200, "application/json",
                       idle_body, strlen(idle_body));
  }
  if(strstr(buf, "\"state\":\"done\"") ||
     strstr(buf, "\"state\":\"error\"") ||
     strstr(buf, "\"state\":\"idle\"")) {
    atomic_store(&g_archive_busy, 0);
  }
#endif
  return serve_owned(req, 200, buf, (size_t)n);
}


static int
archive_support_handler(const http_request_t *req) {
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  const char body[] =
      "{\"ok\":true,\"worker\":\"bfpilot-integrated-archive\","
      "\"fallbackWorker\":\"bfpilot-archive-worker.elf\","
      "\"requiresInjection\":false,"
#else
  const char body[] =
      "{\"ok\":true,\"worker\":\"bfpilot-archive-worker.elf\","
      "\"requiresInjection\":true,"
#endif
      "\"jobPath\":\"" BFPILOT_ARCHIVE_JOB "\","
      "\"statusPath\":\"" BFPILOT_ARCHIVE_STATUS "\","
      "\"supported\":[\"rar\",\"7z\",\"7z.001\",\"zip\"],"
      "\"passwords\":{\"rar\":true,\"7z\":true,"
      "\"zip\":\"ZipCrypto only; AES zip reports unsupported\"},"
      "\"multipart\":{\"rar\":true,\"7z.001\":true,"
      "\"zip\":false},"
      "\"threading\":{\"default\":\"auto\",\"autoMax\":2,"
      "\"manualMax\":8,\"statusField\":\"effectiveThreads\","
      "\"formats\":{\"rar\":{\"autoMax\":1,\"manualMax\":1,"
      "\"reason\":\"RAR multi-threaded extraction is disabled pending "
      "large-archive PS5 stability validation\"},"
      "\"7z\":{\"autoMax\":2,\"manualMax\":8},"
      "\"zip\":{\"autoMax\":1,\"manualMax\":1}}},"
      "\"scheduling\":{\"defaultNice\":4,\"minNice\":-20,"
      "\"maxNice\":20,\"bestEffort\":true},"
      "\"options\":{\"cleanupPartial\":true},"
      "\"telemetry\":[\"inputWaitMBps\",\"outputWaitMBps\","
      "\"cpuUtilPercent\",\"rarMtThreadedBlocks\","
      "\"rarMtLargeBlocks\"],"
      "\"allowedRoots\":[\"/data\",\"/mnt/usb0-7\",\"/mnt/ext0-7\"]}";
  return websrv_send(req->fd, 200, "application/json",
                     body, sizeof(body) - 1);
}


int
transfer_archive_prepare_request(const http_request_t *req,
                                 const char *initial_data,
                                 size_t initial_size,
                                 size_t content_size) {
  char *body = NULL;
  char src[1024], dst[1024], password[512], threads[32], nice[32],
       cleanup_partial[16];

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  int expected = 0;
  if(!atomic_compare_exchange_strong(&g_archive_busy, &expected, 1)) {
    int fd = open(BFPILOT_ARCHIVE_STATUS, O_RDONLY);
    if(fd >= 0) {
      char status[2048];
      ssize_t n = read(fd, status, sizeof(status) - 1);
      close(fd);
      if(n > 0) {
        status[n] = 0;
        if(strstr(status, "\"state\":\"done\"") ||
           strstr(status, "\"state\":\"error\"") ||
           strstr(status, "\"state\":\"idle\"")) {
          atomic_store(&g_archive_busy, 0);
          expected = 0;
          if(atomic_compare_exchange_strong(&g_archive_busy, &expected, 1)) {
            goto archive_busy_claimed;
          }
        }
      }
    }
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 409, "archive extraction is already running");
  }
archive_busy_claimed:
#endif

  if(read_request_body(req, initial_data, initial_size, content_size,
                       8192, &body) != 0) {
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return serve_error(req, 400, "archive prepare body is too large or invalid");
  }

  int ok = form_get_arg(body, "src", src, sizeof(src)) &&
           form_get_arg(body, "dst", dst, sizeof(dst));
  if(!form_get_arg(body, "password", password, sizeof(password))) {
    password[0] = 0;
  }
  if(!form_get_arg(body, "threads", threads, sizeof(threads))) {
    snprintf(threads, sizeof(threads), "%s", "0");
  }
  if(!form_get_arg(body, "nice", nice, sizeof(nice))) {
    snprintf(nice, sizeof(nice), "%d", BFPILOT_ARCHIVE_DEFAULT_NICE);
  }
  if(!form_get_arg(body, "cleanupPartial", cleanup_partial,
                   sizeof(cleanup_partial))) {
    snprintf(cleanup_partial, sizeof(cleanup_partial), "%s", "0");
  }
  free(body);

  int src_allowed = archive_path_allowed(src);
  int dst_allowed = archive_path_allowed(dst);
  int src_text_ok = config_value_safe(src, 0);
  int dst_text_ok = config_value_safe(dst, 0);
  int password_text_ok = config_value_safe(password, 1);
  int threads_text_ok = config_value_safe(threads, 1);
  int nice_text_ok = config_value_safe(nice, 1);
  int cleanup_text_ok = config_value_safe(cleanup_partial, 1);
  if(!ok || !src_allowed || !dst_allowed || !src_text_ok || !dst_text_ok ||
     !password_text_ok || !threads_text_ok || !nice_text_ok ||
     !cleanup_text_ok) {
    char err[256];
    snprintf(err, sizeof(err),
             "bad archive request fields ok=%d src_allowed=%d "
             "dst_allowed=%d src_text=%d dst_text=%d password_text=%d "
             "threads_text=%d nice_text=%d cleanup_text=%d",
             ok, src_allowed, dst_allowed, src_text_ok, dst_text_ok,
             password_text_ok, threads_text_ok, nice_text_ok,
             cleanup_text_ok);
    int rc = serve_error(req, 400, err);
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }

  struct stat st;
  if(stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
    int rc = serve_error(req, 404, "archive source is not a file");
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }
  if(stat(dst, &st) == 0) {
    int rc = serve_error(req, 409, "archive destination already exists");
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }
  if(errno != ENOENT) {
    int rc = serve_error(req, 500, strerror(errno));
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }

  if(mkdirs(BFPILOT_ARCHIVE_DIR) != 0) {
    int rc = serve_error(req, 500, strerror(errno));
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }

  unsigned normalized_threads = archive_normalize_threads_text(threads);
  int normalized_nice = archive_normalize_nice_text(nice);
  int cleanup_partial_enabled = archive_bool_text_true(cleanup_partial);

  json_buf_t job = {0};
  if(json_append(&job, "source=") != 0 ||
     json_append(&job, src) != 0 ||
     json_append(&job, "\ndestination=") != 0 ||
     json_append(&job, dst) != 0 ||
     json_append(&job, "\npassword=") != 0 ||
     json_append(&job, password) != 0 ||
     json_append(&job, "\nthreads=") != 0 ||
     json_appendf(&job, "%u", normalized_threads) != 0 ||
     json_append(&job, "\nnice=") != 0 ||
     json_appendf(&job, "%d", normalized_nice) != 0 ||
     json_append(&job, "\ncleanupPartial=") != 0 ||
     json_append(&job, cleanup_partial_enabled ? "1" : "0") != 0 ||
     json_append(&job, "\n") != 0) {
    free(job.data);
    int rc = serve_error(req, 500, "out of memory");
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }
  if(write_text_atomic(BFPILOT_ARCHIVE_JOB_TMP, BFPILOT_ARCHIVE_JOB,
                       job.data, job.len, 0600) != 0) {
    free(job.data);
    int rc = serve_error(req, 500, strerror(errno));
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }
  free(job.data);

  if(archive_write_status_prepared(src, dst, normalized_threads,
                                   normalized_nice,
                                   cleanup_partial_enabled) != 0) {
    int rc = serve_error(req, 500, strerror(errno));
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return rc;
  }

  bfpilot_log("archive prepare source=%s destination=%s password=%s threads=%u mode=%s nice=%d",
              src, dst, password[0] ? "provided" : "empty",
              normalized_threads, normalized_threads == 0 ? "auto" : "manual",
              normalized_nice);
  bfpilot_search_mark_stale("archive");

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  bfpilot_log("archive integrated worker job queued");
#endif

  json_buf_t res = {0};
  if(json_append(&res,
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
                 "{\"ok\":true,\"worker\":\"bfpilot-integrated-archive\","
                 "\"fallbackWorker\":\"bfpilot-archive-worker.elf\","
                 "\"requiresInjection\":false,\"jobPath\":") != 0 ||
#else
                 "{\"ok\":true,\"worker\":\"bfpilot-archive-worker.elf\","
                 "\"requiresInjection\":true,\"jobPath\":") != 0 ||
#endif
     json_string(&res, BFPILOT_ARCHIVE_JOB) != 0 ||
     json_append(&res, ",\"statusPath\":") != 0 ||
     json_string(&res, BFPILOT_ARCHIVE_STATUS) != 0 ||
     json_append(&res, ",\"source\":") != 0 ||
     json_string(&res, src) != 0 ||
     json_append(&res, ",\"destination\":") != 0 ||
     json_string(&res, dst) != 0 ||
     json_appendf(&res, ",\"threads\":%u,\"threadMode\":\"%s\"",
                  normalized_threads,
                  normalized_threads == 0 ? "auto" : "manual") != 0 ||
     json_appendf(&res, ",\"priorityNice\":%d", normalized_nice) != 0 ||
     json_append(&res, ",\"cleanupPartial\":") != 0 ||
     json_append(&res, cleanup_partial_enabled ? "true" : "false") != 0 ||
     json_append(&res, ",\"next\":") != 0 ||
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
     json_string(&res, "archive extraction started") != 0 ||
#else
     json_string(&res, "inject bfpilot-archive-worker.elf") != 0 ||
#endif
     json_append(&res, "}") != 0) {
    free(res.data);
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
    atomic_store(&g_archive_busy, 0);
#endif
    return -1;
  }
  return serve_owned(req, 200, res.data, res.len);
}


static int
cancel_handler(const http_request_t *req) {
  if(atomic_load(&g_job.busy)) {
    atomic_store(&g_job.cancel, 1);
  }
  return status_handler(req);
}


int
transfer_request(const http_request_t *req, const char *url) {
  if(!strcmp(url, "/api/fs/list")) return list_request(req);
  if(!strncmp(url, "/api/fs/search", 14) &&
     (url[14] == 0 || url[14] == '/')) {
    return bfpilot_search_request(req, url);
  }
  if(!strcmp(url, "/api/fs/stat")) return stat_request(req);
  if(!strcmp(url, "/api/fs/du")) return du_request(req);
  if(!strcmp(url, "/api/fs/places")) return places_request(req);
  if(!strcmp(url, "/api/fs/usb")) return places_request(req);
  if(!strcmp(url, "/api/fs/shortcut/add")) return shortcut_add_request(req);
  if(!strcmp(url, "/api/fs/shortcut/delete")) return shortcut_delete_request(req);
  if(!strcmp(url, "/api/fs/shortcut/rename")) return shortcut_rename_request(req);
  if(!strcmp(url, "/api/fs/copy")) return spawn_copy_or_move(req, 0);
  if(!strcmp(url, "/api/fs/move")) return spawn_copy_or_move(req, 1);
  if(!strcmp(url, "/api/fs/delete")) return delete_handler(req);
  if(!strcmp(url, "/api/fs/mkdir")) return mkdir_handler(req);
  if(!strcmp(url, "/api/fs/rename")) return rename_handler(req);
  if(!strcmp(url, "/api/fs/job/status")) return status_handler(req);
  if(!strcmp(url, "/api/fs/transfer/stats")) return transfer_stats_handler(req);
  if(!strcmp(url, "/api/fs/archive/status")) return archive_status_handler(req);
  if(!strcmp(url, "/api/fs/archive/support")) return archive_support_handler(req);
  if(!strcmp(url, "/api/fs/job/cancel")) return cancel_handler(req);
  return serve_error(req, 404, "no such endpoint");
}


static int
upload_segment_safe(const char *seg) {
  if(!seg || !*seg) return 0;
  if(!strcmp(seg, ".") || !strcmp(seg, "..")) return 0;
  for(const char *p = seg; *p; p++) {
    if(*p == '/' || *p == '\\') return 0;
  }
  return 1;
}


static int
write_all_fd(int fd, const void *data, size_t size) {
  const char *p = data;
  while(size > 0) {
    ssize_t n = write(fd, p, size);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) return -1;
    p += n;
    size -= (size_t)n;
  }
  return 0;
}

static int
check_free_space_for_bytes(const char *path, unsigned long long required,
                           unsigned long long *available_out,
                           unsigned long long *needed_out) {
  struct statvfs vfs;
  if(available_out) *available_out = 0;
  if(needed_out) *needed_out = 0;
  if(required == 0) return 0;
  if(statvfs(path, &vfs) != 0) return -1;

  unsigned long long block =
      vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                   : (unsigned long long)vfs.f_bsize;
  unsigned long long available =
      block != 0 && vfs.f_bavail > ULLONG_MAX / block
          ? ULLONG_MAX
          : (unsigned long long)vfs.f_bavail * block;
  unsigned long long needed =
      required > ULLONG_MAX - BFPILOT_SPACE_MARGIN_BYTES
          ? ULLONG_MAX
          : required + BFPILOT_SPACE_MARGIN_BYTES;

  if(available_out) *available_out = available;
  if(needed_out) *needed_out = needed;
  return available >= needed ? 0 : 1;
}


static void
drain_body(int fd, size_t already_read, size_t content_size) {
  char buf[8192];
  size_t remaining = content_size > already_read ? content_size - already_read : 0;
  while(remaining > 0) {
    size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
    ssize_t n = recv(fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(n == 0) break;
    remaining -= (size_t)n;
  }
}


int
transfer_upload_request(const http_request_t *req, const char *initial_data,
                        size_t initial_size, size_t content_size) {
  char dest[1024], fname[256], relpath[768];
  char dir[1024], final_path[1024], staging_path[1152];
  int has_relpath = 0;

  if(!websrv_get_query_arg(req, "path", dest, sizeof(dest)) ||
     !path_is_safe(dest) ||
     !websrv_get_query_arg(req, "filename", fname, sizeof(fname)) ||
     !upload_segment_safe(fname)) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad upload target");
  }

  has_relpath = websrv_get_query_arg(req, "relpath", relpath, sizeof(relpath));
  snprintf(dir, sizeof(dir), "%s", dest);
  size_t dlen = strlen(dir);
  while(dlen > 1 && dir[dlen - 1] == '/') dir[--dlen] = 0;

  if(has_relpath && relpath[0]) {
    char tmp[768];
    snprintf(tmp, sizeof(tmp), "%s", relpath);
    char *seg = tmp;
    while(seg && *seg) {
      char *slash = strchr(seg, '/');
      if(slash) *slash = 0;
      if(*seg) {
        if(!upload_segment_safe(seg)) {
          drain_body(req->fd, initial_size, content_size);
          return serve_error(req, 400, "bad relative path");
        }
        size_t used = strlen(dir);
        if(used + strlen(seg) + 2 >= sizeof(dir)) {
          drain_body(req->fd, initial_size, content_size);
          return serve_error(req, 400, "upload path too long");
        }
        snprintf(dir + used, sizeof(dir) - used, "/%s", seg);
      }
      if(!slash) break;
      seg = slash + 1;
    }
  }

  if(mkdirs(dir) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, strerror(errno));
  }
  unsigned long long upload_available = 0;
  unsigned long long upload_needed = 0;
  int space_rc = check_free_space_for_bytes(
      dir, (unsigned long long)content_size, &upload_available,
      &upload_needed);
  if(space_rc != 0) {
    int saved_errno = errno;
    if(space_rc < 0) {
      return serve_error(req, 500, strerror(saved_errno));
    }
    char msg[192];
    snprintf(msg, sizeof(msg),
             "not enough free space for upload: need %llu bytes, "
             "available %llu bytes",
             upload_needed, upload_available);
    return serve_error(req, 507, msg);
  }
  join_path(final_path, sizeof(final_path), dir, fname);
  int staging_n = snprintf(staging_path, sizeof(staging_path),
                           "%s.bfpilot-upload-%ld-%ld",
                           final_path, (long)getpid(), (long)time(NULL));
  if(staging_n < 0 || (size_t)staging_n >= sizeof(staging_path)) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "upload staging path too long");
  }

  int out = open(staging_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if(out < 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, strerror(errno));
  }

  int prealloc_attempted = 0;
  int prealloc_rc = 0;
  if(content_size >= UPLOAD_PREALLOC_MIN_SIZE) {
    prealloc_attempted = 1;
    prealloc_rc = posix_fallocate(out, 0, (off_t)content_size);
    if(prealloc_rc == ENOSPC || prealloc_rc == EFBIG) {
      close(out);
      unlink(staging_path);
      return serve_error(req, 507, strerror(prealloc_rc));
    }
    if(prealloc_rc != 0) {
      bfpilot_log("upload posix_fallocate ignored rc=%d path=%s size=%lu",
                  prealloc_rc, staging_path, (unsigned long)content_size);
    }
  }

  char *buf = malloc(UPLOAD_BUF_SIZE);
  if(!buf) {
    close(out);
    drain_body(req->fd, initial_size, content_size);
    unlink(staging_path);
    return serve_error(req, 500, "out of memory");
  }

  int failed = 0;
  char err[200] = {0};
  size_t bytes = 0;
  size_t remaining = content_size;
  long started_ms = monotonic_ms();

  if(initial_size > remaining) initial_size = remaining;
  if(initial_size > 0) {
    if(write_all_fd(out, initial_data, initial_size) != 0) {
      failed = 1;
      snprintf(err, sizeof(err), "write: %s", strerror(errno));
    } else {
      bytes += initial_size;
    }
    remaining -= initial_size;
  }

  while(remaining > 0) {
    size_t want = remaining < UPLOAD_BUF_SIZE ? remaining : UPLOAD_BUF_SIZE;
    ssize_t n = recv(req->fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      failed = 1;
      snprintf(err, sizeof(err), "recv: %s", strerror(errno));
      break;
    }
    if(n == 0) {
      failed = 1;
      snprintf(err, sizeof(err), "short upload");
      break;
    }
    remaining -= (size_t)n;
    if(!failed) {
      if(write_all_fd(out, buf, (size_t)n) != 0) {
        failed = 1;
        snprintf(err, sizeof(err), "write: %s", strerror(errno));
      } else {
        bytes += (size_t)n;
      }
    }
  }

  free(buf);
  int close_rc = close(out);
  if(close_rc != 0 && !failed) {
    failed = 1;
    snprintf(err, sizeof(err), "close: %s", strerror(errno));
  }
  if(!failed && rename(staging_path, final_path) != 0) {
    failed = 1;
    snprintf(err, sizeof(err), "rename: %s", strerror(errno));
  }
  int saved_errno = failed ? (errno ? errno : EIO) : 0;
  long ended_ms = monotonic_ms();
  long elapsed_ms = ended_ms > started_ms ? ended_ms - started_ms : 0;
  struct stat upload_st;
  unsigned long destination_dev =
      stat(dir, &upload_st) == 0 ? (unsigned long)upload_st.st_dev : 0;
  pthread_mutex_lock(&g_upload.lock);
  g_upload.elapsed_ms = elapsed_ms;
  g_upload.bytes = (unsigned long)bytes;
  g_upload.destination_dev = destination_dev;
  g_upload.error_code = saved_errno;
  snprintf(g_upload.path, sizeof(g_upload.path), "%s", final_path);
  pthread_mutex_unlock(&g_upload.lock);
  double mbps = elapsed_ms > 0 ?
      ((double)bytes * 1000.0 / (double)elapsed_ms) / (1024.0 * 1024.0) : 0.0;
  bfpilot_log("upload end rc=%d errno=%d bytes_read=%lu bytes_written=%lu "
              "elapsed_ms=%ld average_mbps=%.2f dst_dev=%lu "
              "prealloc_attempted=%d prealloc_rc=%d path=%s",
              failed ? -1 : 0, saved_errno, (unsigned long)bytes,
              (unsigned long)bytes, elapsed_ms, mbps, destination_dev,
              prealloc_attempted, prealloc_rc, final_path);

  if(failed) {
    drain_body(req->fd, 0, remaining);
    unlink(staging_path);
    return serve_error(req, 500, err[0] ? err : "upload failed");
  }
  bfpilot_search_mark_stale("upload");

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_path) != 0 ||
     json_appendf(&b,
                  ",\"size\":%lu,\"elapsedMs\":%ld,\"averageMBps\":%.2f,"
                  "\"destinationDev\":%lu,\"preallocate\":{\"attempted\":%s,"
                  "\"rc\":%d}}",
                  (unsigned long)bytes, elapsed_ms, mbps, destination_dev,
                  prealloc_attempted ? "true" : "false", prealloc_rc) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}
