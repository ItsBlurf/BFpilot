/* Integrated asynchronous PKG submission. */

#pragma once

#include <stddef.h>

#include "websrv.h"

#define BFPILOT_PKG_MAX_PATHS 2048U

int bfpilot_pkg_install_start(const char *const *paths, size_t path_count,
                              char *error, size_t error_size);
int bfpilot_pkg_status_request(const http_request_t *req);
int bfpilot_pkg_is_busy(void);
