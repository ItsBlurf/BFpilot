/*
 * Path helpers shared by transfer API (safe to unit-test on host).
 */

#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Absolute path, no ".." segments. */
int bfpilot_path_is_safe(const char *path);

/*
 * Resolve absolute path with case-insensitive matching of each segment.
 * Writes the real on-disk path into out. Returns 0 on success, -errno on failure.
 */
int bfpilot_path_resolve_ci(const char *in, char *out, size_t out_sz);

/* chmod/fchmod path to mode (e.g. 0444). Returns 0 on success, -errno on failure. */
int bfpilot_path_chmod(const char *path, mode_t mode);

/* Parse octal mode string "444" / "0444" / "755". Returns -1 on bad input. */
int bfpilot_parse_mode_octal(const char *s, mode_t *out_mode);
