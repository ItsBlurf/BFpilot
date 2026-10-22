#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BFAB_MAX_ITEMS 2048U
#define BFAB_PATH_SIZE 1024U
#define BFAB_ID_SIZE 64U
#define BFAB_PASSWORD_SIZE 512U
#define BFAB_MANIFEST_MAX (76U + BFAB_ID_SIZE + BFAB_PASSWORD_SIZE + \
                           BFAB_MAX_ITEMS * (16U + 2U * BFAB_PATH_SIZE))
#define BFAB_EVENT_SIZE 100U
enum { BFAB_MERGE = 1, BFAB_CLEANUP = 2, BFAB_CANCEL_REQUESTED = 4 };
enum { BFAB_PREPARED = 1, BFAB_RUNNING, BFAB_FINISHED, BFAB_CANCELLED,
       BFAB_ERROR };
enum { BFAB_PENDING = 0, BFAB_ACTIVE, BFAB_DONE, BFAB_FAILED };
enum { BFAB_BEGIN = 1, BFAB_FINISH, BFAB_CANCEL, BFAB_HALT };

typedef struct bfab_item {
  char source[BFAB_PATH_SIZE], destination[BFAB_PATH_SIZE];
  uint32_t state;
  int32_t result;
} bfab_item_t;
typedef struct bfab_job {
  char id[BFAB_ID_SIZE], password[BFAB_PASSWORD_SIZE];
  bfab_item_t *items;
  uint32_t count, next, succeeded, failed, state, threads, flags;
  int32_t nice, error;
  uint64_t revision, created;
} bfab_job_t;
typedef struct bfab_event {
  char id[BFAB_ID_SIZE];
  uint64_t sequence;
  uint32_t kind, index;
  int32_t result;
} bfab_event_t;

/* No I/O or allocation. Local paths retain arbitrary non-control FS bytes. */
int bfab_validate(const bfab_job_t *);
int bfab_manifest_encode(const bfab_job_t *, void *, size_t, size_t *);
/* Output is cleared on failure; caller owns item storage in all cases. */
int bfab_manifest_decode(const void *, size_t, bfab_job_t *, bfab_item_t *, size_t);
int bfab_event_encode(const bfab_event_t *, void *, size_t);
int bfab_event_decode(const void *, size_t, bfab_event_t *);
/* Input job must be decoded/validated and owned by one locked coordinator.
   Check before appending; apply only after the complete event is durable.
   Both reject stale/foreign events without changing any job/item bytes. */
int bfab_event_check(const bfab_job_t *, const bfab_event_t *);
int bfab_event_apply(bfab_job_t *, const bfab_event_t *);

#ifdef __cplusplus
}
#endif
