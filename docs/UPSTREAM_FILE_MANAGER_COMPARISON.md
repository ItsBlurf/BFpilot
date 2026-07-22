# Upstream File-Manager Comparison

Research date: 2026-07-22

## Exact sources checked

| Source | Revision checked | Notes |
| --- | --- | --- |
| BFpilot | `00028d2dcf5afd566d321bf32c0b1fd2d9698c7f` / v0.4.1 baseline | MIT; implementation target |
| `owendswang/ps5-web-file-manager` | every tag from v0.1 (`1ed55b4`) through v0.9 (`1a24160`) plus the untagged v0.5 release commit `ba0704d` | GPL-3.0 |
| local `projects/elf-arsenal` | complete local snapshot, including `assets/files.html`, `src/transfer.c`, and bundled ShadowMountPlus | no top-level license file in this snapshot; embedded components have their own licenses |
| `phantomptr/ps5upload` | local `364d165f5a6e1bd370ef3949487d2d1f305f6261`, especially `payload/src/runtime.c` | reference only |
| PS5 workspace research | filesystem traversal, I/O limits, debugging notes, SDK samples | reference only |

No GPL or uncertain-license source/assets were copied into MIT-licensed BFpilot.
The adopted behavior was implemented independently against BFpilot's existing
architecture.

## `ps5-web-file-manager` release-by-release audit

| Release | Upstream change examined | BFpilot decision |
| --- | --- | --- |
| v0.1 | Initial browse, upload, download, create folder, rename, copy, move, delete | Already present, with staging, progress, conflict handling, and safer traversal |
| v0.2 | Lower-firmware frontend adjustments and Media launcher tile | Already covered by BFpilot's controller/mobile layout and separate Media-tile installer; no upstream asset imported |
| v0.3 | Create/edit text files and a broader text-extension list | Adopted: create plus edit API/UI; added `.md`, `.lua`, `.htm`, `.hpp`, and `.csv`; BFpilot also validates UTF-8, limits files to 5 MiB, detects edit conflicts, and preserves an existing BOM/newline style |
| v0.4 | Image preview, overwrite-space reclaim calculation, permissions, completion details, large-directory UI | Image viewer, chmod, persistent completion details, ETA, and virtualized rows were already present. Deleting the known-good destination to reclaim space was rejected; BFpilot stages the full replacement first |
| v0.5 | Same-filesystem move without copy preparation; experimental copy workers/pipeline | Direct same-device `rename` was already present. Extra copy workers/pipeline rejected pending PS5 hardware evidence |
| v0.6 | Return to safer single-thread recursive copy; timing and ETA | Already present; hardened with physical same-device walks, checked joins, fatal-error abort, inode revalidation, and a 64-level recursion cap |
| v0.7 | Older-browser CSS changes, installer icon/param handling, ShadowMount warning | Warning adopted for every BFpilot write path to common scan roots. Installer behavior is architecture-specific, while BFpilot keeps its separate known-working tile installer |
| v0.8 | Mobile layout and multilingual filename fixes | Narrow/mobile UI and UTF-8 filenames were already supported. Translation bundles deliberately omitted: BFpilot remains English-only |
| v0.8b | Correct internal M.2 available-space display | Already present through `statvfs`, including explicit USB/ext places |
| v0.9 | Multi-path POST operations and one-time destination-writable validation | POST bodies adopted for copy/move/delete so Unicode/long paths do not overflow the request target. BFpilot deliberately keeps one conservative job per source for conflict prompts, cancellation, and single-writer recovery |

The current upstream large-file copy pipeline (enabled at 256 MiB) was inspected
but not adopted. Its small-file workers are disabled, earlier releases changed
the threading model, and there is no BFpilot hardware benchmark showing that
added buffers and synchronization improve PS5 stability or throughput.

## Elf Arsenal audit

| Feature/source | BFpilot decision |
| --- | --- |
| Dual-pane file page with browse/target, copy, move, delete, mkdir, rename, upload, download, USB discovery | Core actions already present. BFpilot's Places + browse + explicit Target panel supplies the same source/destination model without duplicating two full directory views on the PS5 browser or narrow screens |
| Recursive drag-and-drop folder upload via WebKit `FileSystemEntry` | Adopted independently. The drop zone enumerates folders when that browser API exists, falls back to ordinary dropped files, preserves relative paths, and still uploads sequentially |
| Programmatic `webkitdirectory`/`directory` attributes | Adopted as an older-WebKit compatibility assist in addition to the HTML attribute |
| Transfer progress, speed, ETA, and cancel | Already present and backed by BFpilot's single-job status API |
| Direct destination writes/truncation in its transfer backend | Rejected. BFpilot keeps destination-filesystem staging and only renames a complete result into place |
| Recursive transfer implementation | Rejected as a replacement: it lacks BFpilot's complete `lstat`/`st_dev`/checked-join/source-inode safeguards |
| Bundled ShadowMountPlus behavior | Used only to validate scan-root warnings and concurrency risk; BFpilot neither mounts images nor edits another payload's configuration |

The local `projects/elf-arsenal-main` directory was also checked and is empty;
it contains no second implementation to merge.

## ps5upload and local research audit

| Candidate | BFpilot decision |
| --- | --- |
| Recursion limit (64) | Adopted for size, copy, delete, forced cleanup, merge validation, and merge finalization walkers; excess nesting fails with `ELOOP` |
| Same-device rename guard | Already present; cross-device operations use staged copy/delete rather than an unsafe rename assumption |
| Progress, cancellation, cleanup | Already present |
| Large asynchronous/double-buffer copy and 16 MiB buffers | Deferred. Local PS5 guidance favors a sequential 4-8 MiB copy buffer until hardware measurement justifies more memory/writeback pressure; BFpilot uses 8 MiB |
| `posix_fallocate` | Deferred for transfer data. BFpilot's `statvfs` preflight, safety margin, same-filesystem staging, and zero-write handling avoid known large preallocation problems in the upload path |
| Resumable desktop transfer protocol | Not applicable to an ordinary single HTTP browser upload without a new chunk/session protocol; partial uploads are cleaned safely |
| Generic retry after `EIO`, `ESTALE`, `EBADF`, or `EFAULT` | Rejected. These are fatal vnode/descriptor/storage signals in this workspace and the operation aborts without retrying the same descriptor |

## Adopted safety and UX changes

- Text create uses `openat` with `O_CREAT | O_EXCL` beneath a no-follow directory
  descriptor; save uses a unique same-directory temporary file, preserves mode,
  `fsync`s completed data, and atomically renames only after success.
- The editor's version token prevents silently overwriting a file changed since it
  was opened. Reads/saves are strict UTF-8 and capped server-side at 5 MiB.
- Copy overwrite retains the old regular file until a complete staged replacement
  is ready. Existing folder conflicts use a fully prepared, prevalidated staged
  merge, rejecting type/device conflicts before destination mutation.
- Recursive size, copy, merge, staging-cleanup, and delete walks use `lstat`, never
  follow symlinks, reject mount/device crossings and overlong paths, cap depth at
  64, and stop on the first error.
- Regular copy sources are reopened no-follow and checked against their original
  device/inode before bytes are read. Upload, copy, delete, editor, and integrated
  archive mutations share the single-writer gate.
- Copy, move, and delete use bounded form POST bodies. Upload, archive, and copy
  staging names include monotonic nanoseconds to avoid collisions.
- The UI warns before copy, move, extract, upload, text create, or text save in
  common ShadowMountPlus scan locations.

## Stability conclusions and limits

- Staging stays on the destination filesystem so final replacement can use
  same-filesystem `rename`; it also preserves the previous destination on failure.
- `statvfs(...).f_bavail` plus a 256 MiB margin intentionally requires room for a
  complete staged overwrite. This costs temporary space in exchange for recovery.
- BFpilot keeps one recursive writer and one sequential upload/file buffer instead
  of creating parallel I/O pressure on PFS/exFAT/USB paths.
- Static checks and cross-compilation can catch ABI, import, traversal, and error-
  handling defects, but they cannot prove that a payload will never crash or kernel
  panic on every firmware, jail/mount state, HEN, or storage device. Final confidence
  requires clean hardware tests on the intended firmware and media.
