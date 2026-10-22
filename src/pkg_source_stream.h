#pragma once
#include "pkg_source_store.h"
#include "remote_io.h"
typedef struct bfps_stream {
  bfpilot_io_t *endpoint;
  bfpilot_io_file_t *file;
  bfpilot_pkg_source_stamp_t stamp;
} bfps_stream_t;
int bfps_stream_open(const bfps_source_handle_t *, bfps_stream_t *);
int bfps_stream_unchanged(bfps_stream_t *);
int bfps_stream_close(bfps_stream_t *, int);
