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
#ifdef __cplusplus
}
#endif
