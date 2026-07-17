/*
 * BFpilot - direct AppInst launcher tile installer.
 *
 * This intentionally uses the same direct AppInstUtil pattern as websrv,
 * Payload Manager, and ftpsrv. If a loader rejects AppInstUtil imports, this
 * ELF can fail before main; in that case there will be no boot marker in
 * /data/BFpilot/boot.log and no launcher-installer.log entry.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "boot_marker.h"
#include "notify.h"
#include "version.h"

#include "paths.h"

#define BFPILOT_DIAG_SKIPPED (-2147483000)
#define BFPILOT_APPINST_AUTHID UINT64_C(0x4801000000000013)

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_end\n"                                            \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                            \
          ".incbin \"" file "\"\n" #name "_end:\n" #name "_size:\n"            \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const uint8_t name[];                                                 \
  extern const size_t name##_size

INCASSET(bfpilot_param_json, "assets-app/param.json");
INCASSET(bfpilot_icon0_png, "assets-app/icon0.png");

typedef int (*app_install_title_dir_fn)(const char *, const char *, void *);

int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);


static int
mkdir_if_needed(const char *path) {
  if(mkdir(path, 0755) == 0) return 0;
  return errno == EEXIST ? 0 : -errno;
}


static void
ensure_log_dir(void) {
  (void)mkdir_if_needed(BFPILOT_DATA_ROOT);
  (void)mkdir_if_needed(BFPILOT_DATA_DIR);
}


static void
installer_log(const char *fmt, ...) {
  char body[1024];
  char line[1400];
  va_list ap;

  va_start(ap, fmt);
  int body_n = vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
  va_end(ap);
  if(body_n < 0) {
    snprintf(body, sizeof(body), "log format failed");
  } else if((size_t)body_n >= sizeof(body)) {
    body[sizeof(body) - 1] = 0;
  }

  int n = snprintf(line, sizeof(line), "%ld pid=%ld %s\n",
                   (long)time(NULL), (long)getpid(), body);
  if(n < 0) return;
  if((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

  fputs(line, stdout);
  fflush(stdout);

  ensure_log_dir();
  int fd = open(BFPILOT_LAUNCHER_LOG,
                O_WRONLY | O_CREAT | O_APPEND, 0600);
  if(fd >= 0) {
    (void)write(fd, line, (size_t)n);
    close(fd);
  }
}


static int
write_file(const char *path, const uint8_t *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) return -errno;

  size_t done = 0;
  while(done < size) {
    ssize_t wr = write(fd, data + done, size - done);
    if(wr < 0) {
      int err = errno;
      close(fd);
      return -err;
    }
    done += (size_t)wr;
  }

  if(close(fd) != 0) return -errno;
  return 0;
}


int
main(void) {
  bfpilot_boot_marker("bfpilot-launcher-installer", BFPILOT_BUILD_MODE);

  char app_dir[256];
  char sce_sys_dir[256];
  char param_path[256];
  char icon_path[256];
  int title_dir_rc = BFPILOT_DIAG_SKIPPED;
  int install_all_rc = BFPILOT_DIAG_SKIPPED;
  int final_rc = -1;
  pid_t pid = getpid();
  uint64_t original_authid = kernel_get_ucred_authid(pid);
  int set_authid_rc;
  int restore_authid_rc = BFPILOT_DIAG_SKIPPED;
  int user_service_rc;

  installer_log("entered main websrv-pattern-appinst tag=%s build=%s",
                VERSION_TAG, BUILD_VERSION);
  installer_log("launcher metadata title_id=%s deeplink=http://127.0.0.1:5905/ "
                "param_bytes=%lu icon_bytes=%lu icon_png_magic=%s",
                BFPILOT_APP_TITLE_ID, (unsigned long)bfpilot_param_json_size,
                (unsigned long)bfpilot_icon0_png_size,
                bfpilot_icon0_png_size >= 8 &&
                bfpilot_icon0_png[0] == 0x89 && bfpilot_icon0_png[1] == 'P' &&
                bfpilot_icon0_png[2] == 'N' && bfpilot_icon0_png[3] == 'G'
                    ? "yes" : "no");
  bfpilot_notify("BFpilot launcher installer", "websrv-pattern installer");

  user_service_rc = sceUserServiceInitialize(NULL);
  installer_log("sceUserServiceInitialize return code=0x%08x",
                user_service_rc);
  installer_log("privilege context original_authid=0x%016llx "
                "target_authid=0x%016llx",
                (unsigned long long)original_authid,
                (unsigned long long)BFPILOT_APPINST_AUTHID);
  set_authid_rc = kernel_set_ucred_authid(pid, BFPILOT_APPINST_AUTHID);
  installer_log("kernel_set_ucred_authid target return code=0x%08x "
                "current_authid=0x%016llx",
                set_authid_rc,
                (unsigned long long)kernel_get_ucred_authid(pid));
  if(set_authid_rc != 0) {
    installer_log("final result=privilege_setup_failed final_rc=%d "
                  "set_authid_rc=0x%08x", final_rc, set_authid_rc);
    sceUserServiceTerminate();
    bfpilot_notify("BFpilot launcher installer", "system authid setup failed");
    return 1;
  }

  installer_log("about to sceAppInstUtilInitialize");
  int appinst_init_rc = sceAppInstUtilInitialize();
  installer_log("AppInst init return code=0x%08x", appinst_init_rc);
  if(appinst_init_rc != 0) {
    if(original_authid != 0) {
      restore_authid_rc = kernel_set_ucred_authid(pid, original_authid);
      installer_log("kernel_set_ucred_authid restore return code=0x%08x "
                    "current_authid=0x%016llx",
                    restore_authid_rc,
                    (unsigned long long)kernel_get_ucred_authid(pid));
    }
    installer_log("final result=failed_nonfatal final_rc=%d "
                  "appinst_init_rc=0x%08x restore_authid_rc=0x%08x",
                  final_rc, appinst_init_rc, restore_authid_rc);
    sceUserServiceTerminate();
    bfpilot_notify("BFpilot launcher installer", "AppInst init failed");
    return 1;
  }
  installer_log("home tile category=Media applicationCategoryType=65536");

  snprintf(app_dir, sizeof(app_dir), BFPILOT_APP_ROOT "/%s",
           BFPILOT_APP_TITLE_ID);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

  installer_log("existing tile uninstall skipped: non-destructive refresh");

  errno = 0;
  int mkdir_app_rc = mkdir_if_needed(app_dir);
  installer_log("%s mkdir return code=%d errno=%d",
                app_dir, mkdir_app_rc, errno);
  errno = 0;
  int mkdir_sce_sys_rc = mkdir_if_needed(sce_sys_dir);
  installer_log("%s mkdir return code=%d errno=%d",
                sce_sys_dir, mkdir_sce_sys_rc, errno);
  errno = 0;
  int write_param_rc = write_file(param_path, bfpilot_param_json,
                                  bfpilot_param_json_size);
  installer_log("%s write return code=%d errno=%d",
                param_path, write_param_rc, errno);
  errno = 0;
  int write_icon_rc = write_file(icon_path, bfpilot_icon0_png,
                                 bfpilot_icon0_png_size);
  installer_log("%s write return code=%d errno=%d",
                icon_path, write_icon_rc, errno);

  uint32_t appinst_handle = 0;
  int handle_rc = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx",
                                       &appinst_handle);
  app_install_title_dir_fn install_title_dir = handle_rc == 0 ?
      (app_install_title_dir_fn)kernel_dynlib_resolve(
          -1, appinst_handle, "Wudg3Xe3heE") : NULL;
  installer_log("AppInstallTitleDir resolved=%s handle_rc=0x%08x",
                install_title_dir ? "yes" : "no", handle_rc);

  if(mkdir_app_rc == 0 && mkdir_sce_sys_rc == 0 &&
     write_param_rc == 0 && write_icon_rc == 0) {
    if(install_title_dir) {
      title_dir_rc = install_title_dir(BFPILOT_APP_TITLE_ID,
                                       BFPILOT_APP_PARENT, NULL);
      installer_log("AppInstallTitleDir return code=0x%08x", title_dir_rc);
    } else {
      installer_log("AppInstallTitleDir return code=skipped");
    }

    if(title_dir_rc != 0) {
      installer_log("about to sceAppInstUtilAppInstallAll fallback");
      install_all_rc = sceAppInstUtilAppInstallAll(NULL);
      installer_log("AppInstallAll fallback return code=0x%08x",
                    install_all_rc);
    } else {
      installer_log("AppInstallAll fallback return code=skipped");
    }
  } else {
    installer_log("AppInstallTitleDir return code=skipped");
    installer_log("AppInstallAll fallback return code=skipped");
  }

  if(title_dir_rc == 0 || install_all_rc == 0) {
    final_rc = 0;
  }

  if(original_authid != 0) {
    restore_authid_rc = kernel_set_ucred_authid(pid, original_authid);
    installer_log("kernel_set_ucred_authid restore return code=0x%08x "
                  "current_authid=0x%016llx",
                  restore_authid_rc,
                  (unsigned long long)kernel_get_ucred_authid(pid));
  }

  installer_log("final result=%s final_rc=%d appinst_init_rc=0x%08x "
                "user_service_rc=0x%08x "
                "mkdir_app_rc=%d mkdir_sce_sys_rc=%d "
                "write_param_rc=%d write_icon_rc=%d title_dir_rc=0x%08x "
                "install_all_rc=0x%08x set_authid_rc=0x%08x "
                "restore_authid_rc=0x%08x",
                final_rc == 0 ? "installed" : "failed_nonfatal", final_rc,
                appinst_init_rc, user_service_rc, mkdir_app_rc,
                mkdir_sce_sys_rc, write_param_rc, write_icon_rc,
                title_dir_rc, install_all_rc, set_authid_rc,
                restore_authid_rc);

  sceAppInstUtilTerminate();
  sceUserServiceTerminate();
  if(final_rc == 0) {
    bfpilot_notify("BFpilot launcher installer",
                   "Media tile installed or refreshed");
    return 0;
  }

  bfpilot_notify("BFpilot launcher installer", "failed; see installer log");
  return 1;
}
