# Performance and stability design

BFpilot optimizes for sustained PS5 throughput. Peak speed is not accepted if
it weakens staging, mount boundaries, cancellation, or fatal-I/O handling.

## Current data paths

| Operation | Fast path | Stability boundary |
|---|---|---|
| Upload | 2 MiB page-aligned `recv` then `write`; 4 MiB requested receive window | One writer, free-space preflight, same-directory staging, 256 MiB writeback checkpoints, rename only after close |
| Download | 2 MiB page-aligned sequential `read` then socket send; 4 MiB requested send window | Buffered on every filesystem; short source reads fail instead of returning a truncated success |
| Copy | Two page-aligned 8 MiB slots for regular files at least 64 MiB; serial fallback for smaller files or allocation/thread failure | Source fd/inode/device/size revalidation, no-follow reopen, same-device recursive fence, staging, cancellation, 256 MiB writeback checkpoints |
| Move | Same-filesystem `rename` | Cross-device moves use the staged copy path, then delete the source only after finalization |
| ZIP extract | 16 MiB aligned input and 64 MiB aligned output buffers | Free-space preflight, path validation, CRC/size validation, staging, preallocation only before decoding a known-size member, writeback checkpoints |
| RAR/7z extract | Upstream multi-thread decoders with bounded thread selection | Existing staged extraction, safety limits, cleanup, and format checks remain unchanged |
| Index/list/search | Directory-relative `fstatat(..., AT_SYMLINK_NOFOLLOW)`; search stops after the requested page plus one match | Sequential crawl, `st_dev` boundaries, visited inode/device set, fatal-error threshold; `matchedExact` reports early-stop semantics |

## Deliberately excluded

* Global PS5 `sendfile`: FreeBSD documents its zero-copy advantage, but the
  available PS5 filesystem implementations and field reports disagree about
  PFS/exFAT/nullfs pager safety. BFpilot keeps the buffered path until a
  filesystem whitelist is proven on supported firmware and media.
* `F_NOCACHE` on download fds: it conflicts with `sendfile`-style VM paging and
  has caused regressions in PS5 transfer implementations.
* Double-buffered browser upload: disk writes can keep the receiver away from
  `recv` long enough to collapse the TCP window. The stable STOR cadence is
  retained.
* Whole-file `posix_fallocate` after a browser starts POSTing: on PFS it can
  block long enough to stall the connection. Archive members may preallocate
  before decode because there is no live network body to drain.
* Parallel directory writers: metadata and PFS contention can be slower and
  less stable than one ordered writer. Only the read side of a single large
  file overlaps its writer.

## Telemetry

`GET /api/fs/job/status` reports copy mode counts, bytes read/written, buffer
size, writeback checkpoint count, rate, and device ids.

`GET /api/fs/transfer/stats` reports the last upload and download, including
buffer alignment, receive/read/send/write/checkpoint time, rate, device id, and
errno. Socket logs record both requested and effective buffer sizes.

Archive status already separates input/output wait rates, CPU time, thread
activity, and decoder throughput. ZIP checkpoint latency is written to the
archive log.

## Hardware validation

Run at least three warm and three cold-ish repetitions for each direction and
media pair. Compare medians, not a single peak:

1. PC to `/data`, PC to USB, `/data` to PC, and USB to PC.
2. `/data` to `/data`, `/data` to USB, USB to `/data`, and USB to USB.
3. One large stored ZIP, one large deflated ZIP, one RAR, and one LZMA2 7z.
4. A many-small-file directory for copy, upload, extract, list, and Index All.
5. Cancel midway, disconnect midway, full-disk simulation, unplug/remount only
   on disposable test media, and re-run after every error.

Use wired LAN, record firmware/media/filesystem, retain `/data/BFpilot` logs,
and reject a speed change if it introduces a hang, partial final path, retry on
`EIO`/`ESTALE`/`EBADF`/`EFAULT`, or console instability.

For an end-to-end, bounded-memory upload/download/copy run:

```sh
BF_ALLOW_PS5_WRITE=1 PS5_IP=<console-ip> make ps5-transfer-perf
```

The default streams 512 MiB, verifies the uploaded file and local-copy SHA-256,
exercises the large-file copy pipeline, writes a JSON report under `logs/`, and
removes only its uniquely named test directory.

## Research basis

* [FreeBSD `sendfile(2)`](https://man.freebsd.org/cgi/man.cgi?query=sendfile&sektion=2)
  for zero-copy, partial-send, and readahead semantics
* [FreeBSD `posix_fadvise(2)`](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+13.0-RELEASE&query=posix_fadvise&sektion=2)
  for advisory sequential access
* [FreeBSD `posix_fallocate(2)`](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+8.3-RELEASE&query=posix_fallocate&sektion=2)
  for the successful-allocation guarantee used before archive decode
* [`phantomptr/ps5upload` runtime](https://github.com/phantomptr/ps5upload/blob/main/payload/src/runtime.c)
  and [buffer configuration](https://github.com/phantomptr/ps5upload/blob/main/payload/include/config.h)
* [`seregonwar/zftpd` HTTP configuration](https://github.com/seregonwar/zftpd/blob/main/include/http_config.h)

The FreeBSD interfaces define API behavior; PS5-specific choices remain gated
by the local filesystem findings and must be confirmed on each supported
firmware/media combination.
