/*
 * Private PKG worker embedded inside bfpilot.elf.
 *
 * Sony binds AppInst's IPMI peer identity during Initialize. A long-running
 * file manager cannot reliably become that peer after it has already started
 * serving requests. This tiny, loopback-only worker starts as a fresh elfldr
 * process, acquires the firmware-appropriate identity before Initialize, and
 * performs only serialized InstallByPackage calls. It is a build input, never
 * a separately distributed payload.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define PKG_WORKER_PORT 9041
#define PKG_WORKER_PROTOCOL "BFPKG/1"
#define PKG_PATH_CAPACITY 1040
#define INIT_TIMEOUT_SECONDS 10
#define INIT_TIMEOUT_SENTINEL (-0xDEAD)
#define PS5_SHELLCORE_AUTHID UINT64_C(0x3800000000000010)
#define PS5_SYSTEM_INSTALL_AUTHID UINT64_C(0x4801000000000013)

typedef struct appinst_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} appinst_metadata_t;

typedef struct appinst_pkg_info {
  char content_id[48];
  int32_t content_type;
  int32_t content_platform;
} appinst_pkg_info_t;

typedef struct appinst_playgo_info {
  char languages[30][8];
  char playgo_scenario_ids[64][3];
  char content_ids[64][48];
  unsigned char unknown[6480];
} appinst_playgo_info_t;

_Static_assert(sizeof(appinst_pkg_info_t) == 56, "AppInst pkg-info ABI");
_Static_assert(sizeof(appinst_playgo_info_t) == 9984,
               "AppInst PlayGo ABI");

int sceAppInstUtilInitialize(void);
int sceAppInstUtilInstallByPackage(const appinst_metadata_t *,
                                   appinst_pkg_info_t *,
                                   appinst_playgo_info_t *);

static volatile int g_init_done;
static volatile int g_init_rc = INIT_TIMEOUT_SENTINEL;

static void *
initialize_appinst(void *unused) {
  (void)unused;
  g_init_rc = sceAppInstUtilInitialize();
  g_init_done = 1;
  return NULL;
}

static int
start_appinst_initialize(void) {
  pthread_t thread;
  g_init_done = 0;
  g_init_rc = INIT_TIMEOUT_SENTINEL;
  int rc = pthread_create(&thread, NULL, initialize_appinst, NULL);
  if(rc != 0) {
    g_init_rc = -rc;
    g_init_done = 1;
    return -rc;
  }
  (void)pthread_detach(thread);
  return 0;
}

static int
appinst_init_status(void) {
  return g_init_done ? g_init_rc : INIT_TIMEOUT_SENTINEL;
}

static int
wait_for_appinst_initialize(unsigned int seconds) {
  const unsigned int attempts = seconds * 10;
  for(unsigned int attempt = 0; attempt < attempts; attempt++) {
    if(g_init_done) return g_init_rc;
    usleep(100000);
  }
  return appinst_init_status();
}

static int
parse_firmware_major(const char *version) {
  if(!version) return 0;
  const char *release = strstr(version, "releases/");
  if(!release) release = strstr(version, "release/");
  if(!release) release = strstr(version, "releng/");
  if(release) {
    release = strchr(release, '/');
    if(release) {
      int major = 0;
      int digits = 0;
      release++;
      while(*release >= '0' && *release <= '9') {
        major = major * 10 + (*release++ - '0');
        digits++;
      }
      if(digits && major > 0 && major < 100) return major;
    }
  }

  const char *scan = version;
  if(!strncmp(scan, "FreeBSD 11.0", 12)) scan += 12;
  while(*scan) {
    if(*scan < '0' || *scan > '9') {
      scan++;
      continue;
    }
    int major = 0;
    int digits = 0;
    const char *end = scan;
    while(*end >= '0' && *end <= '9') {
      major = major * 10 + (*end++ - '0');
      digits++;
    }
    if(digits > 0 && digits <= 2 && major > 0 && major < 100 &&
       end[0] == '.' && end[1] >= '0' && end[1] <= '9' &&
       end[2] >= '0' && end[2] <= '9' &&
       (end[3] < '0' || end[3] > '9')) return major;
    scan = end;
  }
  return 0;
}

static int
firmware_major(void) {
  char version[256];
  size_t size = sizeof(version) - 1;
  if(sysctlbyname("kern.version", version, &size, NULL, 0) != 0) return 0;
  if(size >= sizeof(version)) size = sizeof(version) - 1;
  version[size] = 0;
  return parse_firmware_major(version);
}

static int
acquire_install_identity(int major) {
  pid_t pid = getpid();
  if(pid <= 0 || !kernel_get_proc(pid)) return -1;
  int rc = 0;
  if(kernel_set_ucred_uid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_ruid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svuid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_rgid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svgid(pid, 0) != 0) rc = -1;

  intptr_t root = kernel_get_root_vnode();
  if(!root || kernel_set_proc_rootdir(pid, root) != 0 ||
     kernel_set_proc_jaildir(pid, root) != 0) rc = -1;

  uint64_t target = major >= 10 ? PS5_SYSTEM_INSTALL_AUTHID
                                : PS5_SHELLCORE_AUTHID;
  if(kernel_set_ucred_authid(pid, target) != 0 ||
     kernel_get_ucred_authid(pid) != target) rc = -1;

  uint8_t caps[16];
  uint8_t attrs[32];
  memset(caps, 0xff, sizeof(caps));
  memset(attrs, 0, sizeof(attrs));
  attrs[0] = 0x80;
  if(kernel_set_ucred_caps(pid, caps) != 0 ||
     kernel_set_ucred_attrs(pid, attrs) != 0) rc = -1;
  return rc;
}

static int
send_all(int fd, const char *data) {
  size_t size = strlen(data);
  size_t offset = 0;
  while(offset < size) {
    ssize_t sent = send(fd, data + offset, size - offset, 0);
    if(sent < 0 && errno == EINTR) continue;
    if(sent <= 0) return -1;
    offset += (size_t)sent;
  }
  return 0;
}

static int
path_is_safe(const char *path) {
  if(!path || path[0] != '/' || strlen(path) >= PKG_PATH_CAPACITY) return 0;
  if(strstr(path, "/../") || strstr(path, "/..\\")) return 0;
  size_t length = strlen(path);
  if(length >= 2 && path[length - 1] == '.' && path[length - 2] == '.') {
    return 0;
  }
  const char *extension = strrchr(path, '.');
  return extension && !strcasecmp(extension, ".pkg");
}

static int
map_install_path(const char *path, char *output, size_t output_size) {
  int length = !strncmp(path, "/data/", 6)
      ? snprintf(output, output_size, "/user%s", path)
      : snprintf(output, output_size, "%s", path);
  return length < 0 || (size_t)length >= output_size ? -1 : 0;
}

static int
install_package(const char *path, char content_id[48]) {
  char install_path[PKG_PATH_CAPACITY];
  if(!path_is_safe(path) ||
     map_install_path(path, install_path, sizeof(install_path)) != 0) {
    return -EINVAL;
  }

  appinst_pkg_info_t pkg_info;
  appinst_playgo_info_t playgo;
  memset(&pkg_info, 0, sizeof(pkg_info));
  memset(&playgo, 0, sizeof(playgo));
  appinst_metadata_t metadata = {
      .uri = install_path,
      .ex_uri = "",
      .playgo_scenario_id = "",
      .content_id = "",
      .content_name = "",
      .icon_url = "",
  };
  int rc = sceAppInstUtilInstallByPackage(&metadata, &pkg_info, &playgo);
  pkg_info.content_id[sizeof(pkg_info.content_id) - 1] = 0;
  snprintf(content_id, 48, "%s", pkg_info.content_id);
  return rc;
}

int
main(void) {
  signal(SIGPIPE, SIG_IGN);
  int major = firmware_major();
  int identity_rc = -1;
  for(int attempt = 0; attempt < 20 && identity_rc != 0; attempt++) {
    identity_rc = acquire_install_identity(major);
    if(identity_rc != 0) usleep(250000);
  }

  if(identity_rc == 0) {
    (void)start_appinst_initialize();
  } else {
    g_init_rc = -ENOTSUP;
    g_init_done = 1;
  }
  int server = socket(AF_INET, SOCK_STREAM, 0);
  if(server < 0) {
    payload_exit(0);
    return 0;
  }
  int reuse = 1;
  (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(PKG_WORKER_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
     listen(server, 2) != 0) {
    close(server);
    payload_exit(0);
    return 0;
  }

  int stop = 0;
  while(!stop) {
    int client = accept(server, NULL, NULL);
    if(client < 0) {
      if(errno == EINTR) continue;
      break;
    }
    struct timeval timeout = {.tv_sec = 20, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
    char request[PKG_PATH_CAPACITY + 32];
    ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    if(received > 0) {
      request[received] = 0;
      request[strcspn(request, "\r\n")] = 0;
      char response[160];
      if(!strcmp(request, "PING")) {
        int init_rc = appinst_init_status();
        snprintf(response, sizeof(response),
                 "READY %s fw=%d init=0x%08X identity=%d",
                 PKG_WORKER_PROTOCOL, major, (unsigned)init_rc, identity_rc);
      } else if(!strcmp(request, "STOP")) {
        snprintf(response, sizeof(response), "OK STOP");
        stop = 1;
      } else if(!strncmp(request, "INSTALL ", 8)) {
        int init_rc = wait_for_appinst_initialize(INIT_TIMEOUT_SECONDS);
        if(init_rc != 0) {
          snprintf(response, sizeof(response), "ERR INIT 0x%08X",
                   (unsigned)init_rc);
        } else {
          char content_id[48] = {0};
          int rc = install_package(request + 8, content_id);
          if(rc == 0) {
            snprintf(response, sizeof(response), "OK INSTALL %.47s",
                     content_id);
          } else {
            snprintf(response, sizeof(response), "ERR INSTALL 0x%08X",
                     (unsigned)rc);
          }
        }
      } else {
        snprintf(response, sizeof(response), "ERR PROTOCOL");
      }
      (void)send_all(client, response);
    }
    close(client);
  }
  close(server);
  payload_exit(0);
  return 0;
}
