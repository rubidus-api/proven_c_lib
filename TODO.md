# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

1. `proven_buf_append()` overlap contract
   - The current append path assumes independent source and destination slices.
   - Decide whether overlapping views should be rejected explicitly or handled with move semantics.
   - Add a regression test once the contract is fixed.

2. Public CI coverage
   - Add GitHub Actions workflows for at least hosted Linux GCC and Clang builds.
   - If practical, add one additional platform job to cover a non-Linux compiler/runtime combination.
   - Keep the workflow limited to the already documented regression and strict-error checks.

## Already addressed in this pass

- `proven_sysio_scanner_t` now treats tokens that reach the end of the loaded buffer as an out-of-bounds failure instead of accepting a truncated token.
- `proven_sysio_scan_chunk_impl()` now rejects non-seekable inputs before reading, so pipes/stdin-like handles no longer lose unread bytes.
- POSIX append open flag handling now treats append as a write intent.
- `proven_mmap_create()` now rejects misaligned offsets before calling the PAL.
- Windows `PROVEN_MMAP_EXEC` requests are rejected explicitly instead of being ignored.
- The mmap docs and regression test list now mention offset granularity.
