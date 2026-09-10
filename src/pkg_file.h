/* Validated metadata from a local PS4/PS5 PKG container. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define BFPILOT_PKG_FORMAT_UNKNOWN 0U
#define BFPILOT_PKG_FORMAT_CNT 1U
#define BFPILOT_PKG_FORMAT_FIH 2U

typedef struct bfpilot_pkg_file_info {
  uint64_t size;
  uint32_t content_type;
  uint32_t content_flags;
  uint32_t format;
  char content_id[49];
  char title_id[16];
} bfpilot_pkg_file_info_t;

/* Pure header parser used by the file validator and host regression tests. */
int bfpilot_pkg_header_validate(const unsigned char *header,
                                size_t header_size, uint64_t file_size,
                                bfpilot_pkg_file_info_t *info);

/* Sony's InstallByPackage path is not safe for NPXS system packages. */
int bfpilot_pkg_file_is_system(const bfpilot_pkg_file_info_t *info);

/*
 * Validates a known CNT or FIH package header and regular-file identity.
 * CNT additionally validates its declared size and printable content ID;
 * FIH metadata is opaque and is returned by Sony after submission.
 */
int bfpilot_pkg_file_validate(const char *path,
                              bfpilot_pkg_file_info_t *info,
                              char *error, size_t error_size);

typedef struct bfpilot_pkg_field {
  char name[64];
  char value[256];
} bfpilot_pkg_field_t;

#define BFPILOT_PKG_MAX_FIELDS 64

typedef struct bfpilot_pkg_details {
  uint64_t size;
  uint32_t content_type;
  uint32_t content_flags;
  char content_id[64];
  char title_id[32];
  char title_name[256];
  char app_ver[32];
  char min_fw[32];
  char category[32];
  char platform[16]; /* "PS4" or "PS5" */
  int has_icon;
  uint64_t icon_offset;
  uint32_t icon_size;
  size_t field_count;
  bfpilot_pkg_field_t fields[BFPILOT_PKG_MAX_FIELDS];
} bfpilot_pkg_details_t;

int bfpilot_pkg_parse_details(const char *path, bfpilot_pkg_details_t *details);
int bfpilot_pkg_read_icon(const char *path, void **icon_buf, size_t *icon_size);

