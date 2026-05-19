# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

- Verification infrastructure: add a stronger ThreadSanitizer stress pass for the job queue and a host-oracle differential float check that compares this library against the platform C library without linking the implementation together.
- Eisel-Lemire / Ryu-class float conversion upgrade: long-term accuracy work for the out-of-exact-range decimal-to-double path and the matching formatter path.

## Already addressed in this pass

- Decimal underflow policy: decimal inputs below the smallest subnormal are documented and tested as signed zero with the input sign preserved.
- Scientific normalization guard: the formatter already uses a bounded local loop cap in `proven_fmt_normalize_scientific()`.
- Build driver portability: `nob.c` already probes `-std=c23` first, falls back to `-std=c2x`, and keeps the selected standard flag in the build hash.
- Map owned-key storage: added `PROVEN_KEY_TYPE_U8_OWNED` and `proven_map_set_u8_owned()` so callers that need map-owned keys can duplicate bytes on insert and release them on remove or destroy.
- Public contract hardening: array and map mutation entry points now reject corrupted public structs before allocator callbacks are touched, and filesystem append opens now treat append as write intent while rejecting append-plus-truncation conflicts.
- Sysio scanner init allocator contract: `proven_sysio_scanner_init()` now validates the full allocator trait, rejects partial allocators up front, and leaves the scanner zero-safe on failure.
- Public CI coverage: the GitHub Actions workflow file was removed after deciding this repository will not use Actions for deployment or validation.
- `proven_buf_append()` now uses move semantics so overlapping source views remain well-defined.
- `proven_sysio_scanner_t` now treats tokens that reach the end of the loaded buffer as an out-of-bounds failure instead of accepting a truncated token.
- `proven_sysio_scan_chunk_impl()` now rejects non-seekable inputs before reading, so pipes/stdin-like handles no longer lose unread bytes.
- POSIX append open flag handling now treats append as a write intent.
- `proven_mmap_create()` now rejects misaligned offsets before calling the PAL.
- Windows `PROVEN_MMAP_EXEC` requests are rejected explicitly instead of being ignored.
