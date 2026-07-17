/*
 * Canonical on-console data layout for BFpilot.
 * Single root: /data/BFpilot — do not introduce alternate casings or
 * sibling trees (/data/bfpilot, /data/BFPILOT, /data/bfpilot-launcher-installer).
 */

#pragma once

#define BFPILOT_DATA_ROOT          "/data"
#define BFPILOT_DATA_DIR           "/data/BFpilot"
#define BFPILOT_HOMEBREW_DIR       "/data/homebrew"

#define BFPILOT_LOG_PATH           BFPILOT_DATA_DIR "/log.txt"
#define BFPILOT_CRASH_LOG_PATH     BFPILOT_DATA_DIR "/crash.log"
#define BFPILOT_BOOT_LOG_PATH      BFPILOT_DATA_DIR "/boot.log"
#define BFPILOT_SHORTCUTS_PATH     BFPILOT_DATA_DIR "/shortcuts.txt"
#define BFPILOT_SHORTCUTS_TMP      BFPILOT_DATA_DIR "/shortcuts.tmp"
#define BFPILOT_SEARCH_IDX         BFPILOT_DATA_DIR "/search.idx"
#define BFPILOT_SEARCH_IDX_TMP     BFPILOT_DATA_DIR "/search.idx.tmp"
#define BFPILOT_SEARCH_CRAWL_LOG   BFPILOT_DATA_DIR "/search_crawl.log"
#define BFPILOT_LAUNCHER_LOG       BFPILOT_DATA_DIR "/launcher-installer.log"
#define BFPILOT_INSTALLER_ELF_PATH BFPILOT_DATA_DIR "/bfpilot-launcher-installer.elf"

/* Integrated archive jobs live under the same data root. */
#define BFPILOT_ARCHIVE_INTEGRATED_DIR BFPILOT_DATA_DIR "/archive-integrated"
#define BFPILOT_ARCHIVE_STANDALONE_DIR BFPILOT_DATA_DIR "/archive"

/* PS5 home Media tile (param.json applicationCategoryType 65536). */
#define BFPILOT_APP_TITLE_ID "BFPL00001"
#define BFPILOT_APP_ROOT     "/user/app"
#define BFPILOT_APP_PARENT   BFPILOT_APP_ROOT "/"
#define BFPILOT_DEEPLINK_URI "http://127.0.0.1:5905/"
