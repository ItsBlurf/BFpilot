/*
 * BFpilot - minimal PS5 browser file manager payload.
 *
 * Runtime surface is intentionally small: one HTTP file-manager server,
 * startup notification, and no companion side services.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "boot_marker.h"
#include "diag.h"
#include "notify.h"
#include "paths.h"
#include "search.h"
#include "tile_bootstrap.h"
#include "transfer.h"
#include "version.h"
#include "websrv.h"

#define BFPILOT_RELOAD_TOKEN "bs5fm-local-reload"

#ifndef BFPILOT_WEB_PORT
#define BFPILOT_WEB_PORT 5905
#endif

#ifndef BFPILOT_PAYLOAD_NAME
#define BFPILOT_PAYLOAD_NAME "bfpilot"
#endif

#ifndef BFPILOT_DEBUG_NOTIFICATIONS
#define BFPILOT_DEBUG_NOTIFICATIONS 0
#endif

#ifndef BFPILOT_ENABLE_INTEGRATED_ARCHIVE
#define BFPILOT_ENABLE_INTEGRATED_ARCHIVE 0
#endif

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
#include "archive_worker.h"
#endif

static void
detect_lan_ip(char *out, size_t out_size) {
  struct ifaddrs *ifaddr = NULL;

  snprintf(out, out_size, "<PS5_IP>");
  if(getifaddrs(&ifaddr) != 0) return;

  for(struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if(ifa->ifa_flags & IFF_LOOPBACK) continue;

    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    const char *ip = inet_ntop(AF_INET, &sa->sin_addr, out, out_size);
    if(ip && strncmp(out, "169.254.", 8) != 0) {
      freeifaddrs(ifaddr);
      return;
    }
  }

  freeifaddrs(ifaddr);
  snprintf(out, out_size, "<PS5_IP>");
}


typedef struct ready_state {
  char ip[64];
  int notified;
  unsigned short port;
} ready_state_t;


static void
debug_notify(const char *title, const char *message) {
#if BFPILOT_DEBUG_NOTIFICATIONS
  bfpilot_notify(title, message);
#else
  (void)title;
  (void)message;
#endif
}


static unsigned short
parse_port_value(const char *value, unsigned short fallback) {
  if(!value || !value[0]) return fallback;
  char *end = NULL;
  long port = strtol(value, &end, 10);
  if(!end || *end != 0 || port < 1 || port > 65535) {
    return fallback;
  }
  return (unsigned short)port;
}


static unsigned short
runtime_port_requested(int argc, char **argv) {
  unsigned short port = BFPILOT_WEB_PORT;

  for(int i = 1; i < argc; i++) {
    if(!argv || !argv[i]) continue;
    if(!strcmp(argv[i], "--port") && i + 1 < argc) {
      port = parse_port_value(argv[++i], port);
    } else if(!strncmp(argv[i], "--port=", 7)) {
      port = parse_port_value(argv[i] + 7, port);
    }
  }

  return port;
}


static int
local_http_get(unsigned short port, const char *path, char *out,
               size_t out_size) {
  if(out_size == 0) return -1;
  out[0] = 0;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return -1;

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  char request[256];
  int n = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\n"
                   "Host: 127.0.0.1:%u\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   path, (unsigned int)port);
  if(n < 0 || (size_t)n >= sizeof(request) ||
     websrv_write_all(fd, request, (size_t)n) != 0) {
    close(fd);
    return -1;
  }

  size_t used = 0;
  while(used + 1 < out_size) {
    ssize_t r = recv(fd, out + used, out_size - used - 1, 0);
    if(r < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(r == 0) break;
    used += (size_t)r;
  }
  out[used] = 0;
  close(fd);

  return used > 0 ? 0 : -1;
}


static long
json_long_field(const char *json, const char *field) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":", field);

  const char *p = strstr(json ? json : "", needle);
  if(!p) return -1;
  p += strlen(needle);
  while(*p == ' ' || *p == '\t') p++;

  char *end = NULL;
  long value = strtol(p, &end, 10);
  return end && end != p ? value : -1;
}


/* Reinjection handoff — restored to v0.3.8 behavior (do not gate on search). */
static int
old_server_busy(unsigned short port) {
  char response[2048];
  if(local_http_get(port, "/api/fs/job/status", response,
                    sizeof(response)) != 0) {
    return 0;
  }
  if(strstr(response, "\"busy\":true") != NULL) return 1;
  if(local_http_get(port, "/api/fs/archive/status", response,
                    sizeof(response)) != 0) {
    return 0;
  }
  return strstr(response, "\"state\":\"running\"") != NULL ||
         strstr(response, "\"state\":\"finalizing\"") != NULL ||
         strstr(response, "\"state\":\"starting\"") != NULL;
}


static int
wait_for_old_server_down(unsigned short port) {
  char response[512];
  for(int i = 0; i < 30; i++) {
    if(local_http_get(port, "/api/status", response, sizeof(response)) != 0) {
      return 1;
    }
    usleep(100000);
  }
  return 0;
}


static int
handoff_existing_server(unsigned short port) {
  char response[2048];
  if(local_http_get(port, "/api/status", response, sizeof(response)) != 0) {
    return 0;
  }
  if(!strstr(response, "\"name\":\"BFpilot\"") &&
     !strstr(response, "\"name\":\"BS5FileManager\"")) {
    bfpilot_log("handoff: port %u belongs to a different service; leaving it "
                "running", (unsigned int)port);
    bfpilot_notify("BFpilot reload blocked",
                   "Port is used by another service");
    return -1;
  }

  long old_pid = json_long_field(response, "pid");
  bfpilot_log("handoff: existing BFpilot-compatible server pid=%ld port=%u",
              old_pid, (unsigned int)port);
  bfpilot_notify("BFpilot reload: old server detected",
                 old_pid >= 0 ? "Attempting clean shutdown" :
                 "Attempting clean shutdown; old pid unknown");
  if(old_pid == (long)getpid()) {
    return 0;
  }

  if(old_server_busy(port)) {
    bfpilot_notify("BFpilot reload blocked",
                   "Old file operation is still running");
    bfpilot_log("handoff: blocked because old file job is busy");
    return -1;
  }

  snprintf(response, sizeof(response),
           "/api/control/shutdown?token=%s", BFPILOT_RELOAD_TOKEN);
  char shutdown_response[512];
  if(local_http_get(port, response, shutdown_response,
                    sizeof(shutdown_response)) == 0 &&
     strstr(shutdown_response, "200 OK") &&
     strstr(shutdown_response, "\"ok\":true") &&
     wait_for_old_server_down(port)) {
    bfpilot_notify("BFpilot reloaded", "Old listener stopped cleanly");
    bfpilot_log("handoff: old listener stopped cleanly");
    return 1;
  }

  bfpilot_notify("BFpilot reload blocked",
                 "Old listener did not stop cleanly");
  bfpilot_log("handoff: old listener did not accept clean shutdown; not "
              "sending signals");
  return -1;
}


static void
on_web_ready(unsigned short port, void *arg) {
  ready_state_t *state = arg;
  char url[128];

  snprintf(url, sizeof(url), "Open http://%s:%u/",
           state->ip, (unsigned int)port);

  bfpilot_checkpoint("web server ready");
  bfpilot_log("web ui ready http://%s:%u/", state->ip, (unsigned int)port);

  if(!state->notified) {
    bfpilot_notify("BFpilot started", url);
    state->notified = 1;
  }

  /* Home Media tile via separate installer ELF + elfldr — AppInst stays out of main. */
  bfpilot_tile_bootstrap_try();

  debug_notify("BFpilot debug", "web server ready");
}


int
main(int argc, char **argv) {
  umask(0);
  bfpilot_boot_marker(BFPILOT_PAYLOAD_NAME, BFPILOT_BUILD_MODE);
  bfpilot_diag_init();
  bfpilot_diag_install_signal_handlers();
  bfpilot_checkpoint("process start");
  bfpilot_log("build version tag=%s build=%s mode=%s",
              VERSION_TAG, BUILD_VERSION, BFPILOT_BUILD_MODE);
  bfpilot_log("PID=%ld", (long)getpid());
  bfpilot_log("argv argc=%d", argc);
  for(int i = 0; i < argc; i++) {
    bfpilot_log("argv[%d]=%s", i, argv && argv[i] ? argv[i] : "(null)");
  }
  bfpilot_log("PS5 SDK build flags sdk_path=%s enable_launcher=%d "
              "disable_launcher=%d",
              BFPILOT_SDK_PATH, BFPILOT_ENABLE_LAUNCHER,
              BFPILOT_DISABLE_LAUNCHER);
  bfpilot_log("diagnostic paths log=%s crash=%s data_dir=%s",
              BFPILOT_LOG_PATH, BFPILOT_CRASH_LOG_PATH, BFPILOT_DATA_DIR);

  ready_state_t ready;
  memset(&ready, 0, sizeof(ready));
  detect_lan_ip(ready.ip, sizeof(ready.ip));
  ready.port = runtime_port_requested(argc, argv);
  websrv_set_runtime_port(ready.port);
  websrv_set_runtime_diag(1);
  bfpilot_log("runtime web_port=%u", (unsigned int)ready.port);
  bfpilot_log("runtime launcher=separate_payload");

  puts(".----------------------------------------------.");
  puts("|  BFpilot                                     |");
  printf("|  %-18s  browser file manager        |\n", VERSION_TAG);
  puts("'----------------------------------------------'");
  puts("");
  puts("  active: standalone web file manager");
  printf("  mode: %s\n", BFPILOT_BUILD_MODE);
  puts("  scope: browse, upload, download, copy, move, delete, rename, mkdir");
#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  puts("  archive: integrated extractor enabled; no worker injection required");
#else
  puts("  archive: prepare jobs only; inject bfpilot-archive-worker.elf to extract");
#endif
  puts("  ps5 app: Media tile via separate bfpilot-launcher-installer.elf");
  puts("  payloads: double-click .elf/.bin to load via elfldr :9021");
  printf("  data dir: %s (only)\n", BFPILOT_DATA_DIR);
  printf("  web ui: http://%s:%u/\n", ready.ip, (unsigned int)ready.port);
  puts("  inject/deploy port: 9021");
  puts("");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

#if BFPILOT_DEBUG_NOTIFICATIONS
  int notification_rc = bfpilot_notify_test();
  bfpilot_diag_set_notification_rc(notification_rc);
  bfpilot_log("notification test rc=0x%08x", notification_rc);
#else
  bfpilot_diag_set_notification_rc(BFPILOT_DIAG_SKIPPED);
  bfpilot_log("notification test rc=skipped");
#endif

  bfpilot_diag_set_service_rcs(BFPILOT_DIAG_SKIPPED, BFPILOT_DIAG_SKIPPED);
  bfpilot_diag_set_launcher_status("separate_payload", BFPILOT_DIAG_SKIPPED);
  bfpilot_log("sceNetCtlInit rc=skipped");
  bfpilot_log("sceUserServiceInitialize rc=skipped");
  bfpilot_log("launcher skipped: separate payload (inject installer for Media tile)");

  bfpilot_checkpoint("handoff check");
  debug_notify("BFpilot debug", "handoff check");
  int handoff_status = handoff_existing_server(ready.port);
  bfpilot_log("handoff_existing_server rc=%d", handoff_status);
  if(handoff_status < 0) {
    bfpilot_checkpoint("handoff blocked");
    puts("  reload: old listener still active; exiting this injection");
    return 0;
  }

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  int archive_daemon_rc = bfpilot_archive_start_daemon();
  bfpilot_log("archive daemon start rc=%d", archive_daemon_rc);
#endif

  while(1) {
    bfpilot_checkpoint("web listen starting");
    int rc = websrv_listen(ready.port, on_web_ready, &ready);
    bfpilot_log("websrv_listen returned rc=%d", rc);
    if(websrv_exit_requested()) {
      bfpilot_checkpoint("web shutdown requested");
      puts("  web ui: shutdown requested");
      break;
    }
    if(!ready.notified) {
      char msg[128];
      snprintf(msg, sizeof(msg), "port %u error %d, retrying",
               (unsigned int)ready.port, -rc);
      bfpilot_notify("BFpilot could not start", msg);
      ready.notified = 1;
    }
    sleep(rc == -EADDRINUSE || rc == -EACCES ? 5 : 2);
  }

#if BFPILOT_ENABLE_INTEGRATED_ARCHIVE
  bfpilot_archive_stop_daemon();
#endif
  bfpilot_search_shutdown();

  _exit(0);
  return 0;
}
