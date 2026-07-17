/*
 * BFpilot - early payload entry marker.
 */

#include "boot_marker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "notify.h"
#include "paths.h"
#include "version.h"

#ifndef VERSION_TAG
#define VERSION_TAG "bfpilot-unknown"
#endif

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"
#endif

#ifndef BFPILOT_BOOT_NOTIFY
#define BFPILOT_BOOT_NOTIFY 1
#endif


static void
mkdir_ignore(const char *path) {
  if(mkdir(path, 0755) != 0 && errno != EEXIST) {
    return;
  }
}


void
bfpilot_boot_marker(const char *payload_name, const char *build_mode) {
  const char *payload = payload_name && payload_name[0] ?
                        payload_name : "BFpilot";
  const char *mode = build_mode && build_mode[0] ? build_mode : "unknown";
  char line[512];
  char notify_msg[128];

  printf("BFpilot BOOT: %s %s entered main\n", payload, mode);
  fflush(stdout);

  mkdir_ignore(BFPILOT_DATA_ROOT);
  mkdir_ignore(BFPILOT_DATA_DIR);

  int n = snprintf(line, sizeof(line),
                   "%ld payload=%s mode=%s pid=%ld tag=%s build=%s entered main\n",
                   (long)time(NULL), payload, mode, (long)getpid(),
                   VERSION_TAG, BUILD_VERSION);
  if(n > 0) {
    if((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    int fd = open(BFPILOT_BOOT_LOG_PATH,
                  O_WRONLY | O_CREAT | O_APPEND, 0600);
    if(fd >= 0) {
      (void)write(fd, line, (size_t)n);
      close(fd);
    }
  }

#if BFPILOT_BOOT_NOTIFY
  snprintf(notify_msg, sizeof(notify_msg), "%s loaded", payload);
  (void)bfpilot_notify_send("BFpilot BOOT", notify_msg);
#else
  (void)notify_msg;
#endif
}
