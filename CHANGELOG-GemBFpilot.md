# GemBfpilot vs Original BFPILOT (Changelog & Differences)

This document outlines all the modifications, optimizations, and new features introduced in **GemBfpilot** compared to the original **BFPILOT**. You can share this document with Codex or any other AI/developer so they understand the exact state of the codebase.

## 1. Web UI Overhaul (Dark Mode & Virtual Scrolling)
* **File:** `assets/files.html` (completely rewritten)
* **Changes:**
  * Replaced the simple table-based frontend with a modern, high-performance dark mode UI.
  * Implemented **Search Highlighting**: Search terms are dynamically highlighted in both filenames and directory paths using `<mark>` tags.
  * Implemented **Virtual Scrolling (Infinite Scrolling)**: The DOM now recycles rows to prevent the PS5 Web Browser from running out of memory (OOM) when opening directories or search results with over 10,000+ files.

## 2. SSD Crawler Optimization (`getdents` & `lstat` elimination)
* **File:** `src/search.c`
* **Changes:**
  * Replaced the standard POSIX `readdir()` loop with direct FreeBSD `getdents()` syscall wrapper using a 64KB buffer. This drastically reduces the number of context switches required to traverse massive PS5 SSD partitions.
  * **Zero-lstat for Regular Files:** Eliminated the heavy `lstat()` syscall for regular files by relying on the `d_type == DT_REG` flag provided natively by `getdents`. This makes the multi-threaded crawler exponentially faster as it bypasses inode lookups.

## 3. Networking Optimization (`sendfile` & `TCP_NODELAY`)
* **Files:** `src/fs.c` and `src/websrv_lite.c`
* **Changes:**
  * Replaced the traditional `read()` / `write()` 32KB buffer loop used for file downloads with the FreeBSD `sendfile()` syscall. This allows the PS5 kernel to stream files directly from the SSD to the network socket.
  * Added `TCP_NODELAY` to eliminate Nagle's algorithm and boosted socket send/receive buffers (`SO_SNDBUF` and `SO_RCVBUF`) to 2MB. This dramatically improves overall webserver responsiveness and maximizes file transfer speeds.

## 4. Search Engine Memory Footprint Halved
* **File:** `src/search.c` and `src/search.h`
* **Changes:**
  * Removed the `lower` string field from `search_entry_t`. The original codebase cached a lowercase copy of every single filepath in memory to perform case-insensitive searches.
  * Switched to using `strcasestr()` from the PS5 SDK (`string.h`), which performs fast case-insensitive substring matching directly on the original path. This effectively cuts the RAM usage of the search index by 50%.

## 5. Process Shutdown Stability
* **File:** `src/lite_main.c`
* **Changes:**
  * Replaced standard `exit(0)` calls with `_exit(0)`. The original `exit()` caused the PS5 to hang or crash during payload termination because it attempted to invoke standard library cleanup routines that interfere with the PS5's payload execution environment.

## 6. Build System & Windows Compatibility
* **Files:** `build.sh`, `Makefile`, and `gen-asset-module.py`
* **Changes:**
  * Replaced the Unix-only `xxd` shell pipeline with a custom Python script (`gen-asset-module.py`) to compile `files.html` into a C header `files_html.h`.
  * Removed incompatible Bash `[[ ]]` conditionals from the Makefiles and replaced them with standard POSIX `case` blocks. This allows the project to be compiled cleanly on Windows using Git Bash or standard Make.
