# BFpilot 0.5.0

## Added

- SMB2/3 connections, share browsing, file operations, transfers and text editing.
- SMB-hosted PKG details, icons and installation sources; payload loading from SMB.
- Built-in archive service with AES ZIP and additional formats.
- Multiple-archive extraction with destination and merge/replace options.
- Individual selected-item downloads alongside combined TAR downloads.
- Results beside Logs and Exit for file, archive and PKG status.

## Fixed

- Large-folder copies waiting for a full file count before starting.
- Overwrite, merge, cancellation and recovery handling.
- Extracted-file permissions; new extracted files and directories use `0777`.
- RAR cancellation during output, contributed by oz5hk.
- PKG request handling and queue-status updates.
- Long paths, folder sizing, double-clicks and selection in large directories.
- Finished archive progress reappearing after reload and delayed replies after sleep.

## Changed

- Updated to PS5 payload SDK 0.45 and LLVM 23.1.4.
- Installer identity now uses the SDK firmware reading before the kernel-version fallback.
- Fixed Windows helper linking and rebuilds after SDK/compiler changes.
- English-only interface; Triangle/F1 search shortcut removed.
- R1 opens the highlighted item regardless of selection. Square selects;
  X keeps normal browser behavior with cursor.

## Known limitations

- Direct SMB extraction is unsupported; copy the archive locally first.
- A folder merge does not undo replacements that finished before a later error.
- PKG queue acceptance is separate from installation completion.

## Update

Load `bfpilot.elf` and reload the browser page. No separate helper is required.
Settings remain under `/data/BFpilot`. Checksums: [SHA256SUMS](../SHA256SUMS).

Credits and licenses: [NOTICE.md](../NOTICE.md).

## Support

- [PayPal](https://paypal.me/nblurf)
- Bitcoin (BTC): `1CW8JWSnWc5w7yKhYk7ixAFG487nU5Jrzc`
- USDT (Tron / TRC20): `TRcrCeonXzgyxjR4LGzo4JdfEHYGGqD3gX`
