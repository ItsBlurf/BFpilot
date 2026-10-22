#ifndef BFPILOT_FULL_ARCHIVE_CLIENT_H
#define BFPILOT_FULL_ARCHIVE_CLIENT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct bfpilot_full_archive_callbacks {
  void *context;
  int (*started)(void *context, uint64_t task_id);
  void (*accepted)(void *context);
  int (*cancelled)(void *context);
  void (*progress)(void *context, uint64_t completed, uint64_t total);
  void (*current)(void *context, const char *path);
} bfpilot_full_archive_callbacks_t;
/* quiescent is true only when the service confirms that extraction stopped.
 * An uncertain transport failure must retain its private stage. */
int bfpilot_full_archive_extract(const char *source, const char *destination,
    const char *password, const bfpilot_full_archive_callbacks_t *callbacks,
    char *error, size_t error_size, int *quiescent);
/* Attach only when the service's retained source/stage identity matches. */
int bfpilot_full_archive_resume(uint64_t task_id, const char *source,
    const char *destination, const bfpilot_full_archive_callbacks_t *callbacks,
    char *error, size_t error_size, int *quiescent);
int bfpilot_full_archive_probe(void);
/* Call after publication/cleanup and removal of the recovery marker. */
int bfpilot_full_archive_ack(uint64_t task_id);
/* Stops only this application's idle helper through its private protocol. */
int bfpilot_full_archive_stop(void);
#ifdef __cplusplus
}
#endif
#endif
