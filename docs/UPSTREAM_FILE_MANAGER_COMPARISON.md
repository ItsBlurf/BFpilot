# File-manager comparison

Updated 22 October 2026. This comparison is in progress. It replaces the earlier
v1.4/v1.8 comparison and its unsupported blanket performance/stability claims.

The reference application is [PS5 Web File Manager v1.11b](https://github.com/owendswang/ps5-web-file-manager/tree/708e5314105673d26047c9bae991b0bafc661fe6).
Its 7-Zip helper is a separate component; the helper source reviewed here is
[26.03-2](https://github.com/owendswang/wfm-7zip-helper/tree/131ef9d75ded2f1c3b3f886a6c06dde4d5cda82f).
BFpilot's validated development baseline is v0.4.5-dev.8. This is local
development work; no new release or publication is claimed.

| Area | BFpilot state | Reference / remaining comparison |
| --- | --- | --- |
| Local file operations | Browse, create, rename, copy, move, merge, delete, permissions and byte-preserving filenames. Copies discover work in one pass; same-volume moves avoid prescanning. | Both applications offer the core operations. Long external-storage stress and matching throughput measurements remain. |
| Large folders | Actual 85,000-file host copy with every byte checked; bounded native tree/cancellation/error controls; background folder size and bounded browser rendering. | These tests do not establish universal filesystem or firmware compatibility. |
| Uploads | Persistent HTTP connections, progress, checked completion, atomic replacement and interrupted-upload recovery. | Compare the same data, storage and completion semantics; upload resumability remains a separate capability. |
| Downloads | Direct file download and combined streaming TAR for multiple items. | The reference also offers individual multi-item downloads. An equivalent optional queue is pending. |
| SMB | Under development. | The reference supports SMB2/3, saved connections, share enumeration and remote operations. This is a meaningful current gap. |
| Archives | Integrated UnRAR/7z/plain ZIP retain their native paths. An embedded owned service adds broad 7-Zip formats and AES/other ZIP methods, cancellation and checked final publication. | Native R8 passes 18 format cases and exact output checks, including compressed TAR and CPIO. Rare formats and all multipart variants are not exhaustively exercised. |
| Temporary output | Private staging on the destination volume outside known homebrew scan directories; final publication by rename. | The reference helper publishes individual files through temporary siblings. Whole-directory merge rollback is not guaranteed by either description. |
| Package queues | Sony AppInst submission through an embedded private worker, complete-command transport and ordered queues. | Both use Sony's installation API. Submission acceptance is distinct from completed installation; real base/update/DLC batch acceptance remains qualified. |
| Browser usability | Mouse/toolbar opening and virtual selection pass Chrome, Firefox and WebKit; selected-row contrast and responsive layouts pass the tested axe checks. | Physical PS5 WebCard/controller acceptance remains separate from desktop automation. |
| Images and editing | Image viewer and text editor are present. | Compare actual workflows rather than marking an existing feature missing. |
| Task recovery | Broad archive jobs persist exact task/source/stage identity, reattach without resubmission and retain the task through checked publication. | Native coordinator restart during finalization recovers the original task and all 1,000 exact output files. This is not a power-loss test or resumable local copy/upload. |
| Hardware utilization | Native archive diagnostics expose process CPU and I/O measurements where available; unavailable RAM counters remain unavailable. | A separate helper's CPU/RAM must be measured separately; coordinator metrics alone cannot represent total utilization. |

No comparable v1.11b transfer or extraction benchmark has yet established an
overall speed advantage. Filesystem cache state, device direction, data layout,
compression/encryption, and final writeback gates affect the result. Application
completion and checked durable publication must be reported separately when
their semantics differ.

The observed failure controls cover specific errors and recovery paths. They
are not proof that every reported kernel panic is repaired, that every firmware
was exercised, or that a third-party mount/scanner caused every report.

BFpilot's application license is MIT. The reference application is GPL-3.0.
Imported 7-Zip/helper and other third-party components retain their own terms;
see [NOTICE.md](../NOTICE.md), the bundled license texts and the corresponding
source/build scripts. Reference behavior and independently written protocol
clients do not replace component license obligations.
