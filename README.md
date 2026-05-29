# BS5FileManager

BS5FileManager is a lightweight PS5 browser file-manager payload. It serves a
single web UI on port `5905`, installs a small PS5 home-screen browser launcher
tile, and lets the console perform large file operations locally instead of
pushing everything through a slow remote copy path.

The project is intentionally scoped to file management only. It does not bundle
patch payloads, game mounting logic, extra network daemons, forced cleanup tools,
or companion service managers.

## Runtime

Download the current ELF from the
[v0.1.1 release](https://github.com/ItsBlurf/BS5FileManager/releases/tag/v0.1.1),
or build it locally from source.

Inject `bs5filemanager.elf` to your payload loader on port `9021`, then open:

```text
http://<PS5_IP>:5905/
```

On each run, the payload installs or refreshes a PS5 home-screen tile named
`BS5FileManager` using title ID `BSFM00001`. The tile uses the `BS5 / FM` icon
and opens the local console URL:

```text
http://127.0.0.1:5905/
```

The payload also removes the older experimental launcher title ID `BS5F00001`
before registering `BSFM00001`. This avoids a stale "files exist but tile is not
registered" state from earlier builds.

Launcher state is stored in `/data/BS5fm`. The PS5 app tile itself still lives
under `/user/app/BSFM00001`, which is the console app location used by the app
install API.

The payload also shows a PS5 notification with the external browser URL when
the web server is ready. Port `5905` is fixed so this payload can run next to
older builds that may still be listening on another port.

## Features

- Single browser file-manager UI
- PS5 home-screen web launcher tile
- Browse PS5 paths from the web UI
- Copy and move using PS5-side filesystem operations
- Recursive delete with progress and cancel
- Transfer progress with copied size, total size, MB/s speed, ETA, item counts,
  and current path
- Rename files and folders
- Create folders
- Upload files or folder trees from the browser
- Download one selected file through the browser
- Target shortcuts for `/data/homebrew` and `/mnt/usb0`
- shortcuts
- Click-to-calculate recursive folder size
- Full-width progress text in the UI so large totals and paths do not get
  clipped too aggressively

Folder sizes are calculated only when a folder is selected. Directory listings
stay lightweight and do not recursively scan every visible folder.

## Architecture

At a high level the payload has five parts:

- `src/lite_main.c` starts the payload, detects the LAN IP, sends startup
  notifications, refreshes the launcher tile, hands off from an older running
  BS5FileManager instance when safe, and starts the HTTP server on port `5905`.
- `src/app_installer.c` writes the embedded launcher metadata/icon under
  `/user/app/BSFM00001`, stores its update marker under `/data/BS5fm`, and
  registers the PS5 home-screen tile. It also attempts to remove the old
  `BS5F00001` launcher entry. The installer first calls the SDK title-directory
  install API, then falls back to resolving the same install function from
  `libSceAppInstUtil.sprx`, and finally falls back to the broader app install
  call when needed.
- `src/websrv_lite.c` is a compact HTTP server. It serves the embedded UI,
  static downloads under `/fs`, and JSON APIs under `/api/fs`.
- `src/transfer.c` implements the file-manager API: list, stat, folder size,
  copy, move, delete, mkdir, rename, upload, job status, and cancel.
- `assets/files.html` is the complete web UI embedded into the ELF at build
  time by `gen-asset-module.py`.

Generated C files are written under `gen/` during build and are ignored by Git.
Built ELF files are also ignored; release binaries are published on GitHub.

## Dependencies

Build requirements:

- `ps5-payload-sdk`
- GNU `make`
- Python 3
- LLVM/Clang toolchain compatible with the PS5 payload SDK
- A shell environment that can run the SDK build commands

Set `PS5_PAYLOAD_SDK` before building. If LLVM tools are not discoverable from
your shell, set `HOST_LLVM_BINDIR` to the directory containing `llvm-strip`.

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean
make
```

Output:

```text
bs5filemanager.elf
```

Deploy helper:

```sh
make deploy PS5_HOST=<PS5_IP> PS5_PORT=9021
```

## Safety Notes

BS5FileManager avoids kernel-level cleanup. If an older BS5FileManager instance
is still running, reinjection first checks whether a file operation is active.
If idle, it asks the old instance to shut down and falls back to stopping that
old userland process by PID only if the old build does not support the clean
shutdown endpoint. If a copy, move, or delete job is busy, the new injection
exits after refreshing the launcher instead of killing the active job.

Recursive folder size, copy, move, and delete can still take time on very large
external drives. Progress polling is tolerant of short hiccups, and active jobs
continue on the PS5 side even if the browser temporarily loses contact.
Copy, move, and delete jobs report PS5-side throughput in `MB/s` plus ETA from
the server-side job counters.

If the browser opens but the home-screen tile does not appear, watch the PS5
notifications for `BS5FileManager app failed`. The payload reports AppInst
initialization or launcher registration error codes there so the failing PS5 app
install step can be diagnosed without guessing.
