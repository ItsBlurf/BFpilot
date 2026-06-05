# BFpilot
this is still in BETA phase but its working and does a good enough job.
BFpilot is a small PS5 userland payload that starts a browser-based file
manager on port `5905`.

It is intentionally narrow: no daemons,or anythink that doesnt serve the purpose of a file manager

Current prepared release: `v0.2.1`

## Recommended Payload

Use `bfpilot.elf` as the main release payload.

`bfpilot.elf` is the maximum-compatibility file manager:

- Starts the HTTP file manager at `http://<PS5_IP>:5905/`.
- Does not install or refresh a PS5 home-screen tile.
- Does not compile or link `src/app_installer.c`.
- Does not link `libSceAppInstUtil`.
- Does not depend on AppInstUtil, SystemService, UserService, or optional
  launcher-only Sony libraries.
- Writes boot and runtime diagnostics under `/data/BFpilot`.

Use `bfpilot-debug.elf` when you need extra proof that the payload reached
`main()` and progressed through startup. It is still file-manager-only and does
not link AppInstUtil.

Use `bfpilot-full.elf` when you want the 0.2.0-style integrated launcher
install path. This is the best first installer-capable build for your 11.6 case:
it starts BFpilot and attempts the home-screen tile install inside the same
process, after optional PS5 service initialization. It does not direct-link
AppInstUtil; it dynamically resolves the same AppInst functions used by the
previous full build.

Use `bfpilot-launcher-installer.elf` only after `bfpilot.elf` works. It is a
separate optional payload whose only job is installing or refreshing the
home-screen tile. It intentionally direct-links AppInstUtil, matching the
websrv/Payload Manager/ftpsrv launcher-install pattern. If the loader rejects
that import, the installer can fail before `main()`. The file manager is still
unaffected.

Use `bfpilot-launcher-installer-safe.elf` only for diagnostics. It does not
direct-link AppInstUtil and reports whether AppInstUtil is already reachable
through the safe runtime path.

## What It Does

- Runs a file-manager web UI at `http://<PS5_IP>:5905/`.
- Supports browse, upload, download, copy, move, rename, mkdir, and delete.
- Shows progress for copy, move, delete, and browser uploads.
- Creates missing copy/move target folders automatically.
- Keeps default shortcuts at `/data/homebrew` and `/mnt/usb0`.
- Writes boot markers to `/data/BFpilot/boot.log`.
- Writes runtime logs to `/data/BFpilot/log.txt`.
- Writes fatal-signal diagnostics to `/data/BFpilot/crash.log`.

The optional launcher tile, if installed by the separate installer payload,
opens:

```text
http://127.0.0.1:5905/
```

## Runtime Notes

Inject the ELF to your payload loader on port `9021`.

First test:

```text
send bfpilot.elf to <PS5_IP>:9021
open http://<PS5_IP>:5905/
open http://<PS5_IP>:5905/api/status
open http://<PS5_IP>:5905/api/diag
```

Debug test:

```text
send bfpilot-debug.elf to <PS5_IP>:9021
open http://<PS5_IP>:5905/api/diag
check /data/BFpilot/boot.log
check /data/BFpilot/log.txt
```

Full install test:

```text
send bfpilot-full.elf to <PS5_IP>:9021
open http://<PS5_IP>:5905/
check /data/BFpilot/log.txt
```

Alternate port test while an old instance is still running:

```text
send bfpilot-debug.elf --port 5906 to <PS5_IP>:9021
open http://<PS5_IP>:5906/api/status
```

Launcher installer test:

```text
send bfpilot-launcher-installer.elf to <PS5_IP>:9021
check /data/BFpilot/launcher-installer.log
```

If `bfpilot-launcher-installer.elf` gives no notification and no log, send
`bfpilot-launcher-installer-safe.elf` next. If the safe installer reaches
`main()` but says AppInstUtil is unavailable, the direct installer is being
rejected before it can run.

If the launcher installer appears to do nothing, run the probes in this order:

```text
tests/installer_enter_probe.elf
tests/installer_linkonly_appinst.elf
tests/installer_runtime_resolve_appinst.elf
```

If `installer_linkonly_appinst.elf` does not create
`/data/BFpilot/linkonly_appinst_entered.txt`, direct AppInstUtil imports are
failing before `main()` on that loader/firmware.

If `installer_runtime_resolve_appinst.elf` logs
`kernel_dynlib_handle libSceAppInstUtil.sprx rc=0xffffffff`, AppInstUtil is not
reachable through BFpilot's safe runtime path on that firmware/loader. That does
not invalidate the `bfpilot-full.elf` path on firmware where the same runtime
resolution is available, such as the 11.6 case you reported.

## Diagnostics

Save these files when reporting failures:

```text
/data/BFpilot/boot.log
/data/BFpilot/log.txt
/data/BFpilot/crash.log
/data/BFpilot/launcher-installer.log
```

If there is no notification and no `/data/BFpilot/boot.log` entry, the payload
probably failed before `main()` or the loader rejected it. If the boot marker
appears but the server does not start, the failure happened after `main()`.

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean all
```

Individual targets:

```sh
make bfpilot
make debug
make full
make launcher-installer
make hello-boot
make hello-http
make hello-notify
make installer-enter-probe
make installer-linkonly-appinst
make installer-runtime-resolve-appinst
make inspect-imports
```

Outputs:

```text
bfpilot.elf
bfpilot-debug.elf
bfpilot-full.elf
bfpilot-launcher-installer.elf
bfpilot-launcher-installer-safe.elf
tests/hello_boot.elf
tests/hello_http.elf
tests/hello_notify.elf
tests/installer_enter_probe.elf
tests/installer_linkonly_appinst.elf
tests/installer_runtime_resolve_appinst.elf
```

The compatibility check must pass for release builds:

```sh
make inspect-imports
```

It fails if `bfpilot.elf` or `bfpilot-debug.elf` contain AppInstUtil,
`sceAppInst`, `app_installer`, or `BFPL00001` installer fingerprints. Direct
`libSceAppInstUtil.sprx` import is allowed only for
`bfpilot-launcher-installer.elf` and
`tests/installer_linkonly_appinst.elf`.

## Project Layout

- `src/lite_main.c` starts the file-manager payload, handles clean reload, and
  starts the web server.
- `src/boot_marker.c` writes the earliest cross-payload boot marker.
- `src/websrv_lite.c` serves `/`, `/api/status`, `/api/diag`, `/fs`, and
  `/api/fs/*`.
- `src/transfer.c` contains upload, copy, move, delete, and long-running job
  handling.
- `src/launcher_installer_main.c` is the isolated optional launcher installer.
- `tests/` contains small probe payloads for loader, HTTP, notification, and
  AppInstUtil testing.

See [docs/COMPATIBILITY_STRATEGY.md](docs/COMPATIBILITY_STRATEGY.md) for the
firmware compatibility strategy and [docs/FIRMWARE_TESTING.md](docs/FIRMWARE_TESTING.md)
for the test protocol.
