/*
 * Find bfpilot-launcher-installer.elf and push it to elfldr once.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "notify.h"
#include "payload_launch.h"
#include "tile_bootstrap.h"

static const char *const k_installer_candidates[] = {
    "/data/BFpilot/bfpilot-launcher-installer.elf",
    "/data/homebrew/BFpilot/bfpilot-launcher-installer.elf",
    "/data/homebrew/bfpilot-launcher-installer.elf",
    "/mnt/usb0/bfpilot-launcher-installer.elf",
    "/mnt/usb0/BFpilot/bfpilot-launcher-installer.elf",
    "/mnt/usb1/bfpilot-launcher-installer.elf",
    "/user/data/BFpilot/bfpilot-launcher-installer.elf",
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
    bfpilot_log("tile bootstrap: installer ELF not found (skipped)");
    return;
  }

  bfpilot_log("tile bootstrap: injecting %s", found);
  rc = bfpilot_launch_payload_path(found);
  if(rc == 0) {
    bfpilot_notify("BFpilot", "home tile installer started");
    bfpilot_log("tile bootstrap: installer sent to elfldr ok");
  } else {
    bfpilot_log("tile bootstrap: inject failed rc=%d", rc);
    if(rc == -ECONNREFUSED) {
      bfpilot_notify("BFpilot", "tile install skipped (elfldr offline)");
    }
  }
}
