# BS5FileManager

BS5FileManager is a lightweight PS5 browser file-manager payload. It serves one
web UI on port `5905`, installs a small PS5 home-screen launcher tile, and lets
the console perform large file operations locally instead of pushing everything
through a slow remote copy path.

The project is intentionally scoped to file management only. It does not bundle
patch payloads, game mounting logic, extra network daemons, forced cleanup tools,
or companion service managers.

## Runtime

Inject `bs5filemanager.elf` to your payload loader on port `9021`, then open:

```text
http://<PS5_IP>:5905/
```

On first run, the payload installs or updates a PS5 home-screen tile named
`BS5FileManager`. The tile opens the local console URL:

```text
http://127.0.0.1:5905/
```

When the launcher is newly installed or updated, the payload also asks the PS5
to open that local browser URL once. Later loads keep the tile available without
forcing the browser open again.

Launcher state is stored in `/data/BS5fm`. The PS5 app tile itself still lives
under `/user/app/BS5F00001`, which is the console app location used by the app
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
- Rename files and folders
- Create folders
- Upload files or folder trees from the browser
- Download one selected file through the browser
- Target shortcuts for `/data/homebrew` and `/mnt/usb0`
- USB shortcut discovery for `/mnt/usb0` through `/mnt/usb7`
- Click-to-calculate recursive folder size
- Job progress with copied size, total size, speed, ETA, item counts, and current path

Folder sizes are calculated only when a folder is selected. Directory listings
stay lightweight and do not recursively scan every visible folder.

## Architecture

At a high level the payload has five parts:

- `src/lite_main.c` starts the payload, detects the LAN IP, sends startup
  notifications, installs the launcher tile if needed, and starts the HTTP
  server on port `5905`.
- `src/app_installer.c` writes the embedded launcher metadata/icon under
  `/user/app/BS5F00001`, stores its update marker under `/data/BS5fm`, and
  registers the PS5 home-screen tile. The installer resolves the title-directory
  install function from `libSceAppInstUtil.sprx` at runtime and falls back to
  the broader app install call when needed.
- `src/websrv_lite.c` is a compact HTTP server. It serves the embedded UI,
  static downloads under `/fs`, and JSON APIs under `/api/fs`.
- `src/transfer.c` implements the file-manager API: list, stat, folder size,
  copy, move, delete, mkdir, rename, upload, job status, and cancel.
- `assets/files.html` is the complete web UI embedded into the ELF at build
  time by `gen-asset-module.py`.

Generated C files are written under `gen/` during build and are ignored by Git.
Built ELF files are also ignored; publish release binaries separately if needed.

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

BS5FileManager avoids forced process unloads and kernel-level cleanup. If an
older payload is still running, this build does not interact with it; it uses
port `5905` instead.

Recursive folder size, copy, move, and delete can still take time on very large
external drives. Progress polling is tolerant of short hiccups, and active jobs
continue on the PS5 side even if the browser temporarily loses contact.
Copy, move, and delete jobs report PS5-side throughput in `MB/s` plus ETA from
the server-side job counters.
