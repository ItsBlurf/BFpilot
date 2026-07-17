/*
 * Media-category home tile install (pldmgr-style), callable from main ELF.
 */

#pragma once

/* Install or refresh BFPL00001 under /user/app if param/icon changed.
 * Returns 0 on success / already up to date, negative or nonzero on failure.
 * Non-fatal for the file manager: callers should continue serving the UI.
 */
int bfpilot_install_app_if_needed(void);
