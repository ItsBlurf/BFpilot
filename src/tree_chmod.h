#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Same-device, no-symlink walk. Returns zero or a negative errno. */
int bfpilot_tree_chmod(const char *path, mode_t mode,
                       int (*cancelled)(void *), void *context);
/* Archive publication gate: apply permissions and check every file's fsync. */
int bfpilot_tree_finalize(const char *path, mode_t mode,
                          int (*cancelled)(void *), void *context);
typedef struct bfpilot_tree_totals {
  unsigned long long files, bytes;
} bfpilot_tree_totals_t;
int bfpilot_tree_finalize_stats(const char *path, mode_t mode,
    int (*cancelled)(void *), void *context, bfpilot_tree_totals_t *totals);
#ifdef __cplusplus
}
#endif
