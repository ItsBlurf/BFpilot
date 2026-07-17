# BFpilot Compatibility Strategy

BFpilot follows the **Payload Manager** model: one primary ELF provides the
web service **and** the PS5 home **Media** tile install. An optional separate
installer remains for recovery only.

## Payload Roles

### `bfpilot.elf` (primary)

Main release payload. Test this first on every firmware and loader.

- Starts the HTTP file manager on port `5905`.
- Starts the integrated archive daemon for RAR, 7z, split 7z, and ZIP
  extraction.
- **Installs/refreshes the Media home tile** (`BFPL00001`, category 65536,
  deeplink `http://127.0.0.1:5905/`) from embedded `assets-app/param.json` +
  `icon0.png` via AppInstUtil (same idea as Payload Manager `app_installer.c`).
- Can **load other payloads** by streaming them to elfldr (`127.0.0.1:9021`)
  via `/api/fs/launch`.
- Optional **recovery**: if embedded install fails and
  `/data/BFpilot/bfpilot-launcher-installer.elf` exists as a **file**, injects
  it once via elfldr.
- Writes boot/runtime/crash diagnostics under **`/data/BFpilot` only**.
- Writes integrated archive job/status/logs under
  `/data/BFpilot/archive-integrated`.

### `bfpilot-launcher-installer.elf` (optional recovery)

Isolated payload for installing or refreshing `/user/app/BFPL00001` if the
main ELF’s embedded install fails on a particular loader. Place the **file**
at `/data/BFpilot/bfpilot-launcher-installer.elf` — do **not** create a
sibling directory tree under `/data`.

- Direct-links AppInstUtil + UserService + SystemService + `kernel_sys`.
- Temporarily sets authid `0x4801000000000013`.
- Stages embedded param/icon; Media category `65536`.
- Logs to `/data/BFpilot/launcher-installer.log`.

The tile target is:

```text
titleId: BFPL00001
applicationCategoryType: 65536   (Media)
deeplinkUri: http://127.0.0.1:5905/
```

### Data directory (single root)

Canonical: **`/data/BFpilot`**

Do not introduce:

- `/data/bfpilot`
- `/data/BFPILOT`
- `/data/bfpilot-launcher-installer` (as a directory root)

Installer/helper ELFs are files under the canonical dir, not separate trees.

## Archive worker

`bfpilot-archive-worker.elf` is a build diagnostic / standalone extractor.
Integrated extract in `bfpilot.elf` is the normal path.
