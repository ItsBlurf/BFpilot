#ifndef BFPILOT_STAGING_H
#define BFPILOT_STAGING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* parent must exist. Select a private stage on the same filesystem, outside
 * homebrew game directories. The caller creates the leaf exclusively. */
int bfpilot_stage_path(const char *parent, const char *kind, char *out, size_t size);
int bfpilot_stage_path_avoiding(const char *parent, const char *kind,
                               const char *source, char *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
