# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

- Eisel-Lemire / Ryu-class float conversion upgrade.
  - Scope: long-term accuracy work for the out-of-exact-range decimal-to-double path and the matching formatter path.
  - Current state: the hosted scanner already uses the shared double-only helper path, the portable no-long-double regression is in place, and the present fixed-precision formatter remains the default.
  - Next staged steps:
    1. Expand the representative corpus for exact-range, subnormal-boundary, and shortest-format cases so the upgrade target stays pinned to known spellings.
    2. Replace the current decimal-to-double fallback path with a target-deterministic exact-range backend that stays within the documented no-long-double contract.
    3. Replace the shortest-output policy backend with the final round-trip-oriented formatter once the parser backend is in place.
  - Out of scope for this pass: broad algorithmic rewrites that would change the current fixed-precision default or add hidden heap allocation on hot paths.

## Already addressed in this pass

- Verification infrastructure: added `tests/test_job_stress_tsan` for a denser job-queue stress pass and `tests/test_float_host_oracle` for host-oracle float comparisons against the platform C library.
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
