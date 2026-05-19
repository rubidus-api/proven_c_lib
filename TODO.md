# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

No open items.

## Already addressed in this pass

- Public contract hardening: array and map mutation entry points now reject corrupted public structs before touching allocator callbacks, and filesystem append opens now treat append as write intent while rejecting append-plus-truncation conflicts.
- Public CI coverage: removed the GitHub Actions workflow file after deciding this repository will not use Actions for deployment or validation.
- `proven_buf_append()` now uses move semantics so overlapping source views remain well-defined.
- `proven_sysio_scanner_t` now treats tokens that reach the end of the loaded buffer as an out-of-bounds failure instead of accepting a truncated token.
- `proven_sysio_scan_chunk_impl()` now rejects non-seekable inputs before reading, so pipes/stdin-like handles no longer lose unread bytes.
- POSIX append open flag handling now treats append as a write intent.
- `proven_mmap_create()` now rejects misaligned offsets before calling the PAL.
- Windows `PROVEN_MMAP_EXEC` requests are rejected explicitly instead of being ignored.
