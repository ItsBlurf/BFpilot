# BFpilot Compatibility Strategy

BFpilot now keeps the stable file manager and the fragile launcher installer as
separate payloads.

## Payload Roles

### `bfpilot.elf`

Main release payload. Test this first on every firmware and loader.

- Starts the HTTP file manager on port `5905`.
- Does not install or refresh a launcher tile.
- Does not include integrated launcher installer source.
- Does not link AppInstUtil.
- Does not depend on SystemService, UserService, AppInstUtil, or `kernel_sys`.
- Writes boot/runtime/crash diagnostics under `/data/BFpilot`.

Broad firmware support comes from avoiding imports that can fail before
`main()` on some loader/firmware combinations.

### `bfpilot-launcher-installer.elf`

Isolated optional payload for installing or refreshing `/user/app/BFPL00001`.
Run it only after `bfpilot.elf` works.

- Direct-links the working websrv-style installer dependency set:
  `kernel_sys`, SystemService, UserService, and AppInstUtil.
- Initializes UserService.
- Temporarily sets authid `0x4801000000000013`.
- Validates/stages embedded `param.json` and `icon0.png`.
- Registers the tile through AppInst.
- Restores the original authid before exit.
- Logs to `/data/BFpilot/launcher-installer.log`.

The tile target is:

```text
http://127.0.0.1:5905/
```

## Why The Split Exists

Working PS5 launcher projects such as ps5 websrv, Payload Manager, and
elf-arsenal use the complete AppInst service/privilege composition. Partial
approaches are unreliable:

- AppInst-only direct imports can be rejected before `main()`.
- Runtime AppInst lookup can return unavailable on some environments.
- Loading AppInst without the matching service/authid context can fail before a
  useful install result is produced.

BFpilot keeps that risk out of the file manager. If the installer fails, the
file manager payload remains compatible and usable.

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

It fails if `bfpilot.elf` contains launcher/AppInstUtil fingerprints or direct
launcher imports. It also fails if the installer is missing any required direct
dependency: `libkernel_sys.sprx`, `libSceSystemService.sprx`,
`libSceUserService.sprx`, or `libSceAppInstUtil.sprx`.

The supported release build contains only the file manager payload and the
isolated launcher installer.
