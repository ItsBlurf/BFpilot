#pragma once
#include <stdint.h>
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BFMERGE_MAX_ENTRIES 250000U
typedef struct bfpilot_merge_options {
  void *context;
  int (*cancelled)(void *);
  /* Called during preflight and again immediately before mutation. A negative
     errno protects an active control path or pending source from overwrite. */
  int (*protect)(void *, const char *destination, const struct stat *existing);
  void (*progress)(void *, const char *relative, uint32_t done, uint32_t total);
} bfpilot_merge_options_t;
typedef struct bfpilot_merge_result {
  uint64_t files, bytes;
  uint32_t entries, published;
  char error_path[1024];
} bfpilot_merge_result_t;
/* Merge only into an existing local same-volume directory. On a fresh call,
   finalize/sync the entire stage and validate conflicts before publication.
   Retain an immutable CRC manifest at the explicit private internal path until
   the caller has durably published terminal status and settled helper
   ownership/ACK. Complete staged files replace old files atomically; unrelated
   entries stay. Partial publication is possible on cancel/I/O error; retry
   reconciles exact recorded identities rather than re-extracting or guessing
   about missing data. A missing stage root is accepted only if every recorded
   output is verified. This is process-recovery support, not a power-loss or
   concurrent-writer claim. */
int bfpilot_archive_merge(const char *stage, const char *destination,
                          const char *request_id, const char *manifest_path,
                          const bfpilot_merge_options_t *,
                          bfpilot_merge_result_t *);
/* Caller must first settle helper ownership. Verifies the bound receipt before
   removing it; does not remove arbitrary stages or destination contents. */
int bfpilot_archive_merge_forget(const char *stage, const char *destination,
                                 const char *request_id,
                                 const char *manifest_path);
#ifdef __cplusplus
}
#endif
