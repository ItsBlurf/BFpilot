# Temporary files and recovery

Copies, uploads and extractions write temporary output on the destination
filesystem before placing it at the final path. Internal storage normally uses
`/data/BFpilot/staging`; removable drives use `.bfpilot-staging` at their root.
This keeps unfinished executables outside homebrew folders that other payloads
may scan.

BFpilot checks the filesystem and rejects staging directories that are symbolic
links. It also avoids putting a folder copy's temporary output inside its source.
Backing up `/data/BFpilot` can therefore use `/data/.bfpilot-staging`. A destination
inside a mounted game filesystem is rejected when no suitable temporary area
exists outside that mount.

A completed new destination is renamed into place. An existing file stays intact
until its replacement is ready. A folder merge checks type conflicts first,
keeps unrelated files and replaces files one at a time. Replacements completed
before a later error remain in place; the merge cannot undo all earlier changes.
Cross-filesystem moves finish the copy before removing the source.

Extraction checks permissions and flushes output before placing it at the final
path. RAR reports output bytes separately from compressed input; its final output
size is unknown until completion. Progress for known-size output stays below 100%
until the operation finishes.

Cancel normally removes the operation's temporary output. A failure or process
interruption can leave temporary files and recovery records. Check the recorded
path and logs before deleting them. Keep backups of important files.
