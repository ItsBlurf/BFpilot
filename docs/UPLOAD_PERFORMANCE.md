# Upload troubleshooting

Upload speed depends on the network, firmware, destination filesystem and drive.
Raw SSD speed does not predict the speed of a write through the PS5 filesystem.

BFpilot uses a 2 MiB upload buffer, HTTP keep-alive, a requested 4 MiB receive
window and a free-space check before writing. Data goes to a temporary file,
with a flush every 256 MiB, before it is renamed into place.

## Finding a bottleneck

1. Use wired LAN where possible and avoid other large transfers.
2. Upload a file of at least 500 MB to `/data`, then repeat on USB.
3. Compare `averageMBps`, `recvMs`, `writeMs`, `syncMs` and `syncCount` in the response.
4. Check the transfer log for errors and compare several runs.

Example log fields:

```text
upload end ... average_mbps=... recv_ms=... write_ms=... sync_ms=... sync_count=... buf_size=2097152 aligned=1
```

High `write_ms` or `sync_ms` points toward storage time. High `recv_ms` points
toward waiting for incoming data, which can involve the network or client.
These timings help narrow the cause; they do not identify it on their own.

Implementation: `src/transfer.c`, `src/websrv_lite.c`, `src/fs.c` and
`assets/files.html`. See [transfer implementation](PERFORMANCE_STABILITY.md)
for buffers, copy behavior and a repeatable transfer check.
