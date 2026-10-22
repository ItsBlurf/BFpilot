# WFM 7-Zip helper

## Performance print logs

PS5 release builds (`make` / `make ps5`) omit performance and informational
print logs. Use `make debug` to enable them; Linux builds always retain them
for PC debugging. Runtime error messages remain enabled in release builds.
Prefetch and buffered output are identical in both PS5 modes.

Debug builds print `[wfm-perf]` lines to stdout, flushed immediately so an
elfldr/netcat connection can capture them. Logs start at each archive open,
then sample at most once per two seconds when input reads or decoder progress
callbacks run. A final sample is printed after streams have been cleaned up,
including on cancel/error; the following request result is `0` on success.
Sample IDs distinguish concurrent archive calls. Passwords are never logged.

Rates use **MiB/s**. `out_avg` is successful output bytes divided by extraction
wall time; `out_recent` and `in_recent` use the latest sample interval.
`read_call_rate` and `write_call_rate` divide interval bytes by time spent inside
file I/O calls only: they are not end-to-end throughput or physical-media
benchmarks. The exact totals are `in_bytes` and `out_bytes`.

`read_ms`, `write_ms`, `writer_wait_ms`, and `copy_ms` are cumulative times.
`recent_read`, `recent_writer_wait`, and `recent_copy` divide those counter
deltas by interval wall time. Writer wait measures producer backpressure and
final draining; copy measures copying into helper-owned output buffers.
`prefetch_wait_ms` measures waits for the input reader; `input_copy_ms` and
`input_copied` measure copying prefetched input into decoder-owned buffers.
These times overlap across threads and must not be added as exclusive stages.
They do not separately measure AES, checksum, or pure decoder computation.

CPU percentages are cumulative: `extract_thread_cpu` covers the caller thread;
`write_call_cpu` covers CPU time inside buffered file-write calls (not the
entire writer thread); `process_cpu` includes all helper tasks. CPU clock
availability and the selected global AES-CBC function are logged at
extraction start. AES dispatch reports the
available implementation, not whether a particular archive uses encryption.
Input I/O hooks are guarded by `WFM_PERF` and affect only helper builds.
Read counters cover instrumented caller-thread reads and the helper's prefetch
worker after archive opening/inspection. They include speculative bytes that
can be discarded on a seek, so `in_bytes` can exceed the archive size.
Other decoder threads and direct container staging reads are not covered.
Write metrics count accepted `write()`
bytes; they do not establish persistence on physical media.

`wfm-7zip-helper.elf` is a separate, long-running PS5 payload used by
ps5-web-file-mgr for archive extraction.

## Lifecycle

1. ps5-web-file-mgr optionally sends `/data/wfm/wfm-7zip-helper.elf` to the
   elfldr listening on `127.0.0.1:9021` when the file manager starts.
2. The helper can instead be sent to elfldr manually.
3. The helper sets its process name to `wfm-7zip-helper`, terminates any older
   process with that exact name, and then acquires a single-instance lock.
4. It listens only on `/system_tmp/wfm-7zip-helper.sock` and displays the
   notification `WFM 7zip helper v26.03-2` after `bind()` and `listen()` succeed.
5. If the fixed socket cannot be created or listened on, the helper exits with
   an error; it never selects an alternate path or network port.
6. The helper remains running and accepts local IPC requests.

The helper is not embedded in or linked into ps5-web-file-mgr.

## Protocol

The stream transport uses a fixed 20-byte, big-endian frame prefix solely to
preserve message boundaries on `SOCK_STREAM`:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `W7HP` |
| 4 | 2 | message type |
| 6 | 2 | reserved flags (zero) |
| 8 | 4 | payload size |
| 12 | 8 | request/job ID |

The maximum frame payload is 1 MiB. Message boundaries do not depend on
individual `send()` or `recv()` calls. No path or password uses a delimiter or
reserved marker such as `@password`.

Availability is checked with the permanent `PING`/`PONG` exchange; there is
no version negotiation or capability handshake. `EXTRACT` starts with four
big-endian 32-bit values (flags, password length, item count and reserved),
followed by the password bytes. Each item then contains source and destination
lengths followed by their exact bytes. A single WFM task therefore transfers
its complete archive batch to the helper, which processes that batch in order.
Source and destination are independently limited to 256 KiB and the password
to 256 KiB. Optional extension field `1` carries the one-byte overwrite
approval after the browser has confirmed replacement/merge with the user.
Embedded NUL is rejected because it is not valid in a POSIX path
and cannot be represented by the current C API. Characters such as newlines,
`@`, `%`, `&`, `=`, quotes and backslashes have no protocol meaning.

`ERROR` starts with big-endian 32-bit code and message lengths followed by the
exact bytes. `PROGRESS` starts with big-endian 64-bit total and completed byte
counts. `CURRENT_FILE` starts with a big-endian 32-bit path length followed by
the exact UTF-8 path bytes.

Existing command and event meanings are permanent. Future optional fields are
appended as repeated big-endian 32-bit field-ID and length pairs followed by
the field bytes. Older receivers validate and skip fields they do not know.
New informational event types are also optional; every helper must still send
the original terminal `DONE`, `ERROR` or password event understood by the
first main program. A feature that cannot be expressed as optional fields must
use a new command type and must not change an existing command.

The daemon can run multiple extraction tasks concurrently. Task IDs are unique
within the daemon until their terminal result is acknowledged; reusing one
returns `duplicate_task_id`. `CANCEL` affects only the matching attached task.
A client disconnect only detaches it: extraction continues and the daemon
retains status and terminal errors. A restarted main program uses `LIST_TASKS`
and `ATTACH_TASK` to rebuild its task list and resume events, then sends
`ACK_TASK` after receiving `DONE` or `ERROR`. The web file manager keeps its
own single-file-task policy; helper concurrency remains available internally.

The transport implements the complete task exchange and `PING` / `PONG`.
The extraction worker is connected to upstream registered handlers for 7z,
ZIP, RAR/RAR5, TAR, GZIP, BZIP2, XZ, Zstandard, LZMA, PPMd, ARJ, CPIO, LZH,
CAB and XAR. It tries the extension-preferred handler first and then uses each
handler's native signature probe, so a valid archive can still be recognized
under a wrong extension. Compound TAR compression (`.tar.gz`, `.tgz`,
`.tar.bz2`, `.tbz`, `.tbz2`, `.tar.xz`, `.txz`) is decoded and then extracted
as TAR. Numbered split input (`.001`) requires its next `.002` volume, is
joined first and, when the joined output is itself an archive, probed and
extracted recursively. Missing requested volumes return their exact file name.
Password, progress, current-file and cancel callbacks are connected. A single
archive uses its native 7-Zip handler `SetTotal()` / `SetCompleted()` values.
For a multi-archive request, progress follows 7-Zip's UI algorithm: the total is
the sum of archive file sizes and each archive's native ratio is scaled to that
archive's file size. No archive-member scan is performed. Cancellation is
checked from both 7-Zip progress and output writes.

## Output safety and limits

Archive members are normalized as 7-Zip presents them for extraction.
Absolute paths, drive-style paths, `.` / `..`, control characters and
filesystem-invalid component/path lengths reject the archive. Archive symbolic
links, hard links and special nodes are silently skipped.
The destination is opened once as a directory file descriptor. Every directory
and file below it is then resolved with `fstatat()`, `mkdirat()` and `openat()`
using `AT_SYMLINK_NOFOLLOW` / `O_NOFOLLOW`; output traversal does not return to
absolute path-string lookup. Immediately before each archive is processed, its
top-level member names are checked against that archive's target directory. A
conflict returns `archive_overwrite_required`; the browser uses the same native
confirmation style as paste, then retries with overwrite approval. Batch items
remain sequential and are not treated as one transaction. Approved regular
files can be overwritten and directories can be merged. Individual members
whose destination is a symlink, special node, or incompatible type (file versus
directory) are silently skipped without following or replacing that node. An
unsafe or incompatible destination root remains a task error.

There is no custom archive-bomb scoring, item-count limit, recursive archive
scan, or compression-ratio policy. During the existing member/conflict pass,
known `kpidSize` values are summed once and compared with destination free
space; if any file size is unknown, this whole-archive check is skipped.
The handler-provided size of each file is still compared with currently
available space immediately before that file is opened. Each regular file is written to
a uniquely named same-directory `.wfm-part-*` file created with `O_EXCL` and is
renamed to its final name only after 7-Zip reports that individual item as
successfully extracted. There is no whole-folder staging, old-file backup, or
batch rollback. Cancellation or failure removes only the current unfinished
temporary file; previously completed files and directories remain, matching
the existing sequential paste behavior.

Regular files with a known size of at least 32 MiB use two owned 4 MiB output
buffers and a dedicated writer thread. Decoding can continue while one buffer
is written; a full queue makes the decoder wait, so memory use stays bounded.
The threshold applies to each member's unpacked size, not the compressed
archive or volume size; unknown sizes use the ordinary direct-write path.
Before renaming a successful member, the helper drains the queue and joins the
writer, including any final partial buffer, and checks for write errors. On
cancellation or failure it discards queued data, joins any in-flight write and
removes the temporary file. If the writer cannot start, output falls back to
synchronous buffering; if the buffer cannot be allocated, it writes directly.

Input streams preserve native size/property interfaces and use a separate
reader with two owned 2 MiB buffers. Prefetch starts lazily on reads of at least
1 MiB during extraction, avoiding speculative reads for small members and
metadata. Seek stops and joins the worker, rewinds to the consumed logical
position, then performs the requested seek. Startup failure falls back to
ordinary synchronous reads. Extra memory is bounded to 4 MiB per input stream.

Helper builds define `WFM_RAR_LARGE_OUTPUT`: RAR5's output quantum is 4 MiB
instead of 256 KiB, still capped by the dictionary-window boundary. This reduces
output callback/write-call overhead while preserving independent buffered
writing. The helper copies decoder output into owned buffers; it does not make
RAR5 decoding itself multithreaded.

File and explicit directory modification times supplied by the archive handler
are restored after successful output publication. Every extracted regular file
and every member directory is explicitly changed
to mode `0777` with `fchmod()`, independent of process `umask` or permissions
stored in the archive. After a successful merge, each affected top-level
member directory is recursively traversed without following symlinks, so its
pre-existing descendants are also `0777`. Newly created destination
directories are `0777`; pre-existing ancestors such as `/data` are not changed.

## 7-Zip source policy

This is a new extraction-only Bundle. Both builds define `Z7_EXTRACT_ONLY`.
`EngineObjects.mk` links the required registered archive handlers, decoders
and crypto readers. It does not link the
7-Zip GUI, FileManager, Agent, SFX, benchmark, archive creation/update, or
encoder objects.

The PS5 compiler advertises newer x86 instruction support than its intrinsic
headers provide. Guarded upstream changes add opt-out macros for VAES
(`Z7_DISABLE_VAES`) and SHA-512 hardware instructions
(`Z7_DISABLE_SHA512_HW`); the PS5 makefile enables the corresponding upstream
stub fallbacks. Linux keeps upstream hardware detection. Small
`Z7_EXTRACT_ONLY` guards in the ZIP, TAR, GZIP and BZIP2 handlers remove their
output/update interfaces and code paths. Function/data section garbage
collection removes unused crypto writer sections; symbol inspection confirms
that no encoder or `UpdateItems` implementation remains in the helper binary.

Distribution licensing, the exact upstream revision and the modification list
are recorded in `THIRD_PARTY_NOTICES.md`; upstream license texts remain under
`DOC/` and in the relevant decoder source files.

## Builds

From the repository root, `make` builds both the PS5 ELF and the Linux test
binary using the same helper sources:

```sh
make
```

Outputs:

```text
wfm-7zip-helper.elf
wfm-7zip-helper-linux
```

Individual builds are also available:

```sh
make ps5
make linux
```

`make debug` builds the PS5 payload with diagnostic prints enabled (and the
unchanged Linux debug binary). It still writes `wfm-7zip-helper.elf`. PS5 debug
and release objects are cached separately; switching back with `make ps5`
selects the release objects without needing `make clean`.

The PS5 build uses `$PS5_PAYLOAD_SDK/toolchain/prospero.mk`. The Linux daemon
uses `/tmp/wfm-7zip-helper.sock`; the PS5 daemon uses the required
`/system_tmp/wfm-7zip-helper.sock`.

Linux output-stream regression checks use AddressSanitizer and
UndefinedBehaviorSanitizer and cover full and partial buffers, short writes,
`EINTR`, worker-start failure, cancellation, `ENOSPC` and zero-byte writes.
They also cover the exact size threshold, either output-buffer allocation
failing, a slow writer under repeated backpressure, and `ENOSPC` after a
successful initial write:

```sh
make -C CPP/7zip/Bundles/WfmHelper -f makefile.linux test-output
```

`test-input` checks prefetch bytes, short reads, EOF/error restart, relative and
end seeks, small-read threshold, worker-start fallback and in-flight shutdown.
`test-input-release` runs the same checks with diagnostic printing disabled,
including automatic prefetch activation during extraction.
On systems where LeakSanitizer cannot inspect processes, run these targets
with `ASAN_OPTIONS=detect_leaks=0` (address/undefined checks remain enabled).
`_o_linux/EngineExtractTest` is a native registered-handler integration harness:
pass an archive, an existing destination directory, and optionally a password.

Deployment defaults to elfldr port 9021:

```sh
make -C CPP/7zip/Bundles/WfmHelper -f makefile.ps5 deploy \
  PS5_HOST=<ps5-address> PS5_PORT=9021
```
