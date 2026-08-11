# Changelog

## v0.4.4 Alpha 2 (2026-08-11)

* Added PS5-native `7F FIH` PKG support while retaining strict `7F CNT` validation
* Corrected the FW 11+ AppInst identity boundary and bounded direct tile initialization
* Added archive cancellation with cleanup and tuned RAR/7z automatic threading to four
* Hardened reinjection, diagnostics, and server worker synchronization
* Made clean Windows builds byte-for-byte reproducible
* Verified on FW 11.60 with the Internet Browser PKG, ZIP/RAR/7z extraction, and sustained 512 MiB transfers

## v0.4.4 Alpha 1 (2026-08-11)

* Replaced separate release helpers with one all-in-one `bfpilot.elf`
* Kept Media-tile AppInst direct and embedded a private fresh-process worker for
  reliable full-content PKG installation
* Added bounded AppInst initialization, loopback-only worker control, and clean
  worker shutdown during exit or reinjection
* Added strict PKG validation, NPXS rejection, and `/data` to `/user/data` local-path mapping
* Kept tile reinjection idempotent with exact assets plus a successful-registration marker
* Added persistent path/sort state, guarded Circle/back navigation, parent-folder
  reveal, and right-biased long-path display from the upstream v1.0-v1.4 audit
* Integrated the latest file-manager safety, transfer, archive, and UX fixes

## v0.4.3 (2026-07-27)

* Added create/edit text files and recursive folder uploads
* Added safer staged copy, move, delete, upload, and archive operations
* Kept direct-target and navigate-then-paste transfer workflows together
* Improved large-file transfer speed, search response time, and diagnostics
* Refined the dark file-manager layout; New Text File and Delete stay visible

## v0.4.3 Alpha 3 (2026-07-27)

* Reworked the file toolbar into one compact desktop row with secondary actions
  in a More menu and responsive wrapping on narrower displays
* Darkened the interface and reduced decorative color for a more restrained
  file-manager appearance
* Made the direct target and clipboard transfer workflows visually distinct
  while keeping both available together

## v0.4.3 Alpha 2 (2026-07-27)

* The drag-to-upload strip stays hidden until files enter the browser
* Restored direct Copy/Move to an editable target alongside the clipboard
  navigate-and-paste workflow
* Target shortcuts now fill the direct target instead of changing the current
  folder

## v0.4.3 Alpha 1 (2026-07-26)

* Copy and Move now work like a file-manager clipboard: select items, stage the
  action, navigate to a destination, then choose **Paste Here**
* Restored double-click folder opening while keeping single-click selection and
  the explicit **Open** button
* Fixed the upload drop area consuming the file-list height in short or empty
  folders
* Dragging files anywhere over the browser now reveals the upload state and
  accepts the drop
* Fixed virtual scrolling to listen to the actual file-list scroller
* Reorganized file actions and disabled controls that do not apply to the
  current selection
* The clipboard panel now explains the workflow, shows staged items and the
  current destination, and includes quick destination shortcuts
* The three-column layout collapses earlier so the clipboard no longer gets
  clipped on common laptop-sized browser windows

* Large regular-file copies overlap sequential reads with one writer through
  two fixed 8 MiB, PS5-page-aligned slots; small files and initialization
  failures retain the serial path
* Copy/upload/large ZIP output adds coarse 256 MiB writeback checkpoints to
  prevent unbounded dirty-page growth during sustained multi-gigabyte writes
* Upload, serial copy, pipelined copy, and ZIP buffers request 16 KiB alignment
* Downloads use a 2 MiB aligned sequential buffer and request a 4 MiB TCP send
  window; filesystem-dependent `sendfile` remains disabled for PS5 safety
* Directory listing and Index All metadata lookups are anchored to the open
  parent directory with `AT_SYMLINK_NOFOLLOW`
* Search returns as soon as the requested page plus one match is known and
  reports whether the match count is exact, avoiding full two-million-entry
  scans for common first-page queries
* Transfer telemetry now exposes copy mode, checkpoint count, buffer alignment,
  per-stage timing, effective socket buffers, and last-download throughput

## v0.4.2 Test Build (2026-07-22)

* Create and edit common UTF-8 text/source files (`.txt`, `.ini`, `.json`,
  `.xml`, `.cfg`, `.log`, `.sh`, `.bat`, `.py`, `.c`, `.cpp`, `.h`, `.hpp`,
  `.html`, `.htm`, `.css`, `.js`, `.lua`, `.md`, `.csv`, `.yaml`, `.yml`,
  `.conf`, `.sql`) with a 5 MiB server limit, optimistic version checking,
  permission preservation, `fsync`, and atomic replacement; existing UTF-8
  BOM and dominant newline style are preserved
* Upload drop zone accepts files and recursively enumerates dropped folders
  when the browser exposes the WebKit entry API; folder uploads remain
  sequential and preserve relative paths
* Copy overwrite keeps the existing file until the staged replacement succeeds
* Existing directory conflicts now use validated staged merge semantics
* Copy/delete preparation and recursion are physical, same-device walks with
  checked paths, a 64-level depth cap, and first-error abort behavior
* Copy/move/delete UI actions use form POST bodies to avoid long encoded URLs
* Upload/copy staging names are collision-resistant within the same second
* ShadowMountPlus warning before writes to its common scan locations
* Upload, copy, delete, editor-save, and integrated archive jobs are mutually
  excluded; uploads report backend byte progress, and copied source files are
  reopened with no-follow and inode/device verification
* Windows launcher builds now use the PS5 FreeBSD PIE linker profile, with
  automated ELF-header validation for every produced payload

## v0.4.1

* **Separate payloads** (stable model): `bfpilot.elf` = file manager only; `bfpilot-launcher-installer.elf` = home **Media** tile (`applicationCategoryType` 65536, `BFPL00001` → `http://127.0.0.1:5905/`)
* Main ELF does **not** link AppInst (loaders reject all-in-one); inject the installer separately or place it under `/data/BFpilot/` so the main ELF can auto-start it after the UI is up
* **One data directory**: all logs/index/shortcuts/archive state under `/data/BFpilot` only
* Action bar wraps (no horizontal scrollbar) for New Folder / Rename / Download / Load payload / chmod / Extract / Upload / Delete
* **Load payload**: double-click `.elf`/`.bin`/`.js` or toolbar — streams to elfldr on `127.0.0.1:9021`
* New API: `GET /api/fs/launch?path=...`
* **chmod** UI + `GET /api/fs/chmod?path=&mode=` (presets 444/644/755/777) (#10)
* Single-click folder name opens directory (#7); checkboxes multi-select (#11)
* Case-insensitive path resolve on list/Go (#8)
* Extract zip/rar/7z including `.7z.001` (#9)
* Version strings locked to **v0.4.1** (#12)

## v0.4.0 Stable (2026-07-14)

Upload speed + Index All stability. Tested on FW 11.60.

* HTTP keep-alive (64 req/conn)
* 2 MiB upload buffer, 1 MiB download buffer
* Listen SO_RCVBUF 4 MiB / SO_SNDBUF 2 MiB, backlog 32
* No multi-GB fallocate during upload, no bulk TCP_NODELAY
* Index All multi-root + XDEV + soft-fail roots (from 0.3.9)
* Copy/move free-space preflight
* See `docs/UPLOAD_PERFORMANCE.md`

## v0.3.10

* Keep-alive + larger upload/download buffers
* Less toast spam on multi-file upload

## v0.3.9

* STOR-style web upload path
* Index All multi-root crawl, XDEV, soft-fail
* Copy/move free-space preflight + larger buffers
* `make bfpilot-lite` / `ENABLE_ARCHIVE=0`
* `scripts/goal_verify_io.py`

## v0.3.8

* Overwrite / merge / skip on copy-move conflicts
* Checkbox multi-select + select all
* Image viewer (pan/zoom/rotate)
* Log dropdown + PS5 error history
* umask 0 + 0777 on created files
* ZIP ZipCrypto/ZIP64; extract defaults to Target path
* Single Index All mode

## Earlier

* Exit / re-inject without reboot
* Archives inside main ELF (RAR / 7z / ZIP)
* Dark UI + virtual scrolling
* Search index without duplicated lowercase paths
* Launcher tile split into its own ELF
