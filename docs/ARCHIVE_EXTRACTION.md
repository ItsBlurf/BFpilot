# Archive extraction

Extraction is managed by `bfpilot.elf`, which embeds and starts its private
7-Zip service when needed. No separate archive payload installation is needed.

## Path

Runtime state is stored under `/data/BFpilot/archive-integrated/`.

Job/status/log: `job.ini`, `status.json`, `archive-worker.log`, `daemon.lock`.

## Formats

From `/api/fs/archive/support`:

* **RAR** — RAR4/5, passwords, multipart (`.partN.rar` / `.rNN`).
* **7z** — LZMA/LZMA2, passwords and `.7z.001` splits.
* **ZIP/ZIPX** — stored/deflate/ZIP64, ZipCrypto, AES and supported additional methods.
* TAR, gzip, bzip2, xz, zstd, Unix Compress, LZMA, PPMd, ARJ, CPIO,
  LZH/LHA, CAB and XAR/XIP are handled by the private service, including
  supported compound TAR forms and numbered splits.

Automatic extraction uses the full engine's bounded read-ahead and two-slot
4 MiB output pipeline. Decoder thread counts are controlled by the decoder;
telemetry reports `threadMode: decoder-default` and does not invent a thread
count or include helper CPU in coordinator-only CPU counters. Explicit
`threads=1..8` requests retain the integrated RAR/7z decoders; ZIP is serial.
The original ZIP central-directory and 7z entry/space/expansion checks still
apply before automatic dispatch. Integrated ZIP uses page-aligned16 MiB input and64 MiB output buffers,
sequential input hints, known-size output preallocation, and a 256 MiB
writeback checkpoint for sustained large members. RAR and 7z keep their
existing bounded manual decoder thread selection and output buffering.

## Limits

* Destination must be under `/data`, `/mnt/usb0-7`, or `/mnt/ext0-7`
* Stages to a temp name, renames on success
* Direct SMB extraction is unavailable; copy the archive locally, extract,
  then copy the result back if needed
* Free-space check with a margin
* Partial cleanup on failure when enabled
* Browser cancellation is supported while an extraction is prepared, running,
  or finalizing; staged partial output is removed when cleanup is enabled

## UI

Select archive → set Target → Extract → optional password → watch `/api/fs/archive/status`.

## Sequential batch API

POST `/api/fs/archive/batch` with repeated ordered `src`, `dst` form pairs
and optional common `password`, `threads`, `nice`, `cleanupPartial` fields.
At most2048 local regular archives are accepted, within the12MiB form limit.
Destinations must be absent and distinct. The browser currently uses the
single-archive action; the multiple-archive dialog is still in development.

GET `/api/fs/archive/batch/status` returns queue identity/counts and individual
outcomes. The server retains its queue across browser refresh/main replacement.
Cancellation through `/api/fs/job/cancel` stops the current item cooperatively
and leaves later items unattempted. A failed archive does not prevent later
archives; check both `succeeded` and `failed` even when queue state is `done`.
Uncertain recovery stops for inspection instead of repeating an extraction.

Batch state lives in the private `batch/` directory. Password and exact path
spaces are preserved; passwords are not exposed by status. Keep these private
files when diagnosing recovery. Controlled main replacement is tested; file
sync is not a guarantee against storage loss during a power cut.

## Code

* UnRAR — `third_party/unrar-ps5/`
* 7z/LZMA — `third_party/unrar-ps5/lzma2601/`
* miniz — `third_party/miniz/`
* Full extraction service — `third_party/wfm-7zip-helper/` (7-Zip/WFM attribution retained)
