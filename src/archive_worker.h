/*
 * BFpilot - archive extraction entry points.
 */

#pragma once
#include "archive_batch_codec.h"
#include <sys/stat.h>

/* Shared runtime/API contract. Four was fastest in sustained PS5 RAR tests;
   manual tuning remains available up to eight. ZIP itself is serial. */
#define BFPILOT_ARCHIVE_MAX_THREADS 8U
#define BFPILOT_ARCHIVE_AUTO_MAX_THREADS 4U
#define BFPILOT_ARCHIVE_RAR_MAX_THREADS 8U

#ifdef __cplusplus
extern "C" {
#endif

int bfpilot_archive_run_prepared_job(void);
int bfpilot_archive_start_daemon(void);
/* Call only after both the job and prepared status have been published. */
void bfpilot_archive_notify_prepared(void);
void bfpilot_archive_stop_daemon(void);
/* Start requires the caller's ordinary archive reservation. Items are copied
   before return. The batch retains admission ownership between worker jobs. */
int bfpilot_archive_batch_start(const bfab_job_t *);
int bfpilot_archive_batch_busy(void);
int bfpilot_archive_batch_cancel(void);
/* malloc-owned JSON (caller frees); no passwords are exposed. */
int bfpilot_archive_batch_status(char **out, size_t *size);
int bfpilot_archive_batch_summary(char **out, size_t *size);
int bfpilot_archive_batch_protect_path(const char *, const struct stat *);

#ifdef __cplusplus
}
#endif
