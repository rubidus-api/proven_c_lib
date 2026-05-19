# proven_c_lib TODO

This file tracks the remaining follow-up items from the current main-branch review. Items already resolved in this pass are intentionally omitted.

## Open items

- Float scanner exponent bounds: review whether `proven_scan_f64()` should treat underflow and overflow boundaries more explicitly after the current decimal parser changes. Next verification hook: add boundary tests for `1e-330`, `1e308`, `1e309`, and the smallest subnormal spellings.
- Float conversion portability: remove the remaining `long double` dependence in the scan and format conversion paths if target-deterministic `double` scaling is required. Next verification hook: build and compare float outputs on a target where `long double` is not wider than `double`.
- Map borrowed-key contracts: document and harden the lifetime rules for `PROVEN_KEY_TYPE_U8_BORROWED`, and decide whether an owned-key path and hardened validation gate are needed. Next verification hook: add tests that reject internal borrowed keys and exercise any owned-key path if it is introduced.
- Streaming scanner boundary behavior: review whether token-split cases across buffered sysio fragments need `PROVEN_ERR_NEED_MORE` semantics or a line-based fallback. Next verification hook: add a boundary test with a token that spans the loaded buffer size.
- Documentation and version synchronization: check for remaining wording, version, and contract mismatches after code changes. Next verification hook: run the doc grep checks and confirm the top-of-tree version strings stay in sync with `include/proven/version.h`.

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
