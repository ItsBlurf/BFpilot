# Background folder size and transfer progress

Request `GET /api/fs/du?async=1&path=<encoded absolute path>` to start a folder
size calculation. The response includes `id`, `busy`, `size`, `entries`, `files`,
`dirs`, `truncated`, `errno` and `error`. While `busy` is true, counts are partial
and `truncated` is true. A successful final response has both flags false.

Poll the same endpoint with `async=1`, `path` and the returned `id`. Cancel with
the same parameters plus `cancel=1`. An expired or superseded ID returns 404;
it cannot cancel a newer calculation. One background calculation runs at a time.
A new request cancels the old calculation and can return 409 while it closes
its directory handles; retry shortly. Starting a transfer cancels sizing.
Polling renews a 30-second lease so an abandoned browser does not keep scanning.

Traversal counts link metadata without following symbolic links, stays on the
root filesystem and checks traversal errors. An error returns a failure and
marks the counts incomplete. The legacy endpoint without `async=1` supports
quick queries with a 200 ms cooperative traversal budget; larger callers should
use background requests. A filesystem syscall can take longer than that budget.

`/api/fs/job/status` includes `totalKnown`. Copy discovers files during its
single pass. Until `totalKnown` is true, `totalBytes` and `totalFiles` describe
discovered work; clients should show completed bytes/files without a percentage
or ETA for the entire tree. A successful same-filesystem move can finish through
rename without reading every child; zero copied bytes then means no file data
was copied, rather than a measured empty source.
