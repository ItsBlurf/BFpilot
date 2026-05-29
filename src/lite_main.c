/*
 * BS5FileManager - minimal PS5 browser file manager payload.
 *
 * Runtime surface is intentionally small: one HTTP file-manager server,
 * startup notification, and no companion side services.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "app_installer.h"
#include "notify.h"
#include "version.h"
#include "websrv.h"

#define BS5FM_WEB_PORT 5905

int sceNetCtlInit(void);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
int sceUserServiceInitialize(void *);


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
  int launch_browser;
  int browser_launched;
} ready_state_t;


static void
init_ps5_services(void) {
  int user_prio = 256;
  int err = sceNetCtlInit();
  printf("  service: sceNetCtlInit 0x%08x\n", err);

  err = sceUserServiceInitialize(&user_prio);
  printf("  service: sceUserServiceInitialize 0x%08x\n", err);
}


static void
launch_browser_once(ready_state_t *state) {
  if(!state->launch_browser || state->browser_launched) return;

  int err = sceSystemServiceLaunchWebBrowser("http://127.0.0.1:5905/", NULL);
  if(err == 0) {
    puts("  ps5 browser: opened http://127.0.0.1:5905/");
  } else {
    printf("  ps5 browser: launch failed 0x%08x\n", err);
  }
  state->browser_launched = 1;
}


static void
on_web_ready(unsigned short port, void *arg) {
  ready_state_t *state = arg;
  char url[128];

  snprintf(url, sizeof(url), "Open http://%s:%u/",
           state->ip, (unsigned int)port);

  printf("  web ui ready: http://%s:%u/\n", state->ip, (unsigned int)port);

  if(!state->notified) {
    bs5fm_notify("BS5FileManager started", url);
    state->notified = 1;
  }
  launch_browser_once(state);
}


int
main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  ready_state_t ready;
  memset(&ready, 0, sizeof(ready));
  detect_lan_ip(ready.ip, sizeof(ready.ip));

  puts(".----------------------------------------------.");
  puts("|  BS5FileManager                              |");
  printf("|  %-18s  browser file manager        |\n", VERSION_TAG);
  puts("'----------------------------------------------'");
  puts("");
  puts("  active: standalone web file manager");
  puts("  scope: browse, upload, download, copy, move, delete, rename, mkdir");
  puts("  ps5 app: BS5FileManager opens http://127.0.0.1:5905/");
  printf("  web ui: http://%s:%u/\n", ready.ip, (unsigned int)BS5FM_WEB_PORT);
  puts("  inject/deploy port: 9021");
  puts("");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  init_ps5_services();

  int app_install_status = bs5fm_install_app_if_needed();
  if(app_install_status >= 0) {
    puts("  ps5 app: ready");
    ready.launch_browser = app_install_status > 0;
  } else {
    puts("  ps5 app: install failed, continuing web server");
  }

  while(1) {
    int rc = websrv_listen(BS5FM_WEB_PORT, on_web_ready, &ready);
    if(!ready.notified) {
      char msg[128];
      snprintf(msg, sizeof(msg), "port %u error %d, retrying",
               (unsigned int)BS5FM_WEB_PORT, -rc);
      bs5fm_notify("BS5FileManager could not start", msg);
      ready.notified = 1;
    }
    sleep(rc == -EADDRINUSE || rc == -EACCES ? 5 : 2);
  }

  return 0;
}
