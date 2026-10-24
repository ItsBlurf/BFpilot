# Installation and compatibility

BFpilot ships as one `bfpilot.elf`. The file manager, archive service, package
services and Media-tile registration are included in it.

## Media tile

The tile uses ID `BFPL00001`, category `65536` (Media), and opens
`http://127.0.0.1:5905/`. BFpilot starts its HTTP server before checking the tile,
so registration errors do not prevent access to the file manager.

Tile assets are checked against the included files. Registration is skipped only
when the files match and an earlier registration succeeded. BFpilot uses the
TitleDir function when available and falls back to `sceAppInstUtilAppInstallAll`
when that function cannot be resolved.

## Package installation

Packages are validated again before submission. Supported headers include
standard `7F CNT` and PS5-native fake-package `7F FIH`. Sony reads FIH metadata;
BFpilot reports the content ID returned by the installer. NPXS system packages
are rejected. Internal `/data/...` paths are mapped to `/user/data/...` for
AppInst; other absolute local paths are passed through.

Package submission runs in a separate process, started through the existing ELF
loader when needed. Sony associates AppInst with the process's identity during
initialization, so the worker sets that identity first. Firmware 11 and newer
use SYSTEM; older or unknown versions use ShellCore. Firmware identification
uses the SDK query first, with the kernel version as a fallback. The PlayGo structure is
9,984 bytes. These details are defined in `src/appinst_pkg_abi.h` and the package
worker source.

The worker accepts commands on loopback port 9041. Initialization waits up to
10 seconds in the worker and 15 seconds for direct tile registration. A known
failed or outdated BFpilot worker can be stopped before its replacement starts;
an unknown service on that port is left alone.

The tile-registration code temporarily changes process credentials and checks
their restoration afterward. The separate package worker avoids changing the
file manager's credentials. Missing loader or kernel support produces an error
instead of continuing with guessed offsets.

## SMB package sources

SMB packages are served to Sony through an included local HTTP service. It reads
requested ranges from the server without copying the whole package to the PS5.
It checks that the source has not changed and keeps running across an ordinary
BFpilot exit or reload. Unused sources expire after 24 hours without activity.

Keep the SMB server and source available until installation finishes. A rejected
submission releases its source; an unconfirmed response keeps the source because
Sony may already have accepted it. Check PS5 notifications and installed content
before retrying. Queue acceptance is separate from installation completion.

## Testing

Tested on firmware 11.60, with desktop UI checks in Chrome, Firefox and WebKit.

## Known limitations

- Direct SMB extraction is unsupported. Copy locally, extract, then copy the
  result back if needed.
- A folder merge replaces files individually. An error does not undo replacements
  that already finished. See [temporary files and recovery](PRIVATE_STAGING.md).
- An extraction batch accepts up to 2,048 local archive files.
- PKG queue acceptance reports submission to Sony, not completed installation.

## Files on the PS5

Settings, logs, the index and recovery data are under `/data/BFpilot`. Temporary
output stays on the destination filesystem, including USB/ext drives when used.
Media-tile assets are under `/user/app/BFPL00001`.
