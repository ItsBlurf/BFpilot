# Archive extraction

Extraction runs inside `bfpilot.elf`. The standalone `bfpilot-archive-worker.elf` is only a diagnostic build of the same code.

## Paths

| Integrated | Standalone worker |
|------------|-------------------|
| `/data/BFpilot/archive-integrated/` | `/data/BFpilot/archive/` |

Job/status/log: `job.ini`, `status.json`, `archive-worker.log`, `daemon.lock`.

## Formats

From `/api/fs/archive/support`:

* **RAR** — RAR4/5, passwords, multipart (`.partN.rar` / `.rNN`). RAR stays single-thread for stability.
* **7z** — LZMA/LZMA2, passwords, `.7z.001` splits
* **ZIP** — stored/deflate/ZIP64, ZipCrypto only (AES zip is rejected). Split zip not enabled.

## Limits

* Destination must be under `/data`, `/mnt/usb0-7`, or `/mnt/ext0-7`
* Stages to a temp name, renames on success
* Free-space check with a margin
* Partial cleanup on failure when enabled
* Cancel from the browser after extract starts is not supported yet

## UI

Select archive → set Target → Extract → optional password → watch `/api/fs/archive/status`.

## Code

* UnRAR — `third_party/unrar-ps5/`
* 7z/LZMA — `third_party/unrar-ps5/lzma2601/`
* miniz — `third_party/miniz/`
