/* Asynchronous Media-tile install through BFpilot's direct AppInst runtime. */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "appinst_direct.h"
#include "diag.h"
#include "notify.h"
#include "tile_bootstrap.h"
#include "websrv.h"

static atomic_int g_tile_started;
static atomic_int g_tile_busy;

static void *
tile_thread(void *unused) {
  (void)unused;
  bfpilot_appinst_result_t result;
  bfpilot_launcher_diag_t diag;
  memset(&diag, 0, sizeof(diag));
  int rc = bfpilot_appinst_install_tile(&result);

  diag.launcher_enabled = 1;
  diag.launcher_attempted = 1;
  diag.appinst_init_rc = result.init_result;
  diag.title_dir_resolved = result.title_dir_resolved;
  diag.install_title_dir_resolved = result.title_dir_resolved;
  diag.install_title_rc = result.primary_result;
  diag.uninstall_resolved = 0;
  diag.uninstall_rc = BFPILOT_DIAG_SKIPPED;
  diag.install_all_resolved = result.install_all_resolved;
  diag.install_all_rc = result.fallback_result;
  diag.user_app_writable = result.user_app_writable;
  diag.launcher_install_rc = rc;
  snprintf(diag.launcher_final_state, sizeof(diag.launcher_final_state),
           "%s", rc == 0 && result.skipped_unchanged ? "up_to_date" :
                   rc == 0 ? "installed" :
                   rc == -ENOSYS ? "appinst_unavailable" : "failed");
  websrv_set_launcher_diag(&diag);
  bfpilot_diag_set_launcher_status(diag.launcher_final_state, rc);

  if(rc == 0) {
    bfpilot_notify("BFpilot", result.skipped_unchanged
        ? "Media tile already up to date"
        : "Media tile installed or refreshed");
  } else {
    bfpilot_notify("BFpilot", "Media tile install failed; file manager remains ready");
  }
  atomic_store(&g_tile_busy, 0);
  return NULL;
}

void
bfpilot_tile_bootstrap_try(void) {
  int expected = 0;
  if(!atomic_compare_exchange_strong(&g_tile_started, &expected, 1)) return;
  atomic_store(&g_tile_busy, 1);

  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_t *thread_attr = NULL;
  int rc = pthread_attr_init(&attr);
  if(rc == 0) {
    thread_attr = &attr;
    int stack_rc = pthread_attr_setstacksize(&attr, 512U * 1024U);
    if(stack_rc != 0) {
      bfpilot_log("tile bootstrap: could not set worker stack rc=%d", stack_rc);
    }
  } else {
    bfpilot_log("tile bootstrap: could not initialize thread attributes rc=%d",
                rc);
  }
  rc = pthread_create(&thread, thread_attr, tile_thread, NULL);
  if(thread_attr) pthread_attr_destroy(&attr);
  if(rc == 0) {
    pthread_detach(thread);
    bfpilot_log("tile bootstrap: direct AppInst worker started");
    return;
  }

  atomic_store(&g_tile_busy, 0);
  atomic_store(&g_tile_started, 0);
  bfpilot_diag_set_launcher_status("thread_failed", -rc);
  bfpilot_log("tile bootstrap: pthread_create failed rc=%d", rc);
}

int
bfpilot_tile_bootstrap_busy(void) {
  return atomic_load(&g_tile_busy) != 0;
}
