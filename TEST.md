# proven Test Matrix (v26.07.12f)

This is the **catalog**: what every test checks, and where to start when one fails. Tests are plain C executables built and run by `nob.c`; no external framework is involved.

For the **policy** — how tests are named, what each class is for, the rules a new test has to satisfy, and an honest account of how this project actually develops — see [`docs/TESTING.md`](docs/TESTING.md).

## Naming

```text
tests/test_<class>_<subject>.c
```

The filename is the identifier. **There are no numbers.** Numbers were tried and they rotted: this catalog once ran `1..50` with `7a`, `30a`, `30b`, `30c`, `40a` wedged in wherever something new arrived, and five of its entries described files that had been deleted months earlier. The tests themselves were named `test_phase1` … `test_phase22` — the development order, which is the one fact about a test that nobody ever needs.

The class says what kind of question the test answers:

| Class | Question | Count |
|---|---|---|
| `unit` | Does this module do what it says, used the way a caller uses it? | 46 |
| `contract` | Does it *refuse* what it says it refuses? | 10 |
| `regression` | Does a defect that actually shipped stay fixed? | 7 |
| `differential` | Does it agree with an oracle we did not write? | 4 |
| `portability` | Does it compile, link, and keep its platform branches intact where we cannot run it? | 10 |
| `stress` | Does it survive concurrency, under a sanitizer, long enough for a race to be likely? | 1 |
| `docs` | Are the claims the documentation makes still true? | 4 |
| `bench` | How fast is it? (Not a correctness gate.) | 2 |

## Table of contents

- [Running tests](#running-tests)
- [Log format](#log-format)
- [Test modes](#test-modes)
- [Unit tests](#unit-tests)
- [Contract and hardening tests](#contract-and-hardening-tests)
- [Regression tests](#regression-tests)
- [Differential tests](#differential-tests)
- [Portability tests](#portability-tests)
- [Stress tests](#stress-tests)
- [Documentation tests](#documentation-tests)
- [Benchmarks](#benchmarks)
- [Regression subset](#regression-subset)
- [Cross compile-only matrix](#cross-compile-only-matrix)
- [Failure triage workflow](#failure-triage-workflow)
- [Change policy](#change-policy)
- [Release validation](#release-validation)

## Running tests

Compile the build driver first:

```sh
cc nob.c -o nob
```

Run the full hosted debug suite:

```sh
./nob build -build-root /home/user/work/build/proven_c_lib
```

Run the warnings-as-errors gate:

```sh
./nob strict-error -build-root /home/user/work/build/proven_c_lib
```

Run sanitizer modes:

```sh
./nob asan -build-root /home/user/work/build/proven_c_lib
./nob ubsan -build-root /home/user/work/build/proven_c_lib
./nob tsan -build-root /home/user/work/build/proven_c_lib
```

Run focused regression modes:

```sh
./nob regression -build-root /home/user/work/build/proven_c_lib
./nob regression-asan -build-root /home/user/work/build/proven_c_lib
./nob regression-ubsan -build-root /home/user/work/build/proven_c_lib
```

Run the float parse path benchmark:

```sh
./nob bench-float -build-root /home/user/work/build/proven_c_lib
```

Run freestanding checks:

```sh
./nob freestanding -build-root /home/user/work/build/proven_c_lib
```

Run cross compile-only coverage:

```sh
./nob cross -build-root /home/user/work/build/proven_c_lib
```

Missing optional cross compilers are skipped. A compiler that exists but cannot build the target probe is skipped with a warning. A real compile error in an available target fails the command.

Clean generated output:

```sh
./nob clean
```

## Log format

`nob.c` prints structured metadata before every hosted or freestanding test executable is linked and run. `./nob cross` uses the same shape for each available cross target, with `path=cross/<target-name>`. The build driver also prints build-level begin, environment, phase, source, test, summary, fail, and pass lines so platform setup problems on Windows/MSYS2 are visible instead of looking like a silent no-op.

Standard build-driver lines:

```text
[PROVEN][BUILD][BEGIN] mode=<mode> cc=<compiler> ld=<linker> build_root=<root> build_dir=<mode output directory>
[PROVEN][BUILD][ENV] runtime=<runtime label> platform=<posix-or-windows>
[PROVEN][BUILD][PHASE] library compilation start source_count=<n>
[PROVEN][BUILD][SOURCE][REBUILD] path=<source file>
[PROVEN][BUILD][SOURCE][CACHED] path=<source file>
[PROVEN][BUILD][PHASE] test link-and-run start test_count=<n>
[PROVEN][BUILD][TEST][REBUILD] path=<test executable>
[PROVEN][BUILD][TEST][CACHED] path=<test executable>
[PROVEN][BUILD][TEST][RUN] path=<test executable>
[PROVEN][BUILD][SUMMARY] mode=<mode> rebuilt_sources=<n> cached_sources=<n> rebuilt_tests=<n> cached_tests=<n>
[PROVEN][BUILD][NOTE] <Windows/MSYS2 note when applicable>
[PROVEN][BUILD][PASS] mode=<mode> build_dir=<mode output directory>
```

Standard build-run lines:

```text
[PROVEN][TEST][BEGIN] path=<test executable path> title=<short title>
[PROVEN][TEST][INTENT] <what this executable validates>
[PROVEN][TEST][FAIL_HINT] <where to start if this executable fails>
[PROVEN][TEST][PASS] path=<test executable path>
```

Standard failure lines:

```text
[PROVEN][TEST][FAIL] path=<test executable path> stage=<link|install|run>
[PROVEN][TEST][FAIL_HINT] <stage-specific or test-specific debugging hint>
```

Standard assertion lines printed by `tests/proven_test.h`:

```text
[PROVEN][CHECK][FAIL] file=<source file> line=<line>
[PROVEN][CHECK][COND] <failed C condition>
[PROVEN][CHECK][INTENT] <why this check exists>
[PROVEN][CHECK][FAIL_HINT] <what to inspect first>
```

Standard informational and pass lines printed by test executables:

```text
[PROVEN][TEST][INFO] <message>
[PROVEN][TEST][PASS] <message>
[PROVEN][SECTION][BEGIN] name=<sub-test name>
[PROVEN][SECTION][INTENT] <sub-test intent>
[PROVEN][SECTION][FAIL_HINT] <sub-test failure hint>
```

The older test files still use many direct `PROVEN_TEST_INFO` calls. Those messages now share the `[PROVEN][TEST][INFO]` prefix. New or substantially edited tests should prefer `PROVEN_TEST_SECTION(name, intent, hint)` for each logically separate sub-check group.

The float parse path benchmark uses the same test harness and emits its timing rows through `[PROVEN][TEST][INFO]` so the captured output can be saved directly as a dated markdown report. Those reports live in maintainer-local `docs/internal/` (kept outside the published repository), so a reader of this repository will not find them here.

## Test modes

### `build`

Intent: compile the hosted library in debug mode and run the complete hosted runtime suite.

What it checks:

- All hosted source files compile together with the public headers.
- All hosted test executables link against the same object set.
- The full set of runtime behavior checks succeeds without sanitizer instrumentation.

Failure tip: start from the first `[PROVEN][TEST][FAIL]` line. If the stage is `link`, inspect the immediately preceding compiler or linker diagnostic. If the stage is `run`, inspect the test-specific failure hint and the failing assertion.

### `release`

Intent: compile the hosted library with optimization and run the complete hosted runtime suite.

What it checks:

- Optimized builds do not rely on debug-only initialization or timing.
- Undefined behavior that is hidden in debug mode is less likely to survive optimization.
- Public headers and implementation still agree under `-O3`.

Failure tip: compare with `./nob build`. A failure only in release mode often means undefined behavior, invalid aliasing, stale borrowed views after reallocation, or missing initialization.

### `strict`

Intent: run the hosted suite with extra compiler warnings enabled.

What it checks:

- The code remains warning-clean under the compiler's normal warning policy.
- Suspicious conversions, unused variables, and portability risks are visible before release.

Failure tip: warnings are not fatal in this mode, but they should still be treated as defects. Use the warning location and then rerun `strict-error` after fixing it.

### `strict-error`

Intent: make warnings fatal and run the hosted suite.

What it checks:

- The codebase is warning-clean enough to be used as a dependency in strict C projects.
- Header changes do not introduce warnings into tests that include the public API.

Failure tip: fix the first compiler warning as a source issue, not by suppressing it globally. If a warning is target-specific, isolate it behind the relevant PAL or feature guard.

### `asan`

Intent: run the hosted suite under AddressSanitizer.

What it checks:

- Heap use-after-free, double free, buffer overflow, stack overflow, and some leak paths.
- Allocator trait routing for heap, arena, pool, arrays, maps, and growable strings.

Failure tip: read the ASan stack trace first. For growable containers, suspect stale views or stale element pointers after reallocation. For arenas, suspect capacity arithmetic or alignment rounding.

### `ubsan`

Intent: run the hosted suite under UndefinedBehaviorSanitizer.

What it checks:

- Signed integer overflow, invalid shifts, misaligned access, invalid casts in instrumented code, and other UB classes supported by the compiler.
- C23 checked arithmetic wrappers are not bypassed in overflow-prone paths.

Failure tip: the failing expression is usually the bug. Do not paper over it with casts unless the conversion is explicitly proven and guarded.

### `tsan`

Intent: run the hosted suite under ThreadSanitizer.

What it checks:

- Data races in the job system and any hosted code that uses atomics or worker threads.
- Worker startup, queue submission, shutdown, and exactly-once job execution under instrumentation.

Failure tip: if TSAN reports a race, inspect `src/proven/job.c` and `platform/proven_sys_thread.c` first. Make the memory ordering and ownership contract explicit before changing code.

### `regression`, `regression-asan`, `regression-ubsan`

Intent: run only focused historical regressions, optionally with sanitizers.

What it checks:

- Previously fixed bugs stay fixed.
- Source-contract checks remain synchronized with portability expectations.
- Bug-fix verification is faster than a complete suite run.

Failure tip: do not delete a regression because it feels narrow. It exists because the same class of bug already happened. Read the corresponding section below and preserve the contract.

### `bench-float`

Intent: run the float parse path benchmark executable only and write a dated report. The reports live in maintainer-local `docs/internal/` (kept outside the published repository).

What it checks:

- Each path-specific corpus still matches host `strtod` before any timing starts.
- The shared ASCII parser, the `proven_strtod()` wrapper, and host `strtod` can be timed on the same path-oriented corpora.
- The benchmark output stays suitable for archival in a dated docs file.

Failure tip: if a path corpus fails, inspect `src/proven/float_parse.c` and `src/proven/float_decimal.c` first. If the timing rows look implausible, check the compiler mode, the corpus split, and any host-specific CPU throttling before changing the parser.

### `freestanding`

Intent: compile and run the reduced `PROVEN_FREESTANDING` configuration.

What it checks:

- Hosted-only modules are excluded.
- `PROVEN_FMT_NO_FLOAT` and `PROVEN_NO_U16STR` builds still work.
- Core allocator-backed containers, formatting without floats, scanning, and algorithms work without OS-backed services.

Failure tip: any dependency on filesystem, mmap, sysio, environment, time, threads, or hosted heap is suspicious in this mode. Keep the freestanding subset small and explicit.

### `cross`

Intent: compile the library and smoke tests for every available target compiler.

What it checks:

- Public headers are portable across hosted Linux, Windows MinGW, and freestanding embedded targets that exist on the build server.
- Missing optional compilers are skipped; real compile failures fail the run.

Failure tip: identify the target name in the log, then check whether the failure is from compiler availability, sysroot usability, or actual source incompatibility. Cross compilation does not replace runtime testing on the target.

## Test catalog


The hosted full run currently builds and executes 75 tests. `./nob regression` runs an 11-test subset, `./nob freestanding` a 5-test subset, and `./nob bench-float` 2 benchmarks.

These counts are checked against `nob.c` - `tests/test_docs_alias_completeness` exists precisely because a list nobody checks stops being true.

Tests are named `test_<class>_<subject>`, and the name is the identifier - there are no numbers.
Numbers rot: this catalog used to run 1..50 with `7a`, `30a`, `30b`, `30c`, `40a` wedged in wherever something new arrived, and five of its entries described files that had been deleted months earlier.

The class says what kind of question the test answers:

- **`unit`** — One module's public API, used the way a caller uses it. These are the tests that say what the library *does*. (31 tests)
- **`contract`** — The public invariants: misuse, corrupted structs, exhausted allocators, refused input. These say what the library *refuses to do*, which is the half a caller cannot infer from the happy path. (8 tests)
- **`regression`** — One test per defect that actually shipped. Each is named for what broke, not for a version or a number, and each was verified to FAIL against the pre-fix source. A regression test that passes before the fix is not a regression test. (6 tests)
- **`differential`** — Correctness against an independent oracle - the host libc, or a corpus with known-good answers. These catch what a self-written expectation cannot: a wrong belief held consistently by both the code and its test. (0 tests)
- **`portability`** — Freestanding builds, compile-only cross targets, source-level platform contracts, and the build driver's own standard probe. Most of these cannot be *run* on the host, so they check what can be checked: that the code compiles, links, and keeps its platform branches intact. (1 tests)
- **`stress`** — Concurrency under a sanitizer, over enough iterations to make a race likely rather than theoretical. (0 tests)
- **`docs`** — The documentation is checked by the build, not by eye: every public function has an alias, every example the manual prints is a program that compiles and runs, and no example drifts from its chapter. (2 tests)
- **`bench`** — Timing, not correctness. A benchmark regression is a signal to investigate; a checksum drift inside one is a correctness failure and does fail the build. (0 tests)

## Unit tests

One module's public API, used the way a caller uses it. These are the tests that say what the library *does*.

### `tests/test_unit_algorithm` — algorithms

Intent: verify generic sort and binary search helpers using both scalar and struct comparators.

Sub-checks:

- Sorts an integer array and verifies ascending order.
- Binary-searches for an existing and a missing integer.
- Sorts structs using a comparator that sorts by score descending and ID ascending.
- Verifies comparator tie-breaking order.

Failure tip: inspect `src/proven/algorithm.c`. Comparator return convention must stay consistent: callers expect negative, zero, and positive values to drive ordering.

### `tests/test_unit_arena` — arena allocator

Intent: verify bump-allocation behavior, alignment, exhaustion, reset, realloc, and zero-copy semantics for the arena allocator.

Sub-checks:

- Initializes an arena over a fixed backing buffer.
- Allocates default-aligned and explicitly 32-byte-aligned blocks.
- Confirms out-of-memory requests fail instead of overwriting the backing buffer.
- Confirms reset releases the whole arena lifetime at once.
- Exercises reallocation and verifies zero-copy or migration semantics according to arena constraints.

Failure tip: inspect `src/proven/arena.c`, especially offset rounding, overflow checks, and reset behavior. Under ASan, any failure is likely a true bounds or lifetime bug.

### `tests/test_unit_array` — growable array

Intent: verify generic array allocation, validation, push/pop, growth, migration, element access, and arena-backed use.

Sub-checks:

- Initializes an array with a typed element size and initial capacity.
- Checks validation catches corrupted length/capacity state.
- Pushes typed values through macros.
- Forces growth and verifies data migrated correctly.
- Pops values and checks boundary rejection on empty pop.
- Checks invalid get/set ranges.
- Creates an arena-backed array to ensure allocator independence.

Failure tip: inspect `src/proven/array.c`. Growth failures usually mean element-size multiplication, capacity doubling, or realloc failure-atomic behavior changed. Remember that pointers into array storage are invalid after growth.

### `tests/test_unit_buffer_u8str_basics` — buffer and U8 string basics

Intent: verify fixed-capacity buffers, U8 string views, literal construction, append behavior, C-string conversion, and bounds defense.

Sub-checks:

- Creates a buffer through an allocator and appends data.
- Verifies `PROVEN_LIT` computes literal sizes without including the NUL terminator.
- Confirms buffer append rejects out-of-bounds writes.
- Creates a U8 string and appends multiple fragments.
- Converts a slice to a heap-owned C string.
- Confirms C-string termination, equality helpers, and integer overflow guards.

Failure tip: inspect `src/proven/buffer.c` and `src/proven/u8str.c`. Off-by-one capacity mistakes usually show up here first, especially around the extra NUL byte for C-string compatibility.

### `tests/test_unit_coro` — stackless coroutine

Intent: verify coroutine macros preserve caller-owned state across yields and complete after multiple resume calls.

Sub-checks:

- Defines a simulated network fetcher that yields across multiple phases.
- Repeatedly resumes the coroutine from a main loop.
- Confirms final payload value is set after completion.
- Confirms the main loop observed multiple ticks rather than one blocking call.

Failure tip: inspect `include/proven/coro.h`. Coroutine state must live in caller-owned storage and must not be reset between resumes.

### `tests/test_unit_error_results` — error and result primitives

Intent: verify the explicit error/result style has stable semantics and no hidden control flow.

Sub-checks:

- Confirms `PROVEN_OK` is accepted as success.
- Confirms representative failures such as `PROVEN_ERR_NOMEM` are rejected as success.
- Builds a successful memory result and verifies both the error and value fields.
- Builds a failed memory result and verifies the error and null value fields.

Failure tip: inspect `include/proven/error.h` and generated/result typedefs. Do not change enum values or result layouts without updating every call site, alias, manual, and test that relies on them.

### `tests/test_unit_float_bigint_divmod` — float big-integer division

Intent: verify the big-integer divide/modulo used by the exact float fallback.

Failure tip: inspect `src/proven/float_decimal.c`; a wrong quotient or remainder here silently corrupts the exact arbiter that decides ties.

### `tests/test_unit_float_shortest_format_roundtrip` — shortest formatter round-trip

Intent: verify values formatted by the shortest formatter parse back to the identical bits.

Note: this test existed on disk but was never registered in `nob.c`, so it had never run. It is registered now.

Failure tip: inspect `src/proven/float_format.c` and the Grisu3/Dragon4 shortest-digit engines if a value fails to round-trip.

### `tests/test_unit_fmt_f64_accuracy` — float formatter accuracy

Intent: verify fixed-point rounding, scientific carry, and special-value text for floating-point formatting.

Sub-checks:

- Checks normal-path rounding to six fractional digits.
- Checks carry from the fractional tail into the integer part.
- Checks scientific notation carry around the mantissa boundary.
- Checks NaN and infinity text stay stable.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_unit_fmt_f64_accuracy.c`.

### `tests/test_unit_fmt_fastpath` — formatter truncation comparison

Intent: compare truncating fixed-capacity formatting against the growable reference path for exact-fit, truncation, malformed format, and excess-argument cases.

Sub-checks:

- Checks exact-fit truncation output matches the reference path.
- Checks over-capacity truncation keeps the same prefix bytes and counts.
- Checks excess-argument validation.
- Checks malformed-format validation.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_unit_fmt_fastpath.c`.

### `tests/test_unit_foundation` — foundation primitives

Intent: verify the core error and checked-arithmetic assumptions used by all higher-level modules.

Sub-checks:

- Confirms `PROVEN_IS_OK` and `proven_is_ok` classify success and failure correctly.
- Confirms checked add detects overflow and preserves the wrapped C result where the C23 checked-arithmetic API says it should.
- Confirms checked subtract detects underflow.
- Confirms checked multiply detects overflow and succeeds for safe products.
- Confirms simple result structs can carry both an error and a value.

Failure tip: inspect `include/proven/error.h` and the `PROVEN_CKD_*` definitions in `include/proven/types.h`. If this test fails, avoid debugging later modules until the foundation behavior is fixed.

### `tests/test_unit_fs_advanced` — advanced filesystem

Intent: verify directory lifecycle, nested file creation, rename/move, listing, sorting expectations, and cleanup.

Sub-checks:

- Removes stale test directories from earlier failed runs.
- Creates a directory.
- Creates multiple files inside it.
- Renames/moves one file.
- Lists directory entries into a library array.
- Confirms expected files are present.
- Releases listed strings and removes test files/directories.

Failure tip: inspect `platform/proven_sys_fs.c` for directory iteration and path handling. On failure, check whether cleanup from a previous run left permissions or stale entries behind.

### `tests/test_unit_fs_basic` — basic filesystem

Intent: verify hosted file open, write, read-all, size queries, and absolute-path classification.

Sub-checks:

- Opens a temporary file for create/write/truncate.
- Writes known content and checks byte count.
- Reads the whole file and checks size and byte equality.
- Reopens the file and queries its size.
- Verifies absolute path classification for POSIX, drive-letter Windows paths, UNC paths, and extended Windows paths.

Failure tip: inspect `src/proven/fs.c` and `platform/proven_sys_fs.c`. If only Windows path cases fail, check path-prefix parsing rather than POSIX filesystem behavior.

### `tests/test_unit_fs_metadata_perms` — filesystem metadata and permissions

Intent: verify hosted permission and locking-related filesystem behavior stays explicit and isolated behind the PAL.

Sub-checks:

- Creates a temporary file.
- Changes permissions to read-only and back when the platform supports it.
- Opens files for write where needed.
- Acquires and releases advisory locks where supported.

Failure tip: inspect `platform/proven_sys_fs.c`. Permission and locking semantics are OS-dependent; keep differences in PAL code and avoid assuming POSIX behavior on every target.

### `tests/test_unit_job` — job system

Intent: verify the hosted worker-thread job system executes submitted jobs exactly once and shuts down cleanly.

Sub-checks:

- Initializes a job system with four workers and a 1024-entry queue.
- Dispatches 1000 jobs.
- Uses atomics to count total executed jobs.
- Uses indexed atomics to detect duplicate or missing job execution.
- Shuts down workers and flushes synchronization barriers.

Failure tip: inspect `src/proven/job.c` and `platform/proven_sys_thread.c`. For races, run `./nob tsan`. Check admission state, sequence counters, queue claim/commit ordering, and shutdown wakeups.

### `tests/test_unit_list` — intrusive list

Intent: verify zero-allocation intrusive list behavior and container-of usage.

Sub-checks:

- Initializes an empty sentinel list.
- Appends embedded nodes from caller-owned structs.
- Iterates in reverse and sums payload values.
- Removes nodes while iterating.
- Reads first and last entries through container-of style access.

Failure tip: inspect `include/proven/list.h`. Intrusive lists do not own node storage. A failure usually means `next`/`prev` linkage was corrupted or a detached node was reused incorrectly.

### `tests/test_unit_map` — hash map

Intent: verify open-addressing map behavior for integer and U8 string keys, including tombstones, growth, and scratch allocation.

Sub-checks:

- Creates an integer-key map and confirms capacity normalization.
- Inserts, retrieves, updates, and deletes entries.
- Confirms deletion reduces live length and leaves tombstones usable.
- Creates a U8 string-key map and inserts enough entries to force growth.
- Verifies all expected string keys remain reachable after rehash.
- Tracks scratch allocation during safe rehash paths.

Failure tip: inspect `src/proven/map.c`. Check hash/equality callbacks, tombstone reuse, threshold calculation, and whether borrowed keys or value pointers are being used after rehash.

### `tests/test_unit_map_owned_key` — map owned-key storage

Intent: verify owned U8 keys are duplicated into map storage, survive source-buffer mutation, and free their copied bytes on remove and destroy.

Sub-checks:

- Creates a U8 owned-key map.
- Inserts a key from mutable source storage and confirms the lookup survives source-buffer mutation.
- Removes the entry and confirms the copied key bytes are released once.
- Inserts enough owned keys to force rehash and confirms every copied key still resolves after growth.
- Destroys the map and confirms all owned key allocations have matching frees.

Failure tip: inspect the owned-key duplication, cleanup, and rehash migration paths in `src/proven/map.c` if a key is lost, leaks, or follows a mutated source buffer.

### `tests/test_unit_memory_slicing` — memory slicing

Intent: verify owned memory can be exposed as immutable and mutable views and sliced without losing pointer or length identity.

Sub-checks:

- Creates raw byte storage and wraps it in the owned memory abstraction.
- Converts owned memory to a read-only view and checks pointer, size, and byte contents.
- Converts owned memory to a mutable view and checks that writes through the mutable view are visible through the original buffer and read-only view.
- Slices read-only and mutable views and checks offset, length, and shared backing storage.

Failure tip: inspect `src/proven/memory.c` and `include/proven/memory.h`. Most failures here are offset arithmetic mistakes, accidental copies instead of views, or unchecked slice preconditions used with the wrong ranges.

### `tests/test_unit_memory_views` — memory byte views

Intent: verify the fixed-width integer aliases, semantic pointer/offset types, alignment helpers, and the first memory slice/view contracts.

Sub-checks:

- Confirms `proven_u8`, `proven_i8`, `proven_u16`, `proven_i16`, `proven_u32`, `proven_i32`, `proven_u64`, and `proven_i64` have the expected byte widths.
- Confirms semantic pointer/size/offset types are usable for memory calculations.
- Exercises default alignment logic.
- Builds a memory core structure and confirms pointer and size fields remain exact.

Failure tip: start in `include/proven/types.h`, `include/proven/align.h`, and `include/proven/memory.h`. A width failure usually means a typedef or platform feature branch changed. An alignment failure usually means the helper no longer implements power-of-two alignment correctly.

### `tests/test_unit_mmap` — memory mapped files

Intent: verify hosted memory mapping rejects invalid flags and exposes file bytes through mapped memory.

Sub-checks:

- Creates a test file with known content.
- Rejects invalid mmap flag combinations: zero flags, private plus shared, zero protection, unknown protection bits, and misaligned offsets.
- Maps a file range.
- Verifies mapped bytes match expected content.
- Modifies mapped memory and syncs/unmaps it.
- Reads the file back to verify the modification reached disk when mapping mode requires it.

Failure tip: inspect `src/proven/mmap.c` and `platform/proven_sys_fs.c`. Pay special attention to offset alignment, map length, file handle lifetime, and unmap ownership.

### `tests/test_unit_pool` — pool allocator

Intent: verify the fixed-size pool allocator enforces item-size constraints and recycles freed blocks through a bounded LIFO bin.

Sub-checks:

- Initializes a pool for `proven_u64` sized blocks.
- Confirms an allocation request with the wrong size is rejected.
- Allocates several blocks through the pool and checks fallback allocation when the bin is empty.
- Frees blocks and checks `bin_len` growth up to capacity.
- Confirms freeing beyond bin capacity falls back to the underlying allocator.
- Reallocates and verifies LIFO pointer reuse.
- Tears down the pool without leaking bin storage.

Failure tip: inspect `src/proven/pool.c`. Wrong-size requests should not be silently accepted. Bin overflow must never lose ownership of the block being freed.

### `tests/test_unit_ring` — bounded ring

Intent: verify fixed-capacity FIFO semantics, wraparound, full/empty detection, and overflow guards.

Sub-checks:

- Creates a ring and verifies initial head, tail, length, and capacity.
- Pushes and pops values in FIFO order.
- Fills the ring and verifies extra push is rejected.
- Pops across physical wraparound boundaries.
- Verifies empty pop rejection.
- Checks integer-overflow bounds for capacity calculations.

Failure tip: inspect `src/proven/ring.c`. The first suspects are head/tail modulo math, `len` updates, and full-vs-empty boundary handling.

### `tests/test_unit_scan` — scanner

Intent: verify scanner parsing for integers, floats, tokens, skip-until operations, format scanning, and fixed-width integer destinations.

Sub-checks:

- Scans unsigned and signed integers.
- Scans positive, negative, and exponent-style floating-point values.
- Scans tokens and string views.
- Skips until substrings and numbers.
- Confirms not-found behavior.
- Scans using `{}` and spec-style format patterns.
- Scans native and fixed-width integer aliases.

Failure tip: inspect `src/proven/scan.c`. The most common bugs are cursor advancement on failure, overflow detection, and accepting invalid trailing characters.

### `tests/test_unit_scan_f64_accuracy` — float scanner accuracy

Intent: verify float scanning preserves exact small values, signed zero, a round-trip style decimal token, exponent extremes, and cursor restoration on malformed input.

Sub-checks:

- Confirms exact bit patterns for `0.0`, `-0.0`, `1.0`, `-1.0`, `0.5`, `0.1`, and `123456789.0`.
- Confirms the parsed bits for `0.30000000000000004` match the source literal.
- Confirms `1.7976931348623157e308`, `2.2250738585072014e-308`, and `4.9e-324` remain finite and stable.
- Confirms `1e309` reports `PROVEN_ERR_OVERFLOW`.
- Confirms malformed input restores the scanner cursor to its original position.
- `tests/test_unit_scan_f64_bounds` covers underflow-to-signed-zero spellings, the true-min half threshold, subnormal-boundary spellings around DBL_MIN, and overflow boundary behavior at the same parser boundary.

Failure tip: inspect `src/proven/scan.c`, especially the decimal mantissa accumulation, exponent scaling, and final finite-value check. If a malformed token leaves the cursor advanced, inspect the failure-atomic rollback path first.

### `tests/test_unit_sysio_env` — sysio and environment

Intent: verify standard stream access, formatter-backed console output, environment lookup, missing-variable errors, and long environment-key handling.

Sub-checks:

- Uses proven sysio/formatter APIs without including `<stdio.h>` directly in the test.
- Prints structured output to prove standard stream wrappers are usable.
- Reads a likely existing environment variable such as `PATH`.
- Confirms a fake environment variable reports failure.
- Creates and reads an environment variable whose key is larger than the old fixed stack limit.

Failure tip: inspect `src/proven/sysio.c` and `platform/proven_sys_env.c`. Long-key failures usually mean a fixed-size C-string conversion path returned. Windows failures may involve UTF-8 to UTF-16 conversion and allocator ownership.

### `tests/test_unit_sysio_scanner` — sysio-backed scanner

Intent: verify scanner behavior over file-backed sysio data instead of only in-memory string views.

Sub-checks:

- Creates a temporary file and writes integer/token content.
- Opens the file for reading.
- Initializes a sysio scanner with an allocator-backed buffer.
- Scans two integers across the file stream and confirms EOF after the final token.
- Verifies `tests/test_unit_sysio_scanner_boundary` resumes across a chunk boundary, refills as needed, and only reports EOF after the final token is consumed.
- Cleans up scanner and file resources.

Failure tip: inspect `src/proven/sysio.c`, `src/proven/scan.c`, and file read wrappers. If in-memory scan tests pass but this fails, suspect buffer refill or file-position behavior, especially at the current-buffer boundary.

### `tests/test_unit_sysio_scanner_init` — sysio scanner init allocator validation

Intent: verify buffered scanner initialization rejects partial allocators and leaves the scanner zero-safe on failure.

Sub-checks:

- Passes an allocator that only exposes `alloc_fn` and expects `PROVEN_ERR_INVALID_ARG`.
- Confirms the partial allocator is never called.
- Confirms a rejected initialization leaves the scanner fields cleared.
- In hosted builds, confirms a valid heap allocator still initializes and deinitializes the scanner normally.

Failure tip: inspect `proven_sysio_scanner_init` in `src/proven/sysio.c`. If a partial allocator is accepted, the full allocator trait check is missing; if the scanner keeps non-zero state after failure, the failure path is not zero-safe.

### `tests/test_unit_time_fmt` — time and formatting integration

Intent: verify time measurement, sleep duration, modern format syntax, datetime formatting, and escaped braces.

Sub-checks:

- Reads monotonic or high-resolution time before and after a short sleep.
- Confirms elapsed nanoseconds are at least approximately the requested sleep.
- Formats positional and automatic arguments.
- Converts the Unix epoch to a datetime and checks year/month.
- Formats a datetime value through `PROVEN_ARG`.
- Verifies `{{` and `}}` produce literal braces.

Failure tip: inspect `platform/proven_sys_time.c` for clock conversion and `src/proven/fmt.c` for datetime formatting. Timing failures can be caused by a broken clock source or by assuming exact scheduling latency.

### `tests/test_unit_u16str` — U16 strings

Intent: verify optional UTF-16/code-unit string support and its append policies.

Sub-checks:

- Creates and destroys a U16 string.
- Appends code units into fixed capacity.
- Confirms atomic append failure leaves content and length unchanged.
- Confirms partial append writes the count that fits and reports out-of-bounds.
- Confirms growable append reallocates and completes the write.

Failure tip: inspect `src/proven/u16str.c` and `include/proven/u16str.h`. Treat U16 values as UTF-16 code units, not Unicode scalar values. Check `PROVEN_NO_U16STR` guards if the failure is compile-time.

### `tests/test_unit_u8str_mutation` — U8 string mutation

Intent: verify U8 string search, slicing, replacement, insertion, removal, and the three append policies: atomic fixed-capacity, partial fixed-capacity, and growable.

Sub-checks:

- Finds substrings from different offsets and reports `PROVEN_INDEX_NOT_FOUND` for missing needles.
- Checks starts-with, ends-with, and slice equality.
- Performs same-length, shrinking, and growing `replace_at` operations.
- Inserts and removes byte ranges.
- Replaces the first matching substring.
- Confirms fixed-capacity append fails atomically when full.
- Confirms partial append writes as many bytes as possible and reports the written count.
- Confirms growable append reallocates and completes the operation.

Failure tip: inspect `src/proven/u8str.c`. For failures after a reallocation path, assume saved views or C-string pointers are stale unless proven otherwise. For fixed-capacity failures, check whether the operation is documented as atomic or partial.

### `tests/test_unit_float_bits` — float bit extraction

Intent: verify the internal float bit helpers preserve raw IEEE-754 bit patterns for f32 and f64 values, including signed zero, infinities, and NaN payloads.

Failure tip: inspect src/proven/float_decimal.c if the raw byte-copy helpers stop matching the object representation.

### `tests/test_unit_float_exact_range` — float exact-range backend

Intent: verify representative exact-range decimal spellings keep their documented bit patterns without the host strtod fallback.

Failure tip: inspect src/proven/scan.c and the shared float decimal helper if the exact-range backend falls back to host strtod or the corpus drifts.

### `tests/test_unit_float_f32_boundaries` — float32 boundary neighbors

Intent: verify the float32 upgrade and shortest corpora pin the ULP-adjacent neighbors around FLT_MIN and FLT_TRUE_MIN so the parser-driven backend keeps the documented boundary spellings.

Failure tip: inspect tests/test_differential_float_corpus_f64.c and tests/test_unit_float_shortest_roundtrip.c if a float32 boundary-neighbor corpus value disappears or changes spelling.

### `tests/test_unit_float_format_policy` — float format policy scaffold

Intent: verify the new float format policy seam preserves the current simple formatter behavior, rejects unsupported shortest-mode requests, and reports invalid inputs clearly.

Failure tip: inspect src/proven/float_format.c and include/proven/float_format.h if the policy dispatch or fixed formatter helper regresses.

### `tests/test_unit_float_parse_api` — float parse API

Intent: verify the public ASCII float parser and strtod-like wrapper expose consumed-length, endptr, and range signaling over the shared exact backend.

Failure tip: inspect include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if the public parser seam or wrapper contract drifts.

### `tests/test_unit_float_rfc_0001_cases` — RFC-0001 parse audit

Intent: verify the decimal-to-binary64 rewrite still satisfies the explicit named cases from docs/proposals/rfc-0001.

Failure tip: inspect docs/proposals/rfc-0001, include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if a named RFC audit case fails.

### `tests/test_unit_float_shortest_known` — float shortest known values

Intent: verify the shortest float formatting policy emits the documented exact spellings for representative f64 and f32 values.

Failure tip: inspect src/proven/float_format.c if the shortest-policy output drifts or if RYU requests stop reaching the active backend.

### `tests/test_unit_float_shortest_roundtrip` — float shortest round-trip

Intent: verify shortest float formatting round-trips through host strtod for representative f64 and f32 values.

Failure tip: inspect src/proven/float_format.c if the shortest output stops round-tripping, and keep the host strtod oracle limited to tests.

### `tests/test_unit_float_shortest_scientific_guard` — float shortest scientific guard

Intent: verify the shortest float formatter handles very small finite values by producing a valid shortest candidate instead of an invalid scientific normalization result.

Failure tip: inspect src/proven/float_decimal.c and src/proven/float_format.c if the shortest formatter rejects a tiny finite value or emits an invalid scientific spelling.

### `tests/test_unit_float_shortest_tie_break` — float shortest tie-break corpus

Intent: verify the shortest corpus keeps the 0.001 fixed-versus-scientific tie-break cases pinned for both widths.

Failure tip: inspect tests/test_unit_float_shortest_roundtrip.c and tests/test_differential_float_corpus_f64.c if the tie-break corpus disappears or is renamed.

### `tests/test_unit_mem_copy` — bounded memory copy

Intent: verify proven_mem_copy copies within capacity, rejects overflow without writing, treats a zero-size source as a no-op, and rejects null pointers.

Failure tip: inspect proven_mem_copy in src/proven/memory.c if a copy overflows, writes on rejection, or mishandles empty/null inputs.

### `tests/test_unit_scan_f64_bounds` — float scanner boundary behavior

Intent: verify float scanning treats underflow as signed zero, reports overflow deterministically, and preserves cursor rollback at the true boundary cases.

Failure tip: inspect proven_scan_f64 exponent-to-value handling and final finite checks if a boundary token returns the wrong error or wrong sign.

### `tests/test_unit_sysio_scanner_boundary` — sysio scanner boundary refill

Intent: verify buffered sysio scanning resumes across a chunk boundary, refills as needed, and only reports EOF after the final token is consumed.

Failure tip: inspect proven_sysio_scanner_scan_impl staging, refill handling, and EOF transition behavior when a token reaches the end of the buffer.

### `tests/test_unit_u128_mul` — wide multiply helper

Intent: verify the shared 64x64 to 128-bit multiply helper returns exact high and low halves for representative operands.

Failure tip: inspect src/proven/float_decimal.c if the wide multiply helper stops matching the reference product.

### `tests/test_unit_u8str_borrow` — U8 string borrow (fixed-capacity over caller memory)

Intent: verify proven_u8str_borrow/_reset: fixed-capacity ops and fmt work, growing ops refuse to reallocate caller memory, and destroy is a no-op.

Failure tip: inspect proven_u8str_borrow/_reset and the borrowed-flag guards in reserve/append_grow/replace_at_grow/destroy.

## Contract and hardening tests

The public invariants: misuse, corrupted structs, exhausted allocators, refused input. These say what the library *refuses to do*, which is the half a caller cannot infer from the happy path.

### `tests/test_contract_allocator_dealloc` — allocator deallocation policies

Intent: document and verify the different deallocation policies exposed through the allocator trait.

Sub-checks:

- Allocates from an arena through the generic allocator trait.
- Calls the arena free function and verifies it is intentionally a no-op.
- Resets the arena as the correct lifetime-ending operation.
- Allocates from the heap allocator and frees through the heap trait.

Failure tip: inspect `src/proven/arena.c`, `src/proven/heap.c`, and the allocator trait definition. Do not make arena `free` reclaim individual blocks; that would break the arena lifetime model.

### `tests/test_contract_arena_panic` — arena panic path

Intent: verify panic-on-allocation-failure behavior is deterministic and does not fire on successful arena allocation.

Sub-checks:

- Installs a test panic handler with `proven_set_panic_handler`.
- Allocates successfully with `alloc_or_panic` and confirms no panic occurred.
- Requests more memory than the arena can provide.
- Confirms the panic hook was invoked exactly for the out-of-memory path.

Failure tip: inspect `src/proven/arena.c` and `src/proven/panic.c`. Restore the panic hook carefully in tests so later tests are not affected.

### `tests/test_contract_fmt_failure_policy` — formatter failure policy

Intent: verify formatting append policies are explicit: fixed-capacity atomic, fixed-capacity truncating, and allocator-backed growable.

Sub-checks:

- Appends formatted output with growable allocation.
- Populates a small fixed string.
- Confirms fixed-capacity formatting reports out-of-bounds and leaves the string unchanged.
- Confirms truncating formatting writes the partial count and reports required size.
- Confirms content after truncation matches the expected prefix.
- Checks extremely large padding specs for safe overflow handling.

Failure tip: inspect `src/proven/fmt.c`. Track `written`, `required`, and destination length separately. Atomic failure must not modify the destination.

### `tests/test_contract_map_hardening` — map borrowed-key hardening

Intent: verify borrowed U8 keys that point into internal map storage are rejected when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Inserts a normal external borrowed key and confirms it still works.
- Constructs a borrowed view that points into the map's own internal storage.
- Expects `PROVEN_ERR_INVALID_ARG` for that internal-storage key when the validation gate is active.

Failure tip: inspect the borrowed-key range guard in `src/proven/map.c` if an internal pointer is accepted or if ordinary borrowed keys stop working.

### `tests/test_contract_pool_misuse` — pool double-free hardening

Intent: verify the pool free trait catches repeated frees when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Installs a test panic handler.
- Allocates one fixed-size block through the pool allocator trait.
- Frees the block once successfully.
- Frees the same block again and expects the validation path to reach the panic handler when hardening or debug validation is active.

Failure tip: inspect `src/proven/pool.c`. The repeated-free check must remain gated on debug validation or `PROVEN_HARDENED`, and the test should only require the panic path when that gate is active.

### `tests/test_contract_scan_f64_overflow` — float scanner overflow

Intent: verify a very large floating-point token reports `PROVEN_ERR_OVERFLOW` instead of silently accepting infinity.

Sub-checks:

- Builds an input with roughly 1000 decimal digits.
- Scans it as `f64`.
- Confirms the error is `PROVEN_ERR_OVERFLOW`.

Failure tip: inspect `proven_scan_f64` in `src/proven/scan.c` and math helper behavior in the PAL. Do not accept `inf` as a successful parsed finite value.

### `tests/test_contract_sysio_scan_nonseekable` — non-seekable sysio rejection

Intent: verify one-chunk file scanning rejects pipe/stdin-like inputs before consuming data.

Sub-checks:

- Checks the helper returns `PROVEN_ERR_UNSUPPORTED` for a non-seekable handle.
- Checks the scan destination is left unchanged on the early rejection path.
- Checks the original pipe payload is still readable after the rejected scan attempt.

Failure tip: inspect `src/proven/sysio.c` and make sure the one-chunk scan path probes seekability before reading.

### `tests/test_contract_sysio_scan_truncation` — chunked sysio scan truncation

Intent: verify one-chunk file scanning rejects inputs that exceed the fixed buffer and leaves the stream reusable after a failed attempt.

Sub-checks:

- Checks a chunk-full string token reports the bounds error used by the one-chunk scan path.
- Checks the file cursor is still usable after the failure.
- Checks the trailing integer is not consumed by the failed scan.

Failure tip: inspect `src/proven/sysio.c` and `tests/test_contract_sysio_scan_truncation.c`.

### `tests/test_contract_float_module_layout` — float module scaffold

Intent: verify the shared float helpers live in a dedicated internal translation unit instead of being copied into fmt.c and scan.c.

Failure tip: inspect src/proven/float_decimal.c, src/proven/float_decimal.h, fmt.c, scan.c, and nob.c if the shared decimal helper scaffold regresses.

### `tests/test_contract_public_structs` — public array/map/filesystem contracts

Intent: verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.

Failure tip: inspect public invariant guards in array/map mutation entry points and the filesystem open-flag translation if a corrupt struct or append request slips through.

## Regression tests

One test per defect that actually shipped. Each is named for what broke, not for a version or a number, and each was verified to FAIL against the pre-fix source. A regression test that passes before the fix is not a regression test.

### `tests/test_regression_fmt_spec_silently_wrong` — formatter specs that used to be silently wrong

Intent: verify `{:08}` zero-pads instead of eating the `0` as a width digit, and that a spec the argument cannot honour (hex on a double or a string) is rejected rather than ignored.

Note: both defects failed the worst way available — silently. `{:08}` on 42 produced `"      42"` and returned OK; `{:x}` on a double printed `3.500000` and returned OK. A spelling that is accepted and quietly does the wrong thing is worse than one that is rejected.

Failure tip: inspect the spec parser and the applicability guard in `src/proven/fmt.c`.

### `tests/test_regression_fs_copy_to_self` — filesystem self-copy regression

Intent: verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the file.

Sub-checks:

- Creates a source file with known content.
- Attempts to copy the file to the same path and expects `PROVEN_ERR_INVALID_ARG`.
- Reads the file back and verifies size and contents are unchanged.
- Creates a hard link to the same file when supported.
- Attempts to copy across the hard-linked paths and expects failure without corruption.

Failure tip: inspect same-file detection and open/truncate ordering in filesystem copy code. The destination must not be opened with truncation before proving it is not the same file as the source.

### `tests/test_regression_fs_slurp` — filesystem whole-file read/write

Intent: verify whole-file reads go to EOF rather than to a pre-measured size, and that the whole-file write entry points round-trip.

Sub-checks:

- Round-trips a regular file through `proven_fs_write_file` and `proven_fs_read_all`.
- Verifies `proven_fs_write_file` truncates a longer existing file.
- Verifies an empty file reads as `{NULL, 0}` with `PROVEN_OK`.
- Reads a source whose size cannot be known up front (`/proc/self/status`, whose `st_size` is 0) and requires a non-empty result. Skipped where the path is unavailable.
- Verifies `proven_fs_read_all_u8str` is NUL-terminated and valid, including for an empty file.
- Verifies `proven_fs_write_file_atomic` replaces the contents, creates a missing target, and leaves no temp file behind.
- Verifies an invalid allocator and a missing path are rejected as values.

Failure tip: `proven_fs_size` reports 0 for anything that is not a regular file, so the reported size may only seed the read capacity - never bound the read. Inspect `internal_slurp_path` and `internal_read_to_eof` in `src/proven/fs.c`.

### `tests/test_regression_scanner_rollback` — scanner rollback after a failed scan

Intent: verify a scan that fails on an oversized token restores the stream exactly - dropping no byte and duplicating none.

Sub-checks:

- Scans a token successfully, then snapshots the bytes the scanner holds unconsumed.
- Scans a token too large for the scanner buffer and requires failure.
- Requires the unconsumed bytes after the failure to match the snapshot in both count and content.
- Requires a retry of the oversized token to fail the same way, not succeed from stale bytes.
- Scans the same file with a buffer large enough and requires every token to come back in order.

Failure tip: `scanner_fill` compacts the buffer (it memmoves unconsumed bytes to the front and resets the cursor). A snapshot taken before compaction cannot be written back afterwards without accounting for how far the contents moved. Inspect the rollback in `proven_sysio_scanner_scan_impl`.

### `tests/test_regression_sort_duplicates` — sort on duplicate keys

Intent: verify `proven_array_sort` stays sub-quadratic on duplicate and degenerate input.

Sub-checks:

- 20,000 all-equal keys: comparison count must stay below 40n. A two-way partition that sends equal elements to one side needs ~2x10^8 comparisons here.
- 20,000 keys drawn from 8 distinct values: comparison count below 60n.
- Sorted, reverse-sorted, and organ-pipe orderings: comparison count below 60n each - the classic quicksort killers, bounded by median-of-three plus the heapsort depth fallback.
- 48-byte elements with a payload tied to the key, to catch a torn bulk swap.

Note: this suite counts comparisons, not wall-clock time. A timing threshold is a flaky test on a shared machine; the comparison count is exactly what blew up.

Failure tip: inspect the partition in `src/proven/algorithm.c`. Equal elements must be collected into a run that is final and never recursed into.

### `tests/test_regression_v26_05` — v26.05 regressions

Intent: protect historically fixed issues in map rehashing, formatting, scanning, aliasing, and environment handling.

Sub-checks:

- Map self-payload rehash: inserting a value pointer that points inside the map must not corrupt the new value during rehash.
- Map existing-key update before rehash: updating an existing key must not incorrectly grow or lose the value.
- Map large-value rehash allocation tracking: rehash must allocate/free scratch exactly as expected for large values.
- Formatter self `STR_VIEW` grow: formatting from a view into the destination must handle aliasing safely.
- Formatter self `CSTR` grow invalid-arg: unsafe self C-string aliasing should be rejected.
- Formatter huge argument index: very large explicit indexes must fail safely.
- Formatter many args without alias: large argument arrays must not overflow stack or internal accounting.
- Formatter many args with alias scratch: alias-safe scratch paths must allocate and release scratch correctly.
- Scanner invalid cursor: invalid scan state must not be treated as success.
- Buffer append overlap: `test_unit_buffer_u8str_basics` checks that `proven_buf_append()` preserves overlapping source views with move semantics instead of corrupting the appended bytes.
- Array/string self-alias grow: grow operations must not corrupt when source and destination overlap in documented ways.
- `PROVEN_ARG_CSTR_N` safety bounds: C-string-with-length arguments must respect the caller-supplied bound.
- Environment large value: environment values larger than a small stack buffer must be read through dynamic allocation.

Failure tip: this file is intentionally a set of historical tripwires. Do not collapse it into broad smoke coverage. Read the failing sub-check name printed in the log and inspect the corresponding source module.

### `tests/test_regression_v26_07` — v26.07 regressions

Intent: protect the fixed `u8str` NUL-seal, datetime formatting, and pool init defects.

Sub-checks:

- `proven_u8str_reserve` on a zero-initialized string leaves `ptr[len] == 0`, so `proven_u8str_as_cstr` is readable and `proven_u8str_is_valid` accepts it.
- A format that produces no output still leaves the string sealed after its growth allocation.
- A `proven_datetime_t` with year `-1` renders as `-0001-...`, and `INT32_MIN` renders correctly.
- A `proven_pool_init` whose bin allocation fails leaves `bin_cap == 0`, so the free trait cannot write through a null bin.

Note: the string checks allocate from an arena over deliberately poisoned backing memory. Allocators do not return zeroed memory, and on a quiet heap a fresh block is often zero by luck - which is exactly why these defects went unnoticed.

Failure tip: each section names one area - `proven_u8str_reserve` in `u8str.c`, the growth branch and `PROVEN_ARG_DATETIME` case in `fmt.c`, the init ordering in `pool.c`.

## Portability tests

Freestanding builds, compile-only cross targets, source-level platform contracts, and the build driver's own standard probe. Most of these cannot be *run* on the host, so they check what can be checked: that the code compiles, links, and keeps its platform branches intact.

### `tests/test_portability_source_contracts` — source portability contracts

Intent: guard platform branches and documentation/test-output contracts that may not be executable on the current host.

Sub-checks:

- Checks Windows directory open allocation guards before `FindFirstFileW` use.
- Checks Windows `FILETIME` to Unix time conversion uses the correct epoch delta and underflow guard.
- Checks POSIX mmap stores offsets in `off_t` and rejects truncation.
- Checks Windows append mode uses `FILE_APPEND_DATA` rather than a one-time seek-to-end emulation.
- Checks Windows environment key conversion sizes the wide buffer dynamically rather than using a fixed 255-byte stack buffer.
- Checks public environment lookup no longer rejects large keys through a fixed C key buffer.
- Checks 32-bit Linux `_llseek` uses a 64-bit result buffer.
- Checks the job system keeps a single admission state and explicit begin/end submit helpers.
- Checks `nob.c` keeps structured test metadata and emits standard begin, intent, failure-hint, failure, and pass log lines.
- Checks this `TEST.md` documents failure tips, sub-checks, and the log format.

Failure tip: source-contract tests should stay narrow. If a source pattern changes legitimately, update the contract to the new safe pattern in the same commit as the source change and explain it in docs.

### `tests/test_portability_cross_compile_smoke` — cross compile smoke

### `tests/test_portability_cross_link_smoke` — cross link smoke

### `tests/test_portability_float` — float portability

Intent: verify scan and format float conversion paths stay double-only and keep target-deterministic behavior without long double dependence.

Failure tip: inspect src/proven/scan.c and src/proven/fmt.c if long double returns, casts, or target-specific float drift reappear.

### `tests/test_portability_nob_std_probe` — build driver standard probe

Intent: verify nob probes -std=c23 first and falls back to -std=c2x when the compiler rejects c23.

Failure tip: inspect nob.c standard-flag selection and toolchain probing if the fallback does not trigger.

## Documentation tests

The documentation is checked by the build, not by eye: every public function has an alias, every example the manual prints is a program that compiles and runs, and no example drifts from its chapter.

### `tests/test_docs_alias_completeness` — alias layer completeness

Intent: verify every public `proven_*` function has an `xcv_*` alias in `include/proven/alias_xcv.h`.

Sub-checks:

- Parses every header in `include/proven/` (excluding the alias header itself) for public function declarations.
- Requires each one to appear as the target of an `xcv_*` alias; names any that do not.

Note: `tests/test_docs_alias_smoke` only checks that a hand-picked subset of aliases compiles. It cannot notice a *missing* alias, which is how 25 public functions ended up with none. This test closes that gap: adding a public function without an alias now fails the build.

Failure tip: add `#define xcv_<name> proven_<name>` to `include/proven/alias_xcv.h`, keeping the file alphabetical. A half-covered alias layer fails at the caller's call site, not here.

### `tests/test_docs_alias_smoke` — alias layer smoke

Intent: verify the public XCV alias layer compiles and maps representative aliases to canonical proven APIs.

Sub-checks:

- Uses native integer scan aliases.
- Uses scan function aliases.
- Uses formatting argument aliases.
- Uses macro aliases that are expected to stay available for the alias layer.

Failure tip: inspect `include/proven/alias_xcv.h` and `tests/test_docs_alias_smoke.c`. When public symbols are added, renamed, or removed, update the alias header and this smoke test together.

### `tests/test_docs_manual_ch08_contracts` — manual chapter 8 scanner contracts

Intent: verify every behaviour manual chapter 8 states as fact about the scanner is actually true — error codes, cursor restoration on failure, decimal-only integers (`0x10` is zero), the overflow/underflow asymmetry, and the non-transactional structural scan.

Note: prose is where a contract goes to drift. Chapter 8 makes 18 factual claims about the scanner; this test makes each one executable. A false claim fails the build and names itself.

Failure tip: find the named claim in `manual/manual-08-fmt-scan.md` and decide which side is wrong before changing either.

### `tests/test_docs_manual_examples` — manual examples match the manual

Intent: verify every example the manual prints exists in manual/examples/, matches it verbatim, and that no example file is left unquoted.

Failure tip: the example file is the source of truth: it is compiled and run. Copy its body into the chapter rather than hand-editing the chapter to look right.

## Differential tests

Correctness against an independent oracle - the host libc, or a corpus with known-good answers. These catch what a self-written expectation cannot: a wrong belief held consistently by both the code and its test.

### `tests/test_differential_float_corpus_f32` — float upgrade corpus float32 coverage

Intent: verify the upgrade corpus source also keeps the documented float32 shortest literals pinned alongside the existing float64 cases.

Failure tip: inspect tests/test_differential_float_corpus_f64.c if the float32 corpus section disappears or drifts from the documented literals.

### `tests/test_differential_float_corpus_f64` — float upgrade corpus

Intent: verify the representative exact-range, subnormal-boundary, and shortest-format corpus stays pinned to the documented spellings while the float upgrade remains staged.

Failure tip: inspect src/proven/scan.c and src/proven/float_format.c if a representative corpus value changes bit pattern or shortest spelling.

### `tests/test_differential_float_host_oracle_f32` — float host oracle float32

Intent: verify representative finite float32 fixed-format rendering matches the platform C library on the same inputs without sharing implementation code.

Failure tip: inspect src/proven/float_format.c if the float32 fixed formatter stops matching the host oracle corpus.

### `tests/test_differential_float_host_oracle_f64` — float host oracle

Intent: verify representative finite float parsing and simple fixed-format rendering match the platform C library on the same inputs without sharing implementation code.

Failure tip: inspect src/proven/scan.c and src/proven/float_format.c if the host oracle and library disagree on the representative finite corpus.


## Stress tests

Concurrency under a sanitizer, over enough iterations to make a race likely rather than theoretical.

### `tests/test_stress_job_concurrency` — job queue stress

Intent: verify the job queue tolerates a denser concurrent producer pattern and still executes each submitted job exactly once.

Failure tip: run this under TSAN first; inspect queue admission, claim, and shutdown ordering if a slot count drifts or a producer stalls.


## Regression subset

`./nob regression`, `./nob regression-asan`, and `./nob regression-ubsan` currently run:

- `tests/test_regression_v26_05`
- `tests/test_regression_v26_07`
- `tests/test_unit_map_owned_key`
- `tests/test_contract_public_structs`
- `tests/test_regression_fs_copy_to_self`
- `tests/test_regression_fs_slurp`
- `tests/test_regression_scanner_rollback`
- `tests/test_regression_sort_duplicates`
- `tests/test_portability_source_contracts`

Intent: provide a short feedback loop for bug-fix work without running every hosted example and container test.

What it checks:

- Historical behavioral bugs remain fixed.
- Filesystem destructive edge cases remain protected.
- Source-level portability contracts and test-log/documentation contracts remain present.

Failure tip: a regression failure is usually more specific than a full-suite failure. Use the sub-check name and preserve the regression unless the underlying public contract is intentionally changed and documented.

### 34. `tests/test_contract_public_structs` - public array/map/filesystem contracts

Intent: verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.

Sub-checks:

- Calls array reserve and push on an intentionally corrupted public struct and expects `PROVEN_ERR_INVALID_ARG`.
- Calls array destroy on a corrupted struct and checks that the visible fields are cleared after best-effort cleanup.
- Calls map reserve and set on an intentionally corrupted public struct and expects `PROVEN_ERR_INVALID_ARG`.
- Calls map destroy on a corrupted struct and checks that the visible fields are cleared after best-effort cleanup.
- Opens a file with append-plus-create, writes through the returned handle, and confirms POSIX append mode keeps write intent explicit.
- Confirms append plus truncation is rejected as `PROVEN_ERR_INVALID_ARG`.

Failure tip: inspect `src/proven/array.c`, `src/proven/map.c`, `src/proven/fs.c`, and `platform/proven_sys_fs.c`. If a corrupted struct reaches an allocator callback or append behaves like read-only open, the public contract guard is missing.

### 34a. `tests/test_contract_map_hardening` - map borrowed-key hardening

Intent: verify borrowed U8 keys that point into internal map storage are rejected when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Inserts a normal external borrowed key and confirms it still works.
- Constructs a borrowed view that points into the map's own internal storage.
- Expects `PROVEN_ERR_INVALID_ARG` for that internal-storage key when the validation gate is active.

Failure tip: inspect the borrowed-key range guard in `src/proven/map.c` if an internal pointer is accepted or if ordinary borrowed keys stop working.

### 34b. `tests/test_contract_pool_misuse` - pool double-free hardening

Intent: verify the pool free trait catches repeated frees when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Installs a test panic handler.
- Allocates one fixed-size block through the pool allocator trait.
- Frees the block once successfully.
- Frees the same block again and expects the validation path to reach the panic handler when hardening or debug validation is active.

Failure tip: inspect `src/proven/pool.c`. The repeated-free check must remain gated on debug validation or `PROVEN_HARDENED`, and the test should only require the panic path when that gate is active.

## Freestanding tests

`./nob freestanding` currently builds the library with `PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR`, and `-ffreestanding`, then runs five reduced tests.

### `tests/test_portability_freestanding_heap_stub`

Intent: verify the hosted heap allocator is not accidentally available in freestanding mode.

Sub-checks:

- Calls `proven_heap_allocator()` under `PROVEN_FREESTANDING`.
- Confirms the returned allocator is invalid because no default OS heap exists.
- Skips with an info line when compiled outside freestanding mode.

Failure tip: inspect `src/proven/heap.c` and platform heap guards. Freestanding code must not silently pull a hosted allocator.

### `tests/test_portability_compile_freestanding`

Intent: verify the reduced freestanding core can compile and link.

Sub-checks:

- Includes the public headers under freestanding flags.
- Links against the reduced object set selected by `nob.c`.

Failure tip: inspect `nob.c` source exclusion lists and public header feature guards. A hosted-only declaration may have leaked into freestanding builds.

### `tests/test_portability_compile_nofloat`

Intent: verify `PROVEN_FMT_NO_FLOAT` removes floating-point formatting dependencies without breaking the rest of formatting.

Sub-checks:

- Builds with no-float formatting enabled.
- Links the formatter without float-specific argument handling.

Failure tip: inspect `include/proven/fmt.h` and `src/proven/fmt.c` for unguarded `float`, `double`, or math-helper references.

### `tests/test_portability_compile_nou16str`

Intent: verify `PROVEN_NO_U16STR` removes optional U16 string support without breaking core headers and linking.

Sub-checks:

- Builds the umbrella header and reduced object set with U16 support disabled.
- Links successfully without `src/proven/u16str.c`.

Failure tip: inspect `include/proven.h`, `include/proven/u16str.h`, aliases, and the `nob.c` freestanding source list.

### `tests/test_portability_freestanding`

Intent: verify the actual freestanding core runtime behavior, not just compilation.

Sub-checks:

- Initializes allocator-backed arrays and sorts them.
- Binary-searches sorted array data.
- Exercises intrusive lists.
- Exercises bounded rings.
- Exercises hash maps.
- Exercises U8 strings and no-float formatting.
- Exercises scanner and scan-format behavior.
- Uses a deterministic panic hook for failure paths.

Failure tip: inspect only freestanding-safe modules first: memory, arena, pool when included, buffer, U8 string, array, ring, map, algorithm, scan, fmt without float, panic, and non-hosted math helpers. Any filesystem, mmap, sysio, environment, time, thread, or hosted heap dependency is a portability regression.

## Benchmarks

`./nob bench-float` runs:

- `tests/test_bench_float_parse_paths` - times the shared float parser, its wrapper, and the host `strtod` on path-oriented decimal corpora, and records dated output.
- `tests/test_bench_float_parse` - times the decimal parser against the host `strtod` on a mixed corpus. Like the round-trip test above, this file existed but was never registered; it is registered now.

Benchmarks are not correctness gates. A timing regression is a signal to investigate, not a build failure; a checksum drift is a correctness signal and must be.

### `tests/test_bench_float_parse_paths` — float parse path benchmark

Intent: compare the shared float parser, wrapper, and host strtod on separate path-oriented decimal corpora and record dated docs output.

Failure tip: inspect src/proven/float_parse.c, src/proven/float_decimal.c, and the path-specific corpus split if the timing harness fails or any checksum drifts.

### `tests/test_bench_float_parse` — float parse benchmark

Intent: time the decimal parser against the host strtod on a mixed corpus and record the result.

Failure tip: inspect src/proven/float_parse.c and src/proven/float_decimal.c if a timing run regresses or a checksum drifts.

## Cross compile-only matrix

The matrix compiles `tests/test_portability_cross_compile_smoke.c` (or `tests/test_portability_freestanding.c` for freestanding targets) and links it with `tests/test_portability_cross_link_smoke.c`, which exists to prove the objects actually link - a header-only compile check would miss a missing symbol.

`./nob cross` builds object files and smoke tests for available target compilers. The matrix currently includes:

- `native-gcc-hosted` through `gcc`
- `native-clang-hosted` through `clang`
- `linux-aarch64-hosted` through `aarch64-linux-gnu-gcc`
- `linux-armhf-hosted` through `arm-linux-gnueabihf-gcc`
- `linux-i686-hosted` through `i686-linux-gnu-gcc`
- `linux-i686-multilib-hosted` through `gcc -m32`
- `windows-x86_64-winapi` through `x86_64-w64-mingw32-gcc`
- `windows-i686-winapi` through `i686-w64-mingw32-gcc`
- `freestanding-arm-cortex-m4` through `arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb`
- `freestanding-riscv64-elf` through `riscv64-elf-gcc`
- `freestanding-riscv64-unknown-elf` through `riscv64-unknown-elf-gcc`

Hosted targets compile all hosted source files and `tests/test_portability_cross_compile_smoke.c`. Freestanding targets compile only freestanding-safe source files and `tests/test_portability_freestanding.c` as a smoke translation unit.

Failure tip: if the log says the compiler is missing, fix the build-server toolchain rather than the library. If the target probe fails, check sysroot, multilib headers, or target flags. If a later library source file fails, treat it as a real portability bug.

### 34. `tests/test_portability_nob_std_probe` - build driver standard probe

Intent: verify the build driver probes `-std=c23` first and falls back to `-std=c2x` when the compiler rejects the newer spelling.

Sub-checks:

- Creates a wrapper compiler that rejects `-std=c23`.
- Runs `./nob regression` with that wrapper as `-cc`.
- Confirms the build driver completes successfully after retrying with `-std=c2x`.
- Confirms the wrapper log records both the rejected c23 attempt and the accepted c2x attempt.

Failure tip: inspect nob.c standard-flag selection, build-hash construction, and the compiler/toolchain preflight checks if the fallback probe does not reach the c2x path.

### 35. `tests/test_portability_float` - float portability

Intent: verify scan and format float conversion paths stay double-only and keep target-deterministic behavior without long double dependence.

Sub-checks:

- Confirms `src/proven/scan.c` no longer contains `long double` in the decimal conversion path.
- Confirms `proven_scan_scale_pow10` and `proven_scan_convert_decimal` use double-only scaling.
- Confirms `src/proven/fmt.c` no longer contains `long double` in the float formatter path.
- Confirms `proven_fmt_normalize_scientific` uses double-only working values.
- Confirms representative formatting and scanning still produce the documented values for a carried scientific number and a round-trip-style decimal.

Failure tip: inspect `src/proven/scan.c` and `src/proven/fmt.c`. If the source-contract checks fail, the float portability cleanup regressed back to long double-dependent code. If the runtime checks fail, inspect the double-only conversion and normalization math first.

### 36. `tests/test_contract_float_module_layout` - float module scaffold

Intent: verify the shared decimal float helpers live in a dedicated internal translation unit and are not re-embedded inline inside `src/proven/fmt.c` or `src/proven/scan.c`, while the shortest-literal helper stays centralized with the shared float decimal code.

Sub-checks:

- Confirms `src/proven/float_decimal.h` declares the shared ASCII parser, decimal conversion, scientific normalization, and shortest-literal helpers.
- Confirms `src/proven/float_decimal.h` also declares the internal conversion metrics helpers used to distinguish fast paths from the exact fallback in tests.
- Confirms `src/proven/float_decimal.c` defines the shared ASCII parser, exact midpoint comparison, decimal conversion, scientific normalization, shortest-literal helpers, and internal path counters.
- Confirms `src/proven/float_decimal.c` includes the generated cached-power header and that `scripts/generate_float_decimal_tables.py` remains the documented source of that header.
- Confirms `src/proven/scan.c` and `src/proven/fmt.c` include `float_decimal.h` instead of defining the shared helper bodies inline, and that `src/proven/float_format.c` calls the shared shortest-literal helpers instead of keeping the literal tables inline.
- Confirms `nob.c` compiles `src/proven/float_decimal.c` as part of the library build.
- Confirms `THIRD_PARTY_NOTICES.md` records the clean-room status of the float parse rewrite.

Failure tip: inspect `src/proven/float_decimal.c`, `src/proven/float_decimal.h`, `src/proven/float_format.c`, `src/proven/scan.c`, and `nob.c` if the shared float helper scaffold drifts back into the scanner or formatter files.

### 37. `tests/test_unit_float_bits` - float bit extraction

Intent: verify the internal float bit helpers preserve the raw IEEE-754 bit patterns for `f32` and `f64` values.

Sub-checks:

- Confirms `proven_float_bits_f64()` preserves `+0.0`, `-0.0`, `1.0`, `+Inf`, and NaN payload bits.
- Confirms `proven_float_bits_f32()` preserves `+0.0f`, `-0.0f`, `1.0f`, `+Inf`, and NaN payload bits.
- Confirms the helpers return the raw object representation instead of normalizing or reinterpreting the values.

Failure tip: inspect `src/proven/float_decimal.c` and `src/proven/float_decimal.h` if the raw byte-copy helpers stop matching the object representation.

### 38. `tests/test_unit_u128_mul` - wide multiply helper

Intent: verify the shared 64x64 to 128-bit multiply helper returns exact high and low halves for representative operands.

Sub-checks:

- Confirms zero, one, power-of-two, and all-ones vectors produce the exact 128-bit product.
- Confirms a few asymmetric hand-computed vectors also match the reference product.
- Confirms the helper exposes the product in a stable high/low part structure for later float algorithms.

Failure tip: inspect `src/proven/float_decimal.c` if the wide multiply helper stops matching the reference product.

### 38a. `tests/test_unit_float_parse_api` - float parse public API

Intent: verify the public locale-free float parser and `strtod`-style wrapper expose consumed-length, `endptr`, and range signaling over the shared exact backend.

Sub-checks:

- Confirms `proven_parse_double_ascii()` accepts valid decimal and special-value tokens and reports the consumed byte count.
- Confirms `proven_parse_double_ascii()` does not skip leading whitespace and rejects malformed exponent tails.
- Confirms `proven_strtod()` skips leading ASCII whitespace and leaves `endptr` at the first byte after the parsed token.
- Confirms hosted overflow and underflow cases report `ERANGE` while preserving signed infinity and signed zero behavior.
- Confirms internal conversion counters distinguish a Clinger hit, staged Eisel-Lemire hits across positive-exponent, negative-exponent, and subnormal representative inputs, and exact bigint fallback hits for representative uncertain inputs.
- Confirms the documented representative staged inputs currently finish through the shared cached-power product plan, while uncertain negative cases defer straight to the exact fallback.

Failure tip: inspect `include/proven/float_parse.h`, `src/proven/float_parse.c`, and `src/proven/float_decimal.c` if the public parser seam or wrapper contract drifts.

### 38b. `tests/test_unit_float_rfc_0001_cases` - RFC-0001 parse audit

Intent: verify the decimal-to-binary64 rewrite still satisfies the explicit named cases from RFC-0001 (the RFC itself is maintainer-local `docs/internal/` (kept outside the published repository); the cases it names are reproduced in the test).

Sub-checks:

- Confirms the basic RFC finite corpus parses and matches the host oracle.
- Confirms `inf`, `infinity`, `nan`, signed infinity, and signed NaN payload spellings parse through the public ASCII seam.
- Confirms the `2^53` boundary corpus and a true-min midpoint below/exact/above triplet obey round-to-nearest, ties-to-even.
- Confirms DBL_MAX, DBL_MIN, the largest subnormal, the normal/subnormal boundary, and the smallest subnormal still parse to the expected binary64 values.
- Confirms a 110-digit significand plus huge overflow/underflow exponents scan safely.
- Confirms malformed/endptr RFC cases such as `123abc`, `.`, `e10`, `1e`, `1e+`, and leading whitespace through the wrapper keep the documented behavior.

Failure tip: inspect the RFC-0001 case list in the test itself, `include/proven/float_parse.h`, `src/proven/float_parse.c`, and `src/proven/float_decimal.c` if a named RFC audit case fails.

### 39. `tests/test_unit_float_format_policy` - float format policy scaffold

Intent: verify the new float format policy seam preserves the current simple formatter behavior and supports shortest-mode requests explicitly.

Sub-checks:

- Confirms the DEFAULT and SIMPLE policies match the current `PROVEN_ARG_F64` formatter output for representative finite values.
- Confirms NaN and infinity spellings stay aligned with the current formatter.
- Confirms `PROVEN_FLOAT_FORMAT_POLICY_RYU` with shortest mode returns a compact shortest-form output.
- Confirms invalid policy and invalid mode values return `PROVEN_ERR_INVALID_ARG`.
- Confirms too-small output buffers return `PROVEN_ERR_OUT_OF_BOUNDS`.
- Confirms the `f32` policy entry point follows the same shortest and fixed-precision behavior.

Failure tip: inspect `src/proven/float_format.c`, `include/proven/float_format.h`, and `include/proven/float_config.h` if the policy seam or fixed formatter helper regresses.

### 40. `tests/test_unit_float_shortest_known` - float shortest known values

Intent: verify the shortest float formatting policy emits the documented exact spellings for representative f64 and f32 values.

Sub-checks:

- Confirms representative finite f64 values format to the expected shortest strings.
- Confirms representative finite f32 values format to the expected shortest strings through the float32 policy shim.
- Confirms zero, signed zero, integer, power-of-ten, subnormal, and max-finite edge cases keep their documented spellings.

Failure tip: inspect `src/proven/float_format.c` if the shortest-policy output drifts or if RYU requests stop reaching the active backend.

### 41. `tests/test_unit_float_shortest_roundtrip` - float shortest round-trip

Intent: verify shortest float formatting round-trips through host strtod for representative f64 and f32 values, including the broader float32 fraction, power-of-two, signed-symmetry, and 0.001 / 0.0001 tie-break samples added during the staged float upgrade.

Sub-checks:
- Representative finite f64 values, including the largest binary64 subnormal shortest literal pair and the existing subnormal boundary cases
- Representative finite f32 values
- Fixed-versus-scientific tie-break cases around 0.001 for both widths

Failure tip: inspect `src/proven/float_format.c` if the shortest output stops round-tripping or if the representative corpus drifts.

### 41a. `tests/test_unit_float_shortest_tie_break` - float shortest tie-break corpus

Intent: verify the shortest round-trip corpus keeps the 0.001 and 0.0001 fixed-versus-scientific tie-break cases pinned for both f64 and f32 coverage, along with the next 0.01 and 0.00001 precision-band samples.

Sub-checks:
- `tests/test_unit_float_shortest_roundtrip.c` contains the f64 and f32 `0.001`, `0.0001`, `0.01`, and `0.00001` cases
- `tests/test_differential_float_corpus_f64.c` contains the matching upgrade corpus cases

Failure tip: inspect the shortest corpus tests if the tie-break cases disappear or are renamed.

### 42. `tests/test_stress_job_concurrency` - job queue stress

Intent: verify the hosted job queue tolerates a denser concurrent producer pattern and still executes each submitted job exactly once.

Sub-checks:

- Launches four producer threads against a small bounded queue.
- Submits 1024 jobs per producer while yielding between retries.
- Counts total executions with an atomic counter.
- Uses per-slot atomics to detect duplicate or missing execution.
- Closes and destroys the job system only after all producers have joined.

Failure tip: run `./nob tsan` if available and inspect `src/proven/job.c` plus `platform/proven_sys_thread.c` if counts drift or a producer stalls.

### 43. `tests/test_differential_float_host_oracle_f64` - float host oracle

Intent: compare representative finite float parsing and simple fixed-format rendering against the platform C library without sharing implementation code, including signed zero and negative boundary cases, plus the 0.001 / 0.0001 fixed-versus-scientific boundary samples.

Sub-checks:

- Parses a representative finite decimal corpus with host `strtod` and with `proven_scan_f64`.
- Compares parsed bit patterns for exact agreement on the finite corpus.
- Formats the same finite values with the default fixed formatter path.
- Chooses the same scientific-versus-fixed branch threshold as the library for the comparison corpus.
- Compares the library text against host `snprintf` on the same finite inputs.

Failure tip: inspect `src/proven/scan.c` and `src/proven/float_format.c` if the host oracle and library disagree on the representative finite corpus.

### 43a. `tests/test_differential_float_host_oracle_f32` - float host oracle float32

Intent: compare representative finite float32 fixed-format rendering against the platform C library without sharing implementation code, including the 0.001 / 0.0001 boundary samples.

Sub-checks:

- Formats representative finite float32 values with the default fixed formatter path.
- Chooses the same scientific-versus-fixed branch threshold as the library for the comparison corpus.
- Compares the library text against host `snprintf` on the same finite float32 inputs.
- Confirms the written count matches the host oracle string length.

Failure tip: inspect `src/proven/float_format.c` if the float32 host oracle and library disagree on the representative finite corpus.

### 44. `tests/test_differential_float_corpus_f64` - float upgrade corpus

Intent: pin representative exact-range, subnormal-boundary, off-range, and shortest-format float spellings while the long-term parser and formatter upgrade stays staged.

Sub-checks:

- Parses representative boundary spellings around the exact integer range, the binary64 unit-in-the-last-place boundary, the true-min half threshold, the high-end overflow boundary, and the subnormal edge, including negative counterparts where the same exact bits are expected.
- Compares representative scanned values against the host `strtod` bit pattern for the same text.
- Representative exact-range and subnormal-boundary doubles with the shortest policy, including the matching negative special cases and the largest binary64 subnormal shortest pair.
- Confirms the expected shortest spellings for `DBL_MIN`, `DBL_MAX`, `DBL_TRUE_MIN`, and their negative counterparts, plus nearby exact values.
- Confirms the shortest corpus also keeps representative float32 samples pinned for `0.0001f`, `0.2f`, `0.29999998f`, `1.0000002f`, `2.5f`, and `33554432.0f`.

Failure tip: inspect `src/proven/scan.c` and `src/proven/float_format.c` if a representative corpus value changes bit pattern or shortest spelling.

### 44a. `tests/test_differential_float_corpus_f32` - float upgrade corpus float32 coverage

Intent: keep the documented float32 shortest literals pinned in the upgrade corpus alongside the existing float64 coverage.

Sub-checks:

- The upgrade-corpus source keeps a dedicated float32 shortest section.
- The documented float32 shortest literals for `FLT_MIN`, `FLT_TRUE_MIN`, and `FLT_MAX` remain present with their signed counterparts.
- The float32 short-literal coverage also pins representative fraction and power-of-two samples such as `0.2f`, `0.29999998f`, `-0.2f`, `-0.29999998f`, `1.0000002f`, `-1.0000002f`, `2.5f`, `-2.5f`, `33554432.0f`, and `-33554432.0f`.
- The float32 short-literal coverage stays aligned with `float_decimal.c` and the float32 shortest-policy path.

Failure tip: inspect `tests/test_differential_float_corpus_f64.c` first. If the source contract fails, restore the missing float32 section before changing the formatter.

### 44b. `tests/test_unit_float_f32_boundaries` - float32 boundary neighbors

Intent: keep the float32 ULP-adjacent neighbors around `FLT_MIN` and `FLT_TRUE_MIN` pinned in both the upgrade corpus and the shortest round-trip corpus so the parser-driven backend preserves the documented boundary spellings.

Sub-checks:

- Confirms `tests/test_differential_float_corpus_f64.c` still records the float32 value one ULP below `FLT_MIN` and the value one ULP above `FLT_TRUE_MIN` with their documented shortest spellings.
- Confirms `tests/test_unit_float_shortest_roundtrip.c` still round-trips those same float32 boundary neighbors through the scanner.
- Keeps the two values distinct from the already-pinned `FLT_MIN` and `FLT_TRUE_MIN` cases.

Failure tip: inspect `tests/test_differential_float_corpus_f64.c` and `tests/test_unit_float_shortest_roundtrip.c` if one of the boundary-neighbor spellings disappears, changes, or stops round-tripping.

### 45. `tests/test_unit_float_exact_range` - float exact-range backend

Intent: verify that the decimal-to-double path stays deterministic without the host strtod fallback and preserves representative exact-range spellings.

Sub-checks:
- Confirms representative exact-range, subnormal-boundary, true-min half-threshold, and high-end boundary spellings still parse to the documented bits.
- Confirms `src/proven/scan.c` no longer calls host `strtod` for decimal conversion.
- Confirms `src/proven/float_format.c` keeps the shortest-format helper routed through the shared direct-dispatch helper path.

Failure tip: inspect `src/proven/scan.c` and the shared float decimal helper if the exact-range backend falls back to host strtod or the corpus drifts.

### 49. `tests/test_unit_float_shortest_scientific_guard` - float shortest scientific guard

Intent: verify the shortest float formatter handles very small finite values by producing a valid shortest candidate instead of an invalid scientific normalization result.

Sub-checks:
- Confirms representative tiny finite values on both signs still format successfully through the shortest policy.
- Confirms the formatted spelling round-trips through the library scanner.
- Confirms the formatted spelling matches the shortest candidate found by exhaustive fixed-precision search over the documented precision range.

Failure tip: inspect `src/proven/float_decimal.c` and `src/proven/float_format.c` if the shortest formatter rejects a tiny finite value, emits an invalid scientific spelling, or stops matching the shortest exhaustive candidate.

### 51. `tests/test_unit_u8str_borrow` - U8 string borrow (fixed-capacity over caller memory)

Intent: verify `proven_u8str_borrow` and `proven_u8str_reset` over caller-owned memory.

Sub-checks:

- Borrow sets the borrowed flag and starts empty and valid.
- Fixed-capacity append and `append_fmt` work; an over-capacity append fails atomically.
- `append_byte` fills to capacity then returns `PROVEN_ERR_OUT_OF_BOUNDS`.
- A growing call that would reallocate (`append_grow`, `reserve`) is rejected without touching caller memory; a within-capacity grow succeeds.
- `reset` empties the buffer for reuse.
- `destroy` on a borrow frees nothing and clears the handle.

Failure tip: inspect `proven_u8str_borrow`/`proven_u8str_reset` and the `borrowed`-flag guards in `reserve`, `append_grow`, `replace_at_grow`, and `destroy` in `src/proven/u8str.c`.

### 52. `tests/test_unit_mem_copy` - bounded memory copy & move

Intent: verify `proven_mem_copy` performs a bounded byte copy.

Sub-checks:

- Copies a source that fits, including the exact-capacity case.
- Rejects an overflowing source with `PROVEN_ERR_OUT_OF_BOUNDS` and writes nothing.
- Treats a zero-size source as a no-op and rejects a null destination.

Failure tip: inspect `proven_mem_copy` in `src/proven/memory.c`.

## Failure triage workflow

1. Find the first `[PROVEN][TEST][FAIL]` line. Later failures can be cascading noise.
2. Note the `stage` field.
- `link`: inspect the compiler/linker diagnostic immediately above the failure.
- `install`: inspect build-root permissions or stale locked output files.
- `run`: inspect the failing executable's assertion output.
3. Read the `[PROVEN][TEST][INTENT]` and `[PROVEN][TEST][FAIL_HINT]` lines printed before the executable ran.
4. If an assertion failed, read `[PROVEN][CHECK][COND]`, `[PROVEN][CHECK][INTENT]`, and `[PROVEN][CHECK][FAIL_HINT]`.
5. Re-run the narrowest affected command first. For a regression, use `./nob regression`. For a freestanding failure, use `./nob freestanding`. For job/thread failures, use `./nob tsan` when available.
6. After a fix, run at least `./nob build`, plus `strict-error`, sanitizer, freestanding, or cross modes appropriate to the touched area.

## Change policy

When behavior changes:

1. Add or update the narrowest test first.
2. Confirm the new test fails for the expected reason when practical.
3. Implement the change.
4. Make test output explain the intent and failure hint for the changed behavior.
5. Run the narrow test through `nob.c`.
6. Run `./nob build`.
7. Run sanitizer, freestanding, or cross modes when the changed area requires them.
8. Update this file if the test matrix, sub-checks, or failure guidance changes.

## Release validation

Recommended release gate:

```sh
./nob clean
./nob strict-error -build-root /home/user/work/build/proven_c_lib
./nob regression-asan -build-root /home/user/work/build/proven_c_lib
./nob regression-ubsan -build-root /home/user/work/build/proven_c_lib
./nob freestanding -build-root /home/user/work/build/proven_c_lib
./nob cross -build-root /home/user/work/build/proven_c_lib
```

Optional compiler-specific gate:

```sh
./nob strict-error -cc clang -build-root /home/user/work/build/proven_c_lib
```
