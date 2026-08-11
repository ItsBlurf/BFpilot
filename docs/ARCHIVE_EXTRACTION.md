# Archive extraction

Extraction runs inside `bfpilot.elf`; no standalone archive payload is built or distributed.

## Path

Runtime state is stored under `/data/BFpilot/archive-integrated/`.

Job/status/log: `job.ini`, `status.json`, `archive-worker.log`, `daemon.lock`.

## Formats

From `/api/fs/archive/support`:

* **RAR** — RAR4/5, passwords, multipart (`.partN.rar` / `.rNN`). Decoder
  threads default to the hardware-tested automatic cap of four, can be set
  manually up to eight, and are reported in archive telemetry.
* **7z** — LZMA/LZMA2, passwords, `.7z.001` splits; the same four-thread auto
  cap and eight-thread manual limit apply
* **ZIP** — stored/deflate/ZIP64, ZipCrypto only (AES zip is rejected). Split zip not enabled.

ZIP extraction uses page-aligned 16 MiB input and 64 MiB output buffers,
sequential input hints, known-size output preallocation, and a 256 MiB
writeback checkpoint for sustained large members. RAR and 7z keep their
existing bounded decoder thread selection and output buffering.

## Limits

* Destination must be under `/data`, `/mnt/usb0-7`, or `/mnt/ext0-7`
* Stages to a temp name, renames on success
* Free-space check with a margin
* Partial cleanup on failure when enabled
* Browser cancellation is supported while an extraction is prepared, running,
  or finalizing; staged partial output is removed when cleanup is enabled

## UI

Select archive → set Target → Extract → optional password → watch `/api/fs/archive/status`.

## Code

* UnRAR — `third_party/unrar-ps5/`
* 7z/LZMA — `third_party/unrar-ps5/lzma2601/`
* miniz — `third_party/miniz/`
