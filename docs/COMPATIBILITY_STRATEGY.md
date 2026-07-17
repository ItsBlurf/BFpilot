# BFpilot Compatibility Strategy

BFpilot keeps the file manager/archive runtime and the fragile launcher
installer as separate payloads.

## Payload Roles

### `bfpilot.elf`

Main release payload. Test this first on every firmware and loader.

- Starts the HTTP file manager on port `5905`.
- Starts the integrated archive daemon for RAR, 7z, split 7z, and ZIP
  extraction.
- Can **load other payloads** by streaming them to elfldr (`127.0.0.1:9021`)
  via `/api/fs/launch` (no AppInst).
- Can **auto-bootstrap the home tile** by injecting
  `bfpilot-launcher-installer.elf` once if that ELF is found under
  `/data/BFpilot/` (or a few USB/homebrew paths). Still does **not** link
  AppInstUtil itself.
- Does not include integrated launcher installer source in the main link.
- Does not link AppInstUtil.
- Does not depend on SystemService, UserService, AppInstUtil, or `kernel_sys`.
- Writes boot/runtime/crash diagnostics under `/data/BFpilot`.
- Writes integrated archive job, status, lock, and extraction logs under
  `/data/BFpilot/archive-integrated`.

Broad firmware support comes from avoiding imports that can fail before
`main()` on some loader/firmware combinations.

### `bfpilot-launcher-installer.elf`

Isolated payload for installing or refreshing `/user/app/BFPL00001` as a
**Media** category home tile. Prefer putting a copy at
`/data/BFpilot/bfpilot-launcher-installer.elf` so `bfpilot.elf` can start it
automatically after the web UI is ready (or inject it yourself once).

- Direct-links the working websrv-style installer dependency set:
  `kernel_sys`, SystemService, UserService, and AppInstUtil.
- Initializes UserService.
- Temporarily sets authid `0x4801000000000013`.
- Validates/stages embedded `param.json` and `icon0.png`.
- `param.json` uses `applicationCategoryType: 65536` (Media tab).
- Registers the tile through AppInst; terminates AppInst when done.
- Restores the original authid before exit.
- Logs to `/data/BFpilot/launcher-installer.log`.

The tile target is:

```text
http://127.0.0.1:5905/
```

### Archive Runtime

Archive extraction is handled by the integrated daemon inside `bfpilot.elf`.
`bfpilot-archive-worker.elf` is a fallback diagnostic build of the same archive
engine, not the normal user flow.

- Reads `/data/BFpilot/archive-integrated/job.ini` in the integrated main ELF.
- Writes `/data/BFpilot/archive-integrated/status.json` in the integrated main
  ELF.
- Logs to `/data/BFpilot/archive-integrated/archive-worker.log` in the
  integrated main ELF.
- Extracts only to allowed roots: `/data`, `/mnt/usb0..7`, and
  `/mnt/ext0..7`.
- Uses a BFpilot-owned staging directory and renames it into place only after a
  successful extraction.
- Does not link AppInstUtil or launcher installer libraries.
- Uses `/data/BFpilot/archive-integrated/daemon.lock` so repeated payload
  injections do not create competing archive daemons.
- Records the daemon PID in `daemon.lock` and exits when its parent file
  manager process is replaced.
- The standalone diagnostic worker keeps the legacy
  `/data/BFpilot/archive/*` paths so it cannot race integrated jobs.

## Why The Split Exists

Working PS5 launcher projects such as ps5 websrv, Payload Manager, and
elf-arsenal use the complete AppInst service/privilege composition. Partial
approaches are unreliable:

- AppInst-only direct imports can be rejected before `main()`.
- Runtime AppInst lookup can return unavailable on some environments.
- Loading AppInst without the matching service/authid context can fail before a
  useful install result is produced.

BFpilot keeps launcher risk out of the file manager. Archive extraction is
inside `bfpilot.elf` so users do not need a second injection, while the
launcher installer remains isolated because those imports are the highest
compatibility risk.

## Boot Marker

Both release ELFs call:

```c
bfpilot_boot_marker(payload_name, build_mode);
```

early in `main()`.

The marker:

- prints `BFpilot BOOT: <payload_name> <build_mode> entered main`
- creates `/data/BFpilot` if possible
- appends payload name, build mode, PID, and build version to
  `/data/BFpilot/boot.log`
- attempts a notification titled `BFpilot BOOT`
- continues if notification fails

No notification and no boot-log entry usually means the loader rejected the ELF
or execution failed before `main()`. A boot marker without a working server or
installer result means the failure happened after `main()`.

## Reload Behavior

`bfpilot.elf` startup is explicit:

1. Boot marker runs.
2. BFpilot checks whether the selected port already has an old BFpilot server.
3. If an old server is found, BFpilot asks it to shut down through
   `/api/control/shutdown`.
4. If shutdown succeeds, the new server starts.
5. If shutdown fails or a file job is busy, BFpilot logs the reason and exits
   cleanly.

The default web port is `5905`. A runtime alternate port can be passed as:

```text
--port 5906
```

## Import Inspection

Run:

```sh
make inspect-imports
```

The target builds and checks only:

- `bfpilot.elf`
- `bfpilot-launcher-installer.elf`
- `bfpilot-archive-worker.elf`

It fails if `bfpilot.elf` contains launcher/AppInstUtil fingerprints or direct
launcher imports. It also fails if the installer is missing any required direct
dependency: `libkernel_sys.sprx`, `libSceSystemService.sprx`,
`libSceUserService.sprx`, or `libSceAppInstUtil.sprx`.

The supported release build contains only the file manager payload and the
isolated launcher installer for normal use. The archive worker build remains
available for diagnostics and comparison testing.
