# BFpilot v0.4.2 Test Build

This test build added text editing, folder uploads and safer file operations.
These notes describe v0.4.2; use the current README for setup instructions.

## Payloads

- `bfpilot.elf`: file manager and archive extraction on port 5905.
- `bfpilot-launcher-installer.elf`: separate optional Media-tile installer.

Load the file manager first, then the installer if you want the Media tile.

## Changes

- Create and edit UTF-8 text, source and configuration files.
- Preserve existing text encoding markers and newline style; detect conflicting edits.
- Upload files and folders with drag-and-drop in supported browsers.
- Keep an existing file until its replacement is complete.
- Check paths, filesystem boundaries and symbolic links during folder operations.
- Run file-changing operations one at a time.
- Warn before writes to common ShadowMountPlus scan folders.

## SHA-256

- `bfpilot.elf`: `f852ce5ada449df41f7bf553809e5ec3910cd568991badf51a53b2611a515500`
- `bfpilot-launcher-installer.elf`: `17a59cdca9d33812480fd1fe060c607f7bd8ab1919f95b09fa9f258e17fcd71d`
