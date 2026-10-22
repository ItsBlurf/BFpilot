#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Same-device, no-symlink walk. Returns zero or a negative errno. */
int bfpilot_tree_chmod(const char *path, mode_t mode,
                       int (*cancelled)(void *), void *context);
#ifdef __cplusplus
}
#endif
