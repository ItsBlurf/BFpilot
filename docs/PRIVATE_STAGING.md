# Private filesystem staging

Copy, upload and extraction build private output on the destination filesystem
before publishing it. Internal storage uses `/data/BFpilot/staging`. Removable
volumes use `.bfpilot-staging` at the volume root. This keeps unfinished game
executables outside homebrew directories that another payload may scan.

The staging selector verifies device identities and refuses symlink staging
directories. It avoids placing a directory copy's stage inside its own source;
backing up BFpilot's data can use `/data/.bfpilot-staging` instead. A destination
on a nested mounted game filesystem that has no suitable outside staging area
is rejected rather than populated incrementally. Other writable filesystems use
a checked private directory at their own volume root.

Successful new destinations are published by same-filesystem rename. Replacing
a regular file retains the previous file until the replacement is complete.
Merging an existing directory validates type conflicts first, preserves unrelated
files, and publishes completed replacements individually. A directory merge is
not a rollback transaction: files already replaced before a later error remain
replaced. Cross-device moves copy/publish before removing their source.

Extracted files undergo checked permissions and final fsync before the stage is
published. RAR progress reports actual output bytes separately from packed-input
progress; its output total stays unknown until successful completion. Other
known-size formats keep completion below 100% until final publication.

Cancellation cleans only the operation's owned stage. Failed finalization can
retain its stage for diagnosis. An interrupted process can also leave a stage;
inspect the recorded path and logs before removing it. Private staging reduces
exposure to incomplete files, but does not establish the cause or resolution of
every console panic or third-party mount interaction.
