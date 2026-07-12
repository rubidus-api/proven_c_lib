# Changelog

All notable changes to this project will be documented in this file.

The format follows Keep a Changelog:

- keep the log human-curated
- order entries chronologically with the newest first
- group notable changes by release or by `Unreleased`
- use the standard sections `Added`, `Changed`, `Deprecated`, `Removed`,
  `Fixed`, and `Security` when they apply
- avoid dumping raw commit history into the file

## [2026-07-12] — proven_c_lib-v26.07.12e

### Added

- **Manual chapter 8, sections 7-13** — the scanner half of the chapter, which had
  never been written. The chapter listed thirteen sections and ended at a bare
  `## 7. Scanner data model` heading. Closes `docs/BACKLOG.md` **B-001**.

  It was written against *measured* behaviour, not against the header, and that is
  how the surprising parts came to be documented at all:

  - `proven_scan_i64("0x10")` is the integer **zero**, cursor at 1. The integer
    scanners are decimal only - no hex, no octal, no base prefix.
  - `"1e309"` is `PROVEN_ERR_OVERFLOW`, but `"1e-400"` is `PROVEN_OK` with the value
    `0.0`. The asymmetry is deliberate: underflow to zero *is* the correctly rounded
    answer, while overflow has no correct finite answer at all.
  - `proven_scan_u64("-1")` is rejected rather than wrapping to a huge unsigned
    value - which is how a bounds check gets defeated.
  - **The structural scanner is not transactional.** When a literal fails to match,
    the placeholders *before* the mismatch have already been written through: the
    call returns `PROVEN_ERR_NOT_FOUND` and your destination holds a value anyway.
    On failure, treat every destination as clobbered.
  - Trailing input is **not** an error. The scanner matches what you asked for and
    stops; it does not police what you did not ask about.

- `manual/examples/ex_08_scan_recovery.c` — provokes every scan error code on
  purpose, including the non-transactional failure. Compiled and run by the build.
- `tests/test_docs_manual_ch08_contracts` — asserts each of the 18 behaviours
  chapter 8 states as fact. Prose is where a contract goes to drift; this one
  cannot. A false claim fails the build and names itself.

## [2026-07-12] — proven_c_lib-v26.07.12d

The manual's examples are now programs, the tests are named for what they check,
and the testing policy says out loud how this project actually develops.

### Added

- `manual/examples/` — eleven complete programs, one per topic the manual teaches.
  The build driver compiles and **runs** every one of them, under every sanitizer
  mode. They are written the way a caller writes code: explicit allocator, real
  error handling, a destroy for everything owned.
- `tests/test_docs_manual_examples` — requires every example the manual prints to
  be one of those programs, quoted verbatim; fails the build if a chapter and its
  example disagree, if a chapter quotes an example that does not exist, or if an
  example exists that no chapter shows.
- `docs/TESTING.md` — the testing policy: the naming scheme, what each test class
  is *for*, the rules a new test must satisfy, and an honest account of how this
  project develops. It records plainly that this is not TDD: every commit that
  adds a test also changes source in the same commit, and there is not one where a
  failing test lands first.
- `docs/BACKLOG.md` — a **tracked** backlog. The repository had `BACKLOGS.md` and
  `TODO.md`, but both are gitignored: a private queue nobody else can read and no
  commit can reference. Known work that lives on one machine is not tracked work.

### Changed

- **Tests are renamed for what they check.** `test_phase1` … `test_phase22` encoded
  the order they were written in, which is the one fact about a test nobody needs.
  Every test is now `test_<class>_<subject>`, where the class is one of `unit`,
  `contract`, `regression`, `differential`, `portability`, `stress`, `docs`,
  `bench`. 75 files renamed.
- **The test catalog has no numbers.** It ran `1..50` with `7a`, `30a`, `30b`,
  `30c`, `40a` wedged in wherever something new arrived — and five of its entries
  described files deleted months earlier. The filename is the identifier now, and
  the catalog is grouped by class.

### Fixed

- manual chapter 3 listed `proven_u8str_t` without its `borrowed` field, and told
  readers to use `internal.size` for the length. There is no `size` member -
  `proven_buf_t` is `ptr` / `len` / `cap` - so code following the manual did not
  compile.
- manual chapter 5 never said how end-of-file is reported. `proven_fs_read`
  returns `PROVEN_ERR_EOF`, not a zero-byte success, so the obvious read loop
  (`if (r.value == 0) break;`) never takes that branch and treats the end of the
  file as an I/O failure. The chapter now says so, and the worked example shows the
  correct shape.
- manual chapter 5's `proven_fs_stat_t` listing claimed a symlink file type.
  `proven_fs_type_t` has only `_FILE`, `_DIR` and `_OTHER`.

### Known

Two items are registered in `docs/BACKLOG.md` rather than rushed:

- **B-001** — manual chapter 8 ends mid-chapter at a bare `## 7. Scanner data
  model` heading. Sections 7-13 are in the table of contents and absent from the
  document: roughly half the chapter, and the half covering the scanner.
- **B-002** — of the manual's ~190 fenced code blocks, four could be compiled
  before this release. Eleven are now real programs; the rest are still sketches
  that reference imaginary helpers. They are being converted chapter by chapter,
  with the mechanism already in place to keep each finished chapter finished.

## [2026-07-12] — proven_c_lib-v26.07.12c

A documentation-currency release, plus the API-surface gap that the sweep turned up.

### Added

- 25 missing `xcv_*` aliases. `include/proven/alias_xcv.h` claims to cover the
  public API and did not: three functions added in v26.07.12b had no alias, and
  22 more had been missing for months - among them `proven_fs_write_all`,
  `proven_panic`, `proven_strtod`, `proven_pool_destroy` and the `sysio` scanner
  entry points. An alias layer that covers most of the API is worse than none:
  the caller finds the gaps one compile error at a time, at whichever call site
  happens to need the one function nobody aliased. The layer now covers all 203
  public functions.
- `tests/test_alias_completeness` - parses the public headers and the alias
  header and fails the build if any public function has no alias. `test_alias_smoke`
  could never have caught this: it hand-picks a subset of aliases and only checks
  that they compile, so it cannot notice one that is absent. This is the only way
  a list like that stays true.
- Two tests that existed on disk but were never registered in `nob.c` -
  `tests/test_float_format_shortest_roundtrip` and `tests/test_float_parse_benchmark` -
  are registered and now actually run. Both pass.

### Fixed

- `proven_array_sort`'s header comment still described it as "a robust quicksort".
  It is an introsort, and the two properties that matter to a caller - the
  O(n log n) guarantee, and duplicate keys being the fast case rather than the
  quadratic one - were documented nowhere a caller would look.
- The allocator's alignment-class contract was undocumented. v26.07.12b made the
  heap allocator route `align <= alignof(max_align_t)` through malloc/realloc (so
  growth can happen in place) and over-aligned requests through the aligned
  family. A block must therefore be reallocated and freed with the alignment it
  was allocated with - a real obligation on callers that existed only as a comment
  in a `.c` file. Now stated in `allocator.h`, `platform/proven_sys_mem.h`, and
  manual chapter 2.
- `docs/float-correctness-and-performance.md` still said the parser used
  `long double` to seed its exponent estimate. It has not since v26.07.12b; the
  whole engine is integer-only now, formatter and parser alike.
- `proven_fs_stat`'s `perms` field is documented as carrying only the nine
  permission bits, and the manual now says why that changed: it used to hand back
  the raw `st_mode`, whose file-type bits `proven_fs_chmod` rejects, so the
  obvious round-trip failed for every real file.
- `TEST.md` claimed 48 hosted tests (there are 75), documented five test files
  that were deleted months ago, and never mentioned five that exist. It now
  matches the tests on disk and the registry in `nob.c`.
- `manual/manual-07-alias-xcv-index.md` was missing 37 aliases and 340 of its 379
  rows had the wrong line number. Regenerated from the header, and the line-number
  column is gone: it was wrong after every alias inserted above it, which is worse
  than not having the column.
- `manual/manual-01-foundation.md` showed a stale `PROVEN_VERSION_NUM` and suffix.
- `CHECKLIST.md` told the maintainer to sync the version string in `SPEC.md` and
  `docs-site/index.html`, neither of which exists, and its "Active Task" was work
  finished several releases ago.
- References to `docs/internal/` now say plainly that it is maintainer-local and
  not part of the published repository, instead of reading as a path the reader
  could follow.

### Changed

- README states where the platform boundary stops - the PAL covers memory,
  filesystem, time, mmap, environment, console I/O and threads, and does *not*
  cover process control, terminal control, or networking - and names the
  deliberate non-goals (hashing, path manipulation, argument parsing, logging).
  A boundary you have to discover by running into it is a worse boundary than one
  that is written down.
- README documents the whole-file I/O added in v26.07.12b and the sort's
  guarantees, in both language halves; the Korean quick start now matches the
  English one.

## [2026-07-12] — proven_c_lib-v26.07.12b

### Fixed

- `proven_array_sort` was quadratic on duplicate keys. The Lomuto partition used
  a strict `cmp(x, pivot) < 0` test, so every element *equal* to the pivot went
  into the right partition; on a low-cardinality key - a status column, an enum,
  a bucket id - the split collapsed to 1/(n-1). Sorting 100,000 identical
  `int32` keys took **10.6 seconds**. A caller sorting data an attacker can shape
  had a denial of service, not merely a slow path. Replaced with an introsort:
  a Bentley-McIlroy three-way partition (so an equal run is final and never
  recursed into, and every element is compared exactly once), an insertion-sort
  cutoff, and a heapsort fallback past a depth of `2*log2(n)` - which is what
  makes the O(n log n) bound a guarantee rather than a hope, since
  median-of-three alone can still be driven quadratic.
- A `proven_job_system` worker could exit leaving an accepted job unrun. A
  submitter that passed `begin_submit` before the close can have claimed its slot
  with the enqueue CAS without yet publishing `cell->sequence`, which to a
  dequeuer is indistinguishable from an empty queue. The last worker could
  therefore exit, the submitter then publish, and `proven_job_submit` return true
  for a job nobody would ever run - while `proven_job_system_destroy`, documented
  to block until the queue is exhausted, returned. A worker now leaves only once
  no submitter is in flight *and* the queue is still empty when re-checked.
- `proven_fs_read_all` allocated twice the file size for every regular file: the
  buffer was seeded to the exact reported size, so the read loop filled it and
  then had to grow before it could issue the read that would observe EOF. Peak
  memory was 3x the file. EOF is now confirmed with a one-byte probe, and the
  buffer grows only if the source really does outrun its reported size.
- `proven_fs_read_all_u8str` started from a one-byte buffer for any source that
  reports no size: the chunk fallback tested the capacity, which is never 0 once
  a terminator byte is reserved. Reading `/proc/self/status` took 12 reallocs.
- `proven_fs_write_file_atomic` widened permissions. The temp sibling is created
  with `0666 & ~umask` and `rename` carries its mode onto the target, so
  atomically rewriting a `0600` key file republished it as `0644`. The target's
  mode is now copied onto the temp before the rename.
- `proven_fs_write_file_atomic` failed on legal long filenames: a 250-character
  basename - which `proven_fs_write_file` accepts - made the temp sibling exceed
  `NAME_MAX`. The copied stem is now trimmed to leave room for the suffix.
- `proven_fs_stat` put the raw `st_mode` into `perms`, a field typed
  `proven_fs_perms_t`. `st_mode` also carries the file-type bits, and
  `proven_fs_chmod` rejects any bit outside the nine it supports - so
  `chmod(path, stat(path).perms)`, the obvious use of the field, returned
  `PROVEN_ERR_INVALID_ARG` for every real file. `perms` is now masked to the low
  nine bits.

### Changed

- The decimal parser's exponent-bounds estimate no longer uses `long double`,
  the one type in C whose width differs across the targets this library builds
  for (80-bit on x86, 128-bit on aarch64, plain 64-bit on armhf and MSVC). The
  estimate happened to come out identical on all of them - verified over the
  entire input range it can see - so nothing was ever wrong, but a
  correctness-critical path had no business depending on it, and on a soft-float
  target it pulled in libgcc routines for no reason. Replaced with an integer
  fixed-point computation, bit-identical to the exact `floor(k * log2(10))` for
  every k in range, and verified to produce byte-identical parse results over
  3,000,000 randomized decimal inputs.
- `proven_fmt` appends literal text in runs instead of one character at a time.
  Each literal character used to cost a checked add, an out-of-line one-byte
  move and a NUL reseal - twice, since the format string is walked once to
  measure and once to write. A 101-character literal went from 1322 ns to 166 ns;
  a four-argument log line from 998 ns to 307 ns.
- `proven_map_set` no longer probes the same chain twice. It validated the map,
  then `set_with_scratch` validated it again, then walked the probe chain looking
  for an existing key, then called `map_insert_no_grow` - which walks the same
  chain and already overwrites a key it finds. The probe is now taken only when
  the map is about to grow, where it still saves an unnecessary rehash. 500k
  int inserts: 215.5 ns -> 190.2 ns per op.
- Sorting wide elements is faster: `swap_elements` takes a bulk-copy branch above
  16 bytes instead of swapping a byte at a time. 100k 48-byte structs:
  59.2 ms -> 16.2 ms.

## [2026-07-12] — proven_c_lib-v26.07.12a

### Fixed

- `proven_fs_read_all` silently returned an empty buffer for any source whose
  size cannot be measured up front. `proven_sys_fs_size` reports 0 for anything
  that is not a regular file, and `read_all` used that 0 as its buffer size, so
  reading a FIFO, a character device, or a `/proc` entry succeeded with zero
  bytes and dropped the contents. `proven_fs_size("/proc/self/status")` is
  `0`/`PROVEN_OK`, so a 1516-byte file read as empty. `read_all` now reads to
  EOF and uses the reported size only to seed the initial capacity: a regular
  file is still one allocation and one pass, an unmeasurable source is read
  correctly, and a regular file that grows mid-read is no longer truncated.
- Stack buffer overflow formatting a `proven_datetime_t` with a negative year.
  `year` is `proven_i32`, but it was cast to `unsigned long long` before
  conversion, so `-1` became `18446744073709551615` — twenty digits plus a NUL
  into a twenty-byte scratch buffer (ASan: stack-buffer-overflow in `itoa_raw`).
  The year now renders with its sign, and the scratch holds any 64-bit value.
- `proven_sysio_scanner_scan_impl` corrupted the stream when it rolled back a
  failed scan. `scanner_fill` compacts the buffer, but the rollback restored the
  cursor and length captured *before* that compaction, so the restored indices
  described different bytes: one byte was dropped from the front of the stream
  and one byte — already returned to the file by the rewind — was read twice.
  The rollback now accounts for how far the buffer moved. On a non-seekable
  input, where the rewind cannot succeed, the bytes already read are kept
  buffered instead of being discarded.
- `proven_u8str_reserve` and the growth path of the formatter left `ptr[len]`
  uninitialized. Both allocate, and allocators do not return zeroed memory, so
  reserving on a zero-initialized string — or formatting something that produces
  no output — broke the NUL seal that `proven_u8str_as_cstr` is documented to
  rely on, and `proven_u8str_is_valid` rejected the result. Both paths now seal
  the terminator.
- `proven_pool_init` published `bin_cap` before allocating the bin behind it, so
  a failed init left a pool claiming slots it did not have. The free trait tests
  `bin_len < bin_cap` and then writes `bin[bin_len]`, which with `bin == NULL` is
  a null write. `bin_cap` is now set only after the bin exists.
- Unchecked `count * size` arithmetic in `proven_sysio_scanner_scan_impl`, a
  public entry point that takes `args_count` from the caller. It is now routed
  through `PROVEN_CKD_MUL` like every other size computation in the library.

### Added

- `proven_fs_read_all_u8str`: the whole-file read most callers actually want,
  returning a NUL-terminated owned `proven_u8str_t` so `proven_u8str_as_view` and
  `proven_u8str_as_cstr` work on the result without a second copy. The terminator
  slot is reserved up front, so it costs no extra allocation over `read_all`.
- `proven_fs_write_file`: one-call create-or-truncate whole-file write, the half
  of the API that was missing next to `read_all`.
- `proven_fs_write_file_atomic`: writes through a sibling temp file and renames
  it over the target, so a concurrent reader never observes a half-written file.
  Atomic with respect to readers, not durable across power loss — proven exposes
  no fsync, and the header says so.
- `[[nodiscard]]` on `proven_sysio_scanner_scan_impl` and
  `proven_sysio_scan_chunk_impl`. `proven_sysio_print_impl` is deliberately left
  unannotated: `proven_print` expands to it and is used as `printf` is.

### Changed

- `proven_sys_mem_realloc` can now grow a block in place. Every allocation used
  to go through `posix_memalign` / `_aligned_malloc`, which cannot be handed to
  `realloc()`, so growth always paid a full copy. Requests at or below
  `alignof(max_align_t)` — every string, buffer, and byte array in the library —
  now come from `malloc` and grow through `realloc`, which for large blocks
  remaps pages instead of copying them. Over-aligned requests keep the aligned
  path. Windows keeps every block on the aligned family (`free` and
  `_aligned_free` are not interchangeable, and the free trait is not told the
  alignment) and uses `_aligned_realloc`. Failure atomicity is unchanged.
  Measured, with every byte of the buffer written: growing a buffer to 256 MiB by
  doubling went from 0.69s to 0.32s (2.1x); 200k small allocations with six
  reallocs each went from 0.05s to 0.035s (1.4x).

## [2026-06-24] — proven_c_lib-v26.06.24b

### Fixed

- Build break on older GCC under the `-std=c2x` fallback: `src/proven/job.c`
  used `alignof` without `<stdalign.h>`. `alignof` is a first-class keyword only
  in C23; under the documented `-std=c2x` fallback (and C11/C17) it is a macro
  that `<stdalign.h>` provides. On a compiler new enough to keyword-ify `alignof`
  under c2x (e.g. GCC 14) it built anyway, which is why builds on newer toolchains
  did not catch it; on an older GCC the c2x fallback failed to compile at
  `job.c:111`, taking down the whole hosted build. Reported by an external tester
  whose default GCC fell back to c2x (their clang regression / ASan / UBSan /
  freestanding runs passed).
- Fix is centralized: `<stdalign.h>` is now included from `include/proven/types.h`,
  the foundation header every translation unit pulls in, so all `alignof`/`alignas`
  users are covered regardless of their own include list. This also closes the same
  latent gap in `fmt.c`, `pool.c`, and `sysio.c` (which previously relied on
  transitive includes). Verified: `alignof` via `proven/types.h` compiles even
  under `-std=c11` (where it is not a keyword); full gcc build, `strict-error`, and
  `freestanding` gates pass.

## [2026-06-24] — proven_c_lib-v26.06.24a

### Changed

- Documentation release (no library code changes). Brought the manuals and README
  current with v26.06.22a and added deep-dive sections to the chapters:
  - `manual/manual-04`: how the hash map works internally (bucket layout,
    FNV-1a / bit-mix hashing, linear probing, tombstones, the 3/4 load factor and
    rehash, the three key modes, and the `set_with_scratch` alias case).
  - `manual/manual-06`: the job system's concurrency model (atomic MPMC ring,
    lifecycle state machine, memory-visibility via the destroy/join sync point) and
    the stackless-coroutine expansion with the "locals do not survive a yield" rule.
  - `manual/manual-02`: how the pool's recycle bin works, with misuse cases.
  - `manual/manual-08`: an "Inside the engine" section for the float parse tiers
    (Clinger / Eisel-Lemire / exact big-integer) and the two formatters (Grisu3 +
    Dragon4 shortest, exact integer `%f`/`%e`).
  - `manual/manual-03`: the `proven_u8str_t` internal layout (`proven_buf_t internal`
    + `borrowed`) with a borrowed-string counter-example.
  - `manual/manual-05`: `proven_fs_stat_t` now documents `uid`/`gid`.
  - `manual/manual.md`: corrected the arena ownership row (caller-backed bump
    pointer, not an owner); header map adds `config.h`, `float_parse.h`,
    `float_format.h`, `float_config.h`.
- Moved the internal-only docs (`docs/internal/`: benchmarks, RFC drafts, overhaul
  plans) out of the repository into the private workspace and gitignored the path.

## [2026-06-22] — proven_c_lib-v26.06.22a

### Added

- `proven_fs_stat` now exposes file ownership: `proven_fs_stat_t` gains
  `unsigned long long uid` and `gid`, populated from `st_uid` / `st_gid` on
  POSIX and set to `0` on Windows (which has no POSIX ownership). The sys-level
  `proven_sys_fs_stat_t` carries the same two fields. Resolves the prov_text_editor
  enhancement request (docs/REPORT.md, 2026-06-19) that blocked the file browser's
  owner/group columns. Verified in `tests/test_phase14_fs_advanced.c` (uid/gid
  equal `getuid()`/`getgid()` for a just-created file on POSIX).

## [2026-06-21] — proven_c_lib-v26.06.21a

### Fixed

- `map.c`: silenced a `-Wunused-parameter` warning on the `map` argument of
  `map_key_is_valid`. Its only use is the hardened overlap check, which is
  compiled out on `-DNDEBUG` non-hardened builds, so downstream release builds
  (`-Wall -Wextra -DNDEBUG`) saw the warning. Added `(void)map;`. Reported via
  `docs/REPORT.md`.

### Changed

- Synced the version string to `proven_c_lib-v26.06.21a` across
  `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/`
  chapters. Also corrected the `manual-01` version-macro example, whose
  `STRING`/`NUM`/`SUFFIX` lines had drifted out of sync with each other.

## [2026-06-18] — proven_c_lib-v26.06.18b

### Added

- `proven_mem_move(dst, dst_cap, src_view)` (`memory.h`): a bounded,
  overlap-safe byte move with the same guards as `proven_mem_copy` (overflow →
  `PROVEN_ERR_OUT_OF_BOUNDS` without writing, null with size → `INVALID_ARG`,
  zero size → no-op). Lets downstream code drop libc `memmove` for overlapping
  array-element shifts. XCV alias `xcv_mem_move`.

## [2026-06-18] — proven_c_lib-v26.06.18a

### Added

- `proven_u8str_borrow(buf, cap)` and `proven_u8str_reset(str)` (`u8str.h`):
  wrap caller-owned memory as a fixed-capacity string and truncate-to-empty for
  reuse. A new `borrowed` flag on `proven_u8str_t` defaults to owned, so a
  zero-initialized handle keeps its existing semantics. The fixed-capacity
  operations and `proven_u8str_append_fmt` work on a borrowed string; the
  growing operations (`reserve`, `*_grow`, `append_byte`, `append_fmt_grow`)
  still succeed while the data fits but return `PROVEN_ERR_OUT_OF_BOUNDS`
  instead of reallocating caller memory, and `proven_u8str_destroy` is a no-op
  for a borrowed string. This lets allocator-free and per-frame call sites use
  the proven string system / formatter without heap allocation. Requested by a
  downstream project (`docs/REPORT.md`, 2026-06-18).
- `proven_mem_copy(dst, dst_cap, src_view)` (`memory.h`): a bounded byte copy
  that rejects overflow without writing, treats a zero-size source as a no-op,
  and rejects null pointers.
- XCV aliases `xcv_u8str_borrow`, `xcv_u8str_reset`, `xcv_mem_copy`.

### Changed

- `proven_u8str_t` gains a trailing `bool borrowed` field. `proven_buf_t`
  layout is unchanged. No public API consumes `sizeof(proven_u8str_t)` by
  contract; source compatibility holds after a recompile.

### Fixed

- `proven_diy_fp_normalize` (`float_decimal.c`) left-shifted a 64-bit value by
  the type width when the significand was zero (`clz` returns 64), which is
  undefined behavior surfaced by UBSan. Guard the zero case to mirror the
  previously-masked result (significand stays 0) without the UB; no change on
  any non-zero input, so formatter output is unchanged.

## [2026-06-17] — proven_c_lib-v26.06.17a

### Fixed

- Made `src/proven/float_decimal.c` compile on hosted targets without 128-bit
  integers (e.g. 32-bit ARM / `linux-armhf-hosted`). The Eisel-Lemire fast path
  is `__int128`-only, but its guard boundaries were inconsistent: helper calls
  sat outside the guard while definitions sat inside (and vice versa), so the
  cross matrix's new Windows link smoke exposed it via `arm-linux-gnueabihf-gcc`
  failures (implicit declarations, used-but-undefined, and unused-function
  `-Werror`). Moved the int128-free `proven_float_pack_binary64_candidate` out
  of the guard, added `#else` stubs for the two Eisel-Lemire entry helpers so
  the unconditional dispatcher links and reports "unsupported" (falling back to
  the scalar exact path, which already had a non-int128 multiply), and marked
  the Eisel-Lemire-only helpers `[[maybe_unused]]`. No behavior change on
  targets with `__int128` (x86-64 output is unchanged). `./nob cross` now passes
  every target, including both Windows link smokes.

## [2026-06-16] — proven_c_lib-v26.06.16x

### Fixed

- Made the panic handler link on Windows / PE-COFF (mingw-w64). The previous
  weakly-linked `proven_panic_handler` default linked on ELF but not on PE: a
  weak function definition in a separate object did not satisfy references,
  producing `undefined reference to proven_panic_handler` on every Windows link
  (the cross matrix is compile-only, so this was latent). Reported in
  `docs/REPORT.md`.

### Added

- Cross matrix link smoke for the Windows targets (`./nob cross`): the
  `windows-*` targets now link the full proven object set into an executable
  (`tests/test_cross_link_smoke.c`) instead of compiling only, so link-time
  symbol-resolution differences from ELF (such as the PE/COFF weak-symbol issue
  above) are caught instead of slipping through.

### Changed

- Replaced the weak-symbol panic override with a portable registration model:
  the library now raises panics via `proven_panic()` and installs handlers via
  `proven_set_panic_handler(proven_panic_handler_t)` (pass `NULL` to restore the
  trapping default). **Breaking:** defining a strong `proven_panic_handler` no
  longer overrides the handler; call `proven_set_panic_handler()` instead.
  Updated call sites (`pool.c`, `arena.h`), the override tests, and the panic
  documentation in `manual/` and `TEST.md`.
- Bumped the version to `proven_c_lib-v26.06.16x` and synced the version string
  across `include/proven/version.h`, `README.md`, `TEST.md`, and `manual/`.

## [2026-06-16] — proven_c_lib-v26.06.16w

### Changed

- Bumped the version to `proven_c_lib-v26.06.16w`, releasing the editor-oriented `proven_u8str` work: the growing in-place edit variants and the multi-algorithm substring search. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.

### Added

- Added growing variants of the in-place string edits: `proven_u8str_insert_grow` and `proven_u8str_replace_at_grow` (plus `xcv_` aliases). They have the same semantics as `proven_u8str_insert` / `proven_u8str_replace_at` but grow the buffer (doubling capacity) when the edit does not fit instead of returning `PROVEN_ERR_OUT_OF_BOUNDS`, so callers (for example a text editor making mid-buffer edits) no longer have to `reserve` manually before every insert. On allocation failure the string is left unchanged. New unit coverage in `tests/test_phase7_u8str_mut`.
- Added `proven_sys_mem_chr` to the platform memory layer: the system `memchr` when hosted, a freestanding-safe SWAR (word-at-a-time) scan otherwise. Verified against `memchr` over 2,000,000 randomized cases.

### Changed

- Rewrote `proven_u8str_view_find` from a naive O(n·m) byte loop to a multi-algorithm search that is self-contained (does not rely exclusively on `memchr`) and behaves identically under freestanding. The fast path samples the haystack, anchors on the rarest needle byte, scans with `proven_sys_mem_chr`, and verifies — fast on real text because a typical needle has a rare or absent byte. When the sample shows a low-entropy haystack (small effective alphabet: DNA, binary, long runs), it falls back to a linear, alphabet-independent algorithm: **Shift-Or / bitap** for needles up to 64 bytes, and **Two-Way (Crochemore-Perrin)** for longer needles. The long-needle fallback is compile-time selectable via `PROVEN_U8STR_FIND_LONG` (1 = Two-Way default, 2 = memchr-adaptive); Two-Way was chosen by benchmark (0.97× vs glibc `memmem` on a long-needle/long-verify input where memchr-adaptive is 3.1×). On realistic text the search is 4–30× faster than glibc `memmem`; single-byte search equals `memchr`; low-entropy cases stay at or below `memmem`. Validated for first-match equivalence against host `memmem` over 3,000,000 cases per alphabet (2/4/26 symbols, needles 0–139 bytes) for the dispatch and each forced algorithm, with zero mismatches; ASan/UBSan clean. Benchmark: `docs/internal/benchmarks/20260616-152810-u8str-find-multi-algorithm.md`.

## [2026-06-16] — proven_c_lib-v26.06.16v

### Changed

- Bumped the version to `proven_c_lib-v26.06.16v` (`PROVEN_VERSION_NUM` 260616), releasing the exact/fast floating-point parser and formatter work, the exhaustive and large-scale validation, and the documentation overhaul. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.
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
