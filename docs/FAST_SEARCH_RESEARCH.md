# Fast Search Research

## Goal

Add fast filename/path search to the main BFpilot file manager payload without
adding launcher imports, external services, or a second injected ELF.

The target user experience is similar to Everything on Windows: type a partial
name, get useful results immediately, and open or operate on results from the
same file manager UI.

## Current BFpilot Baseline

- `bfpilot.elf` serves the web UI on port 5905 and owns the file manager API.
- Archive extraction is integrated into the main ELF and uses
  `/data/BFpilot/archive-integrated`.
- Launcher tile installation remains isolated in
  `bfpilot-launcher-installer.elf`.
- Directory browsing is live and per-folder:
  `/api/fs/list?path=/some/path` calls `opendir`, `readdir`, and `lstat`.
- There is no recursive file index and no search route today.
- File operations run through a single transfer job state; search should use a
  separate state so browsing/searching does not block copy, move, delete, upload,
  or archive work.
- Diagnostics and logs are under `/data/BFpilot`.

## External Research

Everything gets its speed from indexing filesystem metadata instead of scanning
the tree for each query.

Useful references:

- voidtools Indexes:
  https://www.voidtools.com/support/everything/indexes/
- voidtools Searching:
  https://www.voidtools.com/support/everything/searching/
- Microsoft NTFS Change Journals:
  https://learn.microsoft.com/en-us/windows/win32/fileio/change-journals

Important observations:

- Everything indexes name and path information, keeps its database in memory
  while running, and saves it to disk on exit.
- Indexed file information can be searched instantly; unindexed information is
  gathered later and can be slow for large result sets.
- Everything uses NTFS/ReFS volume support and change monitoring to keep indexes
  current.
- NTFS USN change journals let software avoid repeatedly rescanning an entire
  volume after changes.
- BFpilot cannot assume NTFS/ReFS, a USN journal, or OS-level filesystem event
  APIs on PS5. The closest stable design is a BFpilot-owned indexer with explicit
  rebuilds and conservative stale tracking.

## Comparative PS5 Project Notes

Pegasus DL is current and has a polished PS5 web workflow, but its search is for
package catalogs, not live filesystem discovery. It is useful as product context
for queue/status UI, but it is not a drop-in model for BFpilot filesystem search.

The `juma-sayeh/PS5-File-Explorer` fork includes broad archive and activity
ideas, but it does not change the main conclusion for search: BFpilot should not
perform recursive disk walks per query, and it should avoid adding heavy runtime
dependencies or fragile imports to the main payload.

## Recommended Design

Add a small search module built into `bfpilot.elf`.

Files:

- `src/search.c`
- `src/search.h`
- `src/transfer.c` route glue
- `assets/files.html` search UI
- `scripts/ps5_smoke.py` or a new focused search smoke script

Routes:

- `GET /api/fs/search/status`
- `GET /api/fs/search/rebuild`
- `GET /api/fs/search/rebuild?mode=system`
- `GET /api/fs/search/rebuild?root=/` as an alias for system indexing
- `GET /api/fs/search/rebuild?root=/data` for targeted test rebuilds
- `GET /api/fs/search/cancel`
- `GET /api/fs/search?q=term&limit=200&offset=0&type=all`
- `GET /api/fs/search?q=term&root=/data&limit=200&offset=0&type=all` for
  targeted filtered queries

Index record:

- full path
- lowercase searchable path
- basename offset or basename string
- directory flag
- size
- mtime

Indexer behavior:

- Build a new index in a detached background thread.
- Crawl with `opendir`, `readdir`, and `lstat`.
- Do not follow symlinks by default.
- Skip `.` and `..`.
- Use explicit max limits for entries, path length, response size, and result
  count.
- Keep the old index available while rebuilding, then swap under a lock.
- Expose progress through `/api/fs/search/status`.
- Log crawl errors with path, errno, and source file.

Default roots:

- Default rebuild mode: all useful detected roots.
- The global index crawls `/` plus detected separate system/user/storage roots,
  including `/system`, `/system_data`, `/system_ex`, `/preinst`, `/preinst2`,
  `/hostapp`, `/data`, `/user`, `/mnt/usb*`, and `/mnt/ext*` when those paths
  exist.
- Root-level directories are discovered dynamically so firmware-specific mount
  points can be included without hardcoding every name.
- System rebuild mode uses the same broad root discovery path and remains as an
  explicit UI affordance for system-file searches.
- Allow mounted storage roots under `/mnt/usb0` through `/mnt/usb7` and
  `/mnt/ext0` through `/mnt/ext7` for explicit root-scoped tests.
- Allow custom absolute user-selected roots only when explicitly requested.
- Do not auto-index `/` at startup.
- Do not index pseudo/volatile roots such as `/dev`, `/proc`, `/sys`, `/net`,
  and `/run`.

Query behavior:

- Lowercase the query once.
- Split by spaces into required terms.
- Match against lowercase path for Everything-style partial path search.
- Return stable JSON results with `path`, `name`, `dir`, `size`, and `mtime`.
- Keep default results capped, with `limit` and `offset` for paging.

UI behavior:

- Add a compact search input, "Index All" button, and explicit "Index System"
  button near the current path controls.
- Highlight typed search terms in result paths.
- Search results reuse the existing file list row style.
- Search result selection must operate on absolute paths, not names relative to
  the current folder.
- Double-clicking a directory result opens that directory.
- Double-clicking a file result opens/downloads that file.
- File actions should work on search results by passing absolute paths.
- Show a concise index status line such as item count, root, stale/running state,
  and last error.

Stale tracking:

- Mark the index stale after BFpilot writes through upload, mkdir, rename, copy,
  move, delete, and archive extraction.
- Phase one can mark stale only; later phases can patch specific path changes
  into the index.
- Rebuild remains explicit to avoid surprise CPU and disk work.

Persistence:

- Start with memory-only indexing for maximum compatibility.
- Add optional `/data/BFpilot/search-index.bin` only after the in-memory version
  is stable.
- A persisted index must include format version, root, entry count, build time,
  and enough validation to reject stale or incompatible data.

## Performance Strategy

Phase one should use a compact in-memory vector and linear substring scan. For a
PS5 file manager, this is likely fast enough for tens or hundreds of thousands of
entries if the query path is already lowercased and the JSON result set is capped.

Only add heavier structures after measuring:

- sorted basename/path arrays for fast prefix queries
- extension buckets
- trigram or n-gram inverted index
- persisted binary index

The first implementation should measure:

- crawl elapsed time
- entries indexed
- directories scanned
- skipped entries
- errno counts
- memory estimate
- query elapsed time
- result count before limit

## Compatibility Rules

- No AppInst, SystemService, UserService, or kernel_sys imports in
  `bfpilot.elf`.
- No new third-party library dependency for phase one.
- No automatic full-root crawl on startup.
- No destructive operations from diagnostics or indexing.
- Keep all logs and optional search state under `/data/BFpilot`.
- Keep archive extraction and search independent so a search rebuild cannot
  block archive progress reporting.

## Implementation Order

1. Add `src/search.c` and `src/search.h` with in-memory index state, crawler,
   status, cancel, rebuild, and query handlers.
2. Wire `/api/fs/search/*` routes through `transfer_request`.
3. Add the compact UI controls and search result mode.
4. Add search status to `/api/diag` route metadata.
5. Add a safe PS5 smoke test under `/data/test/bfpilot-search-*`.
6. Build with the local SDK and run import inspection.
7. Deploy to the PS5 and test a small controlled tree before trying wider
   `/data` indexing.

## Acceptance Criteria

- `make clean all inspect-imports` passes with the PS5 SDK.
- `bfpilot.elf` keeps the current safe import boundary.
- Existing file manager and archive routes still respond.
- Search rebuild on a small `/data/test` tree completes and reports progress.
- Search query returns expected files and folders.
- Search results can be opened and selected without path confusion.
- Cancel stops an active rebuild.
- PS5 diagnostics save search status and logs without mutating user data.

## 2026-07-02 Implementation Update

Implemented phase-one indexed search in the main file manager payload.

Changed files:

- `src/search.c` and `src/search.h`: in-memory search index, background rebuild,
  cancel, status, and query handlers.
- `src/transfer.c`: search route glue and stale markers after BFpilot mutating
  file operations.
- `src/websrv_lite.c`: diagnostic route metadata for search endpoints.
- `assets/files.html`: compact search controls, index status, search result
  mode, and absolute-path selection/action handling.
- `Makefile`: includes `src/search.c` in the web payload.

Current behavior:

- `GET /api/fs/search/status`
- `GET /api/fs/search/rebuild`
- `GET /api/fs/search/rebuild?mode=system`
- `GET /api/fs/search/rebuild?root=/` as an alias for system indexing
- `GET /api/fs/search/rebuild?root=/data` for targeted test rebuilds
- `GET /api/fs/search/cancel`
- `GET /api/fs/search?q=term&limit=200&offset=0&type=all&scope=path`
- `GET /api/fs/search?q=term&root=/data&limit=200&offset=0&type=all&scope=path`
  for filtered queries

Safety constraints implemented:

- Explicit rebuild only; no startup crawl.
- The default rebuild indexes all useful detected roots: `/` plus detected
  separate system/user/storage roots, including `/system`, `/system_data`,
  `/system_ex`, `/preinst`, `/preinst2`, `/hostapp`, `/data`, `/user`,
  `/mnt/usb*`, and `/mnt/ext*`.
- Root-level directories are discovered dynamically and added when they are
  separate mount points.
- Allowed explicit roots are absolute paths without `..` segments, excluding
  pseudo/volatile roots.
- Rejects non-absolute paths and `..` segments.
- Uses `lstat`, does not follow symlinks, and only descends directories on the
  same device as each indexed root.
- Skips pseudo/volatile roots such as `/dev`, `/proc`, `/sys`, `/net`, and
  `/run`.
- Caps index entries at 750000 and response limits at 1000.
- Keeps old index active while a rebuild is running, then swaps the new index
  under a mutex.
- Marks the index stale after upload, mkdir, rename, copy, move, delete, and
  archive prepare.
- Runs the crawler at a lower process priority when `setpriority` is available.

Validation performed locally:

- Installed PS5 payload SDK into `C:\Users\Blurf\ps5sdk` to avoid path-with-space
  Makefile failures.
- Patched installed SDK wrapper scripts for this Windows setup so copied
  symlink wrappers resolve `clang.exe`, `ld.lld.exe`, and LLVM binutils
  correctly.
- Patched the local SDK wrappers and BFpilot Makefile so this machine's
  BusyBox-backed `bash` shim uses POSIX-compatible wrapper syntax and `C:/...`
  shell paths instead of `/c/...` link paths.
- Built and installed static libc++/libc++abi/libunwind into the SDK because the
  integrated archive build requires `<new>` and C++ runtime libraries.
- `PS5_PAYLOAD_SDK=C:/Users/Blurf/ps5sdk mingw32-make bfpilot` completed and a
  follow-up run reported `Nothing to be done for 'bfpilot'`.
- `PS5_PAYLOAD_SDK=C:/Users/Blurf/ps5sdk mingw32-make inspect-imports` passed.
- Parsed the embedded `assets/files.html` script with Node's `new Function`;
  syntax check passed.

Not yet done:

- PS5 hardware smoke test with a small controlled `/data/test/bfpilot-search-*`
  tree.
- Measured index build/query timings on PS5 internal storage and USB storage.

Next measured improvements, only after PS5 smoke testing:

- Add a small PS5 search smoke script that creates a controlled tree, rebuilds
  that root, queries known names, cancels a rebuild, and deletes the test tree.
- Add optional sorting controls and "load more" paging in the UI.
- Consider a persisted binary index only after measuring memory-only stability.
- Consider extension buckets, prefix arrays, or a trigram index only if linear
  scan query time is measured as a real problem on large PS5 datasets.
