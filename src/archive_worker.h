/*
 * BFpilot - archive extraction entry points.
 */

#pragma once

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
void bfpilot_archive_stop_daemon(void);

#ifdef __cplusplus
}
#endif
