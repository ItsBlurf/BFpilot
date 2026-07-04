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

#define BFPILOT_DATA_ROOT "/data"
#define BFPILOT_DATA_DIR "/data/gemBFPILOT"
#define BFPILOT_HOMEBREW_DIR "/data/homebrew"
#define BFPILOT_LOG_PATH "/data/gemBFPILOT/log.txt"
#define BFPILOT_CRASH_LOG_PATH "/data/gemBFPILOT/crash.log"
#define BFPILOT_EXT0_ROOT "/mnt/ext0"
#define BFPILOT_EXT0_HOMEBREW "/mnt/ext0/homebrew"

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
signal_name(int sig) {
  switch(sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGABRT: return "SIGABRT";
  case SIGBUS:  return "SIGBUS";
  case SIGILL:  return "SIGILL";
#ifdef SIGTRAP
  case SIGTRAP: return "SIGTRAP";
#endif
#ifdef SIGSYS
  case SIGSYS:  return "SIGSYS";
#endif
  case SIGFPE:  return "SIGFPE";
  default:      return "UNKNOWN";
  }
}


static void
fatal_signal_handler(int sig) {
  char line[512];
  int saved_errno = errno;

  if(g_handling_signal) {
    _exit(128 + sig);
  }
  g_handling_signal = 1;

  int n = snprintf(line, sizeof(line),
                   "%ld fatal signal=%s(%d) checkpoint=%s errno=%d\n",
                   (long)time(NULL), signal_name(sig), sig,
                   g_checkpoint, saved_errno);
  if(n < 0) {
    n = snprintf(line, sizeof(line),
                 "fatal signal=%d checkpoint=%s errno=%d\n",
                 sig, g_checkpoint, saved_errno);
  }
  if(n < 0) {
    _exit(128 + sig);
  }
  if((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

  write(STDOUT_FILENO, line, (size_t)n);

  ensure_diag_dir();
  int fd = open(BFPILOT_CRASH_LOG_PATH,
                O_WRONLY | O_CREAT | O_APPEND, 0600);
  if(fd >= 0) {
    write(fd, line, (size_t)n);
    close(fd);
  }

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

  fputs(line, stdout);
  fflush(stdout);

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


const char *
bfpilot_diag_launcher_status(void) {
  return g_launcher_status;
}


int
bfpilot_diag_launcher_rc(void) {
  int value;
  pthread_mutex_lock(&g_diag_lock);
  value = g_launcher_rc;
  pthread_mutex_unlock(&g_diag_lock);
  return value;
}


const char *
bfpilot_diag_checkpoint(void) {
  return g_checkpoint;
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
