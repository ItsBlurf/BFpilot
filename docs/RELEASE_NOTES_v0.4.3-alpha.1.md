# BFpilot v0.4.3 Alpha 1

This alpha contains the work added after v0.4.0:

- Separate Media tile installer, one `/data/BFpilot` data directory, and clean
  reinjection/shutdown behavior
- Payload loading through elfldr, chmod, case-insensitive paths, multi-select,
  and RAR/7z/ZIP extraction
- Safe text editing, recursive folder upload, conflict handling, staged
  overwrite/merge, physical no-symlink traversal, and ShadowMountPlus warnings
- Faster bounded large-file copies, writeback checkpoints, download tuning,
  and earlier-return search queries with expanded telemetry
- Clipboard-style Copy/Move/Paste, restored double-click folder opening,
  selection-aware actions, fixed drag feedback, correct empty-folder sizing,
  and responsive clipboard layout

This still needs hardware testing for copy/move conflicts, large transfers,
Rest Mode, and USB disconnect behavior before it should be considered stable.
