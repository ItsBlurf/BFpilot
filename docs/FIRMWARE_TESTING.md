# BFpilot Firmware Testing

Use this order on every firmware/loader combination. Do not test the launcher
installer until the file manager is stable.

Remote diagnostics are read-only unless benchmark mode is explicitly enabled.
Benchmark mode writes only timestamped BFpilot-owned files under
`/data/test/bfpilot-bench` and does not delete remote files.

## 1. Build Locally

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

Failure meaning: local toolchain, source, or import-boundary failure. Do not
inject until this passes.

## 2. Inject File Manager

```sh
python3 payload_sender.py "$PS5_IP" "${BF_PAYLOAD_PORT:-9021}" bfpilot.elf
PS5_IP="$PS5_IP" BF_WEB_PORT="${BF_WEB_PORT:-5905}" make ps5-diag
```

Expected notification/log:

- `BFpilot BOOT`
- a `bfpilot file-manager` entry in `/data/BFpilot/boot.log`
- startup lines in `/data/BFpilot/log.txt`

Expected endpoints:

- `/api/status`: HTTP 200, `mode=file-manager`, `port=5905`,
  `diagReadOnly=true`
- `/api/fs/places`: HTTP 200, built-in places plus only actually mounted
  USB/ext drives
- `/api/diag`: HTTP 200

Failure meaning:

- No boot marker: loader rejected the ELF or failure occurred before `main()`.
- Boot marker but no status: failure after `main()` during startup/bind/listen.
- Status works but places/diag fails: HTTP API regression.

Next action: save `diagnostics/ps5-diag-*.json` and the BFpilot logs.

## 3. Confirm UI Places And Storage

Open:

```text
http://<PS5_IP>:5905/
```

Expected:

- left rail shows Root, Homebrew, Mounts, User, Data, and custom shortcuts
- no `/mnt/usb0..7` or `/mnt/ext0..7` placeholders unless the drive is really
  mounted
- the top storage summary shows free/total/used percentage for Data and each
  mounted drive
- custom shortcuts can be added, renamed, removed, and survive reload because
  they are stored at `/data/BFpilot/shortcuts.txt`

Failure meaning:

- Placeholder USB/ext entries: mount detection regression.
- No disk space: `statvfs`/places response regression.
- Shortcuts do not persist: `/data/BFpilot/shortcuts.txt` write/read failure.

## 4. Safe Transfer Benchmark

Run only after the file manager passes:

```sh
PS5_IP="$PS5_IP" BF_WEB_PORT="${BF_WEB_PORT:-5905}" \
BF_ALLOW_PS5_WRITE=1 \
BF_ALLOWED_REMOTE_ROOTS=/data/test/bfpilot-bench \
python3 scripts/ps5_diag.py --bench --logs
```

Expected files: timestamped source/copy folders only under
`/data/test/bfpilot-bench`.

For a broader file API smoke test, run:

```sh
PS5_IP="$PS5_IP" BF_WEB_PORT="${BF_WEB_PORT:-5905}" \
BF_ALLOW_PS5_WRITE=1 \
make ps5-smoke
```

Expected files: a unique `/data/test/bfpilot-smoke-*` folder that is deleted by
the test after upload, download, copy, rename, move, and cleanup checks pass.

Expected results:

- upload response includes `elapsedMs`, `averageMBps`, `bytesWritten`, and
  `destinationDev`
- copy job includes bytes read/written, elapsed ms, MB/s,
  source/destination device IDs, return state, and errno
- `/api/fs/transfer/stats` reflects the last upload/copy metrics

Failure meaning:

- Slow same-device copy: copy loop or internal storage path.
- Fast same-device copy but slow upload: client/LAN/HTTP receive path.
- Slow only when device IDs differ: USB/cross-filesystem path.

Do not benchmark user files.

## 5. Launcher Installer

Run only after stages 1-4 pass:

```sh
python3 payload_sender.py "$PS5_IP" "${BF_PAYLOAD_PORT:-9021}" \
  bfpilot-launcher-installer.elf
PS5_IP="$PS5_IP" BF_WEB_PORT="${BF_WEB_PORT:-5905}" \
python3 scripts/ps5_diag.py --logs
```

Expected:

- `BFpilot BOOT` marker for `bfpilot-launcher-installer`
- `/data/BFpilot/launcher-installer.log`
- log line confirming title `BFPL00001`
- log line confirming deep link `http://127.0.0.1:5905/`
- UserService init return code
- authid set/restore return codes
- AppInst init return code
- asset/staging results
- final `AppInstallTitleDir` return code

Failure meaning:

- No boot marker/log: loader rejected the installer before `main()`.
- AppInst unavailable/init failed: AppInstUtil context not available on this
  firmware/loader.
- Authid setup failed: privilege operation unavailable.
- Asset/staging failed: title file path or embedded metadata problem.
- Install call failed: AppInst rejected the title directory.
- Tile installed but opens the wrong URL: compare `param.json` and the logged
  deep link.

## 6. Files To Save

For every failure, save:

```text
/data/BFpilot/boot.log
/data/BFpilot/log.txt
/data/BFpilot/crash.log
/data/BFpilot/launcher-installer.log
diagnostics/ps5-diag-*.json
```

Record the exact command, endpoint, HTTP status or return code, and the newest
log lines around the failure.
