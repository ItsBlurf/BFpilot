/* Integrated, asynchronous PKG installer for the all-in-one payload. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "appinst_direct.h"
#include "diag.h"
#include "notify.h"
#include "path_ops.h"
#include "paths.h"
#include "pkg_file.h"
#include "pkg_install.h"

#define BFPILOT_PKG_STATUS_MAX (16U * 1024U)
#define BFPILOT_PKG_PATH_CAPACITY 1024U

typedef struct pkg_job_item {
  char path[BFPILOT_PKG_PATH_CAPACITY];
  bfpilot_pkg_file_info_t info;
} pkg_job_item_t;

typedef struct pkg_job {
  size_t count;
  uint64_t total_bytes;
  pkg_job_item_t items[];
} pkg_job_t;

static atomic_int g_pkg_busy;

static void
pkg_log(const char *format, ...) {
  char message[1200];
  char line[1500];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format ? format : "", args);
  va_end(args);
  int length = snprintf(line, sizeof(line), "%ld pid=%ld %s\n",
                        (long)time(NULL), (long)getpid(), message);
  if(length < 0) return;
  if((size_t)length >= sizeof(line)) length = (int)sizeof(line) - 1;
  bfpilot_log("pkg: %s", message);
  (void)mkdir(BFPILOT_DATA_DIR, 0755);
  int fd = open(BFPILOT_PKG_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if(fd >= 0) {
    (void)write(fd, line, (size_t)length);
    close(fd);
  }
}

static void
json_string(FILE *file, const char *value) {
  fputc('"', file);
  for(const unsigned char *p = (const unsigned char *)(value ? value : "");
      *p; p++) {
    if(*p == '"' || *p == '\\') {
      fputc('\\', file);
      fputc(*p, file);
    } else if(*p == '\n') {
      fputs("\\n", file);
    } else if(*p == '\r') {
      fputs("\\r", file);
    } else if(*p == '\t') {
      fputs("\\t", file);
    } else if(*p < 0x20 || *p == 0x7f) {
      fprintf(file, "\\u%04x", *p);
    } else if(*p >= 0x80) {
      size_t utf8_length = bfpilot_utf8_sequence_length(p);
      if(utf8_length > 0) {
        (void)fwrite(p, 1, utf8_length, file);
        p += utf8_length - 1;
      } else {
        fprintf(file, "\\udb80\\udc%02x", *p);
      }
    } else {
      fputc(*p, file);
    }
  }
  fputc('"', file);
}

static int
write_status(const char *state, const char *path, const char *message,
             const char *content_id, const char *title_id, size_t package_count,
             size_t submitted, size_t failed, uint64_t total_bytes,
             unsigned int percent, const bfpilot_appinst_result_t *submit,
             int result) {
  (void)mkdir(BFPILOT_DATA_DIR, 0755);
  FILE *file = fopen(BFPILOT_PKG_STATUS_TMP, "wb");
  if(!file) return -1;
  fputs("{\"ok\":true,\"state\":", file);
  json_string(file, state);
  fputs(",\"path\":", file);
  json_string(file, path);
  fputs(",\"message\":", file);
  json_string(file, message);
  fputs(",\"contentId\":", file);
  json_string(file, content_id);
  fputs(",\"titleId\":", file);
  json_string(file, title_id);
  fputs(",\"method\":", file);
  json_string(file, submit && submit->used_embedded_worker
      ? "embedded-worker" : "");
  fprintf(file,
          ",\"busy\":%s,\"packageCount\":%lu,\"submitted\":%lu,"
          "\"completed\":0,\"failed\":%lu,\"percent\":%u,"
          "\"downloadedBytes\":0,\"totalBytes\":%llu,"
          "\"result\":%d,\"resultHex\":\"0x%08x\","
          "\"firmwareMajor\":%d,\"authIdResult\":%d,\"initResult\":%d,"
          "\"primaryResult\":%d,\"fallbackResult\":%d,"
          "\"embeddedWorker\":%s,"
          "\"restoreAuthIdResult\":%d,\"updatedAt\":%ld,\"pid\":%ld}\n",
          atomic_load(&g_pkg_busy) ? "true" : "false",
          (unsigned long)package_count, (unsigned long)submitted,
          (unsigned long)failed, percent,
          (unsigned long long)total_bytes, result, (unsigned int)result,
          submit ? submit->firmware_major : 0,
          submit ? submit->authid_result : 0,
          submit ? submit->init_result : 0,
          submit ? submit->primary_result : 0,
          submit ? submit->fallback_result : 0,
          submit && submit->used_embedded_worker ? "true" : "false",
          submit ? submit->restore_authid_result : 0,
          (long)time(NULL), (long)getpid());
  int saved_errno = 0;
  if(fflush(file) != 0 || fsync(fileno(file)) != 0) saved_errno = errno;
  if(fclose(file) != 0 && !saved_errno) saved_errno = errno;
  if(saved_errno) {
    (void)unlink(BFPILOT_PKG_STATUS_TMP);
    errno = saved_errno;
    return -1;
  }
  if(rename(BFPILOT_PKG_STATUS_TMP, BFPILOT_PKG_STATUS_PATH) != 0) {
    saved_errno = errno;
    (void)unlink(BFPILOT_PKG_STATUS_TMP);
    errno = saved_errno;
    return -1;
  }
  return 0;
}

static const char *
base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash && slash[1] ? slash + 1 : path;
}

static void
title_id_from_content_id(const char *content_id, char *title_id,
                         size_t title_id_size) {
  if(!title_id || title_id_size == 0) return;
  title_id[0] = 0;
  const char *dash = content_id ? strchr(content_id, '-') : NULL;
  const char *underscore = dash ? strchr(dash + 1, '_') : NULL;
  if(!dash || !underscore || underscore <= dash + 1) return;
  size_t length = (size_t)(underscore - (dash + 1));
  if(length < 5 || length >= title_id_size) return;
  for(size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)dash[1 + i];
    if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return;
  }
  memcpy(title_id, dash + 1, length);
  title_id[length] = 0;
}

static void *
pkg_worker(void *argument) {
  pkg_job_t *job = argument;
  size_t submitted_count = 0;
  size_t failed_count = 0;
  int last_result = 0;
  bfpilot_appinst_result_t last_submit;
  memset(&last_submit, 0, sizeof(last_submit));

  (void)write_status("starting", job->items[0].path,
                     "validating package queue", job->items[0].info.content_id,
                     job->items[0].info.title_id, job->count, 0, 0,
                     job->total_bytes, 0, NULL, 0);
  bfpilot_notify("BFpilot PKG installer", "Submitting package queue");

  for(size_t i = 0; i < job->count; i++) {
    memset(&last_submit, 0, sizeof(last_submit));
    char validation_error[256];
    bfpilot_pkg_file_info_t current;
    if(bfpilot_pkg_file_validate(job->items[i].path, &current,
                                 validation_error,
                                 sizeof(validation_error)) != 0 ||
       current.size != job->items[i].info.size ||
       strcmp(current.content_id, job->items[i].info.content_id) != 0) {
      failed_count++;
      last_result = -ESTALE;
      pkg_log("selection changed before submit index=%lu path=%s error=%s",
              (unsigned long)(i + 1), job->items[i].path,
              validation_error[0] ? validation_error : "identity changed");
    } else {
      last_result = bfpilot_appinst_install_pkg(
          job->items[i].path, base_name(job->items[i].path), &last_submit);
      if(last_result == 0) {
        submitted_count++;
        if(job->items[i].info.content_id[0] && last_submit.content_id[0] &&
           strcmp(last_submit.content_id, job->items[i].info.content_id) != 0) {
          pkg_log("warning output content-id mismatch expected=%s actual=%s",
                  job->items[i].info.content_id, last_submit.content_id);
        }
      } else {
        failed_count++;
      }
      pkg_log("submit index=%lu/%lu path=%s cid=%s title=%s size=%llu "
              "rc=0x%08x method=embedded-worker primary=0x%08x "
              "fallback=0x%08x",
              (unsigned long)(i + 1), (unsigned long)job->count,
              job->items[i].path, job->items[i].info.content_id,
              job->items[i].info.title_id,
              (unsigned long long)job->items[i].info.size, last_result,
              last_submit.primary_result, last_submit.fallback_result);
    }

    const char *reported_content_id = job->items[i].info.content_id[0]
        ? job->items[i].info.content_id : last_submit.content_id;
    char reported_title_id[sizeof(job->items[i].info.title_id)];
    snprintf(reported_title_id, sizeof(reported_title_id), "%s",
             job->items[i].info.title_id);
    if(!reported_title_id[0]) {
      title_id_from_content_id(reported_content_id, reported_title_id,
                               sizeof(reported_title_id));
    }
    unsigned int percent = (unsigned int)(((i + 1) * 100U) / job->count);
    (void)write_status("submitting", job->items[i].path,
                       last_result == 0 ? "AppInst accepted this package"
                                        : "AppInst rejected this package",
                       reported_content_id, reported_title_id, job->count,
                       submitted_count, failed_count, job->total_bytes,
                       percent, &last_submit, last_result);
  }

  atomic_store(&g_pkg_busy, 0);
  const char *state = failed_count == 0 ? "accepted" :
                      submitted_count == 0 ? "error" :
                                             "completed_with_errors";
  const char *message = failed_count == 0
      ? "AppInst accepted the queue; monitor PS5 notifications"
      : submitted_count == 0
          ? "AppInst rejected the package queue"
          : "some packages were accepted and some were rejected";
  size_t last = job->count - 1;
  const char *reported_content_id = job->items[last].info.content_id[0]
      ? job->items[last].info.content_id : last_submit.content_id;
  char reported_title_id[sizeof(job->items[last].info.title_id)];
  snprintf(reported_title_id, sizeof(reported_title_id), "%s",
           job->items[last].info.title_id);
  if(!reported_title_id[0]) {
    title_id_from_content_id(reported_content_id, reported_title_id,
                             sizeof(reported_title_id));
  }
  (void)write_status(state, job->items[last].path, message,
                     reported_content_id, reported_title_id, job->count,
                     submitted_count, failed_count, job->total_bytes, 100,
                     &last_submit, last_result);
  bfpilot_notify("BFpilot PKG installer",
                 failed_count == 0 ? "Package queue accepted"
                 : submitted_count == 0 ? "Package queue failed"
                                        : "Package queue partially accepted");
  pkg_log("queue finished count=%lu submitted=%lu failed=%lu state=%s",
          (unsigned long)job->count, (unsigned long)submitted_count,
          (unsigned long)failed_count, state);
  free(job);
  return NULL;
}

int
bfpilot_pkg_install_start(const char *const *paths, size_t path_count,
                          char *error, size_t error_size) {
  int expected = 0;
  pkg_job_t *job = NULL;
  if(error && error_size) error[0] = 0;
  if(!paths || path_count == 0 || path_count > BFPILOT_PKG_MAX_PATHS) {
    if(error && error_size) snprintf(error, error_size,
        "select between 1 and %u PKG files", BFPILOT_PKG_MAX_PATHS);
    return -EINVAL;
  }
  if(!atomic_compare_exchange_strong(&g_pkg_busy, &expected, 1)) {
    if(error && error_size) snprintf(error, error_size,
                                     "another PKG queue is being submitted");
    return -EBUSY;
  }
  if(path_count > (SIZE_MAX - sizeof(*job)) / sizeof(job->items[0])) {
    atomic_store(&g_pkg_busy, 0);
    return -EOVERFLOW;
  }
  job = calloc(1, sizeof(*job) + path_count * sizeof(job->items[0]));
  if(!job) {
    atomic_store(&g_pkg_busy, 0);
    return -ENOMEM;
  }
  job->count = path_count;

  for(size_t i = 0; i < path_count; i++) {
    const char *extension = paths[i] ? strrchr(paths[i], '.') : NULL;
    if(!paths[i] || !bfpilot_path_is_safe(paths[i]) || !extension ||
       strcasecmp(extension, ".pkg") ||
       strlen(paths[i]) >= sizeof(job->items[i].path)) {
      if(error && error_size) snprintf(error, error_size,
          "selection %lu is not a valid .pkg path", (unsigned long)(i + 1));
      free(job);
      atomic_store(&g_pkg_busy, 0);
      return -EINVAL;
    }
    snprintf(job->items[i].path, sizeof(job->items[i].path), "%s", paths[i]);
    for(size_t previous = 0; previous < i; previous++) {
      if(!strcmp(job->items[previous].path, job->items[i].path)) {
        if(error && error_size) snprintf(error, error_size,
            "selection %lu duplicates an earlier PKG",
            (unsigned long)(i + 1));
        free(job);
        atomic_store(&g_pkg_busy, 0);
        return -EINVAL;
      }
    }
    int rc = bfpilot_pkg_file_validate(paths[i], &job->items[i].info,
                                       error, error_size);
    if(rc != 0) {
      free(job);
      atomic_store(&g_pkg_busy, 0);
      return rc;
    }
    if(bfpilot_pkg_file_is_system(&job->items[i].info)) {
      if(error && error_size) {
        snprintf(error, error_size,
                 "NPXS system packages are not supported; use the PS5 Debug Settings package installer");
      }
      free(job);
      atomic_store(&g_pkg_busy, 0);
      return -ENOTSUP;
    }
    if(UINT64_MAX - job->total_bytes < job->items[i].info.size) {
      if(error && error_size) snprintf(error, error_size,
                                       "package queue size overflow");
      free(job);
      atomic_store(&g_pkg_busy, 0);
      return -EOVERFLOW;
    }
    job->total_bytes += job->items[i].info.size;
  }

  (void)write_status("queued", job->items[0].path,
                     "waiting for direct AppInst submission",
                     job->items[0].info.content_id,
                     job->items[0].info.title_id, job->count, 0, 0,
                     job->total_bytes, 0, NULL, 0);
  pkg_log("queue accepted count=%lu first=%s cid=%s bytes=%llu",
          (unsigned long)job->count, job->items[0].path,
          job->items[0].info.content_id,
          (unsigned long long)job->total_bytes);

  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_t *thread_attr = NULL;
  int rc = pthread_attr_init(&attr);
  if(rc == 0) {
    thread_attr = &attr;
    int stack_rc = pthread_attr_setstacksize(&attr, 512U * 1024U);
    if(stack_rc != 0) pkg_log("could not set PKG worker stack rc=%d", stack_rc);
  } else {
    pkg_log("could not initialize PKG worker attributes rc=%d", rc);
  }
  rc = pthread_create(&thread, thread_attr, pkg_worker, job);
  if(thread_attr) pthread_attr_destroy(&attr);
  if(rc != 0) {
    if(error && error_size) snprintf(error, error_size,
                                     "could not start PKG worker (%d)", rc);
    atomic_store(&g_pkg_busy, 0);
    (void)write_status("error", job->items[0].path,
                       "could not start integrated PKG worker",
                       job->items[0].info.content_id,
                       job->items[0].info.title_id, job->count, 0, 1,
                       job->total_bytes, 0, NULL, -rc);
    free(job);
    return -rc;
  }
  pthread_detach(thread);
  return 0;
}

int
bfpilot_pkg_status_request(const http_request_t *req) {
  struct stat st;
  int flags = O_RDONLY;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int fd = open(BFPILOT_PKG_STATUS_PATH, flags);
  if(fd < 0) {
    return websrv_send_text(req->fd, 200, "application/json",
        atomic_load(&g_pkg_busy)
          ? "{\"ok\":true,\"state\":\"starting\",\"busy\":true}"
          : "{\"ok\":true,\"state\":\"idle\",\"busy\":false}");
  }
  if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
     st.st_size > (off_t)BFPILOT_PKG_STATUS_MAX) {
    close(fd);
    return websrv_send_error_json(req->fd, 500, "invalid PKG status file");
  }
  size_t size = (size_t)st.st_size;
  char *body = malloc(size);
  if(!body) {
    close(fd);
    return websrv_send_error_json(req->fd, 500, "out of memory");
  }
  size_t used = 0;
  while(used < size) {
    ssize_t got = read(fd, body + used, size - used);
    if(got < 0) {
      if(errno == EINTR) continue;
      free(body);
      close(fd);
      return websrv_send_error_json(req->fd, 500, strerror(errno));
    }
    if(got == 0) break;
    used += (size_t)got;
  }
  close(fd);
  int result = websrv_send(req->fd, 200, "application/json", body, used);
  free(body);
  return result;
}

int
bfpilot_pkg_is_busy(void) {
  return atomic_load(&g_pkg_busy) != 0;
}
