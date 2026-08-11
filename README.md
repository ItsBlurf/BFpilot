# BFpilot

PS5 payload that runs a browser file manager on port **5905**:

```
http://<PS5_IP>:5905/
```

Works from the PS5 browser or any PC/phone on the same network.

Current development version **v0.4.4-alpha.1**. The v0.4.1 baseline was tested on FW **11.60**.

## Payloads

| File | What it is |
|------|------------|
| `bfpilot.elf` | All-in-one file manager, archive extractor, PKG installer, and **Media** tile installer on port **5905**. |

**Typical use:** inject `bfpilot.elf`. It starts the UI first, then installs or refreshes the `BFPL00001` Media tile when needed. There is no second installer ELF.

**Data layout:** everything lives under **`/data/BFpilot`** only (logs, search index, shortcuts, archive jobs).

## Features

**Browse & transfer**
- Two transfer flows: copy or move straight to a target, or stage items, navigate, and choose Paste Here
- Places: `/`, `/data`, `/data/homebrew`, `/user`, `/mnt`, USB/ext when plugged in, custom shortcuts
- Upload files or a whole folder using the picker or drag-and-drop (recursive
  dropped folders need the browser's WebKit entry API)
- Download, copy, move, rename, mkdir, recursive delete
- **Load payload**: double-click `.elf`/`.bin`/`.js` (or toolbar) to send it to elfldr on port **9021**
- Progress + cancel on big jobs; overwrite/merge/skip when the destination already exists
- Free space check before copy/move when size is known
- Same-device, no-symlink recursive traversal with a 64-level depth cap and immediate abort on fatal storage/vnode errors
- ShadowMountPlus warning before writes to its common scan roots
- Single-click selects; double-click opens folders; the Open button works with controllers
- Circle/browser Back returns through folder history; active operations block accidental navigation, and returning to a parent reveals the folder just left
- The last successful folder and sort mode survive a browser reload; long paths keep their rightmost component visible
- Folder size is calculated when a folder is selected

**Selection**
- Checkboxes + select all (works fine with a controller)

**Search**
- **Index All** crawls some useful mounts into RAM (Everything-style queries after that)
- Priority roots first (`/data`, `/user`, USB/ext), then system paths; stays on the same filesystem (`st_dev`) so it doesn't walk sandbox/nullfs nonsense
- Bad roots are skipped instead of killing the whole index
- Cap about 2 million entries; cache at `/data/BFpilot/search.idx`
- Case-insensitive multi-word search, match highlighting

**Archives** (built into `bfpilot.elf`)
- ZIP (stored/deflate/ZIP64, ZipCrypto only — no AES zip)
- RAR4/RAR5 + passwords + multipart
- 7z + passwords + `.7z.001` splits
- Extract only under `/data` and USB/ext mounts
- Password prompt, progress/ETA, cleans up partial output on failure

**Viewers**
- Images: png/jpg/gif/bmp/webp/ico/svg — pan, zoom, rotate
- Create/edit common source, config, markup, script, CSV, and `.shn` text files, max 5 MB; UTF-8 validation, change-conflict detection, atomic save, and preservation of an existing UTF-8 BOM/newline style
- Logs button: BFpilot logs, archive/search/boot logs, VSH log, error history

**Other**
- Shortcuts saved to `/data/BFpilot/shortcuts.txt`
- Exit button shuts down cleanly and frees port 5905 (loader on 9021 stays up)
- New files/dirs get mode `0777`

### Transfer performance

- HTTP keep-alive (up to 64 requests per connection)
- 2 MiB page-aligned upload and download buffers
- Listen socket asks for 4 MiB RCVBUF / 4 MiB SNDBUF
- Large copies use a bounded two-slot 8 MiB read/write pipeline; small files
  use the serial fallback
- Sustained copy/upload/ZIP output checkpoints writeback every 256 MiB
- No bulk `TCP_NODELAY`, no multi-GB `posix_fallocate` while uploading

More detail: [upload tuning](docs/UPLOAD_PERFORMANCE.md) and the
[performance/stability design](docs/PERFORMANCE_STABILITY.md).

## Install

Grab the latest release: https://github.com/ItsBlurf/BFpilot/releases

```sh
python3 payload_sender.py <PS5_IP> 9021 bfpilot.elf
```

Or send the ELFs with [ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager) / [Blurfer](https://github.com/ItsBlurf/Blurfer).

Then open `http://<PS5_IP>:5905/`.

Quick checks: `/api/status`, `/api/diag`, `/api/fs/places`

## Build

Needs [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk):

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean all
make inspect-imports
```

Windows: `build.cmd` if your toolchain is already set up.

Output: `bfpilot.elf`

`inspect-imports` verifies the one-ELF PS5 ABI, requires both AppInstUtil and its
IPMI dependency, and rejects obsolete helper artifacts.

## Logs on the PS5

```
/data/BFpilot/log.txt
/data/BFpilot/boot.log
/data/BFpilot/crash.log
/data/BFpilot/search_crawl.log
/data/BFpilot/search.idx
/data/BFpilot/shortcuts.txt
/data/BFpilot/archive-integrated/archive-worker.log
/data/BFpilot/archive-integrated/status.json
```

## Dev tests (optional)

```sh
PS5_IP=<PS5_IP> BF_WEB_PORT=5905 make ps5-diag
PS5_IP=<PS5_IP> BF_WEB_PORT=5905 BF_ALLOW_PS5_WRITE=1 make ps5-smoke
python3 scripts/goal_verify_io.py
BF_ALLOW_PS5_WRITE=1 PS5_IP=<PS5_IP> make ps5-transfer-perf
```

## Compatibility

BFpilot follows the established one-payload installer pattern: AppInstUtil and
SceIpmi are linked for the direct Media-tile path. Full-content PKG installation
uses a fresh loopback-only worker embedded inside the same `bfpilot.elf`; there
is still no second file to install or inject. The HTTP listener is brought up
before tile work, AppInst initialization is bounded, and clean reinjection stops
the private worker before handing over port 5905.

See [docs/COMPATIBILITY_STRATEGY.md](docs/COMPATIBILITY_STRATEGY.md). Archives: [docs/ARCHIVE_EXTRACTION.md](docs/ARCHIVE_EXTRACTION.md). Changelog: [CHANGELOG.md](CHANGELOG.md).

## Credits

- [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) — file-manager UX and safety research
- [Elf Arsenal](https://git.etawen.dev/soniciso/elf-arsenal) — recursive drag-and-drop upload concept
- [ps5upload](https://github.com/phantomptr/ps5upload) — PS5 transfer/traversal reference
- [ps5-payload-dev](https://github.com/ps5-payload-dev) — SDK / websrv
- [UnRAR](https://www.rarlab.com/rar_add.htm) (Alexander Roshal)
- [miniz](https://github.com/richgel999/miniz)
- [LZMA SDK](https://www.7-zip.org/sdk.html) (Igor Pavlov)

Third-party code under `third_party/` keeps its own licenses.
The upstream audit and adopt/reject rationale are documented in
[docs/UPSTREAM_FILE_MANAGER_COMPARISON.md](docs/UPSTREAM_FILE_MANAGER_COMPARISON.md).

## License

This project is licensed under the **MIT License**. See [LICENSE](./LICENSE) for the full text.

Copyright (c) ItsBlurf and contributors.  
**No warranty** — the software is provided "as is", without warranty of any kind; authors are not liable for claims arising from use of the software.
