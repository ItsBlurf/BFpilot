# WFM 7-Zip helper third-party notices

The helper is built from 7-Zip 26.03 source at upstream commit
`0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17` plus the WFM-specific changes
listed below. The corresponding source tree must be distributed with, or made
available alongside, the helper binary.

## 7-Zip

Copyright (C) Igor Pavlov and other 7-Zip contributors.

Most 7-Zip source is licensed under GNU LGPL-2.1-or-later. The authoritative
license text and component exceptions are in the repository's
`DOC/License.txt`.

RAR/RAR5 unpacking source carries the additional unRAR restriction: it must
not be used to reverse engineer the RAR compression algorithm or to develop a
RAR-compatible archiver. This helper exposes extraction only and contains no
RAR encoder or archive creation interface.

Additional decoder files currently linked by this extraction-only bundle:

- `CPP/7zip/Compress/LzfseDecoder.cpp`: BSD-3-Clause
- `C/ZstdDec.c`: BSD-3-Clause
- `C/Xxh64.c`: BSD-2-Clause

Their copyright and license notices remain in the corresponding source files.

## WFM modifications

- Added `CPP/7zip/Bundles/WfmHelper/`: Unix-socket daemon, private framed IPC,
  task retention/attachment, extraction adapter, destination confinement,
  overwrite confirmation enforcement and PS5/Linux build files.
- Added extraction-only registration glue and an explicit extraction-only
  object list.
- Added guarded `Z7_EXTRACT_ONLY` exclusions to ZIP, TAR, GZIP and BZIP2
  handlers so update/create interfaces are not linked.
- Added guarded PS5 opt-outs for unavailable VAES and SHA-512 hardware
  intrinsic paths while retaining software fallback implementations.
- No decoder algorithm was reimplemented by WFM.
- Added guarded helper-only `WFM_RAR_LARGE_OUTPUT` to the RAR5 decoder to
  increase its output quantum from 256 KiB to 4 MiB; dictionary boundaries and
  the decoding algorithm are unchanged.
- Added helper input prefetch/output buffering and per-task performance logs;
  guarded `WFM_PERF` input-I/O instrumentation in `Common/FileStreams.cpp`.

The helper does not include the 7-Zip GUI, FileManager, Agent, SFX, benchmark,
archive update/create code, or encoders. `wfm-7zip-helper.elf` and
`web-file-mgr.elf` are separately distributed programs communicating only over
a private Unix socket.
