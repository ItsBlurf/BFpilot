# BFpilot Compatibility Strategy

BFpilot keeps the file manager/archive runtime and the fragile launcher
installer as **separate payloads**. AppInst in the main ELF breaks some
loaders; the Media tile installer is proven working on its own.

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
  `/data/BFpilot/` (or USB/homebrew paths). Does **not** link AppInstUtil.
- Does not depend on SystemService, UserService, AppInstUtil, or `kernel_sys`.
- Writes boot/runtime/crash diagnostics under `/data/BFpilot` only.

### `bfpilot-launcher-installer.elf`

Isolated payload for installing or refreshing `/user/app/BFPL00001` as a
**Media** category home tile (`applicationCategoryType: 65536`).

- Direct-links `kernel_sys`, SystemService, UserService, and AppInstUtil.
- Temporarily sets authid `0x4801000000000013`.
- Stages embedded `param.json` and `icon0.png`.
- Registers the tile through AppInst; terminates AppInst when done.
- Logs to `/data/BFpilot/launcher-installer.log`.

The tile target is:

```text
titleId: BFPL00001
applicationCategoryType: 65536   (Media)
deeplinkUri: http://127.0.0.1:5905/
```

### Data directory (single root)

Canonical: **`/data/BFpilot`**

Do not introduce alternate roots (`/data/bfpilot`, `/data/BFPILOT`, or an
installer directory tree). The installer ELF is a **file** under the
canonical dir when used for auto-bootstrap.
