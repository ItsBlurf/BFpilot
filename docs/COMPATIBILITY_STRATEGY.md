# BFpilot Compatibility Strategy

BFpilot ships one payload: `bfpilot.elf`. It contains the file manager,
RAR/7z/ZIP extraction, local PKG submission, and Media-tile registration.

## Installer architecture

- The release contains one distributable file, `bfpilot.elf`. No helper must be
  copied, selected, or injected by the user.
- Media-tile registration runs directly in BFpilot and links
  `SceAppInstUtil` with its `SceIpmi` dependency.
- Full-content PKG submission uses a small worker ELF embedded as bytes inside
  `bfpilot.elf`. BFpilot streams it to the existing elfldr only when a PKG is
  first requested. It is a private build component, not a release artifact.
- The worker listens only on PS5 loopback port 9041, accepts a narrow serialized
  `PING`/`INSTALL`/`STOP` protocol, and is stopped during clean shutdown or
  reinjection. A stale, failed, or mismatched worker is retired once before a
  clean embedded copy is started.
- A fresh process is necessary because Sony binds the AppInst IPMI peer during
  initialization; changing a long-running web process after that point can be
  rejected even when the credential values look correct.
- AppInst initialization is bounded: ten seconds in the private PKG worker and
  fifteen seconds in the direct Media-tile path. A delayed Sony service cannot
  permanently wedge either installation path or later reinjection.
- Starts the HTTP listener before the asynchronous tile check, so file-manager
  startup is observable even if AppInst returns an error.
- Compares embedded tile assets byte-for-byte and also requires a marker written
  only after successful registration before skipping AppInst. This keeps normal
  reinjection idempotent without confusing staged files for a registered tile.
- Uses the firmware-compatible TitleDir NID when available and falls back to
  `sceAppInstUtilAppInstallAll` only when that symbol cannot be resolved.

The tile is `BFPL00001`, category `65536` (**Media**), and opens
`http://127.0.0.1:5905/`.

## PKG installation

PKG files are validated again immediately before submission. BFpilot accepts
both standard `7F CNT` packages and PS5-native fake-package `7F FIH` files.
FIH metadata is intentionally left to Sony's installer because that header does
not expose the CNT content-ID fields; the worker reports Sony's returned ID
after submission. `/data/...` paths
are translated to AppInst's `/user/data/...` view; other absolute local paths
are passed unchanged. NPXS system packages are rejected. The AppInst PlayGo
structure is fixed at the verified 9,984-byte ABI.
The firmware major is read through the safe `kern.version` sysctl; BFpilot does
not carry firmware-specific offsets. Firmware 11 and newer use the SYSTEM
installer identity; older or unknown versions use ShellCore. This follows the
hardware-observed AppInst authority boundary: using SYSTEM too early breaks
the older URL/path preflight, while using ShellCore on firmware 11+ can create
a title record without copying its content. Identity is set
before `sceAppInstUtilInitialize` in the fresh worker. The worker submits a bare
absolute path through `InstallByPackage`; it never falls back to
`AppInstallPkg`, which was hardware-observed to create metadata without the
actual application content.

The tile path uses a short, mutex-protected identity window and restores the
previous credentials with verification. The private PKG worker does not mutate
the long-running BFpilot process and exits as a whole when retired. Unsupported
loader/kernel capability states fail with diagnostics instead of using guessed
offsets or continuing after a failed credential operation.

Hardware verification on FW 11.60 installed the 37,633,547-byte PS5-native FIH
Internet Browser package through the normal BFpilot API. Sony returned content
ID `IV9999-MOUU12023_00-XXXXXXXXXXXXXXXX`, and the resulting
`/user/app/MOUU12023/app.pkg` contained 37,617,664 bytes. The test title was
then uninstalled successfully.

## Data directory

All BFpilot state remains under `/data/BFpilot`. Tile assets are the only files
written under `/user/app/BFPL00001`.
