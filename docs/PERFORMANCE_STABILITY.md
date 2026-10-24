# Transfer implementation

Transfers use fixed-size buffers and one writer at a time. Output is written
to temporary files before replacing the destination.

## Data paths

| Operation | Implementation |
| --- | --- |
| Upload | One 2 MiB aligned buffer, temporary output, free-space checks and a flush every 256 MiB |
| Download | One 2 MiB aligned buffer; a short source read fails instead of returning a truncated success |
| Copy | Two 8 MiB aligned buffers for regular files of at least 64 MiB; a serial path for smaller files or allocation/thread failure |
| Move | Rename on the same filesystem; otherwise copy, finish and then remove the source |
| ZIP | Automatic archive service, or the integrated decoder with 16 MiB input and 64 MiB output buffers |
| RAR/7z | Automatic archive service, with integrated decoders available for manual thread settings |
| Index/search | One directory scan at a time; search stops once the requested page and one extra match are found |

Recursive operations stay within their starting filesystem and do not follow
symbolic links. Source identity and size are checked again when opening a file.
Write, flush and close errors are reported before a replacement is considered
complete. See [temporary files and recovery](PRIVATE_STAGING.md).

## Network and storage choices

HTTP keep-alive reduces connection setup for multi-file uploads. Upload and
download request 4 MiB socket buffers; the system may choose different effective
sizes. Large writes use 256 MiB flush checkpoints to limit outstanding writes.

Downloads use buffered reads rather than `sendfile`, whose behavior varies across
PS5 filesystems. Bulk transfers do not enable `TCP_NODELAY`. Uploads avoid
whole-file `posix_fallocate` while receiving a request, because allocation can
stall the connection. Archive members can allocate space before decoding when
their size is known.

Folder operations use one writer. Only reads for a single large file overlap
its writes; there is no parallel tree-copy writer pool or resumable-upload feature.

## Diagnostics

- `/api/fs/job/status`: copy mode, discovered/completed bytes, buffer size,
  checkpoints, speed and device IDs.
- `/api/fs/transfer/stats`: last upload/download timing, buffer alignment and errors.
- Archive status/logs: decoder speed, waits, thread settings and flush timing.

The main process's CPU figure does not include a separate archive process.
Compare like-for-like process totals when investigating resource use.

## Measuring transfers

Use the same files, direction and storage for repeated runs. Record firmware,
filesystem, drive and network connection. Verify file contents as well as speed.
Start with disposable data when checking cancellation, storage exhaustion or
disconnect behavior.

```sh
BF_ALLOW_PS5_WRITE=1 PS5_IP=<console-ip> make ps5-transfer-perf
```

The default run streams 512 MiB, checks upload and copy SHA-256 values, writes a
report under `logs/` and removes its own uniquely named test directory.

## References

- [FreeBSD sendfile](https://man.freebsd.org/cgi/man.cgi?query=sendfile&sektion=2)
- [FreeBSD posix_fadvise](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+13.0-RELEASE&query=posix_fadvise&sektion=2)
- [FreeBSD posix_fallocate](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+8.3-RELEASE&query=posix_fallocate&sektion=2)
- [ps5upload transfer code](https://github.com/phantomptr/ps5upload/blob/main/payload/src/runtime.c)
- [zftpd HTTP configuration](https://github.com/seregonwar/zftpd/blob/main/include/http_config.h)
