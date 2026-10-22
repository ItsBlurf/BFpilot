# WFM 7-Zip Helper

A separate extraction helper for [PS5 Web File Manager](https://github.com/owendswang/ps5-web-file-manager). It uses 7-Zip's native archive handlers and runs as an independent PS5 payload.

## Brief

`wfm-7zip-helper.elf` provides archive extraction for PS5 Web File Manager. The file manager and helper are separate programs and communicate only through a local Unix socket. The helper contains no web UI, archive creation feature, or network service.

Place the helper at:

```text
/data/wfm/wfm-7zip-helper.elf
```

PS5 Web File Manager will start it through the local ELF loader during startup. File-manager features other than extraction do not require the helper.

## Features

- Extract 7z, RAR/RAR5, ZIP/ZIPX, TAR, GZIP, BZIP2, XZ, Zstandard, Unix Compress, LZMA, PPMd, ARJ, CPIO, LZH/LHA, CAB, XAR/XIP, and numbered split archives.
- Support files ending with `.7z`, `.001`, `.zip`, `.zipx`, `.rar`, `.part01.rar`, `.arj`, `.bz2`, `.bzip2`, `.tbz`, `.tbz2`, `.cab`, `.gz`, `.gzip`, `.tgz`, `.tpz`, `.lzh`, `.lha`, `.tar`, `.xz`, `.txz`, `.z`, `.taz`, `.zst`, `.tzst`, `.xar`, `.xip`, `.cpio`, `.lzma`, and `.pmd` through PS5 Web File Manager.
- Decode and extract compound TAR archives ending with `.tar.gz`, `.tgz`, `.tar.bz2`, `.tbz`, `.tbz2`, `.tar.xz`, or `.txz`.
- Load subsequent multipart volumes automatically. Only the first volume is submitted directly: base `.rar`, `.part01.rar`, or `.001`.
- Probe registered 7-Zip handlers after trying the extension-preferred handler, allowing supported archives with an incorrect extension to be recognized.
- Support encrypted archives, overwrite confirmation, progress, current-file reporting, cancellation, and sequential multi-archive tasks.
- Retain running and completed task state while PS5 Web File Manager restarts, provided the helper process remains alive.
- Reject unsafe archive paths and prevent output traversal through symbolic links.
- Skip archive links, special nodes, and incompatible destination node types without following or replacing them.
- Publish each file through a same-directory `.wfm-part-*` temporary file before renaming it into place.
- Preserve archive modification times and set extracted files and directories to `0777`.
- Check known expanded sizes against available space before extraction and retain per-file and write-time no-space checks.
- Use an extraction-only 7-Zip build without GUI, FileManager, Agent, SFX, benchmark, archive creation/update code, encoders, or RAR creation support.

## Build

The PS5 build requires [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk#quick-start):

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Build both the PS5 helper and Linux test binary from the repository root:

```sh
make
```

Outputs:

```text
wfm-7zip-helper.elf
wfm-7zip-helper-linux
```

Individual targets are also available:

```sh
make ps5
make linux
```

Run the Linux helper for local integration testing:

```sh
./wfm-7zip-helper-linux
```

The Linux helper listens on `/tmp/wfm-7zip-helper.sock`. The PS5 helper listens on `/system_tmp/wfm-7zip-helper.sock`.

## Usage

Copy `wfm-7zip-helper.elf` to the PS5 location expected by PS5 Web File Manager:

```text
/data/wfm/wfm-7zip-helper.elf
```

Start PS5 Web File Manager normally. During startup it will launch the helper through the ELF loader on `127.0.0.1:9021` when available.

The helper can also be sent to an ELF loader manually:

```sh
export PS5_HOST=ps5_ip_address
nc -q0 "$PS5_HOST" 9021 < wfm-7zip-helper.elf
```

Starting a new helper terminates an older process with the exact name `wfm-7zip-helper` and replaces its local socket. Do not restart the helper while an extraction is running.

## Notes

- Extraction is sequential and non-transactional. Files completed before a later failure or cancellation remain in place.
- Overwrite replaces regular files and merges directories; there is no old-file backup or batch rollback.
- Cancellation removes only the current unfinished temporary file.
- A missing helper disables extraction only; other PS5 Web File Manager operations continue to work.
- Tasks survive a web-file-manager process restart, but not a helper crash, helper restart, or PS5 restart.
- The IPC framing, existing message numbers, and existing field meanings are compatibility-stable. Future changes must use optional extension fields or new message types rather than changing existing messages.
- Detailed lifecycle, IPC, progress, and output-safety documentation is in [`CPP/7zip/Bundles/WfmHelper/README.md`](CPP/7zip/Bundles/WfmHelper/README.md).

## Credits

- **[7-Zip](https://7-zip.org/):** Archive handlers, decoders, crypto readers, and supporting library code.
- **[PS5 Web File Manager](https://github.com/owendswang/ps5-web-file-manager):** Browser UI, task authority, extraction requests, confirmation flow, and helper integration.
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk):** PS5 payload toolchain and platform libraries.

## License

This repository is based on **7-Zip 26.03**, upstream commit `0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17`, with WFM-specific extraction-helper modifications. It is not covered by one permissive license:

- Most 7-Zip source code is licensed under **GNU LGPL-2.1-or-later**.
- `CPP/7zip/Compress/Rar*` code is licensed under **GNU LGPL-2.1-or-later with the unRAR restriction**. It must not be used to recreate the proprietary RAR compression algorithm or to develop a RAR-compatible archiver.
- `CPP/7zip/Compress/LzfseDecoder.cpp` and `C/ZstdDec.c` are licensed under **BSD-3-Clause**.
- `C/Xxh64.c` is licensed under **BSD-2-Clause**.
- Files explicitly marked as public domain remain public domain.
- Unless a modified file states otherwise, the WFM-specific helper modifications are distributed under **GNU LGPL-2.1-or-later**.

The authoritative 7-Zip license text and component exceptions are in [`DOC/License.txt`](DOC/License.txt). The exact upstream revision, linked exceptions, and WFM modification summary are in [`CPP/7zip/Bundles/WfmHelper/THIRD_PARTY_NOTICES.md`](CPP/7zip/Bundles/WfmHelper/THIRD_PARTY_NOTICES.md). Copyright and license notices inside individual source files remain authoritative for those files.

When distributing `wfm-7zip-helper.elf`, preserve the applicable notices and provide the corresponding helper source and build scripts, or a valid written source offer, as required by the LGPL. Include or link `DOC/License.txt` and `THIRD_PARTY_NOTICES.md` in the release materials. The BSD notices and the unRAR restriction must also be preserved.

`wfm-7zip-helper.elf` is not embedded in or linked with `web-file-mgr.elf`; they are independently built and distributed programs communicating over a local Unix socket. PS5 Web File Manager's GPL license does not replace or remove the helper's LGPL, BSD, public-domain, or unRAR terms.
