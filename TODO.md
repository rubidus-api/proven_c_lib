# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

- Eisel-Lemire / Ryu-class float conversion upgrade.
  - Scope: long-term accuracy work for the out-of-exact-range decimal-to-double path and the matching formatter path.
  - Current state: the hosted scanner already uses the shared double-only helper path, the portable no-long-double regression is in place, the representative exact-range, subnormal-boundary, and shortest-format corpus is pinned in `tests/test_float_upgrade_corpus`, the exact-range scan corridor now includes the DBL_MAX boundary, the shortest-policy backend now keeps thin per-width shims over one shared shortest helper, the shortest special-case literals now live in the shared float decimal module, the shared shortest-literal dispatch is centralized in one internal helper, the float upgrade corpus now also pins the representative float32 shortest literals plus a few extra float32 fraction and power-of-two samples alongside the positive and negative float64 cases in the host-oracle and upgrade coverage, the scientific-guard shortest regression now pins both signs of the tiny-finite representative case, the largest binary64 subnormal shortest pair is now pinned too, and `tests/test_float_shortest_literal_table.c` now keeps the documented shared shortest literal tables pinned for f64 and f32.
  - Recent validation: `tests/test_float_exact_range_backend.c` now also pins the near-one, subnormal-adjacent, and high-end boundary cases that the latest scan correction touches, `tests/test_float_shortest_binary_search.c` still guards the round-trip formatter rewrite, `tests/test_float_shortest_split.c` guards the direct shared-helper dispatch shape, `tests/test_float_shortest_common_helper.c` guards the shared helper routing, `tests/test_float_module_scaffold.c` now also guards the shared shortest-literal helper placement, `tests/test_float_shortest_scientific_guard.c` guards the tiny-finite scientific normalization path on both signs, and `tests/test_float_upgrade_corpus_f32.c` now guards the float32 corpus source contract.
  - Next staged step:
    1. Continue the broader float upgrade with the remaining parser-driven formatter/backend work once a later rewrite can absorb the current special-case literals safely.
    2. Keep the 0.001 and 0.0001 fixed-versus-scientific tie-break corpus pinned for both widths while the parser-driven backend remains staged.
  - Out of scope for this pass: broad algorithmic rewrites that would change the current fixed-precision default or add hidden heap allocation on hot paths.

## Already addressed in this pass

- Verification infrastructure: added `tests/test_job_stress_tsan` for a denser job-queue stress pass and `tests/test_float_host_oracle` for host-oracle float comparisons against the platform C library.
- Float32 host oracle coverage: added `tests/test_float_host_oracle_f32` so the fixed float32 formatter is checked against the platform C library on representative finite values.
- Float corpus pinning: added `tests/test_float_upgrade_corpus` to pin exact-range, subnormal-boundary, and shortest-format spellings while the staged float upgrade remains open.
- Decimal underflow policy: decimal inputs below the smallest subnormal are documented and tested as signed zero with the input sign preserved.
- Scientific normalization correction: the formatter now re-normalizes after coarse scaling so very small finite values do not overshoot into invalid mantissas.
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
