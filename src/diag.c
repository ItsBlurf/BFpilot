/*
 * BFpilot - runtime diagnostics and append logging.
 */

#include "diag.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>

#include "paths.h"

#define BFPILOT_EXT0_ROOT "/mnt/ext0"
#define BFPILOT_EXT0_HOMEBREW "/mnt/ext0/homebrew"
#define BFPILOT_LOG_RING_CAPACITY 512
#define BFPILOT_LOG_MAX_LINE 1024

typedef struct {
  char line[BFPILOT_LOG_MAX_LINE];
  time_t ts;
} bfpilot_log_entry_t;

static bfpilot_log_entry_t g_log_ring[BFPILOT_LOG_RING_CAPACITY];
static size_t g_log_ring_head = 0;
static size_t g_log_ring_count = 0;

static pthread_mutex_t g_diag_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t          g_start_time = 0;
static int             g_last_errno = 0;
static int             g_netctl_rc = BFPILOT_DIAG_SKIPPED;
static int             g_user_service_rc = BFPILOT_DIAG_SKIPPED;
static int             g_notification_rc = BFPILOT_DIAG_SKIPPED;
static int             g_bind_rc = BFPILOT_DIAG_SKIPPED;
static int             g_listen_rc = BFPILOT_DIAG_SKIPPED;
static int             g_launcher_rc = BFPILOT_DIAG_SKIPPED;
static int             g_first_http_seen = 0;
static char            g_checkpoint[128] = "not-started";
static char            g_launcher_status[64] = "unknown";
static volatile sig_atomic_t g_handling_signal = 0;
static int             g_crash_fd = -1;
static int             g_log_udp_fd = -1;
static struct sockaddr_in g_log_udp_addr;


static int
mkdir_if_needed(const char *path) {
  if(mkdir(path, 0755) == 0) return 0;
  return errno == EEXIST ? 0 : -1;
}


static void
ensure_diag_dir(void) {
  mkdir_if_needed(BFPILOT_DATA_ROOT);
  mkdir_if_needed(BFPILOT_DATA_DIR);
}


static const char *
signal_message(int sig, size_t *size) {
  switch(sig) {
  case SIGSEGV: *size = sizeof("BFpilot fatal SIGSEGV\n") - 1;
                return "BFpilot fatal SIGSEGV\n";
  case SIGABRT: *size = sizeof("BFpilot fatal SIGABRT\n") - 1;
                return "BFpilot fatal SIGABRT\n";
  case SIGBUS:  *size = sizeof("BFpilot fatal SIGBUS\n") - 1;
                return "BFpilot fatal SIGBUS\n";
  case SIGILL:  *size = sizeof("BFpilot fatal SIGILL\n") - 1;
                return "BFpilot fatal SIGILL\n";
#ifdef SIGTRAP
  case SIGTRAP: *size = sizeof("BFpilot fatal SIGTRAP\n") - 1;
                return "BFpilot fatal SIGTRAP\n";
#endif
#ifdef SIGSYS
  case SIGSYS:  *size = sizeof("BFpilot fatal SIGSYS\n") - 1;
                return "BFpilot fatal SIGSYS\n";
#endif
  case SIGFPE:  *size = sizeof("BFpilot fatal SIGFPE\n") - 1;
                return "BFpilot fatal SIGFPE\n";
  default:      *size = sizeof("BFpilot fatal signal\n") - 1;
                return "BFpilot fatal signal\n";
  }
}


static void
fatal_signal_handler(int sig) {
  if(g_handling_signal) {
    _exit(128 + sig);
  }
  g_handling_signal = 1;
  size_t size = 0;
  const char *message = signal_message(sig, &size);
  (void)write(STDOUT_FILENO, message, size);
  if(g_crash_fd >= 0) (void)write(g_crash_fd, message, size);
  _exit(128 + sig);
}


void
bfpilot_diag_init(void) {
  pthread_mutex_lock(&g_diag_lock);
  if(g_start_time == 0) {
    g_start_time = time(NULL);
  }
  pthread_mutex_unlock(&g_diag_lock);

  ensure_diag_dir();
  if(g_crash_fd < 0) {
    g_crash_fd = open(BFPILOT_CRASH_LOG_PATH,
                      O_WRONLY | O_CREAT | O_APPEND, 0600);
  }
}


void
bfpilot_diag_install_signal_handlers(void) {
  signal(SIGSEGV, fatal_signal_handler);
  signal(SIGABRT, fatal_signal_handler);
  signal(SIGBUS, fatal_signal_handler);
  signal(SIGILL, fatal_signal_handler);
#ifdef SIGTRAP
  signal(SIGTRAP, fatal_signal_handler);
#endif
#ifdef SIGSYS
  signal(SIGSYS, fatal_signal_handler);
#endif
  signal(SIGFPE, fatal_signal_handler);
}


void
bfpilot_log(const char *fmt, ...) {
  char body[1024];
  char line[1400];
  va_list ap;
  int saved_errno = errno;

  va_start(ap, fmt);
  int body_n = vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
  va_end(ap);
  if(body_n < 0) {
    snprintf(body, sizeof(body), "log format failed");
  } else if((size_t)body_n >= sizeof(body)) {
    body[sizeof(body) - 1] = 0;
  }

  pthread_mutex_lock(&g_diag_lock);
  int line_n = snprintf(line, sizeof(line), "%ld pid=%ld checkpoint=%s %s\n",
                        (long)time(NULL), (long)getpid(), g_checkpoint, body);
  if(line_n < 0) {
    pthread_mutex_unlock(&g_diag_lock);
    errno = saved_errno;
    return;
  }
  if((size_t)line_n >= sizeof(line)) line_n = (int)sizeof(line) - 1;

  /* Circular in-memory ring buffer */
  size_t ring_idx = g_log_ring_head;
  g_log_ring[ring_idx].ts = time(NULL);
  size_t copy_n = (size_t)line_n < (sizeof(g_log_ring[0].line) - 1) ? (size_t)line_n : (sizeof(g_log_ring[0].line) - 1);
  memcpy(g_log_ring[ring_idx].line, line, copy_n);
  g_log_ring[ring_idx].line[copy_n] = 0;
  g_log_ring_head = (g_log_ring_head + 1) % BFPILOT_LOG_RING_CAPACITY;
  if(g_log_ring_count < BFPILOT_LOG_RING_CAPACITY) {
    g_log_ring_count++;
  }

  fputs(line, stdout);
  fflush(stdout);

  if(g_log_udp_fd >= 0) {
    sendto(g_log_udp_fd, line, (size_t)line_n, 0,
           (struct sockaddr *)&g_log_udp_addr, sizeof(g_log_udp_addr));
  }

  int klog_fd = open("/dev/klog", O_WRONLY);
  if(klog_fd >= 0) {
    write(klog_fd, line, (size_t)line_n);
    close(klog_fd);
  }

  /* Rotate log if exceeding 2MB to preserve PS5 storage */
  struct stat log_st;
  if(stat(BFPILOT_LOG_PATH, &log_st) == 0 && log_st.st_size > (2 * 1024 * 1024)) {
    char old_log[512];
    snprintf(old_log, sizeof(old_log), "%s.old", BFPILOT_LOG_PATH);
    (void)unlink(old_log);
    (void)rename(BFPILOT_LOG_PATH, old_log);
  }

  FILE *file = fopen(BFPILOT_LOG_PATH, "a");
  if(file) {
    fwrite(line, 1, (size_t)line_n, file);
    fclose(file);
  } else {
    g_last_errno = errno;
  }

  pthread_mutex_unlock(&g_diag_lock);
  errno = saved_errno;
}


void
bfpilot_checkpoint(const char *checkpoint) {
  pthread_mutex_lock(&g_diag_lock);
  snprintf(g_checkpoint, sizeof(g_checkpoint), "%s",
           checkpoint ? checkpoint : "unknown");
  pthread_mutex_unlock(&g_diag_lock);

  bfpilot_log("checkpoint=%s", checkpoint ? checkpoint : "unknown");
}


void
bfpilot_diag_set_service_rcs(int netctl_rc, int user_service_rc) {
  pthread_mutex_lock(&g_diag_lock);
  g_netctl_rc = netctl_rc;
  g_user_service_rc = user_service_rc;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_set_notification_rc(int rc) {
  pthread_mutex_lock(&g_diag_lock);
  g_notification_rc = rc;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_set_launcher_status(const char *status, int rc) {
  pthread_mutex_lock(&g_diag_lock);
  snprintf(g_launcher_status, sizeof(g_launcher_status), "%s",
           status ? status : "unknown");
  g_launcher_rc = rc;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_set_bind_rc(int rc) {
  pthread_mutex_lock(&g_diag_lock);
  g_bind_rc = rc;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_set_listen_rc(int rc) {
  pthread_mutex_lock(&g_diag_lock);
  g_listen_rc = rc;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_set_last_errno(int err) {
  pthread_mutex_lock(&g_diag_lock);
  g_last_errno = err;
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_mark_first_http(const char *method, const char *path) {
  pthread_mutex_lock(&g_diag_lock);
  if(g_first_http_seen) {
    pthread_mutex_unlock(&g_diag_lock);
    return;
  }
  g_first_http_seen = 1;
  snprintf(g_checkpoint, sizeof(g_checkpoint), "first-http-request");
  pthread_mutex_unlock(&g_diag_lock);

  bfpilot_log("first successful HTTP request method=%s path=%s",
              method ? method : "?", path ? path : "?");
}


long
bfpilot_diag_uptime(void) {
  time_t start;

  pthread_mutex_lock(&g_diag_lock);
  start = g_start_time;
  pthread_mutex_unlock(&g_diag_lock);

  if(start == 0) return 0;
  return (long)(time(NULL) - start);
}


int
bfpilot_diag_last_errno(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_last_errno;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


int
bfpilot_diag_netctl_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_netctl_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


int
bfpilot_diag_user_service_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_user_service_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


int
bfpilot_diag_notification_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_notification_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


int
bfpilot_diag_bind_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_bind_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


int
bfpilot_diag_listen_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_listen_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


void
bfpilot_diag_get_launcher_status(char *out, size_t out_size) {
  if(!out || out_size == 0) return;
  pthread_mutex_lock(&g_diag_lock);
  snprintf(out, out_size, "%s", g_launcher_status);
  pthread_mutex_unlock(&g_diag_lock);
}


int
bfpilot_diag_launcher_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_launcher_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


void
bfpilot_diag_get_checkpoint(char *out, size_t out_size) {
  if(!out || out_size == 0) return;
  pthread_mutex_lock(&g_diag_lock);
  snprintf(out, out_size, "%s", g_checkpoint);
  pthread_mutex_unlock(&g_diag_lock);
}


void
bfpilot_diag_get_cwd(char *out, size_t out_size) {
  if(out_size == 0) return;
  if(getcwd(out, out_size)) return;

  int err = errno;
  bfpilot_diag_set_last_errno(err);
  snprintf(out, out_size, "<getcwd failed errno %d>", err);
}


static int
probe_stat(const char *path) {
  struct stat st;
  if(stat(path, &st) == 0) return 1;
  bfpilot_diag_set_last_errno(errno);
  return 0;
}


static int
probe_opendir(const char *path) {
  DIR *dir = opendir(path);
  if(dir) {
    closedir(dir);
    return 1;
  }
  bfpilot_diag_set_last_errno(errno);
  return 0;
}


int
bfpilot_diag_can_stat_root(void) {
  return probe_stat("/");
}


int
bfpilot_diag_can_opendir_data(void) {
  return probe_opendir(BFPILOT_DATA_ROOT);
}


int
bfpilot_diag_can_opendir_data_homebrew(void) {
  return probe_opendir(BFPILOT_HOMEBREW_DIR);
}


int
bfpilot_diag_can_opendir_usb0(void) {
  DIR *dir = opendir("/mnt/usb0");
  if(dir) {
    closedir(dir);
    return 1;
  }

  int first_errno = errno;
  dir = opendir("/usb0");
  if(dir) {
    closedir(dir);
    return 1;
  }

  bfpilot_diag_set_last_errno(errno ? errno : first_errno);
  return 0;
}


int
bfpilot_diag_can_opendir_ext0(void) {
  return probe_opendir(BFPILOT_EXT0_ROOT);
}


int
bfpilot_diag_can_opendir_ext0_homebrew(void) {
  return probe_opendir(BFPILOT_EXT0_HOMEBREW);
}


int
bfpilot_diag_can_write_data_bfpilot(void) {
  return -1;
}


int
bfpilot_diag_can_write_user_app(void) {
  return -1;
}


void
bfpilot_diag_set_log_udp_target(const char *ip, unsigned short port) {
  pthread_mutex_lock(&g_diag_lock);
  if(g_log_udp_fd >= 0) {
    close(g_log_udp_fd);
    g_log_udp_fd = -1;
  }
  if(ip && ip[0] && port > 0) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd >= 0) {
      memset(&g_log_udp_addr, 0, sizeof(g_log_udp_addr));
      g_log_udp_addr.sin_family = AF_INET;
      g_log_udp_addr.sin_port = htons(port);
      g_log_udp_addr.sin_addr.s_addr = inet_addr(ip);
      if(g_log_udp_addr.sin_addr.s_addr != INADDR_NONE) {
        g_log_udp_fd = fd;
      } else {
        close(fd);
      }
    }
  }
  pthread_mutex_unlock(&g_diag_lock);
}


static const char *
case_str_search(const char *haystack, const char *needle) {
  if(!needle || !*needle) return haystack;
  if(!haystack) return NULL;
  size_t nlen = strlen(needle);
  for(; *haystack; haystack++) {
    if(strncasecmp(haystack, needle, nlen) == 0) return haystack;
  }
  return NULL;
}


void
bfpilot_diag_clear_logs(void) {
  pthread_mutex_lock(&g_diag_lock);
  g_log_ring_head = 0;
  g_log_ring_count = 0;
  pthread_mutex_unlock(&g_diag_lock);
}


int
bfpilot_diag_dump_logs(char **out_buf, size_t *out_size, size_t tail_count,
                       const char *fmt, const char *level_filter) {
  if(!out_buf || !out_size) return -1;
  *out_buf = NULL;
  *out_size = 0;

  int is_json = (fmt && !strcmp(fmt, "json"));
  size_t limit = tail_count;
  if(limit == 0 || limit > BFPILOT_LOG_RING_CAPACITY) {
    limit = BFPILOT_LOG_RING_CAPACITY;
  }

  pthread_mutex_lock(&g_diag_lock);
  size_t available = g_log_ring_count;
  if(limit > available) limit = available;

  size_t start_idx;
  if(g_log_ring_count < BFPILOT_LOG_RING_CAPACITY) {
    start_idx = available >= limit ? available - limit : 0;
  } else {
    start_idx = (g_log_ring_head + BFPILOT_LOG_RING_CAPACITY - limit) % BFPILOT_LOG_RING_CAPACITY;
  }

  /* Size the selected tail while locked, including worst-case JSON escaping.
   * The ring bounds the allocation to less than 3.1 MiB. Filtering follows
   * tail selection, and count describes the rows actually returned. */
  size_t capacity = 64;
  size_t selected = 0;
  for(size_t i = 0; i < limit; i++) {
    const char *line = g_log_ring[(start_idx + i) % BFPILOT_LOG_RING_CAPACITY].line;
    if(level_filter && *level_filter && !case_str_search(line, level_filter)) continue;
    selected++;
    capacity += is_json ? 3 : 1;
    for(const unsigned char *p = (const unsigned char *)line; *p; p++) {
      capacity += is_json && *p < 0x20 ? 6 :
                  is_json && (*p == '"' || *p == '\\') ? 2 : 1;
    }
  }
  char *buf = malloc(capacity);
  if(!buf) {
    pthread_mutex_unlock(&g_diag_lock);
    return -1;
  }
  size_t len = 0;
  if(is_json) {
    int n = snprintf(buf, capacity, "{\"ok\":true,\"count\":%zu,\"logs\":[", selected);
    if(n < 0 || (size_t)n >= capacity) {
      free(buf);
      pthread_mutex_unlock(&g_diag_lock);
      return -1;
    }
    len = (size_t)n;
  }
  size_t written_items = 0;
  for(size_t i = 0; i < limit; i++) {
    size_t idx = (start_idx + i) % BFPILOT_LOG_RING_CAPACITY;
    const char *line = g_log_ring[idx].line;

    if(level_filter && *level_filter) {
      if(!case_str_search(line, level_filter)) continue;
    }

    if(is_json) {
      if(written_items > 0) {
        buf[len++] = ',';
      }
      buf[len++] = '"';
      for(const char *p = line; *p; p++) {
        if(*p == '"' || *p == '\\') {
          buf[len++] = '\\';
          buf[len++] = *p;
        } else if((unsigned char)*p < 0x20) {
          static const char hex[] = "0123456789abcdef";
          buf[len++] = '\\';
          buf[len++] = 'u';
          buf[len++] = '0';
          buf[len++] = '0';
          buf[len++] = hex[(unsigned char)*p >> 4];
          buf[len++] = hex[(unsigned char)*p & 15];
        } else {
          buf[len++] = *p;
        }
      }
      buf[len++] = '"';
    } else {
      size_t line_len = strlen(line);
      memcpy(buf + len, line, line_len);
      len += line_len;
      if(len == 0 || buf[len - 1] != '\n') {
        buf[len++] = '\n';
      }
    }
    written_items++;
  }

  if(is_json) {
    buf[len++] = ']';
    buf[len++] = '}';
  } else if(len == 0) {
    const char empty[] = "# BFpilot live log buffer (empty)\n";
    len = sizeof(empty) - 1;
    memcpy(buf, empty, len);
  }
  buf[len] = 0;

  pthread_mutex_unlock(&g_diag_lock);
  *out_buf = buf;
  *out_size = len;
  return 0;
}


int
bfpilot_diag_get_memory_info(unsigned long long *free_bytes, unsigned long long *total_bytes) {
  if(free_bytes) *free_bytes = 0;
  if(total_bytes) *total_bytes = 0;

  int available = 0;
  uint64_t ram_total = 0;
  size_t ram_total_size = sizeof(ram_total);
  int mib_phys[2] = {CTL_HW, HW_PHYSMEM};
  if(sysctl(mib_phys, 2, &ram_total, &ram_total_size, NULL, 0) < 0 || ram_total == 0) {
    ram_total = 0;
    ram_total_size = sizeof(ram_total);
    if(sysctlbyname("hw.physmem", &ram_total, &ram_total_size, NULL, 0) < 0) ram_total = 0;
  }
  if(ram_total > 0) {
    available |= BFPILOT_MEMORY_TOTAL_VALID;
    if(total_bytes) *total_bytes = (unsigned long long)ram_total;
  }

  /* VM resident pages are not all used RAM. Only return an actual free-page
   * measurement; unavailable sysctls must not become invented availability. */
  long page_size = getpagesize();
  uint64_t free_pages = 0;
  size_t free_size = sizeof(free_pages);
  int free_rc = sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &free_size, NULL, 0);
  struct vmtotal vmt;
  memset(&vmt, 0, sizeof(vmt));
  size_t vmt_size = sizeof(vmt);
  if(free_rc != 0) {
    int mib_vmt[2] = {CTL_VM, 1}; /* VM_METER */
    free_rc = sysctl(mib_vmt, 2, &vmt, &vmt_size, NULL, 0);
    if(free_rc != 0) {
      memset(&vmt, 0, sizeof(vmt));
      vmt_size = sizeof(vmt);
      free_rc = sysctlbyname("vm.vmtotal", &vmt, &vmt_size, NULL, 0);
    }
    if(free_rc == 0 && vmt.t_free >= 0) free_pages = (uint64_t)vmt.t_free;
    else free_rc = -1;
  }
  if(free_rc == 0 && page_size > 0 && free_pages <= UINT64_MAX / (uint64_t)page_size) {
    available |= BFPILOT_MEMORY_FREE_VALID;
    if(free_bytes) *free_bytes = free_pages * (uint64_t)page_size;
  }
  return available;
}
