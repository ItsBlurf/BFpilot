/*
 * Launch local .elf / .bin payloads through elfldr (127.0.0.1:9021).
 */

#pragma once

#include "websrv.h"

#ifndef BFPILOT_ELFLDR_PORT
#define BFPILOT_ELFLDR_PORT 9021
#endif

/* Stream path to elfldr. Returns 0 on success. */
int bfpilot_launch_payload_path(const char *path);

/* HTTP: GET /api/fs/launch?path=... */
int bfpilot_launch_request(const http_request_t *req);
