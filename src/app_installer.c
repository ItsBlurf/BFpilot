/*
 * PS5 Media home-tile installer for BFpilot (embedded in main ELF).
 * Same pattern as Payload Manager app_installer.c + websrv AppInstUtil.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "app_installer.h"
#include "diag.h"
#include "notify.h"
#include "paths.h"

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

static int
needs_update(const char *path, const uint8_t *expected, size_t expected_size) {
  struct stat st;
  uint8_t *buf;
  FILE *f;
  int mismatch;

  if(stat(path, &st) != 0) return 1;
  if((size_t)st.st_size != expected_size) return 1;

  f = fopen(path, "rb");
  if(!f) return 1;
  buf = (uint8_t *)malloc(expected_size);
  if(!buf) {
    fclose(f);
    return 1;
  }
  if(fread(buf, 1, expected_size, f) != expected_size) {
    free(buf);
    fclose(f);
    return 1;
  }
  fclose(f);
  mismatch = memcmp(buf, expected, expected_size) != 0;
  free(buf);
  return mismatch;
}

int
bfpilot_install_app_if_needed(void) {
  char app_dir[256];
  char sce_sys_dir[256];
  char param_path[256];
  char icon_path[256];
  struct stat st;
  int update_needed = 0;
  int final_rc = -1;
  pid_t pid = getpid();
  uint64_t original_authid;
  int set_authid_rc;
  int restore_authid_rc = BFPILOT_DIAG_SKIPPED;
  int user_service_rc;
  int appinst_init_rc;
  int title_dir_rc = BFPILOT_DIAG_SKIPPED;
  int install_all_rc = BFPILOT_DIAG_SKIPPED;
  uint32_t appinst_handle = 0;
  app_install_title_dir_fn install_title_dir = NULL;

  snprintf(app_dir, sizeof(app_dir), BFPILOT_APP_ROOT "/%s",
           BFPILOT_APP_TITLE_ID);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

  if(stat(app_dir, &st) != 0) {
    update_needed = 1;
  } else {
    if(needs_update(param_path, bfpilot_param_json, bfpilot_param_json_size))
      update_needed = 1;
    if(needs_update(icon_path, bfpilot_icon0_png, bfpilot_icon0_png_size))
      update_needed = 1;
  }

  if(!update_needed) {
    bfpilot_log("app installer: Media tile already up to date title_id=%s",
                BFPILOT_APP_TITLE_ID);
    bfpilot_diag_set_launcher_status("up_to_date", 0);
    return 0;
  }

  bfpilot_log("app installer: installing/updating Media tile title_id=%s "
              "category=65536 deeplink=%s",
              BFPILOT_APP_TITLE_ID, BFPILOT_DEEPLINK_URI);
  bfpilot_notify("BFpilot", "Installing Media home tile...");

  original_authid = kernel_get_ucred_authid(pid);
  user_service_rc = sceUserServiceInitialize(NULL);
  bfpilot_log("app installer: sceUserServiceInitialize rc=0x%08x",
              user_service_rc);

  set_authid_rc = kernel_set_ucred_authid(pid, BFPILOT_APPINST_AUTHID);
  bfpilot_log("app installer: set authid rc=0x%08x", set_authid_rc);
  if(set_authid_rc != 0) {
    bfpilot_diag_set_launcher_status("authid_failed", set_authid_rc);
    sceUserServiceTerminate();
    bfpilot_notify("BFpilot", "Media tile: auth setup failed");
    return -1;
  }

  appinst_init_rc = sceAppInstUtilInitialize();
  bfpilot_log("app installer: AppInst init rc=0x%08x", appinst_init_rc);
  if(appinst_init_rc != 0) {
    if(original_authid != 0)
      restore_authid_rc = kernel_set_ucred_authid(pid, original_authid);
    bfpilot_diag_set_launcher_status("appinst_init_failed", appinst_init_rc);
    sceUserServiceTerminate();
    bfpilot_notify("BFpilot", "Media tile: AppInst init failed");
    (void)restore_authid_rc;
    return -1;
  }

  (void)mkdir_if_needed(app_dir);
  (void)mkdir_if_needed(sce_sys_dir);

  if(write_file(param_path, bfpilot_param_json, bfpilot_param_json_size) != 0 ||
     write_file(icon_path, bfpilot_icon0_png, bfpilot_icon0_png_size) != 0) {
    bfpilot_log("app installer: failed writing param/icon under %s", app_dir);
    sceAppInstUtilTerminate();
    if(original_authid != 0)
      (void)kernel_set_ucred_authid(pid, original_authid);
    sceUserServiceTerminate();
    bfpilot_diag_set_launcher_status("write_failed", -1);
    return -1;
  }

  if(kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &appinst_handle) == 0) {
    install_title_dir = (app_install_title_dir_fn)kernel_dynlib_resolve(
        -1, appinst_handle, "Wudg3Xe3heE");
  }

  if(install_title_dir) {
    title_dir_rc =
        install_title_dir(BFPILOT_APP_TITLE_ID, BFPILOT_APP_PARENT, NULL);
    bfpilot_log("app installer: AppInstallTitleDir rc=0x%08x", title_dir_rc);
  }
  if(title_dir_rc != 0) {
    install_all_rc = sceAppInstUtilAppInstallAll(NULL);
    bfpilot_log("app installer: AppInstallAll fallback rc=0x%08x",
                install_all_rc);
  }

  if(title_dir_rc == 0 || install_all_rc == 0) final_rc = 0;

  if(original_authid != 0)
    restore_authid_rc = kernel_set_ucred_authid(pid, original_authid);
  (void)restore_authid_rc;

  sceAppInstUtilTerminate();
  sceUserServiceTerminate();

  if(final_rc == 0) {
    bfpilot_diag_set_launcher_status("installed", 0);
    bfpilot_notify("BFpilot", "Media home tile ready");
    bfpilot_log("app installer: Media tile installed/refreshed ok");
    return 0;
  }

  bfpilot_diag_set_launcher_status("failed", final_rc);
  bfpilot_notify("BFpilot", "Media tile install failed (see log)");
  return final_rc;
}
