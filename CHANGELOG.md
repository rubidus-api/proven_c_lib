# Changelog

All notable changes to this project will be documented in this file.

The format follows Keep a Changelog:

- keep the log human-curated
- order entries chronologically with the newest first
- group notable changes by release or by `Unreleased`
- use the standard sections `Added`, `Changed`, `Deprecated`, `Removed`,
  `Fixed`, and `Security` when they apply
- avoid dumping raw commit history into the file

## [Unreleased]

### Fixed

- `proven_sysio_scanner_deinit()` now clears the full scanner state after releasing the buffer.
- `proven_map_is_valid()` now checks the public `internal.size` against the bucket layout.
- `proven_fs_open()` now rejects unsupported mode bits before reaching the PAL layer.
- `proven_fs_open()` now rejects truncation requests that do not carry write intent.
- `proven_fs_lock()` now rejects unsupported lock modes instead of treating them as unlock.
- `proven_fs_chmod()` now rejects unsupported permission bits before reaching the PAL layer.

### Changed

- Tightened repository documentation rules and clarified how local-only notes
  differ from public release notes.

## [2026-06-02]

### Changed

- Removed local-only project state files from public Git history and restored
  them locally as ignored files.
- Updated local handoff documents so the next session resumes from the
  post-float-backend state.

### Fixed

- Added `.gitignore` entries for the restored local-only project files.

## [2026-06-01]

### Changed

- Closed the staged float backend work.
- Extended float boundary and shortest-literal coverage.

### Fixed

- Corrected the float scan underflow edge.
- Fixed scientific exponent padding and shortest-float candidate selection.
