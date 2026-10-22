#pragma once
#include "archive_batch_codec.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Caller serializes all operations and holds the archive daemon ownership lock.
   Open checks private directory identity/mode; returned fd belongs to caller. */
int bfab_store_open(const char *directory);
/* Only new PREPARED revision-1 manifests can be published. File fsync + checked
   close + same-directory rename; this is not a power-loss durability guarantee. */
int bfab_store_publish(int directory, const bfab_job_t *);
/* -ENOENT means no batch. A valid prefix with a short final event is recovered
   and truncated/synced before returning; a bad complete event is retained and
   rejected. Output job is cleared on failure. */
int bfab_store_load(int directory, bfab_job_t *, bfab_item_t *, size_t,
                    unsigned *torn_bytes);
/* Checks transition, appends/fsyncs/closes, then applies in memory. On any error
   the caller MUST stop dispatch; a failed append can have an uncertain tail. */
int bfab_store_append(int directory, bfab_job_t *, const bfab_event_t *);
/* Remove only an explicitly known previous batch's event file, after the new
   manifest has been committed. Does not remove the current manifest. */
int bfab_store_retire(int directory, const char *previous_id);
#ifdef __cplusplus
}
#endif
