/* AppInst operations shipped inside the single BFpilot ELF. */

#pragma once

#include <stdint.h>

#define BFPILOT_APPINST_CONTENT_ID_CAPACITY 48U

typedef struct bfpilot_appinst_result {
  int32_t result;
  int32_t firmware_major;
  int32_t authid_result;
  int32_t init_result;
  int32_t primary_result;
  int32_t fallback_result;
  int32_t restore_authid_result;
  int32_t user_app_writable;
  int32_t title_dir_resolved;
  int32_t install_all_resolved;
  int32_t skipped_unchanged;
  int32_t used_embedded_worker;
  char content_id[BFPILOT_APPINST_CONTENT_ID_CAPACITY];
} bfpilot_appinst_result_t;

int bfpilot_appinst_install_tile(bfpilot_appinst_result_t *result);
int bfpilot_appinst_install_pkg(const char *path, const char *content_name,
                                bfpilot_appinst_result_t *result);
int bfpilot_appinst_busy(void);
int bfpilot_appinst_firmware_major(void);
int bfpilot_pkg_worker_stop(void);
