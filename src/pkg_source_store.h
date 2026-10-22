#pragma once
#include "pkg_source_wire.h"
typedef struct bfps_source_handle {
  size_t slot;
  char token[33];
  char path[BFPILOT_PKG_SOURCE_PATH_SIZE];
  bfpilot_pkg_source_stamp_t stamp;
} bfps_source_handle_t;
int bfps_store_init(void);
int bfps_store_register(const char *, const bfpilot_pkg_source_stamp_t *,
                        char[33]);
int bfps_store_drop(const char *);
int bfps_store_acquire(const char *, bfps_source_handle_t *);
void bfps_store_release(const bfps_source_handle_t *);
void bfps_store_touch(const bfps_source_handle_t *);
void bfps_store_expire(void);
void bfps_store_stats(unsigned *, unsigned *);
