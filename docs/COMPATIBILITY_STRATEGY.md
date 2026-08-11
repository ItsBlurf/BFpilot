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
- AppInst initialization runs on a detached worker thread with a ten-second
  bound, so a delayed Sony service cannot prevent the private server from
  responding or permanently wedge later reinjection.
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

PKG files are validated again immediately before submission. `/data/...` paths
are translated to AppInst's `/user/data/...` view; other absolute local paths
are passed unchanged. NPXS system packages are rejected. The AppInst PlayGo
structure is fixed at the verified 9,984-byte ABI.
The firmware major is read through the safe `kern.version` sysctl; BFpilot does
not carry firmware-specific offsets. Firmware 10 and newer use the SYSTEM
installer identity; older or unknown versions use ShellCore. Identity is set
before `sceAppInstUtilInitialize` in the fresh worker. The worker submits a bare
absolute path through `InstallByPackage`; it never falls back to
`AppInstallPkg`, which was hardware-observed to create metadata without the
actual application content.

The tile path uses a short, mutex-protected identity window and restores the
previous credentials with verification. The private PKG worker does not mutate
the long-running BFpilot process and exits as a whole when retired. Unsupported
loader/kernel capability states fail with diagnostics instead of using guessed
offsets or continuing after a failed credential operation.

Hardware verification on FW 11.60 installed a 186,122,240-byte PKG through the
normal BFpilot API, produced the matching `/user/app/.../app.pkg`, and survived
a clean reinjection that stopped the worker before handoff.

## Data directory

All BFpilot state remains under `/data/BFpilot`. Tile assets are the only files
written under `/user/app/BFPL00001`.
