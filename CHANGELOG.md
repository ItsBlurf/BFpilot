# Changelog

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
