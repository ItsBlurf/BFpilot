#pragma once
#include "pkg_source_wire.h"
int bfpilot_pkg_source_ping(void);
int bfpilot_pkg_source_ensure(void);
int bfpilot_pkg_source_register(const char *, const bfpilot_pkg_source_stamp_t *, char *, size_t);
int bfpilot_pkg_source_drop(const char *);
int bfpilot_pkg_source_stats(unsigned *, unsigned *);
int bfpilot_pkg_source_stop(void);
