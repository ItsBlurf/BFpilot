/*
 * BFpilot - direct file serving helpers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "websrv.h"

typedef struct bfpilot_download_stats {
  long          elapsed_ms;
  long          read_ms;
  long          send_ms;
  unsigned long bytes;
  unsigned long source_dev;
  unsigned int  buffer_size;
  int           buffer_aligned;
  int           error_code;
  char          path[512];
} bfpilot_download_stats_t;


int fs_request(const http_request_t *req, const char *url);

uint8_t *fs_readfile(const char *path, size_t *size);

void bfpilot_fs_get_download_stats(bfpilot_download_stats_t *out);
