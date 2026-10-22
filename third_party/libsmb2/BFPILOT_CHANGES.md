# BFpilot additions to libsmb2 6.1.0

Original source pin and license are in BFPILOT_SOURCE.json. The complete library
source remains available for rebuilding/relinking BFpilot. Existing `stat` and
non-replacing `rename` behavior is preserved.

- `smb2_lstat[_async]` requests FILE_OPEN_REPARSE_POINT for metadata.
- `smb2_rename_replace[_async]` explicitly requests replacement. Ordinary rename
  still refuses an existing destination; replacement is chosen only by callers
  that have staged and checked the new output.
- O_NOFOLLOW on file open requests FILE_OPEN_REPARSE_POINT, where the target
  platform exposes O_NOFOLLOW, and requests READ_ATTRIBUTES so callers can
  verify the opened type even with a write-only handle. Native Samba refused
  fstat with EACCES on an ordinary write-only handle lacking that attribute
  right. Read-data access is not added. Callers must inspect returned metadata.
- `ps5/config.h` and BFpilot's build script describe the verified PS5 SDK build.
- Optional per-directory entry and per-context aggregate directory-byte limits
  apply while decoding/prefetching, before iteration. All cached bytes are
  released on close or decoding failure. Zero keeps upstream defaults. BFpilot
  sets 100002 entries (including dot entries) and 64MiB per context, with four
  application contexts at most. The byte counter covers retained entry/name
  payloads, not allocator overhead, transport buffers or total process RAM.

These flags express the client's request. A server can resolve native POSIX
symlinks without exposing a Windows reparse point. Native Samba 4.19.5 controls
with its default `follow symlinks = yes` demonstrated that distinction. Never
describe these additions as a universal no-follow or server-containment guarantee.
Remote walkers also require bounded depth and visited directory identities.
