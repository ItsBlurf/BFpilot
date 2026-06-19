# BFpilot

BFpilot is a lightweight PS5 payload that serves a browser-based file manager at
`http://<PS5_IP>:5905/`.

The project is split into two payloads:

- `bfpilot.elf` - the main file manager payload.
- `bfpilot-launcher-installer.elf` - an optional home-screen tile installer.

The main payload stays deliberately small and compatibility-focused. It does
not import launcher installer libraries, does not install the tile, and can be
used without touching PS5 app installation services.

## Features

- Browse local PS5 paths from a web browser.
- Upload and download files.
- Copy, move, rename, create folders, and delete from the web UI.
- PS5-side copy and move operations with progress and cancellation.
- Transfer timing, throughput, and device diagnostics.
- Clean places sidebar for Root, Homebrew, Mounts, User, Data, mounted drives,
  and custom shortcuts.
- Mounted USB/ext drives are shown only when they are actually present.
- Top storage summary for Data and mounted drives.
- Persistent custom shortcuts with add, rename, and remove controls.
- Optional launcher tile that opens `http://127.0.0.1:5905/`.

## Download

Use the release assets:

- `bfpilot.elf` for the file manager.
- `bfpilot-launcher-installer.elf` only if you want the PS5 home-screen tile.

Run `bfpilot.elf` first. After the web UI is working, run the launcher installer
payload if you want the tile.

## Usage

Inject the file manager payload to your loader, usually on port `9021`:

```sh
python3 payload_sender.py 192.168.1.204 9021 bfpilot.elf
```

Open:

```text
http://192.168.1.204:5905/
```

Health checks:

```text
http://192.168.1.204:5905/api/status
http://192.168.1.204:5905/api/diag
http://192.168.1.204:5905/api/fs/places
```

Install or refresh the launcher tile:

```sh
python3 payload_sender.py 192.168.1.204 9021 bfpilot-launcher-installer.elf
```

The tile opens:

```text
http://127.0.0.1:5905/
```

## Build

Set `PS5_PAYLOAD_SDK` to your PS5 payload SDK path:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean all
make inspect-imports
```

Expected outputs:

```text
bfpilot.elf
bfpilot-launcher-installer.elf
```

`make inspect-imports` verifies that the file manager payload does not contain
launcher/AppInstUtil imports and that the isolated installer contains the
required installer imports.

## Diagnostics

Local read-only diagnostics:

```sh
PS5_IP=192.168.1.204 BF_WEB_PORT=5905 make ps5-diag
```

Smoke test the file APIs using only BFpilot-created files under `/data/test`:

```sh
PS5_IP=192.168.1.204 BF_WEB_PORT=5905 \
BF_ALLOW_PS5_WRITE=1 \
make ps5-smoke
```

Optional benchmark mode writes under `/data/test/bfpilot-bench` by default:

```sh
PS5_IP=192.168.1.204 BF_WEB_PORT=5905 \
BF_ALLOW_PS5_WRITE=1 \
BF_ALLOWED_REMOTE_ROOTS=/data/test/bfpilot-bench \
python3 scripts/ps5_diag.py --bench
```

Runtime logs are stored on the PS5 at:

```text
/data/BFpilot/boot.log
/data/BFpilot/log.txt
/data/BFpilot/crash.log
/data/BFpilot/launcher-installer.log
```

## Compatibility Notes

`bfpilot.elf` is file-manager-only by design. It avoids AppInstUtil,
SystemService, UserService, and privileged launcher imports because those can
fail before `main()` on some firmware/loader combinations.

`bfpilot-launcher-installer.elf` is separate and uses the complete launcher
installer dependency set. If launcher installation fails on a firmware, the file
manager payload remains usable.

More detail is in [docs/COMPATIBILITY_STRATEGY.md](docs/COMPATIBILITY_STRATEGY.md)
and [docs/FIRMWARE_TESTING.md](docs/FIRMWARE_TESTING.md).
