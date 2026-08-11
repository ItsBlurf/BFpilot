/*
 * AppInst integration for the one-file BFpilot payload.
 *
 * Working all-in-one PS5 file managers link SceIpmi and SceAppInstUtil into
 * the main ELF. Keep every Sony installer call under one mutex and initialize
 * once. Local PKG submission additionally needs the correct Sony AppInst
 * identity on affected firmware, so any auth-ID change is limited to this
 * serialized call window and the previous value is verified after restore.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "appinst_direct.h"
#include "diag.h"
#include "paths.h"

#define PS5_SHELLCORE_AUTHID UINT64_C(0x3800000000000010)
#define PS5_SYSTEM_INSTALL_AUTHID UINT64_C(0x4801000000000013)
#define BFPILOT_APPINST_INIT_TIMEOUT_TENTHS 150

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                             \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"           \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const unsigned char name[];                                          \
  extern const size_t name##_size

INCASSET(bfpilot_param_json, "assets-app/param.json");
INCASSET(bfpilot_icon0_png, "assets-app/icon0.png");

static const unsigned char g_tile_registration_marker[] =
    "BFpilot tile registration completed\n";

typedef int (*app_install_title_dir_fn)(const char *, const char *, void *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);

static pthread_mutex_t g_appinst_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_appinst_busy;
static atomic_int g_appinst_init_done;
static atomic_int g_appinst_init_rc;
static int g_appinst_init_started;

typedef struct authid_window {
  uint64_t saved;
  uint64_t target;
  int changed;
} authid_window_t;

static int
parse_firmware_major(const char *version) {
  if(!version) return 0;
  const char *release = strstr(version, "releases/");
  if(!release) release = strstr(version, "release/");
  if(!release) release = strstr(version, "releng/");
  if(release) {
    release = strchr(release, '/');
    if(release) {
      release++;
      int major = 0;
      int digits = 0;
      while(*release >= '0' && *release <= '9') {
        major = major * 10 + (*release - '0');
        digits++;
        release++;
      }
      if(digits > 0 && major > 0 && major < 100) return major;
    }
  }

  /* Skip FreeBSD's leading 11.0 and find the first PS5-style NN.NN token. */
  const char *scan = version;
  if(!strncmp(scan, "FreeBSD 11.0", 12)) scan += 12;
  while(*scan) {
    if(*scan >= '0' && *scan <= '9') {
      int major = 0;
      int digits = 0;
      const char *end = scan;
      while(*end >= '0' && *end <= '9') {
        major = major * 10 + (*end - '0');
        digits++;
        end++;
      }
      if(digits > 0 && digits <= 2 && major > 0 && major < 100 &&
         end[0] == '.' && end[1] >= '0' && end[1] <= '9' &&
         end[2] >= '0' && end[2] <= '9' &&
         (end[3] < '0' || end[3] > '9')) return major;
      scan = end;
    } else {
      scan++;
    }
  }
  return 0;
}

int
bfpilot_appinst_firmware_major(void) {
  char version[256];
  size_t size = sizeof(version) - 1;
  if(sysctlbyname("kern.version", version, &size, NULL, 0) != 0) return 0;
  if(size >= sizeof(version)) size = sizeof(version) - 1;
  version[size] = 0;
  return parse_firmware_major(version);
}

static uint64_t
firmware_install_authid(int firmware_major) {
  return firmware_major >= 11 ? PS5_SYSTEM_INSTALL_AUTHID
                              : PS5_SHELLCORE_AUTHID;
}

static int
authid_acquire_locked(authid_window_t *window,
                      bfpilot_appinst_result_t *result) {
  memset(window, 0, sizeof(*window));
  result->firmware_major = bfpilot_appinst_firmware_major();
  window->target = firmware_install_authid(result->firmware_major);
  window->saved = kernel_get_ucred_authid(getpid());
  if(window->saved == 0) {
    result->authid_result = -ENOTSUP;
    bfpilot_log("AppInst auth-ID unavailable; fw=%d target=0x%016llx; "
                "continuing with loader identity",
                result->firmware_major,
                (unsigned long long)window->target);
    return result->authid_result;
  }
  if(window->saved == window->target) {
    result->authid_result = 0;
    result->restore_authid_result = 0;
    bfpilot_log("AppInst auth-ID already suitable fw=%d value=0x%016llx",
                result->firmware_major,
                (unsigned long long)window->target);
    return 0;
  }

  int rc = kernel_set_ucred_authid(getpid(), window->target);
  if(rc != 0) {
    result->authid_result = rc;
    bfpilot_log("AppInst auth-ID set failed fw=%d saved=0x%016llx "
                "target=0x%016llx rc=%d",
                result->firmware_major,
                (unsigned long long)window->saved,
                (unsigned long long)window->target, rc);
    return rc;
  }
  window->changed = 1;
  if(kernel_get_ucred_authid(getpid()) != window->target) {
    result->authid_result = -EIO;
    bfpilot_log("AppInst auth-ID verification failed fw=%d target=0x%016llx",
                result->firmware_major,
                (unsigned long long)window->target);
    return result->authid_result;
  }
  result->authid_result = 0;
  bfpilot_log("AppInst auth-ID acquired fw=%d saved=0x%016llx "
              "target=0x%016llx",
              result->firmware_major,
              (unsigned long long)window->saved,
              (unsigned long long)window->target);
  return 0;
}

static int
authid_release_locked(authid_window_t *window,
                      bfpilot_appinst_result_t *result) {
  if(!window->changed) return result->restore_authid_result;
  int last_rc = -EIO;
  for(int attempt = 0; attempt < 3; attempt++) {
    int rc = kernel_set_ucred_authid(getpid(), window->saved);
    uint64_t current = kernel_get_ucred_authid(getpid());
    if(rc == 0 && current == window->saved) {
      result->restore_authid_result = 0;
      bfpilot_log("AppInst auth-ID restored value=0x%016llx attempts=%d",
                  (unsigned long long)window->saved, attempt + 1);
      return 0;
    }
    last_rc = rc != 0 ? rc : -EIO;
  }
  result->restore_authid_result = last_rc;
  bfpilot_log("AppInst auth-ID restore FAILED saved=0x%016llx rc=%d",
              (unsigned long long)window->saved, last_rc);
  return last_rc;
}

static void
result_init(bfpilot_appinst_result_t *result) {
  memset(result, 0, sizeof(*result));
  result->result = BFPILOT_DIAG_SKIPPED;
  result->authid_result = BFPILOT_DIAG_SKIPPED;
  result->init_result = BFPILOT_DIAG_SKIPPED;
  result->primary_result = BFPILOT_DIAG_SKIPPED;
  result->fallback_result = BFPILOT_DIAG_SKIPPED;
  result->restore_authid_result = BFPILOT_DIAG_SKIPPED;
}

static void *
initialize_thread(void *unused) {
  (void)unused;
  int rc = sceAppInstUtilInitialize();
  atomic_store_explicit(&g_appinst_init_rc, rc, memory_order_relaxed);
  atomic_store_explicit(&g_appinst_init_done, 1, memory_order_release);
  return NULL;
}

static int
initialize_locked(bfpilot_appinst_result_t *result) {
  if(!g_appinst_init_started) {
    atomic_store_explicit(&g_appinst_init_done, 0, memory_order_relaxed);
    atomic_store_explicit(&g_appinst_init_rc, -EINPROGRESS,
                          memory_order_relaxed);
    pthread_t thread;
    int thread_rc = pthread_create(&thread, NULL, initialize_thread, NULL);
    if(thread_rc != 0) {
      result->init_result = -thread_rc;
      bfpilot_log("AppInst initialize thread failed rc=%d", thread_rc);
      return result->init_result;
    }
    (void)pthread_detach(thread);
    g_appinst_init_started = 1;
  }

  for(int attempt = 0; attempt < BFPILOT_APPINST_INIT_TIMEOUT_TENTHS;
      attempt++) {
    if(atomic_load_explicit(&g_appinst_init_done, memory_order_acquire)) {
      result->init_result = atomic_load_explicit(&g_appinst_init_rc,
                                                 memory_order_relaxed);
      bfpilot_log("AppInst initialize rc=0x%08x", result->init_result);
      return result->init_result;
    }
    usleep(100000);
  }
  result->init_result = -ETIMEDOUT;
  bfpilot_log("AppInst initialize timed out after %d ms",
              BFPILOT_APPINST_INIT_TIMEOUT_TENTHS * 100);
  return result->init_result;
}

static int
mkdir_checked(const char *path) {
  if(mkdir(path, 0755) == 0) return 0;
  if(errno != EEXIST) return -errno;
  struct stat st;
  if(lstat(path, &st) != 0) return -errno;
  return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
}

static int
file_matches(const char *path, const unsigned char *expected, size_t size) {
  struct stat before;
  struct stat opened;
  int flags = O_RDONLY;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  if(lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
     before.st_size != (off_t)size) return 0;
  int fd = open(path, flags);
  if(fd < 0) return 0;
  if(fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
     opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
     opened.st_size != before.st_size) {
    close(fd);
    return 0;
  }
  unsigned char buffer[16384];
  size_t offset = 0;
  int match = 1;
  while(offset < size) {
    size_t wanted = size - offset < sizeof(buffer)
        ? size - offset : sizeof(buffer);
    ssize_t got = read(fd, buffer, wanted);
    if(got < 0 && errno == EINTR) continue;
    if(got <= 0 || memcmp(buffer, expected + offset, (size_t)got) != 0) {
      match = 0;
      break;
    }
    offset += (size_t)got;
  }
  if(close(fd) != 0) match = 0;
  return match && offset == size;
}

static int
write_atomic(const char *temporary, const char *destination,
             const unsigned char *data, size_t size) {
  (void)unlink(temporary);
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int fd = open(temporary, flags, 0644);
  if(fd < 0) return -errno;
  size_t offset = 0;
  int rc = 0;
  while(offset < size) {
    ssize_t written = write(fd, data + offset, size - offset);
    if(written < 0 && errno == EINTR) continue;
    if(written <= 0) {
      rc = written < 0 ? -errno : -EIO;
      break;
    }
    offset += (size_t)written;
  }
  if(rc == 0 && fsync(fd) != 0) rc = -errno;
  if(close(fd) != 0 && rc == 0) rc = -errno;
  if(rc == 0 && rename(temporary, destination) != 0) rc = -errno;
  if(rc != 0) (void)unlink(temporary);
  return rc;
}

static int
install_tile_locked(bfpilot_appinst_result_t *result) {
  char app_dir[256];
  char sce_sys_dir[256];
  char param_path[320];
  char icon_path[320];
  char marker_path[320];
  char param_tmp[352];
  char icon_tmp[352];
  char marker_tmp[352];
  snprintf(app_dir, sizeof(app_dir), BFPILOT_APP_ROOT "/%s",
           BFPILOT_APP_TITLE_ID);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);
  snprintf(marker_path, sizeof(marker_path), "%s/.bfpilot-registered", app_dir);
  snprintf(param_tmp, sizeof(param_tmp), "%s.bfpilot-tmp", param_path);
  snprintf(icon_tmp, sizeof(icon_tmp), "%s.bfpilot-tmp", icon_path);
  snprintf(marker_tmp, sizeof(marker_tmp), "%s.bfpilot-tmp", marker_path);

  int rc = mkdir_checked(BFPILOT_APP_ROOT);
  if(rc == 0) rc = mkdir_checked(app_dir);
  if(rc == 0) rc = mkdir_checked(sce_sys_dir);
  if(rc == 0) rc = write_atomic(param_tmp, param_path, bfpilot_param_json,
                                bfpilot_param_json_size);
  if(rc == 0) rc = write_atomic(icon_tmp, icon_path, bfpilot_icon0_png,
                                bfpilot_icon0_png_size);
  if(rc != 0) return rc;
  result->user_app_writable = 1;

  uint32_t handle = 0;
  int handle_rc = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle);
  app_install_title_dir_fn title_dir = handle_rc == 0
      ? (app_install_title_dir_fn)kernel_dynlib_resolve(
            -1, handle, "Wudg3Xe3heE")
      : NULL;
  result->title_dir_resolved = title_dir != NULL;
  result->install_all_resolved = 1;
  result->primary_result = title_dir
      ? title_dir(BFPILOT_APP_TITLE_ID, BFPILOT_APP_PARENT, NULL)
      : -ENOSYS;
  if(title_dir) {
    rc = result->primary_result;
  } else {
    result->fallback_result = sceAppInstUtilAppInstallAll(NULL);
    rc = result->fallback_result;
  }
  if(rc == 0) {
    rc = write_atomic(marker_tmp, marker_path, g_tile_registration_marker,
                      sizeof(g_tile_registration_marker) - 1);
    if(rc != 0) {
      bfpilot_log("AppInst tile registered but marker write failed rc=%d", rc);
    }
  }
  return rc;
}

int
bfpilot_appinst_install_tile(bfpilot_appinst_result_t *result) {
  if(!result) return -EINVAL;
  result_init(result);
  char app_dir[256];
  char param_path[320];
  char icon_path[320];
  char marker_path[320];
  snprintf(app_dir, sizeof(app_dir), BFPILOT_APP_ROOT "/%s/sce_sys",
           BFPILOT_APP_TITLE_ID);
  snprintf(param_path, sizeof(param_path), "%s/param.json", app_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", app_dir);
  snprintf(marker_path, sizeof(marker_path), BFPILOT_APP_ROOT "/%s/.bfpilot-registered",
           BFPILOT_APP_TITLE_ID);
  if(file_matches(param_path, bfpilot_param_json, bfpilot_param_json_size) &&
     file_matches(icon_path, bfpilot_icon0_png, bfpilot_icon0_png_size) &&
     file_matches(marker_path, g_tile_registration_marker,
                  sizeof(g_tile_registration_marker) - 1)) {
    result->result = 0;
    result->user_app_writable = 1;
    result->skipped_unchanged = 1;
    bfpilot_log("AppInst tile unchanged; Sony calls skipped");
    return 0;
  }

  pthread_mutex_lock(&g_appinst_lock);
  atomic_store(&g_appinst_busy, 1);
  authid_window_t authid;
  (void)authid_acquire_locked(&authid, result);
  int rc = initialize_locked(result);
  if(rc == 0) rc = install_tile_locked(result);
  (void)authid_release_locked(&authid, result);
  result->result = rc;
  atomic_store(&g_appinst_busy, 0);
  pthread_mutex_unlock(&g_appinst_lock);
  bfpilot_log("AppInst tile direct rc=0x%08x init=0x%08x primary=0x%08x "
              "fallback=0x%08x", rc, result->init_result,
              result->primary_result, result->fallback_result);
  return rc;
}

int
bfpilot_appinst_busy(void) {
  return atomic_load(&g_appinst_busy) != 0;
}
