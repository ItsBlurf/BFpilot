# Upload performance

## What the PS5 actually does

| | |
|---|---|
| Gigabit LAN | ~110–112 MiB/s ceiling |
| `/data` writes | Usually way slower than raw SSD (PFS / crypto / scheduler) |
| USB/ext | Sometimes faster than `/data` |
| FW variance | zftpd-style uploads land roughly 20–113 MB/s depending on FW |

## What we ship

* Single-buffer STOR: `recv` → `write` (2 MiB, 16 KiB aligned)
* HTTP keep-alive, up to 64 requests per connection
* Download stream buffer 2 MiB (16 KiB aligned)
* Listen SO_RCVBUF 4 MiB, SO_SNDBUF 4 MiB, backlog 32
* Free-space check + temp file then rename
* A writeback checkpoint every 256 MiB bounds dirty pages on very large files
* UI stops spamming toasts every file on multi-upload

## What we don't do

* Parallel writers into `/data` (often slower)
* Double-buffer upload
* Whole-file `posix_fallocate` while the browser is still POSTing
* `TCP_NODELAY` on bulk transfer
* `sendfile` / aggressive `F_NOCACHE` (panic risk on some media)

## Measuring

1. Inject `bfpilot.elf`
2. Upload something big (≥500 MB) to `/data` and check `averageMBps`,
   `recvMs`, `writeMs`, `syncMs`, and `syncCount` in the response
3. Multi-file uploads should gain most from keep-alive
4. Wired beats Wi-Fi

Log line looks like:

```
upload end ... average_mbps=… recv_ms=… write_ms=… sync_ms=… sync_count=… buf_size=2097152 aligned=1
```

`write_ms` >> `recv_ms` → disk bound. The other way around → network / window.

Code: `src/transfer.c`, `src/websrv_lite.c`, `src/fs.c`, `assets/files.html`.

The full reasoning, exclusions, telemetry, and hardware matrix are in
`docs/PERFORMANCE_STABILITY.md`.
