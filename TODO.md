# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

- Float scanner exponent bounds: resolved so that underflow cases like `1e-330` now round to signed zero and overflow cases report `PROVEN_ERR_OVERFLOW`. Keep any future precision-policy work separate from this boundary fix.
- Float conversion portability: resolved by removing `long double` from the scan and format conversion paths and keeping the remaining float math in double precision.
- Map borrowed-key contracts: internal borrowed keys are now rejected under debug validation or `PROVEN_HARDENED`. The remaining follow-up is whether an owned-key path should be added for callers that need map-owned storage. Next verification hook: add a dedicated owned-key API only if a caller needs it, and keep the borrowed-key lifetime rule documented.
- Streaming scanner boundary behavior: resolved for buffered sysio scans that can refill and retry; keep any remaining float-specific edge cases under separate review if needed.

## Already addressed in this pass

- Public contract hardening: array and map mutation entry points now reject corrupted public structs before allocator callbacks are touched, and filesystem append opens now treat append as write intent while rejecting append-plus-truncation conflicts.
- Build driver portability: `nob.c` now probes `-std=c23` first and falls back to `-std=c2x`, checks compiler/linker availability up front, and records the selected standard flag in the build hash.
- Sysio scanner init allocator contract: `proven_sysio_scanner_init()` now validates the full allocator trait, rejects partial allocators up front, and leaves the scanner zero-safe on failure.
- Public CI coverage: the GitHub Actions workflow file was removed after deciding this repository will not use Actions for deployment or validation.
- `proven_buf_append()` now uses move semantics so overlapping source views remain well-defined.
- `proven_sysio_scanner_t` now treats tokens that reach the end of the loaded buffer as an out-of-bounds failure instead of accepting a truncated token.
- `proven_sysio_scan_chunk_impl()` now rejects non-seekable inputs before reading, so pipes/stdin-like handles no longer lose unread bytes.
- POSIX append open flag handling now treats append as a write intent.
- `proven_mmap_create()` now rejects misaligned offsets before calling the PAL.
- Windows `PROVEN_MMAP_EXEC` requests are rejected explicitly instead of being ignored.
