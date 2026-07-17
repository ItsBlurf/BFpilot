# BFpilot

PS5 payload that runs a browser file manager on port **5905**:

```
http://<PS5_IP>:5905/
```

Works from the PS5 browser or any PC/phone on the same network. No extra app after you inject it.

Version **v0.4.0**. Tested on FW **11.60**.

## Payloads

| File | What it is |
|------|------------|
| `bfpilot.elf` | File manager + archive extract. Inject this. |
| `bfpilot-launcher-installer.elf` | Optional home menu tile (`BFPL00001` → `http://127.0.0.1:5905/`) |
| `bfpilot-archive-worker.elf` | Build-only diagnostic. You don't need this for normal use. |

Releases only ship the first two.

## Features

**Browse & transfer**
- Dual panels: left = browse, right = Target for copy/move/extract
- Places: `/`, `/data`, `/data/homebrew`, `/user`, `/mnt`, USB/ext when plugged in, custom shortcuts
- Upload files or a whole folder (folder picker needs browser support)
- Download, copy, move, rename, mkdir, recursive delete
- Progress + cancel on big jobs; overwrite/merge/skip when the destination already exists
- Free space check before copy/move when size is known
- Folder size on click

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
- Text editor for common source/config types, max 5 MB
- Logs button: BFpilot logs, archive/search/boot logs, VSH log, error history

**Other**
- Shortcuts saved to `/data/BFpilot/shortcuts.txt`
- Exit button shuts down cleanly and frees port 5905 (loader on 9021 stays up)
- New files/dirs get mode `0777`

### Upload / network (v0.4.0)

- HTTP keep-alive (up to 64 requests per connection)
- 2 MiB upload buffer, 1 MiB download buffer
- Listen socket asks for 4 MiB RCVBUF / 2 MiB SNDBUF
- No bulk `TCP_NODELAY`, no multi-GB `posix_fallocate` while uploading

More detail: [docs/UPLOAD_PERFORMANCE.md](docs/UPLOAD_PERFORMANCE.md)

## Install

Grab the latest release: https://github.com/ItsBlurf/BFpilot/releases

```sh
python3 payload_sender.py <PS5_IP> 9021 bfpilot.elf
# optional tile:
python3 payload_sender.py <PS5_IP> 9021 bfpilot-launcher-installer.elf
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

Outputs: `bfpilot.elf`, `bfpilot-launcher-installer.elf`, `bfpilot-archive-worker.elf`

Slim build without archives: `make bfpilot-lite`

`inspect-imports` makes sure the main ELF stays free of AppInstUtil/launcher imports.

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
/data/BFpilot/launcher-installer.log
```

## Dev tests (optional)

```sh
PS5_IP=<PS5_IP> BF_WEB_PORT=5905 make ps5-diag
PS5_IP=<PS5_IP> BF_WEB_PORT=5905 BF_ALLOW_PS5_WRITE=1 make ps5-smoke
python3 scripts/goal_verify_io.py
```

## Compatibility

Main ELF is launcher-free on purpose — no AppInstUtil / UserService / SystemService / `kernel_sys` installer imports that break before `main()` on some FW + loaders. Tile installer is a separate ELF so a bad install path doesn't take down the file manager.

See [docs/COMPATIBILITY_STRATEGY.md](docs/COMPATIBILITY_STRATEGY.md). Archives: [docs/ARCHIVE_EXTRACTION.md](docs/ARCHIVE_EXTRACTION.md). Changelog: [CHANGELOG.md](CHANGELOG.md).

## Credits

- [owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) — image viewer / checkbox multi-select ideas
- [ps5-payload-dev](https://github.com/ps5-payload-dev) — SDK / websrv
- [UnRAR](https://www.rarlab.com/rar_add.htm) (Alexander Roshal)
- [miniz](https://github.com/richgel999/miniz)
- [LZMA SDK](https://www.7-zip.org/sdk.html) (Igor Pavlov)

Third-party code under `third_party/` keeps its own licenses.

## License

This project is licensed under the **MIT License**. See [LICENSE](./LICENSE) for the full text.

Copyright (c) ItsBlurf and contributors.  
**No warranty** — the software is provided "as is", without warranty of any kind; authors are not liable for claims arising from use of the software.