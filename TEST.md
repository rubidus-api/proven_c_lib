# proven Test Matrix (v26.05.16)

This document describes how the `proven` test suite is organized, what each test is intended to prove, what each test checks internally, and where to start when a failure occurs. Tests are plain C executables built and run by `nob.c`. No external test framework is required.

## Table of contents

- [Running tests](#running-tests)
- [Log format](#log-format)
- [Test modes](#test-modes)
- [Hosted test executables](#hosted-test-executables)
- [Regression subset](#regression-subset)
- [Freestanding tests](#freestanding-tests)
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
./nob build -build-root /mnt/ai-share/build/proven_c_lib
```

Run the warnings-as-errors gate:

```sh
./nob strict-error -build-root /mnt/ai-share/build/proven_c_lib
```

Run sanitizer modes:

```sh
./nob asan -build-root /mnt/ai-share/build/proven_c_lib
./nob ubsan -build-root /mnt/ai-share/build/proven_c_lib
./nob tsan -build-root /mnt/ai-share/build/proven_c_lib
```

Run focused regression modes:

```sh
./nob regression -build-root /mnt/ai-share/build/proven_c_lib
./nob regression-asan -build-root /mnt/ai-share/build/proven_c_lib
./nob regression-ubsan -build-root /mnt/ai-share/build/proven_c_lib
```

Run freestanding checks:

```sh
./nob freestanding -build-root /mnt/ai-share/build/proven_c_lib
```

Run cross compile-only coverage:

```sh
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Missing optional cross compilers are skipped. A compiler that exists but cannot build the target probe is skipped with a warning. A real compile error in an available target fails the command.

Clean generated output:

```sh
./nob clean
```

## Log format

`nob.c` prints structured metadata before every hosted or freestanding test executable is linked and run. `./nob cross` uses the same shape for each available cross target, with `path=cross/<target-name>`. This makes long logs useful even when the failing executable exits before printing its own details.

Standard build-run lines:

```text
[PROVEN][TEST][BEGIN] path=<test executable path> title=<short title>
[PROVEN][TEST][INTENT] <what this executable proves>
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

## Hosted test executables

The hosted full run currently builds and executes 32 tests.

### 1. `tests/test_phase1` - memory byte views

Intent: verify the fixed-width integer aliases, semantic pointer/offset types, alignment helpers, and the first memory slice/view contracts.

Sub-checks:

- Confirms `proven_u8`, `proven_i8`, `proven_u16`, `proven_i16`, `proven_u32`, `proven_i32`, `proven_u64`, and `proven_i64` have the expected byte widths.
- Confirms semantic pointer/size/offset types are usable for memory calculations.
- Exercises default alignment logic.
- Builds a memory core structure and confirms pointer and size fields remain exact.

Failure tip: start in `include/proven/types.h`, `include/proven/align.h`, and `include/proven/memory.h`. A width failure usually means a typedef or platform feature branch changed. An alignment failure usually means the helper no longer implements power-of-two alignment correctly.

### 2. `tests/test_foundation` - foundation primitives

Intent: verify the core error and checked-arithmetic assumptions used by all higher-level modules.

Sub-checks:

- Confirms `PROVEN_IS_OK` and `proven_is_ok` classify success and failure correctly.
- Confirms checked add detects overflow and preserves the wrapped C result where the C23 checked-arithmetic API says it should.
- Confirms checked subtract detects underflow.
- Confirms checked multiply detects overflow and succeeds for safe products.
- Confirms simple result structs can carry both an error and a value.

Failure tip: inspect `include/proven/error.h` and the `PROVEN_CKD_*` definitions in `include/proven/types.h`. If this test fails, avoid debugging later modules until the foundation behavior is fixed.

### 3. `tests/test_phase2` - memory slicing

Intent: verify owned memory can be exposed as immutable and mutable views and sliced without losing pointer or length identity.

Sub-checks:

- Creates raw byte storage and wraps it in the owned memory abstraction.
- Converts owned memory to a read-only view and checks pointer, size, and byte contents.
- Converts owned memory to a mutable view and checks that writes through the mutable view are visible through the original buffer and read-only view.
- Slices read-only and mutable views and checks offset, length, and shared backing storage.

Failure tip: inspect `src/proven/memory.c` and `include/proven/memory.h`. Most failures here are offset arithmetic mistakes, accidental copies instead of views, or unchecked slice preconditions used with the wrong ranges.

### 4. `tests/test_phase3` - error and result primitives

Intent: verify the explicit error/result style has stable semantics and no hidden control flow.

Sub-checks:

- Confirms `PROVEN_OK` is accepted as success.
- Confirms representative failures such as `PROVEN_ERR_NOMEM` are rejected as success.
- Builds a successful memory result and verifies both the error and value fields.
- Builds a failed memory result and verifies the error and null value fields.

Failure tip: inspect `include/proven/error.h` and generated/result typedefs. Do not change enum values or result layouts without updating every call site, alias, manual, and test that relies on them.

### 5. `tests/test_phase4` - arena allocator

Intent: verify bump-allocation behavior, alignment, exhaustion, reset, realloc, and zero-copy semantics for the arena allocator.

Sub-checks:

- Initializes an arena over a fixed backing buffer.
- Allocates default-aligned and explicitly 32-byte-aligned blocks.
- Confirms out-of-memory requests fail instead of overwriting the backing buffer.
- Confirms reset releases the whole arena lifetime at once.
- Exercises reallocation and verifies zero-copy or migration semantics according to arena constraints.

Failure tip: inspect `src/proven/arena.c`, especially offset rounding, overflow checks, and reset behavior. Under ASan, any failure is likely a true bounds or lifetime bug.

### 6. `tests/test_phase5` - buffer and U8 string basics

Intent: verify fixed-capacity buffers, U8 string views, literal construction, append behavior, C-string conversion, and bounds defense.

Sub-checks:

- Creates a buffer through an allocator and appends data.
- Verifies `PROVEN_LIT` computes literal sizes without including the NUL terminator.
- Confirms buffer append rejects out-of-bounds writes.
- Creates a U8 string and appends multiple fragments.
- Converts a slice to a heap-owned C string.
- Confirms C-string termination, equality helpers, and integer overflow guards.

Failure tip: inspect `src/proven/buffer.c` and `src/proven/u8str.c`. Off-by-one capacity mistakes usually show up here first, especially around the extra NUL byte for C-string compatibility.

### 7. `tests/test_phase6_pool` - pool allocator

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

### 8. `tests/test_dealloc` - allocator deallocation policies

Intent: document and verify the different deallocation policies exposed through the allocator trait.

Sub-checks:

- Allocates from an arena through the generic allocator trait.
- Calls the arena free function and verifies it is intentionally a no-op.
- Resets the arena as the correct lifetime-ending operation.
- Allocates from the heap allocator and frees through the heap trait.

Failure tip: inspect `src/proven/arena.c`, `src/proven/heap.c`, and the allocator trait definition. Do not make arena `free` reclaim individual blocks; that would break the arena lifetime model.

### 9. `tests/test_phase7_u8str_mut` - U8 string mutation

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

### 10. `tests/test_phase8_array` - growable array

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

### 11. `tests/test_phase9_list` - intrusive list

Intent: verify zero-allocation intrusive list behavior and container-of usage.

Sub-checks:

- Initializes an empty sentinel list.
- Appends embedded nodes from caller-owned structs.
- Iterates in reverse and sums payload values.
- Removes nodes while iterating.
- Reads first and last entries through container-of style access.

Failure tip: inspect `include/proven/list.h`. Intrusive lists do not own node storage. A failure usually means `next`/`prev` linkage was corrupted or a detached node was reused incorrectly.

### 12. `tests/test_phase10_ring` - bounded ring

Intent: verify fixed-capacity FIFO semantics, wraparound, full/empty detection, and overflow guards.

Sub-checks:

- Creates a ring and verifies initial head, tail, length, and capacity.
- Pushes and pops values in FIFO order.
- Fills the ring and verifies extra push is rejected.
- Pops across physical wraparound boundaries.
- Verifies empty pop rejection.
- Checks integer-overflow bounds for capacity calculations.

Failure tip: inspect `src/proven/ring.c`. The first suspects are head/tail modulo math, `len` updates, and full-vs-empty boundary handling.

### 13. `tests/test_phase11_map` - hash map

Intent: verify open-addressing map behavior for integer and U8 string keys, including tombstones, growth, and scratch allocation.

Sub-checks:

- Creates an integer-key map and confirms capacity normalization.
- Inserts, retrieves, updates, and deletes entries.
- Confirms deletion reduces live length and leaves tombstones usable.
- Creates a U8 string-key map and inserts enough entries to force growth.
- Verifies all expected string keys remain reachable after rehash.
- Tracks scratch allocation during safe rehash paths.

Failure tip: inspect `src/proven/map.c`. Check hash/equality callbacks, tombstone reuse, threshold calculation, and whether borrowed keys or value pointers are being used after rehash.

### 14. `tests/test_phase12_algorithm` - algorithms

Intent: verify generic sort and binary search helpers using both scalar and struct comparators.

Sub-checks:

- Sorts an integer array and verifies ascending order.
- Binary-searches for an existing and a missing integer.
- Sorts structs using a comparator that sorts by score descending and ID ascending.
- Verifies comparator tie-breaking order.

Failure tip: inspect `src/proven/algorithm.c`. Comparator return convention must stay consistent: callers expect negative, zero, and positive values to drive ordering.

### 15. `tests/test_phase13_fs` - basic filesystem

Intent: verify hosted file open, write, read-all, size queries, and absolute-path classification.

Sub-checks:

- Opens a temporary file for create/write/truncate.
- Writes known content and checks byte count.
- Reads the whole file and checks size and byte equality.
- Reopens the file and queries its size.
- Verifies absolute path classification for POSIX, drive-letter Windows paths, UNC paths, and extended Windows paths.

Failure tip: inspect `src/proven/fs.c` and `platform/proven_sys_fs.c`. If only Windows path cases fail, check path-prefix parsing rather than POSIX filesystem behavior.

### 16. `tests/test_phase14_fs_advanced` - advanced filesystem

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

### 17. `tests/test_phase15_fs_security` - filesystem metadata and permissions

Intent: verify hosted permission and locking-related filesystem behavior stays explicit and isolated behind the PAL.

Sub-checks:

- Creates a temporary file.
- Changes permissions to read-only and back when the platform supports it.
- Opens files for write where needed.
- Acquires and releases advisory locks where supported.

Failure tip: inspect `platform/proven_sys_fs.c`. Permission and locking semantics are OS-dependent; keep differences in PAL code and avoid assuming POSIX behavior on every target.

### 18. `tests/test_phase16_time_fmt` - time and formatting integration

Intent: verify time measurement, sleep duration, modern format syntax, datetime formatting, and escaped braces.

Sub-checks:

- Reads monotonic or high-resolution time before and after a short sleep.
- Confirms elapsed nanoseconds are at least approximately the requested sleep.
- Formats positional and automatic arguments.
- Converts the Unix epoch to a datetime and checks year/month.
- Formats a datetime value through `PROVEN_ARG`.
- Verifies `{{` and `}}` produce literal braces.

Failure tip: inspect `platform/proven_sys_time.c` for clock conversion and `src/proven/fmt.c` for datetime formatting. Timing failures can be caused by a broken clock source or by assuming exact scheduling latency.

### 19. `tests/test_phase17_mmap` - memory mapped files

Intent: verify hosted memory mapping rejects invalid flags and exposes file bytes through mapped memory.

Sub-checks:

- Creates a test file with known content.
- Rejects invalid mmap flag combinations: zero flags, private plus shared, zero protection, and unknown protection bits.
- Maps a file range.
- Verifies mapped bytes match expected content.
- Modifies mapped memory and syncs/unmaps it.
- Reads the file back to verify the modification reached disk when mapping mode requires it.

Failure tip: inspect `src/proven/mmap.c` and `platform/proven_sys_fs.c`. Pay special attention to offset alignment, map length, file handle lifetime, and unmap ownership.

### 20. `tests/test_phase17_u16str` - U16 strings

Intent: verify optional UTF-16/code-unit string support and its append policies.

Sub-checks:

- Creates and destroys a U16 string.
- Appends code units into fixed capacity.
- Confirms atomic append failure leaves content and length unchanged.
- Confirms partial append writes the count that fits and reports out-of-bounds.
- Confirms growable append reallocates and completes the write.

Failure tip: inspect `src/proven/u16str.c` and `include/proven/u16str.h`. Treat U16 values as UTF-16 code units, not Unicode scalar values. Check `PROVEN_NO_U16STR` guards if the failure is compile-time.

### 21. `tests/test_phase18_sysio` - sysio and environment

Intent: verify standard stream access, formatter-backed console output, environment lookup, missing-variable errors, and long environment-key handling.

Sub-checks:

- Uses proven sysio/formatter APIs without including `<stdio.h>` directly in the test.
- Prints structured output to prove standard stream wrappers are usable.
- Reads a likely existing environment variable such as `PATH`.
- Confirms a fake environment variable reports failure.
- Creates and reads an environment variable whose key is larger than the old fixed stack limit.

Failure tip: inspect `src/proven/sysio.c` and `platform/proven_sys_env.c`. Long-key failures usually mean a fixed-size C-string conversion path returned. Windows failures may involve UTF-8 to UTF-16 conversion and allocator ownership.

### 22. `tests/test_phase19_coro` - stackless coroutine

Intent: verify coroutine macros preserve caller-owned state across yields and complete after multiple resume calls.

Sub-checks:

- Defines a simulated network fetcher that yields across multiple phases.
- Repeatedly resumes the coroutine from a main loop.
- Confirms final payload value is set after completion.
- Confirms the main loop observed multiple ticks rather than one blocking call.

Failure tip: inspect `include/proven/coro.h`. Coroutine state must live in caller-owned storage and must not be reset between resumes.

### 23. `tests/test_phase20_job` - job system

Intent: verify the hosted worker-thread job system executes submitted jobs exactly once and shuts down cleanly.

Sub-checks:

- Initializes a job system with four workers and a 1024-entry queue.
- Dispatches 1000 jobs.
- Uses atomics to count total executed jobs.
- Uses indexed atomics to detect duplicate or missing job execution.
- Shuts down workers and flushes synchronization barriers.

Failure tip: inspect `src/proven/job.c` and `platform/proven_sys_thread.c`. For races, run `./nob tsan`. Check admission state, sequence counters, queue claim/commit ordering, and shutdown wakeups.

### 24. `tests/test_phase21_scan` - scanner

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

### 25. `tests/test_phase22_fmt_best_effort` - formatter failure policy

Intent: verify formatting append policies are explicit: fixed-capacity atomic, fixed-capacity truncating, and allocator-backed growable.

Sub-checks:

- Appends formatted output with growable allocation.
- Populates a small fixed string.
- Confirms fixed-capacity formatting reports out-of-bounds and leaves the string unchanged.
- Confirms truncating formatting writes the partial count and reports required size.
- Confirms content after truncation matches the expected prefix.
- Checks extremely large padding specs for safe overflow handling.

Failure tip: inspect `src/proven/fmt.c`. Track `written`, `required`, and destination length separately. Atomic failure must not modify the destination.

### 26. `tests/test_scan_overflow_f64` - float scanner overflow

Intent: verify a very large floating-point token reports `PROVEN_ERR_OVERFLOW` instead of silently accepting infinity.

Sub-checks:

- Builds an input with roughly 1000 decimal digits.
- Scans it as `f64`.
- Confirms the error is `PROVEN_ERR_OVERFLOW`.

Failure tip: inspect `proven_scan_f64` in `src/proven/scan.c` and math helper behavior in the PAL. Do not accept `inf` as a successful parsed finite value.

### 27. `tests/test_sysio_scanner` - sysio-backed scanner

Intent: verify scanner behavior over file-backed sysio data instead of only in-memory string views.

Sub-checks:

- Creates a temporary file and writes integer/token content.
- Opens the file for reading.
- Initializes a sysio scanner with an allocator-backed buffer.
- Scans two integers and a word from the file stream.
- Cleans up scanner and file resources.

Failure tip: inspect `src/proven/sysio.c`, `src/proven/scan.c`, and file read wrappers. If in-memory scan tests pass but this fails, suspect buffer refill or file-position behavior.

### 28. `tests/test_regression_v26_05` - v26.05 regressions

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
- Array/string self-alias grow: grow operations must not corrupt when source and destination overlap in documented ways.
- `PROVEN_ARG_CSTR_N` safety bounds: C-string-with-length arguments must respect the caller-supplied bound.
- Environment large value: environment values larger than a small stack buffer must be read through dynamic allocation.

Failure tip: this file is intentionally a set of historical tripwires. Do not collapse it into broad smoke coverage. Read the failing sub-check name printed in the log and inspect the corresponding source module.

### 29. `tests/test_regression_fs_copy_to_self` - filesystem self-copy regression

Intent: verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the file.

Sub-checks:

- Creates a source file with known content.
- Attempts to copy the file to the same path and expects `PROVEN_ERR_INVALID_ARG`.
- Reads the file back and verifies size and contents are unchanged.
- Creates a hard link to the same file when supported.
- Attempts to copy across the hard-linked paths and expects failure without corruption.

Failure tip: inspect same-file detection and open/truncate ordering in filesystem copy code. The destination must not be opened with truncation before proving it is not the same file as the source.

### 30. `tests/test_regression_source_contracts` - source portability contracts

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

### 31. `tests/test_arena_panic` - arena panic path

Intent: verify panic-on-allocation-failure behavior is deterministic and does not fire on successful arena allocation.

Sub-checks:

- Overrides the weak panic handler with a test hook.
- Allocates successfully with `alloc_or_panic` and confirms no panic occurred.
- Requests more memory than the arena can provide.
- Confirms the panic hook was invoked exactly for the out-of-memory path.

Failure tip: inspect `src/proven/arena.c` and `src/proven/panic.c`. Restore the panic hook carefully in tests so later tests are not affected.

### 32. `tests/test_alias_smoke` - alias layer smoke

Intent: verify the public XCV alias layer compiles and maps representative aliases to canonical proven APIs.

Sub-checks:

- Uses native integer scan aliases.
- Uses scan function aliases.
- Uses formatting argument aliases.
- Uses macro aliases that are expected to stay available for the alias layer.

Failure tip: inspect `include/proven/alias_xcv.h` and `tests/test_alias_smoke.c`. When public symbols are added, renamed, or removed, update the alias header and this smoke test together.

## Regression subset

`./nob regression`, `./nob regression-asan`, and `./nob regression-ubsan` currently run:

- `tests/test_regression_v26_05`
- `tests/test_regression_fs_copy_to_self`
- `tests/test_regression_source_contracts`

Intent: provide a short feedback loop for bug-fix work without running every hosted example and container test.

What it checks:

- Historical behavioral bugs remain fixed.
- Filesystem destructive edge cases remain protected.
- Source-level portability contracts and test-log/documentation contracts remain present.

Failure tip: a regression failure is usually more specific than a full-suite failure. Use the sub-check name and preserve the regression unless the underlying public contract is intentionally changed and documented.

## Freestanding tests

`./nob freestanding` currently builds the library with `PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR`, and `-ffreestanding`, then runs five reduced tests.

### `tests/test_freestanding_heap_stub`

Intent: verify the hosted heap allocator is not accidentally available in freestanding mode.

Sub-checks:

- Calls `proven_heap_allocator()` under `PROVEN_FREESTANDING`.
- Confirms the returned allocator is invalid because no default OS heap exists.
- Skips with an info line when compiled outside freestanding mode.

Failure tip: inspect `src/proven/heap.c` and platform heap guards. Freestanding code must not silently pull a hosted allocator.

### `tests/test_compile_freestanding`

Intent: verify the reduced freestanding core can compile and link.

Sub-checks:

- Includes the public headers under freestanding flags.
- Links against the reduced object set selected by `nob.c`.

Failure tip: inspect `nob.c` source exclusion lists and public header feature guards. A hosted-only declaration may have leaked into freestanding builds.

### `tests/test_compile_nofloat`

Intent: verify `PROVEN_FMT_NO_FLOAT` removes floating-point formatting dependencies without breaking the rest of formatting.

Sub-checks:

- Builds with no-float formatting enabled.
- Links the formatter without float-specific argument handling.

Failure tip: inspect `include/proven/fmt.h` and `src/proven/fmt.c` for unguarded `float`, `double`, or math-helper references.

### `tests/test_compile_nou16str`

Intent: verify `PROVEN_NO_U16STR` removes optional U16 string support without breaking core headers and linking.

Sub-checks:

- Builds the umbrella header and reduced object set with U16 support disabled.
- Links successfully without `src/proven/u16str.c`.

Failure tip: inspect `include/proven.h`, `include/proven/u16str.h`, aliases, and the `nob.c` freestanding source list.

### `tests/test_freestanding`

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

## Cross compile-only matrix

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

Hosted targets compile all hosted source files and `tests/test_cross_compile_smoke.c`. Freestanding targets compile only freestanding-safe source files and `tests/test_freestanding.c` as a smoke translation unit.

Failure tip: if the log says the compiler is missing, fix the build-server toolchain rather than the library. If the target probe fails, check sysroot, multilib headers, or target flags. If a later library source file fails, treat it as a real portability bug.

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
./nob strict-error -build-root /mnt/ai-share/build/proven_c_lib
./nob regression-asan -build-root /mnt/ai-share/build/proven_c_lib
./nob regression-ubsan -build-root /mnt/ai-share/build/proven_c_lib
./nob freestanding -build-root /mnt/ai-share/build/proven_c_lib
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Optional compiler-specific gate:

```sh
./nob strict-error -cc clang -build-root /mnt/ai-share/build/proven_c_lib
```
