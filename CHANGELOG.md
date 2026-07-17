# Changelog

## v0.4.1

* Home tile installs as **Media** category (`applicationCategoryType` 65536), same idea as Payload Manager
* After the web UI is up, auto-injects `bfpilot-launcher-installer.elf` if found under `/data/BFpilot/` (or common USB/homebrew paths) so the PS5 tile installs without merging AppInst into the main ELF
* **Load payload**: double-click `.elf`/`.bin`/`.js` or use the toolbar button — streams the file to elfldr on `127.0.0.1:9021`
* New API: `GET /api/fs/launch?path=...`
* Launcher installer calls `sceAppInstUtilTerminate` when done

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
