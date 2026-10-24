# Archive extraction

BFpilot includes and starts its archive service automatically. No separate
payload is required. State and logs are under `/data/BFpilot/archive-integrated/`:
`job.ini`, `status.json`, `archive-worker.log` and `daemon.lock`.

## Supported formats

- **RAR:** RAR4/5, passwords and multipart `.partN.rar` / `.rNN` archives.
- **7z:** LZMA/LZMA2, passwords and `.7z.001` splits.
- **ZIP/ZIPX:** stored/deflate/ZIP64, ZipCrypto, AES and supported extra methods.
- TAR, gzip, bzip2, xz, zstd, Unix Compress, LZMA, PPMd, ARJ, CPIO, LZH/LHA,
  CAB and XAR/XIP, including supported compressed TAR forms and numbered splits.

`GET /api/fs/archive/support` reports the available formats.

## Using extraction

Select up to 2,048 local archives, choose **Extract**, then choose separate
folders, the current folder or a custom destination. Enable merge/replace when
needed, enter a common password if applicable and start extraction.

Archives run in order. Results show completed, failed and unattempted items and
remain available after a browser refresh. A failed archive does not stop later
items. Cancel stops the current archive and leaves later items unattempted.

Destinations must be under `/data`, `/mnt/usb0-7` or `/mnt/ext0-7`. For an archive
on SMB, copy it locally first. Direct SMB extraction is not supported.

Output is written to temporary storage and checked before it appears at the
destination. New files and folders use mode `0777`. Merge/replace keeps unrelated
existing files and directory modes. Each replacement is a complete file, but an
error or cancellation does not undo replacements that already finished.

Cleanup can remove incomplete extraction output. A failed merge keeps its
temporary files and recovery record for inspection, even when cleanup is enabled.
See [temporary files and recovery](PRIVATE_STAGING.md).

## Decoder settings

Automatic mode uses the full archive service, with read-ahead and two 4 MiB output
buffers. The decoder chooses its thread count; status reports
`threadMode: decoder-default`. Choosing `threads=1..8` uses the integrated RAR/7z
decoders instead. Integrated ZIP decoding is single-threaded.

ZIP directory checks and 7z path, space and expansion checks run before automatic
extraction. The integrated ZIP decoder uses 16 MiB input and 64 MiB output buffers
and flushes output every 256 MiB during large members.

## Batch API

POST `/api/fs/archive/batch` with ordered, repeated `src` and `dst` form fields.
Optional common fields are `password`, `threads`, `nice` and `cleanupPartial`.
Use `merge=1` to allow existing destinations or shared targets; otherwise
destinations must be absent and distinct.

The limit is 2,048 local regular archive files. Each source and destination can
be up to 1,023 bytes. The form-size limit allows their encoded paths, approximately
48 MiB plus overhead.

GET `/api/fs/archive/batch/status` returns the queue and each item's result.
Use `?summary=1` to return counts and only the current item. Cancel through
`/api/fs/job/cancel`. A queue can finish with failed items, so check both
`succeeded` and `failed` when its state is `done`.

Queue state is saved under `batch/`. Passwords and path spaces are preserved;
passwords are omitted from status responses. `/api/fs/archive/status` includes
the worker's `requestId`.

Merge recovery records live under `merge/` on internal storage, including when
the output is on USB. They identify the source, temporary files and destination
so a restart can continue a partial merge without extracting again. Uncertain
recovery stops for inspection instead of repeating the job.

## Source and licenses

- UnRAR: `third_party/unrar-ps5/`
- 7z/LZMA: `third_party/unrar-ps5/lzma2601/`
- miniz: `third_party/miniz/`
- Full archive service: `third_party/wfm-7zip-helper/`

See [NOTICE.md](../NOTICE.md) for authors, source versions and licenses.
