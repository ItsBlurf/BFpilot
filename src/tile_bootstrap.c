/*
 * Optional recovery: if embedded Media-tile install was skipped/failed and a
 * separate installer ELF exists under the single data dir, push it to elfldr.
 * Never creates alternate /data roots (bfpilot / BFPILOT / bfpilot-launcher-installer).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "notify.h"
#include "paths.h"
#include "payload_launch.h"
#include "tile_bootstrap.h"

/* Only the canonical data tree — installer is a file, not a sibling folder. */
static const char *const k_installer_candidates[] = {
    BFPILOT_INSTALLER_ELF_PATH,
    NULL,
};

static int
file_is_elf(const char *path) {
  struct stat st;
  if(stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64) {
    return 0;
  }
  return 1;
}

void
bfpilot_tile_bootstrap_try(void) {
  static int done;
  const char *found = NULL;
  int rc;

  if(done) return;
  done = 1;

  for(int i = 0; k_installer_candidates[i]; i++) {
    if(file_is_elf(k_installer_candidates[i])) {
      found = k_installer_candidates[i];
      break;
    }
  }

  if(!found) {
    bfpilot_log("tile bootstrap: optional installer ELF not under %s (ok)",
                BFPILOT_DATA_DIR);
    return;
  }

  bfpilot_log("tile bootstrap: recovery inject %s", found);
  rc = bfpilot_launch_payload_path(found);
  if(rc == 0) {
    bfpilot_notify("BFpilot", "recovery tile installer started");
    bfpilot_log("tile bootstrap: installer sent to elfldr ok");
  } else {
    bfpilot_log("tile bootstrap: inject failed rc=%d", rc);
  }
}
