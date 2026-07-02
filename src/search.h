/*
 * BFpilot - indexed filename/path search.
 */

#pragma once

#include "websrv.h"

int bfpilot_search_request(const http_request_t *req, const char *url);
void bfpilot_search_mark_stale(const char *reason);

