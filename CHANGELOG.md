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

### Changed

- Documentation reorganization and refresh. Moved the dated benchmark reports and the design proposals/RFC audits under `docs/internal/` (with a `docs/internal/README.md` describing the folder), since they are development records rather than user docs; `docs/float-correctness-and-performance.md` remains the user-facing summary. Updated the floating-point section of `manual/manual-08-fmt-scan.md` to describe the current exact, correctly-rounded (round-half-to-even) formatter and three-tier parser with worked examples (the old text still described an approximate six-digit/round-half-up formatter). Rewrote the relevant parts of `README.md` (both language halves) to add a "correct, fast number conversion" section with example code and objective validation/benchmark numbers, list the `float_parse`/`float_format` modules, and fix the documentation index, which pointed at files that are not part of the published repository (`SPEC.md`, `AGENTS.md`, `MEMORY.md`).

- Replaced the shortest float formatter. First with a single-pass exact algorithm (Burger-Dybvig / Dragon4, round-to-nearest-ties-to-even) for binary64 and binary32, then with a Grisu3 fast path (64-bit diy_fp + a generated cached-power table) that falls back to the exact path only when it cannot prove the result is shortest. Net result is about 670x faster than the original round-trip-search formatter (59,018 -> 88 ns/call on a mixed corpus) and uniform across magnitudes, with the same correctly-rounded minimal output (validated round-trip and minimality over ~3M doubles and ~5M floats).

### Removed

- Removed the obsolete hand-maintained shortest literal table (`proven_float_shortest_literal_f64`/`_f32` and their tables) now that the shortest formatter computes every value directly, along with the structural tests that pinned the old staged round-trip-search backend.
- Removed the dead round-trip-search fixed-precision machinery in `float_format.c` (`proven_float_format_roundtrip_search_fixed`, `candidate_exact`, `candidate_roundtrips`, `roundtrips_f64`/`_f32`, `adjust_fixed_neighbor`, `build_scientific_ld`, `normalize_scientific_ld`). It was reachable only through the `RYU` policy in `FIXED` mode, which no caller or test used; that combination now routes to the same exact integer path as the other policies. This removes the formatter's last `long double` use, so `float_format.c` is now entirely integer-based. Behavior of the exercised paths is unchanged (re-validated against host `snprintf` over ~3M doubles at precisions 1..18, zero mismatches).

### Fixed

- Canonicalized shortest float output for values just below a power of ten. The digit generators (both Grisu3 and Dragon4) could leave a spurious leading zero with the decimal exponent one too high — e.g. `9.995442674871462e-265` was emitted as `0.9995442674871462e-264` — which round-tripped correctly but was non-canonical and inflated the reported significant-digit count by one. `proven_float_shortest_digits`/`_f32` now strip leading zeros and lower the decimal exponent. Found by a 2.56-billion-value `binary64` differential check against host `strtod` (93 affected values, all near a power of ten); the value, round-trip property, and minimal length are unchanged. The exhaustive `binary32` sweep was unaffected (it had no such cases) and still passes.
- Made fixed-precision float formatting (`%f`/`%e` via `proven_float_format_f64_policy` and the `{}` formatter) exact and correctly rounded. The previous path used `double`/`long double` arithmetic capped at 18 fractional digits and produced wrong digits for high precision, values at or above 2^64, subnormals, and boundary cases; a differential check against host `snprintf` went from roughly 20% mismatches to zero across 4,000,000 value/precision pairs. The new path is integer-only (no long double), correctly rounds to nearest-even, and supports arbitrary precision up to the big-integer capacity.
- Fixed an out-of-bounds read in `proven_float_bigint_cmp_shift_left` when the shift was a multiple of 64: the low zero-padding limbs were not compared and the index underflowed. This helper is shared with the decimal parser's exact comparison; the parser's differential fuzz remains at zero mismatches after the fix.
- Corrected the shortest float formatter to emit the true minimal round-tripping form. The shortest search now generates candidates with the exact digit engine and no longer consults the hand-maintained literal table, several of whose entries were non-minimal (for example the largest subnormal and `FLT_MIN`).

### Added

- Added `docs/float-correctness-and-performance.md`, a self-contained reference describing the parsing/formatting algorithms (three-tier Clinger / Eisel-Lemire / exact-fallback parser; Grisu3 + Dragon4 shortest; exact big-integer `%f`/`%e`), the validation methodology, and the performance comparison against the host C library. Backed by an **exhaustive** sweep of all 4,278,190,080 finite `binary32` values (shortest round-trip + minimality via host `strtof`, parser bit-exact vs host `strtod`) and a **2,560,000,000-value** randomized `binary64` differential sweep against host `strtod`, both with **zero** failures, plus a host-comparison benchmark. Dated raw outputs live under `docs/internal/benchmarks/` (`*-f32-exhaustive-validation.md`, `*-float-vs-host-benchmark.md`). The benchmark shows the library is faster than glibc at parsing typical numbers, shortest formatting (~4-5x), and `%f`/`%e` at normal magnitudes, while staying bit-identical to `strtod`/`snprintf`.
- Added a big-integer division helper (`proven_float_bigint_divmod`, Knuth Algorithm D on base-2^32 limbs, no `__int128`, freestanding-safe) with a limb-array entry point `proven_float_bigint_divmod_u64` and a unit test (`tests/test_float_bigint_divmod`). It is a validated reusable primitive; a measured experiment showed that using it to compute the exact-fallback float result is slower than the existing estimate-seeded search, so the parser keeps the search and the division is not on the parse hot path.
- Made the exact-fallback big-integer capacity configurable through `PROVEN_FLOAT_BIGINT_LIMBS` (in `include/proven/float_config.h`, default 160). Lowering it shrinks the exact-fallback and division stack footprint for embedded targets (for example `-DPROVEN_FLOAT_BIGINT_LIMBS=48` cuts the division frame from ~10.5 KB to ~3.3 KB and the converter from ~6.7 KB to ~2.2 KB). The kept-significand cap is derived from the capacity, so reduced builds still parse correctly up to that many significant digits and stay within one ULP beyond it; the Clinger and Eisel-Lemire fast paths never use the big integer. The build driver now also forwards `-cflags` to test compilation so such config macros stay consistent between library objects and tests.

### Changed

- Made the float parser treat a dangling exponent marker the way `strtod` does. A trailing `e`, `e+`, or `e-` that is not followed by exponent digits (for example `1e`, `1e+`, `1.5eZ`) is no longer rejected; the parser keeps the mantissa parsed so far and stops at the `e`. This affects `proven_parse_double_ascii`, `proven_strtod`, and `proven_scan_f64`, which share the ASCII token scanner.

### Fixed

- Removed the hard limit that rejected decimal inputs with more significant digits than the exact-fallback bigint could hold (about 3080 digits), which previously returned `0` and consumed nothing. The exact significand is now capped at a fixed number of kept digits (800, above the 767-digit worst case for binary64 rounding); digits past the cap shift `exp10` and set a sticky flag that breaks an exact-equal comparison upward, so arbitrarily long inputs parse correctly in bounded time. Validated against host `strtod` with a 300,000-case fuzz over 700-3099 digit mantissas and targeted midpoint sticky tie-break cases.
- Corrected `significant_digits` in the decimal metadata so it no longer counts trailing zeros that are already folded into `exp10`. The inflated count biased the magnitude estimate and the derived binary-exponent search bounds high, which could place the true result outside the exact-search range and yield a power-of-two result for some long-mantissa inputs (for example `12345678901234567890` and `109.31074080952665007690591502623020`). A 5,000,000-case randomized differential check against host `strtod` now reports zero mismatches.

### Changed

- Seeded the exact-fallback binary search from a cheap reconstructed double estimate, narrowing the search to a verified window around it before falling back to the full exponent-bracket range. The window is only adopted after a two-point bracket check, so the rounded result is unchanged while the `fallback` and `boundary_tie` benchmark groups drop by roughly one half.

### Removed

- Removed four unused static float helpers (`proven_float_bigint_add`, `proven_float_compare_mantissa_to_scaled`, `proven_float_compare_decimal_to_bits_legacy`, `proven_float_compare_decimal_to_midpoint_legacy`) left over from the decimal-to-binary64 rewrite; they were dead code and tripped the `-Werror` strict and freestanding builds.

### Changed

- Reverted the staged positive-exponent scalar helper experiment after benchmark runs showed it regressed the path corpus, restoring the prior baseline for `staged_scientific`, `fallback`, and `boundary_tie`.
- Optimized the legacy negative-exponent exact compare path to build a cached pow5 factor once before comparing, which shaved a little more off `fallback` and `boundary_tie`.
- Kept the cached pow5 factor reuse on the legacy negative-exponent compare path after the follow-up compare-path tweak showed mixed results; the retained change avoids repeated `5` multiplication while leaving the public parser behavior unchanged.
- Added an equal-exponent fast path in the adjacent-midpoint helper so the exact boundary compare can skip two generic shifts when both sides already share the same exponent.
- Reworked the hot `proven_float_bigint_mul_u64_factor()` carry step to use a plain low-limb add-and-carry extraction, which remains the retained optimization in the latest benchmarked state after the latest path benchmark run.

### Added

- Added `proven_parse_f64_ascii()` and `proven_strtod()` as public float-parse entry points over the shared decimal-to-binary64 backend.
- Added internal float-parse path counters so tests can distinguish Clinger hits, staged Eisel-Lemire hits, and exact bigint fallback hits.
- Added `THIRD_PARTY_NOTICES.md` to record the clean-room status of the decimal-to-binary64 parser rewrite.
- Added `scripts/generate_float_decimal_tables.py` and a generated cached-`5^q` header so the current fast path no longer depends on hand-maintained power tables.
- Added an opt-in `bench-float` build-driver command plus a dated `docs/benchmarks/2026-06-13-float-parse-benchmark.md` report comparing `proven_parse_double_ascii`, `proven_strtod`, and host `strtod` on a representative decimal corpus.
- Added a dated `docs/benchmarks/2026-06-13-float-parse-path-matrix.md` guide that breaks the float parse workload into Clinger, staged cached-power, exact fallback, wrapper, and host-reference paths.
- Added a dated `docs/benchmarks/2026-06-13-float-parse-path-benchmark.md` report that splits the float parse workload into short-exact, staged-scientific, fallback, and boundary-tie corpora.
- Added a timestamped `docs/benchmarks/2026-06-12-194411-float-parse-path-benchmark.md` report capturing a fresh path benchmark run against host `strtod`.
- Added a timestamped `docs/benchmarks/2026-06-12-192443-float-parse-path-benchmark.md` report capturing the updated path benchmark after the fast-path significand handling fix.
- Adjusted fast-path significand preparation so the staged Eisel-Lemire validation can keep its representative scientific inputs on the staged path without regressing the Clinger-only case.
- Reduced exact-fallback comparison cost by caching the shared `5^q` state across fallback midpoint checks, which cuts the fallback-heavy and boundary-tie benchmark groups materially without changing the public parser API.
- Deferred exact-bigint construction until the parser actually falls back, and switched staged Eisel-Lemire validation to the lightweight mantissa/exponent representation, which pulled the `staged_scientific` benchmark back into the sub-microsecond band.
- Reused generated cached `5^q` tables when preparing exact-compare state, which removed a large repeated-multiply cost from the remaining exact and staged validation paths.
- Reverted a pow5-state-hoist experiment after benchmark runs showed slower `boundary_tie` corpora, restoring the prior wrapper-owned validation state path.
- Collapsed the decimal metadata build into a single input scan instead of scanning the same token twice before fallback or staged validation.
- Replaced the cached-factor bigint multiply loop with a single schoolbook pass, which cut the remaining fallback and boundary-tie exact-compare cost substantially without changing parse results.
- Built adjacent-float midpoints directly from raw mantissa/exponent words instead of through two bigint shifts and an add, which trimmed the remaining boundary-tie validation cost.
- Specialized the common 1-limb cached-factor multiply case, which shaved more time off the remaining fallback-heavy and boundary-tie paths.
- Fixed exact-compare state prep so negative exponents cache the reciprocal pow5 factor again instead of dropping to the legacy multiply loop, which restored the intended cached-factor path in the fallback-heavy cases.
- Fixed `proven_float_bigint_copy_mul_factor()` so the 1-limb fast path copies the source into the working product before multiplying, keeping the negative-exponent exact path correct.
- Added a fused 1-limb copy-multiply loop so the negative-exponent exact path no longer does a separate copy pass before multiplying small cached factors.
- Simplified the fused 1-limb copy-multiply loop to a single carry scan, then benchmarked a direct 128-bit variant and reverted it after it regressed `fallback` and `boundary_tie`; the single carry scan remains the retained baseline.
- Verified the retained fused 1-limb copy-multiply loop on the path benchmark: `short_exact` and `staged_scientific` improved again, while the slower groups stayed at the retained baseline after the reverted experiment.
- Specialized the negative-exponent exact compare path for 1-limb rhs operands, which lowered `fallback` and `boundary_tie` again while keeping `short_exact` and `staged_scientific` near the same band.
- Re-ran the path benchmark on that compare-site specialization and kept the improvement: `fallback` and `boundary_tie` stayed materially faster while the short/staged corpora stayed close to the prior run.
- Applied the same 1-limb rhs specialization to staged validation, which nudged `staged_scientific` down while keeping the fallback-heavy corpora improved.
- Folded the 1-limb rhs multiply into a shared helper used by both the staged and exact negative-exponent compare sites, which kept the benchmark gains while reducing duplicated path logic.
- Added a 2-limb copy-multiply specialization for the negative-exponent exact compare path, which trimmed the fallback-heavy and boundary-tie benchmark groups again.
- Added a tiny-limb cached-factor multiply path for the remaining exact-comparison cases, which kept the fallback path moving while preserving corpus agreement.
- Avoided full 160-limb bigint zeroing when preparing hot compare operands, which materially reduced the cost of the remaining exact fallback and boundary-tie paths.
- Narrowed bigint copies to the live limb prefix instead of copying the full 160-limb backing store, which cut more memory traffic from the remaining exact compare path.
- Removed the last full-buffer zeroing from `proven_float_bigint_mul_factor()` by clearing only the live output prefix, which kept the remaining exact compare work moving down.
- Switched cached pow5 setup and the hot exact-compare bigint constructors to skip unnecessary full-buffer zeroing, which simplified the preparation path without changing parse results.
- Removed the extra bigint copy from the exact compare shift path so only the shifted operand is materialized in the common fallback and boundary-tie validation cases.
- Specialized hot bigint copies so 1- and 2-limb operands use direct assignments instead of a generic limb loop, which pulled the exact fallback and boundary-tie paths down again.
- Kept the hot bigint copy fast path specialized through four limbs and collapsed copy-plus-factor multiplication into one helper, which shaved more time off the exact compare-heavy benchmark groups.
- Re-ran the path benchmark after the copy-plus-factor helper change and confirmed the exact-compare-heavy corpora still match host `strtod`; a later small-rhs shortcut was reverted after it regressed `fallback` and `boundary_tie`, restoring the better numbers.
- Split the exact shifted-compare loop to handle the lowest shifted limb separately from the main walk, which shaved another small amount of work off the fallback-heavy corpora without changing results.
- Turned the remaining lower zero-prefix check in the exact shifted-compare loop into a branchless accumulation, which cut a little more overhead out of the fallback-heavy corpora.
- Removed an unnecessary full-biginteger zeroing from decimal token setup, which cut a large amount of common-path overhead out of the short-exact, staged, fallback, and boundary corpora.
- Delayed the legacy exact-compare RHS copy until the compare actually needs it, which removed another avoidable copy from the positive-exponent compare cases.
- Replaced the bigint limb-shift loop with `memmove`/`memset` in the exact compare path, which shaved more time off the fallback-heavy and boundary-tie corpora.
- Unrolled the common 1- and 2-limb shift case in the bigint exact-compare path, which cut another slice of overhead out of the fallback-heavy and boundary-tie corpora.
- Compared shifted bigints directly without materializing a shifted copy in the exact compare path, which removed another avoidable pass from the fallback-heavy and boundary-tie corpora.
- Rewrote the direct shift-compare loop to compute shifted limbs inline instead of routing through a per-limb helper, which shaved more time off `fallback` while keeping checksum agreement intact.
- Split the shifted-biginteger compare loop into separate shift and no-shift regions, which cut a little more branch noise out of the fallback-heavy path without changing results.
- Reworked `proven_strtod()` to reuse the parser's nonzero-digit metadata instead of rescanning the parsed token for underflow bookkeeping, which trimmed wrapper overhead without changing parse results.
- Switched the hosted `proven_strtod()` input-length scan to `strlen()`, which shaved a little more off the wrapper path in the hosted benchmark run.
- Reused the parser's sign flag for overflow fallback selection instead of re-reading the first token byte, which cleaned up the rare overflow path without changing parse results.
- Split the exact compare loop's top shifted limb into a separate check before the main walk, which removed a small amount of work from the fallback-heavy corpora without changing rounding.
- Removed unused exponent accumulation from token validation, which drops one redundant scan over exponent digits before the parser reaches the exact backend.
- Removed a dead zero-case branch from the decimal builder, which keeps the zero path simple without changing parser behavior.
- Skipped full 160-limb zeroing when rebuilding the exact significand, which cuts unnecessary memory traffic from the fallback path.
- Restored the safe direct shifted-compare helper after a copy-eliding simplification caused a `1e100` benchmark mismatch, keeping the benchmark corpus aligned with host `strtod`.
- Switched bigint zeroing and prefix clearing to `memset` and skipped unnecessary full-buffer zeroing in bigint constructors, which kept the fallback-heavy exact path moving without changing the parser API.
- Reverted a regressing midpoint-fast-path experiment in the exact compare helper after benchmark runs showed slower `fallback` and `boundary_tie` corpora.
- Restored the small-compare helper's cached-power overflow checks after the midpoint experiment, which improved the fallback-heavy and boundary-tie corpora again while keeping the benchmark corpus checksum-stable.

### Fixed

- `proven_sysio_scanner_deinit()` now clears the full scanner state after releasing the buffer.
- `proven_map_is_valid()` now checks the public `internal.size` against the bucket layout.
- `proven_fs_open()` now rejects unsupported mode bits before reaching the PAL layer.
- `proven_fs_open()` now rejects truncation requests that do not carry write intent.
- `proven_fs_lock()` now rejects unsupported lock modes instead of treating them as unlock.
- `proven_fs_chmod()` now rejects unsupported permission bits before reaching the PAL layer.
- `proven_float_format_f64_policy()` now rejects out-of-range fixed-mode precision values with `INVALID_ARG`.
- `proven_float_format_f64_policy()` now keeps tiny finite subnormals on the shortest formatting path instead of falling back to `UNSUPPORTED`.
- `proven_scan_f64()` now routes decimal parsing through a shared exact backend that tokenizes ASCII input once, compares candidate midpoints with bigint arithmetic, and preserves correct rounding at normal, subnormal, and overflow boundaries without host `strtod`.
- The staged `proven_float_try_eisel_lemire()` layer now handles a conservative exact negative-exponent subset in addition to the generated-`5^q` positive-exponent subset.
- The staged `proven_float_try_eisel_lemire()` layer now also uses `__uint128_t` to round a bounded negative-exponent ratio subset for normal-range candidates.
- The staged `proven_float_try_eisel_lemire()` layer now accepts wide-shift normal-range negative exponent ratios such as `1e-27` instead of forcing them into the bigint fallback.
- The staged `proven_float_try_eisel_lemire()` layer now validates candidate bits against exact midpoint comparisons before accepting them, which lets tie-to-even zero-exponent integers such as `9007199254740993` stay on the staged fast path without reintroducing one-ULP regressions.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated `u128` power-of-5 table for wider exact positive-exponent subsets such as `1e40` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now also uses the generated `u128` power-of-5 table for wider negative-exponent ratio subsets such as `1e-30` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now lets the `u256` negative-ratio scaler keep deeper wide-shift cases such as `1e-40` on the staged fast path before exact fallback.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated reciprocal `5^-q` cache for wider negative-exponent candidates such as `1e-100` before falling back to the exact bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated scaled `5^q` cache for wider positive-exponent candidates such as `1e100` before falling back to the exact bigint path.
- The widened staged `proven_float_try_eisel_lemire()` paths now share common cached-power candidate finalization so positive, negative, and subnormal candidates all pass through the same exact midpoint validator.
- The widened staged `proven_float_try_eisel_lemire()` paths now also share one cached-power `u64 x u128 -> u256` product helper for both positive scaled-`5^q` and negative reciprocal-`5^-q` candidate assembly.
- The widened staged `proven_float_try_eisel_lemire()` paths now round wide products into a shared `53-bit significand + unbiased exponent` packing step, which keeps representative subnormals such as `5e-324` on the staged fast path while leaving below-half true-min cases on exact fallback.
- The staged `proven_float_try_eisel_lemire()` entry logic now routes positive exponents through one generated power-of-5 product path and negative exponents through one bounded denominator-or-reciprocal normalization path instead of keeping separate exact-cancellation branches.
- Negative exponents now try the generated reciprocal cached-power candidate path first across the whole staged band, using the older small-`q` denominator normalization only when the reciprocal candidate remains uncertain.
- The staged `proven_float_try_eisel_lemire()` layer now builds explicit candidate plans for positive products, negative reciprocals, and denominator fallbacks, then executes them through one shared plan-dispatch seam.
- The staged `proven_float_try_eisel_lemire()` layer now feeds both positive `5^q` products and negative reciprocal `5^-q` candidates through one signed cached-power product-plan builder before considering the narrow negative denominator fallback.
- The staged `proven_float_try_eisel_lemire()` layer now drops the separate negative denominator-normalization family; uncertain negative cached-power candidates defer directly to the exact bigint fallback.
- Internal staged-path metrics now expose shared cached-power product-plan hits so tests can verify the staged representative corpus stays on that single success family.
- Added an explicit `rfc-0001` audit corpus that pins the public parser against the named RFC cases for basics, specials, 2^53 ties-even boundaries, true-min midpoint below/exact/above, very long significands, huge exponents, and malformed/endptr behavior.
- `proven_scan_f64()` now uses an exact cached-`5^q` positive-exponent fast path ahead of the bigint fallback for a broader range of large finite decimal integers.
- `proven_u8str_view_slice()` now allows empty slices at the end of a view.
- `proven_float_format_*_policy()` now rejects invalid policy enums before mode-specific dispatch.
- `proven_sysio_scan_chunk_impl()` now accepts exact 4096-byte chunk fits instead of treating them as truncation.
- `proven_u8str_fmt_internal()` now rejects unknown argument types instead of silently dropping them.

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
