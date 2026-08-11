/*
 * Launch local .elf / .bin payloads through elfldr (127.0.0.1:9021).
 */

#pragma once

#include <stddef.h>

#include "websrv.h"

#ifndef BFPILOT_ELFLDR_PORT
#define BFPILOT_ELFLDR_PORT 9021
#endif

/* Stream path to elfldr. Returns 0 on success. */
int bfpilot_launch_payload_path(const char *path);

/* Stream an ELF embedded in the one-file BFpilot distribution. */
int bfpilot_launch_payload_memory(const void *data, size_t size,
                                  const char *label);

/* HTTP: GET /api/fs/launch?path=... */
int bfpilot_launch_request(const http_request_t *req);
