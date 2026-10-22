#pragma once
#include "pkg_file.h"
int bfpilot_pkg_remote_validate(const char *,bfpilot_pkg_file_info_t *,char *,size_t);
int bfpilot_pkg_remote_details(const char *,bfpilot_pkg_details_t *);
int bfpilot_pkg_remote_icon(const char *,void **,size_t *);
