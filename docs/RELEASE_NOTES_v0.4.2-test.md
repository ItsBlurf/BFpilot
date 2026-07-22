# BFpilot v0.4.2 Test Build

This prerelease contains the file-manager improvements from the complete
`ps5-web-file-manager`, Elf Arsenal, ps5upload, and PS5 filesystem-stability
audit. It remains English-only.

## Payloads

- `bfpilot.elf`: file manager and integrated archive extraction on port 5905.
  This payload intentionally contains no AppInst imports.
- `bfpilot-launcher-installer.elf`: optional separate Media-tile installer.

Inject `bfpilot.elf` first. Inject the installer separately only if the Media
tile is wanted.

## Highlights

- Create and safely edit common UTF-8 source/config/text files.
- Preserve existing UTF-8 BOM and dominant LF/CRLF/CR newline style.
- Detect edit conflicts and use same-directory atomic replacement.
- Drag-and-drop files or recursively enumerated folders for upload.
- Use bounded POST bodies for copy, move, and delete paths.
- Preserve an existing destination until a staged copy is complete.
- Validate and stage existing-directory merges.
- Add `lstat`, no-symlink, same-device, checked-path, inode revalidation, and
  64-level depth protections to recursive operations.
- Serialize upload, copy, delete, text-edit, and archive mutations through the
  single-writer gate.
- Warn before writing into common ShadowMountPlus scan locations.

## Test-build warning

The payloads pass host regressions, PS5 cross-compilation with `-Werror`, ELF
header/import validation, and UI JavaScript parsing. This exact build has not
yet completed the PS5 hardware test ladder. Back up important data, test a
small operation first, and do not stack other debug/file-manager payloads while
evaluating it.

The full upstream adoption/rejection rationale is in
`docs/UPSTREAM_FILE_MANAGER_COMPARISON.md`.

## SHA-256

- `bfpilot.elf`: `f852ce5ada449df41f7bf553809e5ec3910cd568991badf51a53b2611a515500`
- `bfpilot-launcher-installer.elf`: `17a59cdca9d33812480fd1fe060c607f7bd8ab1919f95b09fa9f258e17fcd71d`
