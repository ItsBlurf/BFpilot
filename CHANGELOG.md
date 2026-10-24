# Changelog

## 0.5.0

See the [release notes](docs/RELEASE_NOTES_NEXT.md) for details and known limitations.

- Updated the build to SDK 0.45 and LLVM 23.1.4; corrected firmware detection
  and Windows helper linking, with rebuilds when the toolchain changes.
- Added SMB2/3 connections, share browsing, file operations, transfers, previews,
  text editing, space information and remote PKG support.
- Added more archive formats, AES ZIP, multiple-archive extraction and recovery
  of saved queues and partial folder merges.
- Added individual or combined downloads and top-bar Results.
- Improved large-folder copies, temporary files, overwrite handling, cancellation,
  archive permissions, long paths and PKG request handling.
- Included oz5hk's RAR cancellation fix.
- Kept the interface English-only and removed the Triangle/F1 search shortcut.
- Assigned R1 to open the highlighted item while keeping Square selection and
  normal X behavior.
- Reduced the console layout size and removed the extra filename outline.
- Fixed old archive progress reappearing after reload and delayed replies after sleep.

## v0.4.4 — 2026-09-10

- Added Media-tile registration and a single payload containing the file manager,
  archive support and package worker.
- Added PKG details, icon preview, installation and download actions.
- Added a permissions dialog with presets and recursive changes.
- Added clickable storage summaries and improved console navigation.
- Added PS5-native FIH packages alongside CNT support, with system-package checks
  and internal-storage path mapping.
- Improved startup, shutdown, reinjection, archive cancellation and concurrency.
- Saved the last path and sort order; improved long-path display and file actions.
- Checked ZIP/RAR/7z, a FIH package and 512 MiB transfers on firmware 11.60.

## v0.4.4 Alpha 2 — 2026-08-11

- Added FIH packages and corrected installer identity on firmware 11 and newer.
- Limited installer startup waits and improved archive cancellation.
- Improved reinjection, diagnostics and worker synchronization.
- Checked the Internet Browser PKG, archives and 512 MiB transfers on firmware 11.60.

## v0.4.4 Alpha 1 — 2026-08-11

- Combined separate helpers into one `bfpilot.elf` with an included package worker.
- Added package validation, system-package rejection and internal-storage path mapping.
- Improved tile registration, startup limits and shutdown.
- Saved path/sort preferences and improved back navigation and parent-folder reveal.

## v0.4.3 — 2026-07-27

- Added text creation/editing and recursive folder uploads.
- Improved copy, move, delete, upload and archive handling.
- Kept direct-target transfers alongside Copy/Move/Paste.
- Improved large-file transfers, search and the dark interface.

## v0.4.3 Alpha 3 — 2026-07-27

- Made the toolbar more compact, with secondary actions in More.
- Adjusted the dark theme and layout for smaller screens.
- Made target transfers and clipboard transfers easier to distinguish.

## v0.4.3 Alpha 2 — 2026-07-27

- Showed upload feedback only while dragging files.
- Restored direct Copy/Move alongside Paste Here.
- Made target shortcuts set the destination without navigating away.

## v0.4.3 Alpha 1 — 2026-07-26

- Added clipboard-style Copy/Move/Paste and restored double-click opening.
- Fixed the empty-folder layout, upload feedback and virtual scrolling.
- Improved action availability and the clipboard panel.
- Added buffered large-file copying, periodic output flushing and transfer timing.
- Improved search paging and directory-relative file checks.

## v0.4.2 Test Build — 2026-07-22

- Added UTF-8 text editing with conflict checks and newline preservation.
- Added recursive drag-and-drop uploads in supported browsers.
- Kept existing files until their replacements were complete.
- Improved path, symbolic-link and filesystem-boundary checks.
- Added ShadowMountPlus write warnings and serialized file-changing operations.
- Added ELF-header validation to Windows builds.

## v0.4.1

- Used separate file-manager and Media-tile installer payloads.
- Added payload loading, permissions, multi-selection and case-insensitive paths.
- Added split 7z archives and improved toolbar wrapping.
- Stored logs, index, shortcuts and archive state under `/data/BFpilot`.

## v0.4.0 — 2026-07-14

- Improved upload buffering and connection reuse.
- Improved Index All and copy/move free-space checks.
- Tested on firmware 11.60.

## v0.3.10

- Increased transfer buffers and reduced repeated upload notifications.

## v0.3.9

- Improved uploads and multi-root indexing.
- Added copy/move free-space checks and larger buffers.
- Added I/O source checks in `scripts/check_io.py`.

## v0.3.8

- Added overwrite/merge/skip choices, checkboxes and select-all.
- Added image pan/zoom/rotation and log selection.
- Used `0777` for new files and added ZipCrypto/ZIP64 support.

## Earlier

- Added exit/reload, archives, dark theme, virtual scrolling and indexed search.
- Introduced a separate launcher installer, later combined into the main payload.
