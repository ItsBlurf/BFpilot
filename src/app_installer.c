/*
 * BS5FileManager - install the PS5 home-screen web launcher tile.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ps5/kernel.h>

#include "app_installer.h"
#include "notify.h"

#define BS5FM_APP_TITLE_ID "BS5F00001"
#define BS5FM_APP_ROOT "/user/app"
#define BS5FM_APP_PARENT BS5FM_APP_ROOT "/"
#define BS5FM_DATA_DIR "/data/BS5fm"
#define BS5FM_MARKER_PATH BS5FM_DATA_DIR "/launcher.ok"
#define BS5FM_INSTALL_MARKER "bs5filemanager-launcher-v3\n"

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

INCASSET(bs5fm_param_json, "assets-app/param.json");
INCASSET(bs5fm_icon0_png, "assets-app/icon0.png");

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

typedef int (*app_install_title_dir_fn)(const char *, const char *, void *);

static const uint8_t g_install_marker[] = BS5FM_INSTALL_MARKER;


static int
install_app(const char *title_id, const char *dir) {
  app_install_title_dir_fn sceAppInstUtilAppInstallTitleDir = NULL;
  uint32_t handle = 0;

  if(kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    sceAppInstUtilAppInstallTitleDir =
        (app_install_title_dir_fn)kernel_dynlib_resolve(-1, handle,
                                                        "Wudg3Xe3heE");
  }

  if(sceAppInstUtilAppInstallTitleDir) {
    return sceAppInstUtilAppInstallTitleDir(title_id, dir, NULL);
  }

  return sceAppInstUtilAppInstallAll(NULL);
}


static int
write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *file = fopen(path, "wb");
  if(!file) return -1;

  size_t written = fwrite(data, 1, size, file);
  int close_rc = fclose(file);

  return (written == size && close_rc == 0) ? 0 : -1;
}


static int
file_differs(const char *path, const uint8_t *expected, size_t expected_size) {
  struct stat st;
  if(stat(path, &st) != 0) return 1;
  if(st.st_size < 0 || (size_t)st.st_size != expected_size) return 1;

  FILE *file = fopen(path, "rb");
  if(!file) return 1;

  uint8_t *actual = malloc(expected_size ? expected_size : 1);
  if(!actual) {
    fclose(file);
    return 1;
  }

  size_t read = fread(actual, 1, expected_size, file);
  fclose(file);

  int differs = read != expected_size || memcmp(actual, expected, expected_size);
  free(actual);

  return differs;
}


static int
mkdir_if_needed(const char *path) {
  if(mkdir(path, 0755) == 0) return 0;
  return errno == EEXIST ? 0 : -1;
}


static int
ensure_data_dir(void) {
  if(mkdir_if_needed("/data") != 0) return -1;
  return mkdir_if_needed(BS5FM_DATA_DIR);
}


int
bs5fm_install_app_if_needed(void) {
  char app_dir[256];
  char sce_sys_dir[256];
  char param_path[256];
  char icon_path[256];
  struct stat st;

  snprintf(app_dir, sizeof(app_dir), BS5FM_APP_ROOT "/%s", BS5FM_APP_TITLE_ID);
  snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", app_dir);
  snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
  snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);

  int app_exists = stat(app_dir, &st) == 0;
  int needs_install = !app_exists ||
                      file_differs(param_path, bs5fm_param_json,
                                   bs5fm_param_json_size) ||
                      file_differs(icon_path, bs5fm_icon0_png,
                                   bs5fm_icon0_png_size) ||
                      file_differs(BS5FM_MARKER_PATH, g_install_marker,
                                   sizeof(g_install_marker) - 1);

  if(!needs_install) return 0;

  if(stat(app_dir, &st) == 0) {
    bs5fm_notify("BS5FileManager app", "Updating PS5 home-screen launcher");
  } else {
    bs5fm_notify("BS5FileManager app", "Installing PS5 home-screen launcher");
  }

  int err = sceAppInstUtilInitialize();
  if(err) {
    printf("  launcher install: sceAppInstUtilInitialize failed 0x%08x\n", err);
    return -1;
  }

  if(app_exists) {
    int uninstall_err = sceAppInstUtilAppUnInstall(BS5FM_APP_TITLE_ID);
    printf("  launcher install: refresh old tile 0x%08x\n", uninstall_err);
  }

  if(mkdir_if_needed(app_dir) != 0 || mkdir_if_needed(sce_sys_dir) != 0) {
    printf("  launcher install: mkdir failed errno %d\n", errno);
    return -1;
  }

  if(write_file(param_path, bs5fm_param_json, bs5fm_param_json_size) != 0) {
    printf("  launcher install: failed writing %s\n", param_path);
    return -1;
  }

  if(write_file(icon_path, bs5fm_icon0_png, bs5fm_icon0_png_size) != 0) {
    printf("  launcher install: failed writing %s\n", icon_path);
    return -1;
  }

  err = install_app(BS5FM_APP_TITLE_ID, BS5FM_APP_PARENT);
  if(err) {
    printf("  launcher install: install_app failed 0x%08x\n", err);
    return -1;
  }

  if(ensure_data_dir() != 0) {
    printf("  launcher install: warning, failed creating %s errno %d\n",
           BS5FM_DATA_DIR, errno);
  } else if(write_file(BS5FM_MARKER_PATH, g_install_marker,
                sizeof(g_install_marker) - 1) != 0) {
    printf("  launcher install: warning, failed writing %s\n",
           BS5FM_MARKER_PATH);
  }

  bs5fm_notify("BS5FileManager app ready",
               "Tile opens http://127.0.0.1:5905/");
  return 1;
}
