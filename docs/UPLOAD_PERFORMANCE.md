# Upload performance

## What the PS5 actually does

| | |
|---|---|
| Gigabit LAN | ~110–112 MiB/s ceiling |
| `/data` writes | Usually way slower than raw SSD (PFS / crypto / scheduler) |
| USB/ext | Sometimes faster than `/data` |
| FW variance | zftpd-style uploads land roughly 20–113 MB/s depending on FW |

## What we ship (v0.4.0)

* Single-buffer STOR: `recv` → `write` (2 MiB)
* HTTP keep-alive, up to 64 requests per connection
* Download stream buffer 1 MiB
* Listen SO_RCVBUF 4 MiB, SO_SNDBUF 2 MiB, backlog 32
* Free-space check + temp file then rename
* UI stops spamming toasts every file on multi-upload

## What we don't do

* Parallel writers into `/data` (often slower)
* Double-buffer upload
* Whole-file `posix_fallocate` while the browser is still POSTing
* `TCP_NODELAY` on bulk transfer
* `sendfile` / aggressive `F_NOCACHE` (panic risk on some media)

## Measuring

1. Inject `bfpilot.elf`
2. Upload something big (≥500 MB) to `/data` and check `averageMBps` / `recvMs` / `writeMs` in the response
3. Multi-file uploads should gain most from keep-alive
4. Wired beats Wi-Fi

Log line looks like:

```
upload end ... average_mbps=… recv_ms=… write_ms=… buf_size=2097152
```

`write_ms` >> `recv_ms` → disk bound. The other way around → network / window.

Code: `src/transfer.c`, `src/websrv_lite.c`, `src/fs.c`, `assets/files.html`.
