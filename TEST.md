# proven Test Matrix (v0.23.0)

This is the **catalog**: what every test checks, and where to start when one fails. Tests are plain C executables built and run by `nob.c`; no external framework is involved.

Labels such as `RFC-0007` or `B-043`, and bare file names such as `b024-find-last-benchmark.c`, refer to the maintainers' design records, backlog and measurement programs, which are not part of the published repository; every test and benchmark this file describes is.

For the **policy** - how tests are named, what each class is for, the rules a new test has to satisfy, and an honest account of how this project actually develops - see `TESTING.md`.

## Naming

```text
tests/test_<class>_<subject>.c
```

The filename is the identifier. **There are no numbers.** Numbers were tried and they rotted: this catalog once ran `1..50` with `7a`, `30a`, `30b`, `30c`, `40a` wedged in wherever something new arrived, and five of its entries described files that had been deleted months earlier. The tests themselves were named `test_phase1` ... `test_phase22` - the development order, which is the one fact about a test that nobody ever needs.

The class says what kind of question the test answers:

| Class | Question | Count |
|---|---|---|
| `unit` | Does this module do what it says, used the way a caller uses it? | 101 |
| `contract` | Does it *refuse* what it says it refuses? | 14 |
| `regression` | Does a defect that actually shipped stay fixed? | 28 |
| `differential` | Does it agree with an oracle we did not write? | 5 |
| `portability` | Does it compile, link, and keep its platform branches intact where we cannot run it? | 13 |
| `stress` | Does it survive concurrency, under a sanitizer, long enough for a race to be likely? | 1 |
| `docs` | Are the claims the documentation makes still true? | 11 |
| `bench` | How fast is it? (Not a correctness gate.) | 5 |

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
- [Cross-build matrix](#cross-build-matrix)
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
./nob build -build-root build-out/proven_c_lib
```

Run the warnings-as-errors gate:

```sh
./nob strict-error -build-root build-out/proven_c_lib
```

Run sanitizer modes:

```sh
./nob asan -build-root build-out/proven_c_lib
./nob ubsan -build-root build-out/proven_c_lib
./nob tsan -build-root build-out/proven_c_lib
```

A run stops at the first test that fails. To see every failure in one run, add `-keep-going`:
each test still runs once, in registry order, and the run ends with a
`[PROVEN][BUILD][KEEP_GOING][SUMMARY]` line and one `[PROVEN][BUILD][KEEP_GOING][FAILED]` line per
failing test, and exits 1. Test executables are linked in parallel, one per CPU, and run one at a
time.

```sh
./nob build -keep-going -build-root build-out/proven_c_lib
```

On a POSIX host, `-jobs N` runs up to N tests at once (and implies `-keep-going`). Each runs in a
directory of its own, `<build dir>/run/<test>/`, holding a symbolic link to every top-level entry
of the repository except `.git` and the build roots: a test reads `include/...` or `TEST.md` by
relative path as usual, and the fixture files it creates stay in its own directory. Each test's
output is captured and printed whole, in registry order. Stress and benchmark tests still run one
at a time afterwards, in the repository root. On Windows the flag is ignored with a note.

```sh
./nob asan -jobs 16 -build-root build-out/proven_c_lib
```

Run focused regression modes:

```sh
./nob regression -build-root build-out/proven_c_lib
./nob regression-asan -build-root build-out/proven_c_lib
./nob regression-ubsan -build-root build-out/proven_c_lib
```

Run the float parse path benchmark:

```sh
./nob bench-float -build-root build-out/proven_c_lib
```

Run freestanding checks:

```sh
./nob freestanding -build-root build-out/proven_c_lib
```

Run cross-build coverage:

```sh
./nob cross -build-root build-out/proven_c_lib
```

Every target ends as PASS, FAIL or SKIP, and the run prints one `[PROVEN][CROSS][RESULT]` line per target and a `[PROVEN][CROSS][SUMMARY]`. A missing compiler, or one that cannot build the target probe, is a SKIP - a verification gap, not a pass. The mandatory targets - `native-gcc-hosted`, `native-clang-hosted`, `windows-x86_64-winapi`, `windows-i686-winapi` - may not be skipped: if one is, the run fails. A real compile or link error fails the run, after every other target has still been tried.

Clean generated output:

```sh
./nob clean                                   # the default build root, build/
./nob clean -build-root build-out/proven_c_lib
```

`clean` removes the selected build root without following symlinks. It refuses `.`, `/` and any path with `..`, and removes a root other than `build` only when it carries the `.proven-build-root` marker that nob writes into every build root it creates.

Rebuilds are header-precise: every object and test executable is built with a compiler dependency file (`-MMD`), and its cache key is the exact command plus the contents of every file that dependency file names. `build_headers.inc` remains the list of headers the build validates, not what triggers a rebuild.

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

The float parse path benchmark uses the same test harness and emits its timing rows through `[PROVEN][TEST][INFO]` so the captured output can be saved directly as a dated markdown report. Those reports live in a maintainers' record, not in the published repository, so a reader of this repository will not find them here.

## Test modes

### `build`

Intent: compile the hosted library in debug mode and run the complete hosted runtime suite.

What it checks:

- All hosted source files compile together with the public headers.
- All hosted test executables link against the same object set.
- The full set of runtime behavior checks succeeds without sanitizer instrumentation.

Failure tip: start from the first `[PROVEN][TEST][FAIL]` line. If the stage is `link`, inspect the immediately preceding compiler or linker diagnostic. If the stage is `run`, inspect the test-specific failure hint and the failing assertion.

### `release`

Intent: compile the hosted library with optimization and `NDEBUG` (`-O3 -DNDEBUG`) and run the complete hosted runtime suite.

What it checks:

- Optimized builds do not rely on debug-only initialization or timing.
- Undefined behavior that is hidden in debug mode is less likely to survive optimization.
- Public headers and implementation still agree under `-O3`.
- The library's misuse validation (pool double-free scan, map key overlap check) is compiled out, as a release build intends; the contract tests for it skip their trap assertions. It was compiled in until B-036 and made pool teardown quadratic: 20,000 frees took 59.6 ms against 0.05 ms (`b036-pool-teardown-benchmark.c`).

### `hardened`

Intent: the optimized build with the misuse validation kept in (`-O2 -DNDEBUG -DPROVEN_HARDENED=1`), running the complete hosted suite.

What it checks:

- The validation that `release` compiles out still works under optimization: the pool and map contract tests exercise their trap paths here.
- Every build logs `[PROVEN][BUILD][PROFILE]` saying which validation it contains, so a log is never read as more than it was.

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

Intent: run the three benchmark executables - the float parse path benchmark, the mixed-corpus float parse benchmark, and the primitive throughput benchmark - and write a dated report. Despite the name, this mode is not float-only. The reports live in a maintainers' record, not in the published repository.

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
- Every target is reported PASS, FAIL or SKIP; a skipped mandatory target (native gcc/clang, both Windows word sizes) or any real failure fails the run.

Failure tip: identify the target name in the log, then check whether the failure is from compiler availability, sysroot usability, or actual source incompatibility. Cross compilation does not replace runtime testing on the target.

### The full suite on Windows

The whole hosted suite also runs natively on Windows, cross-built with mingw-w64:

```sh
./nob build -no-run -cc x86_64-w64-mingw32-gcc -build-root build/win    # or i686-w64-mingw32-gcc
```

`-no-run` builds and installs every test executable and runs none. The build driver asks the compiler for its target (`-dumpmachine`); for a Windows target the executables are `.exe`, linked `-static` with `-lbcrypt`, and never `-ldl`. A maintainers' script (`win11kd-full-suite.sh`, not in this repository) builds both word sizes, sends the tracked tree and the executables to a Windows machine, and runs them there with `win11kd-run-suite.ps1`, which records PASS, FAIL, SKIP (a POSIX-only test that skipped itself - counted apart, since it proves nothing on Windows) and TIMEOUT per test.

Last run, 2026-10-10, Windows 11 test VM: x86-64 303 PASS, 0 FAIL, 8 SKIP; i686 the same. The eight skips are fixtures whose subject is POSIX: `test_portability_nob_std_probe` and `test_portability_nob_clean` (they drive `./nob` through a POSIX shell), `test_portability_compile_nonet` (it drives the host compiler through one), `test_regression_fs_walk_errors` (libc interposition with `dlsym`), `test_unit_fs_walk` (chmod 000 and `ln -s` cycles), `test_regression_fs_backslash_parent` (a backslash as an ordinary byte), `test_regression_fs_private_staging` and `test_regression_fs_perms_and_types` (POSIX modes). `test_unit_sysio_streams`, `test_regression_scanner_float_split` and `test_regression_scanner_short_read` run on Windows too.

## Test catalog


The hosted full run builds and executes 165 registered tests plus the 146 runnable manual examples - 311 executables in all. `./nob regression` re-runs a 36-test subset, `./nob freestanding` a 5-test subset, and `./nob bench-float` 5 benchmarks. The tree holds 178 test files: the 165 above, the 5 freestanding-only and 5 benchmark entries, and 3 cross-only sources that only `./nob cross` builds (two smoke programs and the no-CRT link).

These counts come from the same preprocessed registry manifest compiled by `nob.c` and
`tests/test_docs_test_catalog`. The gate also fails when a registry contains duplicates, a
regression is absent from the hosted suite, a test source belongs to no hosted, freestanding,
benchmark, or cross-only registry, or a registered test has no entry in this file. It exists
because both sides had already stopped being true: this catalog claimed 118 tests when the real
number was different, and ten registered tests were missing from it entirely.

Tests are named `test_<class>_<subject>`, and the name is the identifier - there are no numbers.
Numbers rot: this catalog used to run 1..50 with `7a`, `30a`, `30b`, `30c`, `40a` wedged in wherever something new arrived, and five of its entries described files that had been deleted months earlier.

The class says what kind of question the test answers:

- **`unit`** - One module's public API, used the way a caller uses it. These are the tests that say what the library *does*.
- **`contract`** - The public invariants: misuse, corrupted structs, exhausted allocators, refused input. These say what the library *refuses to do*, which is the half a caller cannot infer from the happy path.
- **`regression`** - One test per defect that actually shipped. Each is named for what broke, not for a version or a number, and each was verified to FAIL against the pre-fix source. A regression test that passes before the fix is not a regression test.
- **`differential`** - Correctness against an independent oracle - the host libc, or a corpus with known-good answers. These catch what a self-written expectation cannot: a wrong belief held consistently by both the code and its test.
- **`portability`** - Freestanding builds, cross-target builds, source-level platform contracts, and the build driver's own standard probe. Most of these cannot be *run* on the host, so they check what can be checked: that the code compiles, links where configured, and keeps its platform branches intact.
- **`stress`** - Concurrency under a sanitizer, over enough iterations to make a race likely rather than theoretical.
- **`docs`** - The documentation is checked by the build, not by eye: every public function has an alias and is named in the manual, the manual never documents a function that does not exist, every example the manual prints is a program that compiles and runs, no example drifts from its chapter, and the version string agrees with itself everywhere, every module section carries its intent/reference/structures/example/counter-example, and every factual claim the chapters make is true. These are the **gates** in `DOCUMENTING.md` Section 2 - each one exists because the thing it forbids already happened.
- **`bench`** - Timing, not correctness. A benchmark regression is a signal to investigate; a checksum drift inside one is a correctness failure and does fail the build.

## Unit tests

One module's public API, used the way a caller uses it. These are the tests that say what the library *does*.

### `tests/test_unit_algorithm` - algorithms

Intent: verify generic sort and binary search helpers using both scalar and struct comparators.

Sub-checks:

- Sorts an integer array and verifies ascending order.
- Binary-searches for an existing and a missing integer.
- Sorts structs using a comparator that sorts by score descending and ID ascending.
- Verifies comparator tie-breaking order.

Failure tip: inspect `src/proven/algorithm.c`. Comparator return convention must stay consistent: callers expect negative, zero, and positive values to drive ordering.

### `tests/test_unit_alloc_check` - an allocator that knows its own blocks

Intent: verify the `alloc_check.h` wrapper (B-040) passes correct use through and refuses misuse at the call, without ever forwarding a refused pointer.

Sub-checks:

- Correct alloc, realloc (the record moves with the block), realloc to 0 and free pass through; live, live-byte and peak counts are exact; bad arguments give an invalid allocator.
- A foreign free, a double free, a realloc of a foreign pointer, a realloc with the wrong old size or alignment, and an allocation past the record each panic (observed through a returning handler), are counted in `faults`, and leave the inner allocator untouched - under ASan a forwarded pointer would be a report.
- The B-023 mistake: a string made through a checked arena and destroyed through a checked heap is refused at the destroy; the right allocator then frees it.
- Without `PROVEN_ALLOC_CHECK`, `proven_alloc_checked` returns the inner allocator itself.
- Planted defects - forwarding a refused free, dropping the old-size check, leaking the block when the record is full - each failed it.

Failure tip: inspect `src/proven/alloc_check.c`.

### `tests/test_unit_alloc_check_on` - alloc_check switched on by its macro

Intent: verify that `PROVEN_ALLOC_CHECK`, defined before the first proven header, makes `proven_alloc_checked` wrap.

Sub-checks:

- The returned allocator is the checker, and it refuses a foreign free.

Failure tip: the `#ifdef` in `proven_alloc_checked`, or a define placed after an include - the manual's first draft of its example did exactly that and the checks were silently off.

### `tests/test_unit_arena` - arena allocator

Intent: verify bump-allocation behavior, alignment, exhaustion, reset, realloc, and zero-copy semantics for the arena allocator.

Sub-checks:

- Initializes an arena over a fixed backing buffer.
- Allocates default-aligned and explicitly 32-byte-aligned blocks.
- Confirms out-of-memory requests fail instead of overwriting the backing buffer.
- Confirms reset releases the whole arena lifetime at once.
- Exercises reallocation and verifies zero-copy or migration semantics according to arena constraints.

Failure tip: inspect `src/proven/arena.c`, especially offset rounding, overflow checks, and reset behavior. Under ASan, any failure is likely a true bounds or lifetime bug.

### `tests/test_unit_array` - growable array

Intent: verify generic array allocation, validation, push/pop, growth, migration, element access, and arena-backed use.

Sub-checks:

- Initializes an array with a typed element size and initial capacity.
- Checks validation catches corrupted length/capacity state.
- Pushes typed values through macros.
- Forces growth and verifies data migrated correctly.
- Pops values and checks boundary rejection on empty pop.
- Checks invalid get/set ranges.
- Creates an arena-backed array to ensure allocator independence.
- Editing in place (RFC-0009 X-002): 20,000 random `insert` (fresh values and elements of the array itself), `remove_at`, `swap_remove`, `extend` (with a slice of itself), `truncate` and `clear` operations agree with a plain-array model after every step, while one grow in ten is refused by the allocator (a refused grow must leave the array unchanged); indexes past the end are `OUT_OF_BOUNDS`, `truncate` cannot lengthen, and an `extend` source that only partly overlaps the array is `INVALID_ARG`.

Failure tip: inspect `src/proven/array.c`. Growth failures usually mean element-size multiplication, capacity doubling, or realloc failure-atomic behavior changed. Remember that pointers into array storage are invalid after growth.

### `tests/test_unit_buffer_u8str_basics` - buffer and U8 string basics

Intent: verify fixed-capacity buffers, U8 string views, literal construction, append behavior, C-string conversion, and bounds defense.

Sub-checks:

- Creates a buffer through an allocator and appends data.
- Verifies `PROVEN_LIT` computes literal sizes without including the NUL terminator.
- Confirms buffer append rejects out-of-bounds writes.
- Creates a U8 string and appends multiple fragments.
- Converts a slice to a heap-owned C string.
- Confirms C-string termination, equality helpers, and integer overflow guards.

Failure tip: inspect `src/proven/buffer.c` and `src/proven/u8str.c`. Off-by-one capacity mistakes usually show up here first, especially around the extra NUL byte for C-string compatibility.

### `tests/test_unit_coro` - stackless coroutine

Intent: verify coroutine macros preserve caller-owned state across yields and complete after multiple resume calls.

Sub-checks:

- Defines a simulated network fetcher that yields across multiple phases.
- Repeatedly resumes the coroutine from a main loop.
- Confirms final payload value is set after completion.
- Confirms the main loop observed multiple ticks rather than one blocking call.

Failure tip: inspect `include/proven/coro.h`. Coroutine state must live in caller-owned storage and must not be reset between resumes.

### `tests/test_unit_encode` - hex and Base64 by use case

Intent: verify the encodings match RFC 4648's own vectors, that decode is the exact inverse, and that the decoders refuse malformed input and undersized buffers rather than guessing.

Sub-checks:

- Hex, standard Base64, and Base64URL encode the RFC 4648 progression ("", "f", "fo", "foo", "foob", "fooba", "foobar") to the known values - lowercase hex, `=`-padded standard, unpadded `-`/`_` URL form. The vectors were verified against Python's base64/binascii before being trusted.
- Decode inverts encode for both alphabets and padded-or-not; UPPERCASE hex decodes the same; the standard and URL forms of the same bytes decode identically.
- Malformed input is `PROVEN_ERR_INVALID_ENCODING` with nothing committed: odd-length hex, a non-hex or non-Base64 character, embedded whitespace (NOT skipped), bad padding, all-padding.
- An output buffer one byte too small is `PROVEN_ERR_OUT_OF_BOUNDS`, and the refused call writes no partial prefix.
- All 256 byte values round-trip through both hex and Base64.

Failure tip: inspect `src/proven/encode.c`. An encoding mismatch is a wrong alphabet or padding; a decoder that accepts junk is the memory bug the module exists to prevent - it validates the whole input before writing a byte.

### `tests/test_unit_entropy_source` - the entropy source is a thing you can install

Intent: verify the OS CSPRNG is the default on a hosted target, that a caller can replace it, and that a source which *fails* leaves the cryptographic generator inert rather than plausible.

Entropy is the one thing a program cannot compute for itself, so it is a hook rather than a hard-coded call: the OS on a hosted target, a board's TRNG on bare metal. Without it, the ChaCha generator ran everywhere and could be seeded nowhere.

Sub-checks:

- With nothing installed, `proven_random_bytes` already works - that is what "hosted" means - and actually produces bytes.
- An installed source replaces it, and the bytes provably come from *there* (the stand-in counts, so 0,1,...,7 is unmistakable). It feeds `proven_random_u64` too.
- `proven_random_set_source(NULL, NULL)` puts the platform default back: installing a source is not a one-way door.
- `proven_chacha_rng_seed_from_entropy` draws its seed from whatever source is installed, **exactly once** - not per byte - and the resulting keystream is ChaCha over the seed, not the seed echoed back.
- A source that FAILS: `proven_random_bytes` reports the failure rather than papering over it, seeding returns false, and the generator is inert - zeros for every byte well *past* the first block, where a zeroed ChaCha state would otherwise start emitting a fixed, publicly derivable keystream - and its trait is invalid.

Failure tip: inspect `proven_random_set_source` and the source dispatch in `src/proven/random.c`. With no source installed a freestanding build must return `false`, never fall back to a clock-seeded PRNG: that looks like success and is a hole nothing reports.

### `tests/test_unit_error_results` - error and result primitives

Intent: verify the explicit error/result style has stable semantics and no hidden control flow.

Sub-checks:

- Confirms `PROVEN_OK` is accepted as success.
- Confirms representative failures such as `PROVEN_ERR_NOMEM` are rejected as success.
- Builds a successful memory result and verifies both the error and value fields.
- Builds a failed memory result and verifies the error and null value fields.

Failure tip: inspect `include/proven/error.h` and generated/result typedefs. Do not change enum values or result layouts without updating every call site, alias, manual, and test that relies on them.

### `tests/test_unit_float_bigint_divmod` - float big-integer division

Intent: verify the big-integer divide/modulo used by the exact float fallback.

Failure tip: inspect `src/proven/float_decimal.c`; a wrong quotient or remainder here silently corrupts the exact arbiter that decides ties.

### `tests/test_unit_float_shortest_format_roundtrip` - shortest formatter round-trip

Intent: verify values formatted by the shortest formatter parse back to the identical bits.

Note: this test existed on disk but was never registered in `nob.c`, so it had never run. It is registered now.

Failure tip: inspect `src/proven/float_format.c` and the Grisu3/Dragon4 shortest-digit engines if a value fails to round-trip.

### `tests/test_unit_fmt_f64_accuracy` - float formatter accuracy

Intent: verify fixed-point rounding, scientific carry, and special-value text for floating-point formatting.

Sub-checks:

- Checks normal-path rounding to six fractional digits.
- Checks carry from the fractional tail into the integer part.
- Checks scientific notation carry around the mantissa boundary.
- Checks NaN and infinity text stay stable.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_unit_fmt_f64_accuracy.c`.

### `tests/test_unit_fmt_custom` - formatting a user-defined type

Intent: verify `PROVEN_ARG_OF(&obj, render)` renders a type the library has never heard of, that width/fill/alignment apply to the rendered result, that a spec the library cannot interpret for that type is refused, that the renderer's own error reaches the caller, and that a non-deterministic renderer is caught.

Sub-checks:

- A `point_t` renders as `(3, -7)` through a renderer that composes - it calls the formatter again, into a stack buffer, with no allocator.
- `{:>12}`, `{:<12}` and `{:*^11}` align it. This is what the measuring pass exists for: the formatter runs the renderer once against a counting sink to learn its width, so a column of user types lines up like any other column, with no scratch allocation.
- `{:x}`, `{:.2}` and `{:+}` on a user type are `PROVEN_ERR_INVALID_FORMAT`. The library has no idea what they would mean; inventing an answer and reporting success is how a formatter starts lying.
- A renderer that returns `PROVEN_ERR_IO` makes the format call return `PROVEN_ERR_IO`; a NULL renderer is `PROVEN_ERR_INVALID_ARG`, not a crash.
- A renderer that emits two bytes on the measuring pass and eight on the real one is rejected, because emitting it would silently break the column it was being aligned into.

Failure tip: inspect `render_custom` and the counting/emitting sinks in `src/proven/fmt.c`.

### `tests/test_unit_fmt_spec` - format spec grammar

Intent: verify precision, bases, case, alternate form, sign, `char` and `bool` - and that a spec the argument cannot honour is refused rather than ignored.

Sub-checks:

- `{:.3}`, `{:.0}` (no decimals - the engine used to silently rewrite precision 0 to 6), `{:.3f}`, `{:g}`.
- A float column actually lines up: `{:>9.2}` on 12.5, 100.0 and -3.125.
- `{:x}` `{:X}` `{:#x}` `{:o}` `{:b}` `{:#b}` `{:08x}` `{:#010x}`.
- `{:+}`, `{: }`, and that zero-padding lands **between** the sign and the digits.
- `char` renders as a character (it used to print 90) and `bool` as `true`/`false`.
- Eight specs that must be **refused**, not ignored.
- A width of 200 produces exactly 200 characters - the first version of the renderer assembled the padded number in a 128-byte buffer and silently produced 127 while returning OK.
- Every pre-existing spelling still means what it meant.

Failure tip: inspect the spec parser and `render_integer` / the float case in `src/proven/fmt.c`.

### `tests/test_unit_fmt_scientific` - the `{:e}` scientific float form

Intent: verify `{:e}` renders always-scientific notation (mantissa, default six fractional digits, signed two-digit-minimum exponent, half-to-even rounding) digit-for-digit like `printf %e`, that `{:.Ne}` honours a chosen precision, that it forces scientific where `{:f}`/`{:g}` would not, and that `{:e}` on a non-float is refused.

Failure tip: inspect the `{:e}` branch in `src/proven/fmt.c` and `PROVEN_FLOAT_FORMAT_MODE_SCIENTIFIC` in `float_format.c`. Every expected value is exactly `printf %e`.

### `tests/test_unit_fmt_fastpath` - formatter truncation comparison

Intent: compare truncating fixed-capacity formatting against the growable reference path for exact-fit, truncation, malformed format, and excess-argument cases.

Sub-checks:

- Checks exact-fit truncation output matches the reference path.
- Checks over-capacity truncation keeps the same prefix bytes and counts.
- Checks excess-argument validation.
- Checks malformed-format validation.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_unit_fmt_fastpath.c`.

### `tests/test_unit_foundation` - foundation primitives

Intent: verify the core error and checked-arithmetic assumptions used by all higher-level modules.

Sub-checks:

- Confirms `PROVEN_IS_OK` and `proven_is_ok` classify success and failure correctly.
- Confirms checked add detects overflow and preserves the wrapped C result where the C23 checked-arithmetic API says it should.
- Confirms checked subtract detects underflow.
- Confirms checked multiply detects overflow and succeeds for safe products.
- Confirms simple result structs can carry both an error and a value.

Failure tip: inspect `include/proven/error.h` and the `PROVEN_CKD_*` definitions in `include/proven/types.h`. If this test fails, avoid debugging later modules until the foundation behavior is fixed.

### `tests/test_unit_fs_advanced` - advanced filesystem

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

### `tests/test_unit_fs_basic` - basic filesystem

Intent: verify hosted file open, write, read-all, size queries, and absolute-path classification.

Sub-checks:

- Opens a temporary file for create/write/truncate.
- Writes known content and checks byte count.
- Reads the whole file and checks size and byte equality.
- Reopens the file and queries its size.
- Verifies an exclusive create over the existing file is `PROVEN_ERR_EXISTS` and a missing name opened for reading is `PROVEN_ERR_NOT_FOUND` (RFC-0009 X-009).
- Verifies absolute path classification for POSIX, drive-letter Windows paths, UNC paths, and extended Windows paths.

Failure tip: inspect `src/proven/fs.c` and `platform/proven_sys_fs.c`. If only Windows path cases fail, check path-prefix parsing rather than POSIX filesystem behavior.

### `tests/test_unit_fs_metadata_perms` - filesystem metadata and permissions

Intent: verify hosted permission and locking-related filesystem behavior stays explicit and isolated behind the PAL.

Sub-checks:

- Creates a temporary file.
- Changes permissions to read-only and back when the platform supports it.
- Opens files for write where needed.
- Acquires and releases advisory locks where supported.

Failure tip: inspect `platform/proven_sys_fs.c`. Permission and locking semantics are OS-dependent; keep differences in PAL code and avoid assuming POSIX behavior on every target.

### `tests/test_unit_hash` - hashing by use case

Intent: verify each hash does what its use case requires - FNV-1a spreads and is order-sensitive, keyed SipHash is a different function under a different key, CRC-32 matches the shared check value, and SHA-256 matches the official vectors and is chunking-independent.

Sub-checks:

- FNV-1a: distinct inputs hash apart, the empty view hashes to the FNV offset basis, and a one-byte change moves the hash.
- SipHash-2-4: two keys give two functions for the same bytes; the reference key/message vectors from the paper match (verified against the little-endian readings, which caught a big-endian transcription in the test itself).
- CRC-32: `"123456789"` is `0xcbf43926`, and `proven_crc32_update` chained over chunks equals the one-shot over the whole.
- CRC-32 slicing-by-8 (RFC-0009 P-101): for every offset 0-7 and lengths 0-40 then every 37th to 1090, the one-shot CRC and a two-chunk split equal the CRC fed one byte per call (which uses only the single table); the 44-byte fox sentence matches zlib's `0x519025e9`.
- SHA-256: the two NIST example vectors and the empty-input digest match; a streamed `init`/`update`/`final` over arbitrary chunk boundaries equals the one-shot; `to_hex` is 64 lowercase hex characters, NUL-terminated.
- Every entry point guards a `{NULL, size>0}` view the way SHA-256 does, rather than dereferencing it.

Failure tip: inspect `src/proven/hash.c`. The algorithms are implemented from their specifications; a KAT mismatch means a rotation, round count, or endianness is off.

### `tests/test_unit_hmac` - SHA-384, SHA-512, HMAC and HKDF

Intent: verify the SHA-2 digests added for TLS, HMAC and HKDF against the documents that define them, and the two memory calls that handling a secret needs.

Sub-checks:

- SHA-512 and SHA-384 of `abc`, the empty message, the 896-bit message and a million `a` fed in a thousand pieces: the values of FIPS 180-4.
- Twenty-two lengths from 0 to 1000 on each side of the padding boundary (111, 112, 113) and the block boundary (127, 128, 129, and their multiples), both digests at each; every split of 300 bytes into two updates, and a byte at a time with empty updates among them, give the one-shot digest.
- HMAC: RFC 4231 test cases 1, 2, 3, 4, 6 and 7, an empty key with an empty message, and keys of exactly one block and one byte more, for SHA-256, SHA-384 and SHA-512 - one-shot and streamed a byte at a time, with nothing written past the MAC's own size. An unknown hash and a null key are `PROVEN_ERR_INVALID_ARG` with nothing written; a state that was not begun ignores update and final; after final every byte of the state is zero.
- HKDF: RFC 5869 A.1, A.2 and A.3 (extract's pseudorandom key and expand's output), the A.1 inputs over SHA-384 and SHA-512, 200 bytes, 1 byte, and exactly one block and one byte more - extract, expand and the combined call, writing exactly the length asked. Different `info` gives different keys. 255 blocks succeed; one byte more is `PROVEN_ERR_OUT_OF_BOUNDS` with nothing written; a short key, an unknown hash and a null input are `PROVEN_ERR_INVALID_ARG`.
- `proven_mem_equal_ct`: equal ranges, a flip of each of 512 bits, different lengths, empty and null ranges. `proven_mem_wipe`: zeroes exactly the bytes named.

Failure tip: inspect the SHA-512 section of `src/proven/hash.c`, `src/proven/hmac.c` and the end of `src/proven/memory.c`. The digests at boundary lengths and the HKDF values over SHA-384 and SHA-512 are not printed by any standard: they were computed with Python's `hashlib` and `hmac` by a script that first reproduced the published vectors. That `proven_mem_equal_ct` takes the same time whatever it finds, and that `proven_mem_wipe` survives optimisation, are properties of the source and are not observed by this test.

### `tests/test_unit_crypto` - the cryptographic primitives under TLS

Intent: verify the internal primitives the TLS unit will stand on against another implementation, and that each refuses what it must. They are not public API; the test reaches them through `src/proven/proven_internal_crypto.h`.

Sub-checks:

- ChaCha20-Poly1305: the example of RFC 8439 section 2.8.2 and fifteen more at lengths on each side of the 16- and 64-byte boundaries; sealing, opening, and a changed tag or associated data refused with the output zeroed.
- AES-GCM: the blocks of FIPS 197 appendix C, twenty more blocks, and thirty-two messages for 128- and 256-bit keys - run on the bitsliced code (forced by a test hook), run again on the processor's AES and carry-less multiply where it has them, sealing in place, and a changed tag, text or associated data refused. Then the two implementations against each other on twenty-four further messages. A key of another size is refused.
- Multi-precision arithmetic: Montgomery multiplication, modular addition and subtraction, exponentiation, inversion modulo a prime, and reduction of a wide value, for the field and order of P-256 and P-384, the order of Curve25519 and odd moduli from 33 to 2049 bits, with 0, 1 and n-1 as operands; the comparisons; an even modulus, 1 and 0 refused.
- P-256 and P-384: public keys and ECDH shared secrets; ECDSA with the deterministic nonce of RFC 6979 - its appendix A.2.5 example first - over SHA-256, SHA-384 and SHA-512, and verification of the same in raw and in DER form; a changed digest or signature, a DER signature one byte long or short, a point off the curve, a compressed point, a scalar of 0 or of the group order, and r or s of zero are refused.
- X25519 and Ed25519: the examples of RFC 7748 section 6.1 and RFC 8032 section 7.1 first, then twenty more of each; a point of small order is refused by the key exchange; a changed R, S, public key or message, and the signature with the group order added to S, are refused.
- RSA verification: PKCS #1 v1.5 and PSS over three hashes and moduli of 2048, 2049, 2056 and 3072 bits, with public exponents 65537 and 3 and salts of the hash length, 0 and 20; a 1024-bit key, a changed digest or signature, another hash, another salt length, a PSS signature offered as PKCS #1 and the reverse, a short signature, and a signature equal to the modulus are refused.

Failure tip: inspect the `src/proven/crypto_*.c` file the failing section names, and `platform/proven_sys_aes.c` for the hardware path. The expected values are in `tests/test_unit_crypto_vectors.h`, written by a private generator from Python `cryptography`; where a standard prints the value, the generator reproduced it first. Not covered here: constant-time behaviour (a private Valgrind check), and the larger adversarial vector sets (a private Project Wycheproof run).

### `tests/test_unit_cert` - X.509 certificates

Intent: verify that certificates are read strictly, matched against names by the stated rules, and that a chain is accepted or refused exactly as another implementation built it to be.

Sub-checks:

- Reading: the fields of a leaf, an RSA root and an intermediate (version, key kind and bytes, signature kind and hash, validity as Unix seconds, CA flag, path length, key usage, extended key usage, alternative names), every view inside the caller's bytes; every proper prefix of a certificate refused; a sweep of single-bit changes over the whole certificate with no read outside the buffer; a trailing byte, a non-minimal length and an indefinite length refused.
- Names: the DNS names, a wildcard for exactly one label, IPv4 and IPv6 literals in two spellings; a prefix, a suffix, a subdomain, an empty label, two labels under a wildcard, another address, an octet with a leading zero and the subject's common name refused. The public-key pin equals SHA-256 of the SubjectPublicKeyInfo.
- PEM: blocks found among other text, labels returned, a short buffer reported with the size needed and the position unmoved; a missing END line, another END label, a bad symbol, an impossible length, misplaced padding and stray bits refused.
- The store: a bundle taken entry by entry with a non-certificate block skipped, growth past its first capacity, copies that outlive the caller's bytes, text with no certificate and bytes that are not one refused without changing it.
- Chain verification, forty cases with the error code, the fault and the depth each must give: RSA, P-256, P-384 and Ed25519 keys with PKCS #1, PSS and ECDSA signatures; an intermediate or the leaf as the anchor; extra certificates in any order; the name rules again; before and after the validity period; an expired and a not-yet-valid leaf; a missing intermediate; an anchor with the right name and another key; one changed signature bit; an issuer that is not a CA, or whose key usage does not allow issuing; a path length exceeded; name constraints permitting, excluding, by address, and against a wildcard; an unknown critical extension; the wrong extended key usage in each direction; a hash that is not accepted; a name only in the common name; a version 3 anchor that is not a CA; a trailing byte.

Failure tip: inspect `src/proven/cert.c`. The chains are in `tests/test_unit_cert_vectors.h`, written by a private generator with Python `cryptography`; their dates are fixed and the test supplies the time. The case's text says what was built. Not covered: a certificate signed with SHA-1 (the generator's library will not make one; SHA-224 stands in for "a hash that is not accepted"), and the Windows root store beyond being opened by the manual example.

### `tests/test_unit_tls_keys` - TLS 1.3 key schedule and records against RFC 8448

Intent: verify the parts of TLS 1.3 below the state machine - the transcript hash, the key schedule, record protection - against the published example handshakes, value by value. Internal code, reached through `src/proven/proven_internal_tls.h`.

Sub-checks:

- RFC 8448 section 3 (a simple 1-RTT handshake) replayed in the RFC's order with the test's own transcript: the early, handshake and master extractions; every `Derive-Secret`, with its context required to equal the transcript hash at that point; both Finished values; the resumption secret; each traffic key and IV; and each protected record sealed to the printed bytes, opened again, refused with one bit changed and refused under the next sequence number.
- Section 4 (resumption): the pre-shared key as the early secret, the binder over the truncated ClientHello, the early traffic secrets, EndOfEarlyData in the transcript.
- Section 5 (HelloRetryRequest): the first ClientHello replaced in the transcript by a `message_hash` message. Section 6 (client authentication): the client's Certificate and CertificateVerify before its Finished. Section 7 (compatibility mode): ChangeCipherSpec records outside the transcript.
- Beyond the traces: `TLS_AES_256_GCM_SHA384` and `TLS_CHACHA20_POLY1305_SHA256` seal and open records from empty to 2^14 bytes; padding is stripped; a record of only zeros, a body shorter than a tag and a type, and one over the limit are refused.

Failure tip: inspect `src/proven/tls_keys.c`. The step letter printed on failure names the operation (the test's header comment lists them). `tests/test_unit_tls_vectors.h` is written by a private generator from the RFC's text. Not covered: the traces are all `TLS_AES_128_GCM_SHA256`, so no published value checks the SHA-384 schedule; and this is not a handshake - no state machine is exercised.

### `tests/test_unit_tls12_keys` - TLS 1.2 PRF, key block and records against known answers

Intent: verify the parts of TLS 1.2 below the state machine - the PRF, the extended master secret, Finished, the key block, record protection for the six AEAD suites - against a script of known answers. Internal code, reached through `src/proven/proven_internal_tls.h`.

Sub-checks:

- The PRF: 18 outputs, the first two being the test vectors published on the IETF TLS list for SHA-256 and SHA-384.
- The extended master secret (RFC 7627) from a premaster secret and a session hash, and the client's and the server's Finished values: four of each, over both hashes.
- For each of the six suites: the keys installed from the key block, then eight records in the two directions - handshake, application data from an empty record to 52 bytes, an alert - each sealed to exactly the expected bytes, and the expected record opened again to its content.
- What record protection refuses, for an AES-128-GCM, an AES-256-GCM and a ChaCha20 suite: a changed tag, a changed content type, a record sealed for the other direction, one cut short by a byte, the same record a second time, and lengths below a tag or above the maximum; a record is its content, a 16-byte tag and, with AES-GCM, an 8-byte explicit nonce.
- Suite numbers that are no suite here (a TLS 1.3 suite, a CBC suite with SHA-384, a 3DES suite) are not found.

Failure tip: inspect `src/proven/tls12_keys.c`. The letter printed on failure names the operation (the test's header comment lists them). `tests/test_unit_tls12_vectors.h` is written by a private generator whose own PRF first reproduced the two published vectors. Not covered: there is no published trace of a whole TLS 1.2 handshake to replay, as RFC 8448 is for 1.3 - agreement of the handshake with other implementations is checked in a private interoperability run; and this is not a handshake.

### `tests/test_unit_tls_legacy` - the legacy TLS set below the state machine

Intent: verify the pieces under the legacy TLS set - what CBC suites, key exchange by RSA and by finite-field Diffie-Hellman, and TLS 1.0 and 1.1 are built from - against values computed outside the library, and what each must refuse. Internal code, reached through `src/proven/proven_internal_tls.h`.

Sub-checks:

- A script of 355 known answers, replayed: ten AES-CBC texts encrypted and decrypted (the first two are SP 800-38A F.2.1 and F.2.5), with the processor's cipher and the portable one, the IV left as the last block; twenty-three HMACs with MD5, SHA-1 and SHA-256 (RFC 2202 and RFC 4231 cases first), each with its message split at every place; ten outputs of the TLS 1.0 PRF; a hundred and eight CBC records - both MACs, both MAC orders, the chained IV of TLS 1.0 and the explicit one of 1.1 and 1.2 - sealed to exactly the expected bytes and opened; forty-eight with more padding than needed, opened; a hundred and forty-four spoiled ones (a padding byte, the padding length, the MAC, the content, a block cut off), refused; twelve Diffie-Hellman exchanges over the six groups, public value and shared secret.
- Record lengths: every length shorter than a record is refused in both MAC orders and with both kinds of IV, and one above the maximum.
- Diffie-Hellman: the server's group is 2048 bits and known with generator 2; generator 3, a prime with a bit changed and half a prime are not known; a peer's value of 0, 1, the prime, the prime less one, one longer than the prime and an empty one are refused, and 2 and the prime less two are taken; exponent lengths for the three sizes.
- RSA for key exchange: a premaster encrypted and recovered, with and without blinding, and when the random padding comes out as zeros; a message too long to pad refused; then eleven ciphertexts that are not a proper premaster - another version, a wrong length, a value not below the modulus, a changed ciphertext, and six encodings each wrong in one place (made by the test with the public key) - every one giving the fallback bytes and nothing else.
- The signature of TLS 1.0 and 1.1: 36 bytes signed without a DigestInfo verify as such, not with a bit changed, not as 35 bytes; a signature with a DigestInfo is not accepted as one without.
- Key block layout: for four CBC suites in every version they exist in, the two directions have different keys and each side opens what the other seals and not its own; the suite table holds twenty-five suites, the nineteen legacy ones last, and no SHA-384 CBC, 3DES or RC4 suite.

Failure tip: inspect `src/proven/tls_legacy.c`, `src/proven/tls_dh.c`, the CBC functions of `src/proven/crypto_aes.c`, and the key-exchange functions of `src/proven/crypto_rsa.c`. `tests/test_unit_tls_legacy_vectors.h` is written by a private generator with Python's `hashlib` and `hmac` and another library's AES; the Diffie-Hellman primes in it are read from the library's source, so that part checks the arithmetic and not the constants (those were checked against two other implementations when they were added). Not covered: that opening a CBC record takes the same time whatever its padding - no test here can see that; a private check runs the code under a tool that reports any branch or index that depends on the decrypted bytes.

### `tests/test_unit_tls` - the TLS engine

Intent: verify the TLS state machine in both versions, both roles, with no network: a client connection and a server connection wired back to back in memory, the test carrying the bytes. The clock and the random source are the test's own, so every run is the same run.

Sub-checks:

- A self-signed identity: `proven_tls_self_signed` makes PEM the strict reader parses - version 3, Ed25519, self-issued, exactly the period asked for, the DNS name and both addresses, usable by a server and a client; a client holding the certificate connects to a server holding the key, and one second past the end it has expired; no names, an empty name, an empty period and a small buffer are refused; a period across 2050 round-trips through both time encodings.
- RSA keys for this side: a server with a 2048-bit RSA key in PKCS #8, and the same key as an `RSA PRIVATE KEY` file, complete a handshake and carry data; twelve more handshakes on one configuration all complete; a session from an RSA handshake is resumed; an RSA certificate with a P-256 key, the reverse, and an RSA certificate with another RSA key are `PROVEN_ERR_INVALID_STATE`; a 1024-bit RSA key is `PROVEN_ERR_INVALID_FORMAT`; with the size limit lowered by the test hook, RSA on both sides (a client certificate), and a certificate the test's issuer signed with RSA verified by one client and refused by another.
- Configuration: every option error is reported by `proven_tls_config_create` - missing anchors, `PIN_ONLY` without pins, a certificate without a key and the reverse, PEM with no certificate or no key, something labelled an RSA key that is not one (`PROVEN_ERR_INVALID_FORMAT`), a P-384 key (`PROVEN_ERR_UNSUPPORTED`), a key that is not the certificate's (`PROVEN_ERR_INVALID_STATE`), client authentication without anchors, an empty ALPN name; the `EC PRIVATE KEY` form is accepted; a config with no certificate cannot serve; a client needs a name of at most 253 bytes.
- Handshakes: the three cipher suites by the two server key types, each established with the server's key hash as the client verified it, and data of 1, 1,000, 16,384, 16,385, 100,000 and 150,000 bytes in both directions; the same handshake delivered a byte at a time; `NEED_MORE`, writes refused after `proven_tls_close`, data before a close delivered and then `PROVEN_ERR_EOF`, the other direction still open; a HelloRetryRequest to P-256 (whole and a byte at a time); key update from each side.
- ALPN: the server's first choice among the client's offers; nothing in common ends with `no_application_protocol` and `PROVEN_ERR_PROTOCOL` on both sides; a server with no list agrees on none.
- Verification: an unknown CA (`PROVEN_ERR_UNTRUSTED`, fault `NO_ISSUER`, alert `unknown_ca`, and the server told), another name (`PROVEN_ERR_NAME_MISMATCH`), an expired certificate and a clock before the validity period, each with its alert; after a failure every call is `PROVEN_ERR_INVALID_STATE`. Pins: `PIN_ONLY` ignores name, chain and dates; an unpinned key is refused with `PIN_MISMATCH`, also when the chain is good.
- Client certificates: required and given (the server holds the key hash and, when configured, the certificate); required and absent (`certificate_required`, and the client learns on its next read); requested and absent (let in, no key reported); a certificate under an unknown CA refused even when only requested.
- Resumption: a first handshake leaves a ticket; the second resumes on both sides with the original server key reported; through a HelloRetryRequest; not under a suite with another hash; not for another server name; a damaged ticket falls back to a full handshake; a good ticket with a wrong key behind it is a fatal `decrypt_error`; one second inside and one past the lifetime; a ticket under a key replaced twice; a server set to issue none.
- TLS 1.2: three ciphers by three kinds of server key (P-256, Ed25519, RSA), each established as TLS 1.2 with the suite for that key and data of 1 to 150,000 bytes each way, `proven_tls_key_update` unsupported; the key exchange on P-256 a byte at a time; which version each pairing of default, 1.2-only and 1.3-only configurations agrees on, the two that agree on none ending with `protocol_version`, and a reversed or unknown version range refused as `PROVEN_ERR_INVALID_ARG`; a server that answers 1.2 although it could speak 1.3 refused by a client that offered 1.3 (`illegal_parameter`) and accepted by one that did not; a peer without the extended master secret refused by each side (`handshake_failure`); a 1.2 hello carrying the fallback signal refused by a server that speaks 1.3 (`inappropriate_fallback`) and accepted by one that does not; a ChangeCipherSpec before the client's key exchange, and one in place of the server's certificate, refused (`unexpected_message`); a HelloRequest and a second ClientHello declined with the connection carrying on, and a peer that keeps asking given up on; client certificates required and given (P-256, RSA), required and absent, requested and absent, and an Ed25519 client key presenting none; tickets - resumed with the server's key remembered, a 1.2 session not used by a 1.3 handshake nor the reverse, not for another name, not past its lifetime, not from a server with resumption off, not under another server's keys; and a single-bit change at every fifth byte of each direction of a 1.2 handshake, with at most the unauthenticated record-version bytes going unnoticed (none did in the run recorded here).
- The legacy set: the nineteen legacy suites in TLS 1.2 and the eight with a SHA-1 MAC in 1.1 and 1.0, the CBC ones with and without encrypt_then_mac (65 handshakes), each agreed with data of 1 to 20,000 bytes each way; an Ed25519 server key with a CBC suite in 1.2 and refused to a 1.1 client; a minimum below 1.2 without the CBC bit, an unknown legacy bit and SSL 3.0 refused as `PROVEN_ERR_INVALID_ARG`; a default server refusing each of the nineteen (`handshake_failure`) and a default client refusing a server that picks one (`illegal_parameter`); one bit not standing for another; two sides that allow everything still agreeing on an ECDHE suite with an AEAD; a hundred pairs of version ranges, each giving the newest version both allow or `protocol_version`; a server that answers 1.0 though it speaks 1.2 refused by a client that offered 1.2, and a fallback signal in a 1.1 hello refused; a peer without the extended master secret refused by each side unless that side's configuration allows it, then connected with no session kept, a required client certificate making the server refuse outright and a requested one not asked for or not shown; P-256 and RSA client certificates in 1.1 and 1.0, and none or an Ed25519 one refused; no session left by a 1.1 or 1.0 connection, a 1.2 CBC session resumed under its suite with and without encrypt_then_mac and not offered by a default client; a bit changed in six places of a CBC record in each version and both MAC orders always answered `bad_record_mac`; a hundred bytes written as one record in 1.2 and 1.1 and two in 1.0; a Diffie-Hellman prime that is not known (`insufficient_security`) and a value of 1 from either side (`illegal_parameter`); an RSA premaster with another version in it and a ClientKeyExchange that encrypts nothing both failing only at Finished, with the same alert; what an idle TLS 1.0 CBC pair holds (at most 2,500 bytes, printed); and a single-bit change at every fifth byte of a TLS 1.0 handshake, with the record-version bytes the only ones that go unnoticed (three, printed).
- Memory: an idle client and server together hold at most 2,200 bytes (measured and printed), a TLS 1.2 pair exactly the same, and a destroyed pair has freed everything.
- A lying wire: a single-bit change at every seventh byte of each direction of the handshake never leaves both sides established; an HTTP request, a record claiming 65,535 bytes, application data before a handshake, a fatal alert, and a plaintext handshake record on an established connection are each refused with the stated alert; a run of ChangeCipherSpec records is tolerated briefly and then refused; key-update requests (forged under the peer's real keys) are answered, and refused once the unsent answers pass the output limit.
- RFC 8448 sections 3 and 7 through the client: given the trace's ClientHello and X25519 key, the client accepts the server's records and produces exactly the client's - the Finished record, the application data record, the close - byte for byte.

Failure tip: inspect `src/proven/tls13.c`, `src/proven/tls_config.c` and `src/proven/tls_issue.c`. No key is stored in the tree: `tests/test_unit_tls_pki.h` derives keys from a fixed pattern and issues the certificates when the test starts, with the library's internal certificate writer. The RFC's records are in `tests/test_unit_tls_vectors.h`, written by a private generator. Not covered here: the server role against a published trace (RFC 8448's server key is RSA, which this version cannot sign with) - that is checked against OpenSSL and GnuTLS in a private interoperability run, as are both roles for every suite and group in both versions; `record_size_limit` from a peer; a ServerKeyExchange or CertificateVerify actually signed with SHA-1 (no SHA-1 scheme is offered or accepted, and no peer at hand will send one unasked).

### `tests/test_unit_crypto_rsa` - RSA signing

Intent: verify the private half of RSA - the constant-time exponentiation, signing by CRT with its check, blinding, the two encodings, reading and making keys - with keys the test makes when it starts, from a fixed seed. Internal code, reached through `src/proven/proven_internal_crypto.h`.

Sub-checks:

- The exponentiation for secret exponents: twenty-four random powers modulo four primes agree with the plain square-and-multiply, exponents with leading zero limbs included; the zeroth power is one and the first is the base; a modulus of 64 limbs is within its bound and one of 67 is refused.
- For a 2048-bit key, and (with the size limit lowered by the test hook) a 1024-bit one, for each of SHA-256, SHA-384 and SHA-512: a PKCS #1 v1.5 signature is accepted by the verifier, is what a plain exponentiation by `d` gives, is the same signature when blinded and again with the pair squared, and does not verify for another digest; a PSS signature is made and verifies with its salt length, another salt gives another valid signature, and an empty salt is not taken for one with a salt; PSS with a salt the modulus has no room for is refused.
- Forty squarings on, the blinding pair still cancels exactly.
- A wrong `dp`, `dq` or `qinv` yields no signature and nothing is written; a digest of the wrong length and a hash that does not exist are refused.
- An `RSAPrivateKey` written and read back is the same key; cut short, or with a byte after it, it is not a key; a modulus that is not the product of the two primes is refused; a 1024-bit key is not accepted for signing.
- Making a key: sizes outside 1024 to 4096 bits, sizes that are not a multiple of 64, and no random source are refused; a key has exactly the size asked for and two different primes; its `d` undoes its `e`.

Failure tip: inspect `src/proven/crypto_rsa.c` (`rsa_private`, `rsa_garner`, `proven_crypto_rsa_blind_make`) and `proven_crypto_mp_pow_ct` in `src/proven/crypto_mp.c`. Keys and random bytes come from fixed seeds, so a failure repeats exactly. Agreement with another implementation, and the absence of secret-dependent branches, are checked outside the suite and are not this test's.

### `tests/test_unit_tls_net` - TLS over sockets: the transport, HTTPS and WebSocket

Intent: verify the TLS engine carried by real connections on the loopback interface: the transport wrapper by itself, then the HTTP client and server with TLS in both handler models.

Sub-checks:

- The transport wrapper: a client and a server handshake over a socket with the server verified by its IP address; 1 to 150,000 bytes echoed intact; a read with nothing to read ends at its deadline; a close seen by the peer as `PROVEN_ERR_EOF`; a connection cut without a TLS close seen as `PROVEN_ERR_RESET`; another server name is `PROVEN_ERR_NAME_MISMATCH` and the server's handshake ends with the alert; a handshake nobody answers ends at its deadline; `proven_tls_transport_conn` on a TLS and on a plain transport.
- HTTPS, with handlers on the loop's thread and again on workers: three GETs over one kept connection; a 200,000-byte request body and a 300,000-byte response body; a `wss://` connection with a text and a 70,000-byte binary message echoed and a clean close; two pipelined requests in one TLS record both answered; a client that connects and says nothing does not delay another request and is closed after the head timeout; a plain `http://` request to the TLS port fails and reaches no handler; an `https://` URL on a client with no wrap is still `PROVEN_ERR_UNSUPPORTED`; a server whose CA the client does not hold is `PROVEN_ERR_UNTRUSTED` with nothing sent.

- TLS 1.2 over sockets: the transport-wrapper cases and the HTTPS and `wss` cases (handlers on workers) once more, with a client configuration limited to TLS 1.2.
- An RSA key signing on several threads: twenty-four in-memory handshakes against one server configuration with a 2048-bit RSA key, from four threads at once, all complete and are verified by their clients - the key's blinding pair is shared and replaced by every signature; and a page is fetched over HTTPS from a server with that key.

Failure tip: inspect `src/proven/tls_transport.c` and the `tls` branches of `src/proven/http_server.c`. This test uses the wall clock (its certificates, issued at start by `tests/test_unit_tls_pki.h`, are valid 2026 to 2036) and the operating system's random source. Not covered: client certificates through the HTTP server, and resumption through the transport wrapper (both are engine paths `test_unit_tls` covers).

### `tests/test_unit_loop` - the event loop

Intent: verify the loop of `loop.h` by itself - timers, functions posted from other threads, socket interest, stopping - before anything is built on it.

Sub-checks:

- Timers: nothing fires before its time; three fire in the order of their times and a cancelled one never does; cancelling twice is harmless; a fired timer is no longer set; a timer cancelled by another that was due with it does not fire; a timer that sets itself again from its callback repeats; a timer set again waits for its new time and fires once with what it was last given; five timers that came due while the loop was not polled fire together in the order of their times; with a clock the test moves, timers at 10, 20, 40 and 100 seconds - beyond one turn of the 16.4-second wheel - each fire once, in order, none before its time and each within one step of it, all four fire in one round after a jump of 200 seconds and none twice, and a far timer that was cancelled never fires; a thousand timers set at once, spread over 300 ms, each fire once and none early.
- Posts: five functions posted from the loop's thread run in the order posted; two other threads post a hundred each, and all two hundred run on the loop's thread, each thread's hundred in the order that thread posted them; no function and no loop are `PROVEN_ERR_INVALID_ARG`.
- Stopping: `proven_loop_stop` from a posted function and from another thread makes `proven_loop_run` return `PROVEN_OK`; a loop with nothing to do sleeps until then.
- Sockets: interest added, changed and removed, with a socket watched for nothing left alone; a registration added twice is `PROVEN_ERR_INVALID_STATE`; of two sockets ready in one round, the one the other's function removes is not delivered; removing what is not registered is harmless; a closed peer is delivered as readable; a handle that is not one is `PROVEN_ERR_INVALID_ARG`; `proven_loop_io_count`; the scratch buffer is 64 KiB and the loop itself writes nothing to it.
- `proven_loop_destroy` with a due timer set and a socket watched calls neither; null is accepted.

Failure tip: inspect `src/proven/loop.c` - `loop_timers_advance` for the wheel, `loop_run_tasks` for posts, `proven_loop_poll` for the order of one round. A timer that fires early is a defect; one that fires late on a loaded machine is not.

### `tests/test_unit_http_event` - the event-driven HTTP server

Intent: verify the server of `http_event.h` over the loopback interface, plain and again over TLS, through every outcome an exchange can have, and measure what an idle connection holds.

Sub-checks:

- Configuration: no `on_request`, no loop, and a TLS configuration with no certificate are each `PROVEN_ERR_INVALID_ARG`.
- Answers: at once; later from a timer; later from a worker thread through `proven_loop_post`; 204 and `HEAD` without a body; a header the server writes itself refused with `PROVEN_ERR_INVALID_ARG`; kept connections; HTTP/1.0; pipelined requests served in order.
- Bodies and backpressure: an upload delivered in pieces; a handler that pauses the body and resumes it from a timer still receives every byte; a 16 MB download to a client that reads nothing until a write has been refused arrives intact, was resumed through `on_writable`, and never held more than the output limit and one piece; 3 MB sent chunked; `Expect: 100-continue`; a response sent before the body arrived.
- Refusals: 400, 413, 431, 501 and 505 each with a close; bad chunk framing and a body shorter than its `Content-Length` end the exchange with an error.
- Leaving: a client that disappears during a response ends the exchange with an error; `proven_http_stream_abort` closes with nothing sent and `on_done(PROVEN_ERR_RESET)`; half a request head gets 408 after the head timeout; destroying the server under an unanswered request ends it with `PROVEN_ERR_RESET`.
- Every case above over plain HTTP, over TLS, and a third time with clients limited to TLS 1.2.
- Accounting: `on_done` was called exactly once for every `on_request`; many connections that have each made a request stay open and close when their clients do.
- Memory: three hundred idle plain connections hold less than 512 bytes of heap each (448 measured on x86-64 Linux), and everything is given back when they close and when the server is destroyed.
- The idle timeout: an answered connection that then says nothing is closed by the server after `idle_timeout_ms`, not before.

Failure tip: inspect `src/proven/http_event.c` - `ev_process` for input, `ev_flush` and `ev_progress` for output and what follows it, `ev_arm` for which timer is running, `ev_finish` for the end of an exchange. The backpressure case waits for a refusal rather than for a fixed time; a failure there that prints `download: got N of M` is a stalled response, which is a lost wake-up in `proven_http_stream_write` or `ev_progress`.

### `tests/test_unit_http_event_loops` - several loops behind one port

Intent: verify `proven_http_event_server_adopt`, the call by which a connection accepted on one thread is given to a server on another thread's loop - and that the pattern built on it keeps every connection on one thread.

Sub-checks:

- What adopt refuses, on one thread with the loop driven by hand: no server and no connection are `PROVEN_ERR_INVALID_ARG`; a connection that is not open is `PROVEN_ERR_INVALID_STATE`; an adopted connection leaves the caller's value closed and the server with one connection, and a request on it is answered; at `max_connections` adopt is `PROVEN_ERR_BUSY` and the connection is still the caller's; after `proven_http_event_server_stop_listening` it is `PROVEN_ERR_INVALID_STATE`; a server that listens again takes connections again.
- Three loops on three threads, each with a server that has no listener, and a fourth thread that accepts and deals connections in turn by posting to the loops: ninety requests on ninety connections are all answered, thirty by each loop; every accepted connection was adopted and none refused; every request was handled on the thread of the loop it was dealt to.
- The same over TLS, each connection's handshake done by the loop it was dealt to.
- Taking it apart: the acceptor is stopped and joined first, then the loops, then - after one last round of each loop - the servers.

Failure tip: inspect `proven_http_event_server_adopt` and `ev_take` in `src/proven/http_event.c`. Run it under ThreadSanitizer: a report there means something of a connection was touched from the accepting thread. A count that is not thirty each means connections were not dealt in the order they were accepted.

### `tests/test_unit_ws_event` - WebSocket on the event-driven server

Intent: verify `ws_event.h` over the loopback interface, plain and again over TLS, with the library's blocking WebSocket client where a client that behaves is wanted and raw sockets where one that does not is.

Sub-checks:

- Accepting: a WebSocket is accepted from inside `on_request`, and `on_done` was called for the request it began as; a configuration with no `on_message`, a subprotocol that is not a token, and no stream are `PROVEN_ERR_INVALID_ARG`; a second accept on the same stream is `PROVEN_ERR_INVALID_STATE`; a plain request is `PROVEN_ERR_NOT_FOUND`, an upgrade for version 12 `PROVEN_ERR_UNSUPPORTED` and one with no key `PROVEN_ERR_INVALID_FORMAT`, each with nothing sent and the handler's own answer (400, 426, 400) delivered; an ordinary page is served by the same server; the subprotocol the server chose is named in the answer.
- Messages: text, an empty binary message and 150,000 bytes are echoed intact, in pieces; a message sent in three fragments is echoed as one, and one the server sends in two pieces arrives as one; a ping is answered with a pong.
- Sending: 6,000 messages pushed at a client that reads nothing until a send has been refused all arrive, whole and in order, with sends refused (`PROVEN_ERR_AGAIN`) and resumed through `on_writable`; a piece that is not first outside a message, a new message inside one, and a send after a close was sent are `PROVEN_ERR_INVALID_STATE`; a ping of 126 bytes is `PROVEN_ERR_OUT_OF_BOUNDS`; the user pointer and peer address are kept.
- Closing: a close from the client is answered and reported with its code (1000, 1001) and `PROVEN_OK`; one with no code is answered with one and reported as 1005; a close from the server (4001 with its reason, and 1000 after the push) is seen by the client and reported with `PROVEN_OK`; a code that may not be sent goes out as 1000 and a second close changes nothing; a close the client never answers ends after the close timeout with the code and `PROVEN_ERR_TIMEOUT`, and a client that answers it with a stream of pings instead is ended all the same; `proven_ws_stream_abort` ends with no close frame and 1006, `PROVEN_ERR_RESET`.
- Liveness: a silent client that answers pings stays connected for 700 ms of a 100 ms ping interval and still works; one that does not is sent a ping and dropped after the pong timeout with 1006, `PROVEN_ERR_TIMEOUT`.
- Bad input: an unmasked frame is answered with close 1002 (`PROVEN_ERR_INVALID_FORMAT`), a frame announcing 300,000 bytes with 1009 (`PROVEN_ERR_OUT_OF_BOUNDS`), text that is not UTF-8 with 1007 (`PROVEN_ERR_INVALID_ENCODING`); a client that vanishes is 1006, `PROVEN_ERR_RESET`.
- Accounting: every accepted connection was closed exactly once; `on_done` was called once for every `on_request`, upgraded or not; destroying the server ends an open connection with `on_closed(1006, PROVEN_ERR_RESET)` and the client sees it end.
- Memory: three hundred idle plain WebSocket connections hold less than 1 KiB of heap each (928 measured on x86-64 Linux), and everything is given back when they close and when the server is destroyed.

Failure tip: inspect `src/proven/ws_event.c` - `ws_on_input` for what arrives, `ws_on_timer` for liveness and the close timeout, `ws_end` and `ws_finish` for the end. The connection underneath is `src/proven/http_event.c` in its `EV_RAW` state, reached through `src/proven/proven_internal_http_event.h`. The cases wait for the handler's own counters rather than for a fixed time: a client has its 101 before the handler's next line has run.

### `tests/test_unit_http_event_client` - the event-driven HTTP client

Intent: verify `http_event_client.h` on one thread - the client and the event-driven server it talks to sit on the same loop, driven by hand - plain and again over TLS, and against answers the test scripts itself on a listener.

Sub-checks:

- What `start` refuses, with no callback made: no loop, no `on_done`, a relative URL, a scheme that is not `http` or `https`, `https` with no TLS configuration, credentials in the URL, `CONNECT`, a method that is not a token, a header the client writes itself, a request head past `max_head_bytes` (`PROVEN_ERR_OUT_OF_BOUNDS`), and a name with no address.
- Responses: a started request returns at once with nothing called; `GET` delivers the head once, the body with its last piece marked, then `on_done(PROVEN_OK)` once; the query goes with the path; `HEAD` and 204 make no body call; an answer 50 ms later; an error status is a response; 3 MB arrive in pieces with nothing delivered between pause and resume; a chunked body arrives with its framing taken off.
- Request bodies: one held in memory is sent whole; a 16 MB body written before the connection exists is taken up to the limit and refused, then resumed through `on_writable` until the server has received exactly those bytes; a body of unknown length is sent chunked; writing past the promised length is `PROVEN_ERR_OUT_OF_BOUNDS`, ending short of it ends the request with `PROVEN_ERR_INVALID_FORMAT`, and a request with no body to write refuses write and end unharmed.
- The address apart from the name: the request connects to the address and says the name in `Host`, and over TLS that name is what the certificate is checked for - a name it is not for ends with `PROVEN_ERR_NAME_MISMATCH` and the request never sent.
- Time and endings: no answer within the response timeout is `PROVEN_ERR_TIMEOUT`; abort calls `on_done(PROVEN_ERR_RESET)` inside the call; two hundred requests (forty over TLS) in flight at once each complete once with their own body; destroying the client ends a request with `PROVEN_ERR_RESET`; the server saw every exchange end.
- Servers that answer badly: a body with no length ends whole when the server closes; a body cut short of its `Content-Length`, a chunked body with no final chunk, and a head that never finishes are `PROVEN_ERR_RESET`; interim responses are passed over and nine in a row are `PROVEN_ERR_INVALID_FORMAT`; an unasked 101 is `PROVEN_ERR_UNSUPPORTED`; something that is not HTTP and two lengths that disagree are `PROVEN_ERR_INVALID_FORMAT`; a head past `max_head_bytes`, a body announced past `max_body_bytes` and a chunked body that grows past it are `PROVEN_ERR_OUT_OF_BOUNDS`.
- A refused connection is `PROVEN_ERR_REFUSED` from `start` or from `on_done` and not both; a server the client does not trust ends with `PROVEN_ERR_UNTRUSTED` and the request never sent.

Failure tip: inspect `src/proven/http_event_client.c` - `cl_process` for the response and the end-of-stream branch after its loop, `cl_flush` and `cl_progress` for the request, `cl_arm` for the time limits, `cl_kill` for the end. A request that never ends after the server closed is that branch: it once waited for the stash to be empty, which a half-received head never is.

### `tests/test_unit_hash_legacy` - legacy digests: SHA-1 and MD5

Intent: verify the two digests of `hash_legacy.h` are the SHA-1 and the MD5 that formats name, bit for bit, against vectors that exist outside this repository.

Sub-checks:

- SHA-1: the FIPS 180-4 vectors - empty, `"abc"`, the 56-byte example, a sentence, and one million `a` streamed in 997-byte chunks.
- MD5: all seven messages of the RFC 1321 test suite.
- Both, at 55, 56, 57, 63, 64, 65, 119 and 120 bytes - each side of the padding boundary and of the block boundary - against the digests `sha1sum` and `md5sum` print.
- Both: a 200-byte input cut in two at every one of its 201 positions, and fed one byte at a time, gives the one-shot digest.
- The hex spellings are 40 and 32 lowercase characters, NUL-terminated; a `{NULL, 0}` view is the empty message.
- The WebSocket accept key of RFC 6455 section 1.3, from `proven_sha1` and `proven_base64_encode`.

Failure tip: inspect `src/proven/hash_legacy.c`. A boundary-length failure points at `legacy_pad` (the `0x80` byte, the zero fill, the byte order of the bit length); a vector failure at one compression function.

### `tests/test_unit_job` - job system

Intent: verify the hosted worker-thread job system retains wakes across idle
parking, tolerates stale permits created by an external consumer, executes
submitted jobs exactly once, and shuts down cleanly.

Sub-checks:

- Initializes a job system with four workers and a 1024-entry queue.
- Lets every worker park before the first submission, then verifies later
  submissions wake them without losing work.
- Dispatches 1000 jobs.
- Uses atomics to count total executed jobs.
- Uses indexed atomics to detect duplicate or missing job execution.
- Closes an empty system to prove every parked worker receives a final wake.
- Holds the only worker inside one job while the calling thread drains four
  later jobs, proving stale retained permits neither duplicate work nor block
  shutdown.
- RFC-0009 X-003: with the only worker held busy, a 64-slot queue takes 64 group jobs and then `proven_job_group_submit` says `PROVEN_ERR_AGAIN` (not `false`) without counting the refused job; `proven_job_group_wait` returns only after the waiting thread has run all 64 itself, and their plain writes are visible; then, with four free workers, 2,000 group jobs finish on other threads and every plain write is visible after the wait (under `./nob tsan`, a decrement placed before the job runs is reported as a race); after close, `submit_ex` and `group_submit` say `PROVEN_ERR_INVALID_STATE` and the count is unchanged; a NULL system is `PROVEN_ERR_INVALID_ARG`.

Failure tip: inspect `src/proven/job.c` and `platform/proven_sys_thread.c`. For races, run `./nob tsan`. Check admission state, sequence counters, queue claim/commit ordering, and shutdown wakeups.

### `tests/test_unit_fs_position_and_sync` - file position, positional I/O, and durability

Intent: verify `seek` / `tell` / `truncate` / `pread` / `pwrite` / `sync`, and the contracts around them.

Sub-checks:

- `SEEK_SET` / `SEEK_CUR` / `SEEK_END` land where arithmetic says, and a read afterwards sees the right byte.
- `pread` and `pwrite` do **not** move the file position - the whole point of positional I/O.
- `pread` past the end is `PROVEN_ERR_EOF`, not a zero-byte success.
- `truncate` shortens and grows (zero-filling), and does not move the position either.
- `proven_fs_sync` succeeds on a writable file; `sync_dir` either works or returns `PROVEN_ERR_UNSUPPORTED` rather than silently returning OK where it does nothing.
- A FIFO seek is `PROVEN_ERR_UNSUPPORTED`, not `PROVEN_ERR_IO`: not being seekable is a property of a pipe, not a failure.
- `proven_fs_write_file_durable` round-trips, preserves the target's permissions, and leaves no temp file behind.

Failure tip: inspect `proven_fs_seek` and friends in `src/proven/fs.c`, and the libc calls behind them in `platform/proven_sys_io.c`.

### `tests/test_unit_list` - intrusive list

Intent: verify zero-allocation intrusive list behavior and container-of usage.

Sub-checks:

- Initializes an empty sentinel list.
- Appends embedded nodes from caller-owned structs.
- Iterates in reverse and sums payload values.
- Removes nodes while iterating.
- Reads first and last entries through container-of style access.

Failure tip: inspect `include/proven/list.h`. Intrusive lists do not own node storage. A failure usually means `next`/`prev` linkage was corrupted or a detached node was reused incorrectly.

### `tests/test_unit_map` - hash map

Intent: verify open-addressing map behavior for integer and U8 string keys, including tombstones, growth, and scratch allocation.

Sub-checks:

- Creates an integer-key map and confirms capacity normalization.
- Inserts, retrieves, updates, and deletes entries.
- Confirms deletion reduces live length and leaves tombstones usable.
- Creates a U8 string-key map and inserts enough entries to force growth.
- Verifies all expected string keys remain reachable after rehash.
- Tracks scratch allocation during safe rehash paths.
- Walking every entry (RFC-0009 X-001): an empty map ends at once; over 1,000 integer keys one walk sees each key once with its value while removing the odd ones and updating the even ones through `proven_map_set`, `proven_map_len` follows, and a second walk sees exactly the updated survivors; adding keys until the map grows makes the next step `PROVEN_ERR_INVALID_STATE`; owned string keys come back intact; a NULL iterator is refused.

Failure tip: inspect `src/proven/map.c`. Check hash/equality callbacks, tombstone reuse, threshold calculation, and whether borrowed keys or value pointers are being used after rehash.

### `tests/test_unit_map_owned_key` - map owned-key storage

Intent: verify owned U8 keys are duplicated into map storage, survive source-buffer mutation, and free their copied bytes on remove and destroy.

Sub-checks:

- Creates a U8 owned-key map.
- Inserts a key from mutable source storage and confirms the lookup survives source-buffer mutation.
- Removes the entry and confirms the copied key bytes are released once.
- Inserts enough owned keys to force rehash and confirms every copied key still resolves after growth.
- Destroys the map and confirms all owned key allocations have matching frees.

Failure tip: inspect the owned-key duplication, cleanup, and rehash migration paths in `src/proven/map.c` if a key is lost, leaks, or follows a mutated source buffer.

### `tests/test_unit_map_keyed` - HashDoS-resistant string keys

Intent: verify a default string-key map hashes with a keyed function an attacker cannot predict, that a trusted map keeps the fast unkeyed FNV on purpose, and that both place and find keys correctly.

Sub-checks:

- A default (`proven_map_create`) string-key map has `trusted_keys == false`, and `proven_map_hash` differs from unkeyed `proven_hash_bytes` for essentially every key - a keyed hash agreeing with FNV on one key is coincidence, on all of them is FNV.
- A trusted map (`proven_map_create_trusted`) has `trusted_keys == true` and `proven_map_hash` equals FNV-1a, which is the fast path it opts into.
- Both kinds insert 500 distinct string keys, read them all back, remove half, and still resolve the survivors - a keyed hash that broke lookups would be safe and useless.
- A malformed `{NULL, size>0}` key is hashed as empty on both kinds rather than dereferenced (the trusted path once used a duplicate internal FNV that read through the NULL).
- On 64-bit targets: 256 integer keys built with the public inverse of the bit-mix finaliser all land in bucket 0 of 4096 on a trusted map (the attack is real, and the trusted hash is unchanged), spread on a default map (at most 4 in bucket 0), and are all found again (RFC-0009 S-001).

Failure tip: inspect the hash selection and the per-process key in `src/proven/map.c`. The written-first assertion is that the default must NOT equal FNV; a stub that still uses FNV lands it red.

### `tests/test_unit_memory_slicing` - memory slicing

Intent: verify owned memory can be exposed as immutable and mutable views and sliced without losing pointer or length identity.

Sub-checks:

- Creates raw byte storage and wraps it in the owned memory abstraction.
- Converts owned memory to a read-only view and checks pointer, size, and byte contents.
- Converts owned memory to a mutable view and checks that writes through the mutable view are visible through the original buffer and read-only view.
- Slices read-only and mutable views and checks offset, length, and shared backing storage.

Failure tip: inspect `src/proven/memory.c` and `include/proven/memory.h`. Most failures here are offset arithmetic mistakes, accidental copies instead of views, or unchecked slice preconditions used with the wrong ranges.

### `tests/test_unit_memory_views` - memory byte views

Intent: verify the fixed-width integer aliases, semantic pointer/offset types, alignment helpers, and the first memory slice/view contracts.

Sub-checks:

- Confirms `proven_u8`, `proven_i8`, `proven_u16`, `proven_i16`, `proven_u32`, `proven_i32`, `proven_u64`, and `proven_i64` have the expected byte widths.
- Confirms semantic pointer/size/offset types are usable for memory calculations.
- Exercises default alignment logic.
- Builds a memory core structure and confirms pointer and size fields remain exact.

Failure tip: start in `include/proven/types.h`, `include/proven/align.h`, and `include/proven/memory.h`. A width failure usually means a typedef or platform feature branch changed. An alignment failure usually means the helper no longer implements power-of-two alignment correctly.

### `tests/test_unit_mmap` - memory mapped files

Intent: verify hosted memory mapping rejects invalid flags and exposes file bytes through mapped memory.

Sub-checks:

- Creates a test file with known content.
- Rejects invalid mmap flag combinations: zero flags, private plus shared, zero protection, unknown protection bits, and misaligned offsets.
- Maps a file range.
- Verifies mapped bytes match expected content.
- Modifies mapped memory and syncs/unmaps it.
- Reads the file back to verify the modification reached disk when mapping mode requires it.

Failure tip: inspect `src/proven/mmap.c` and `platform/proven_sys_fs.c`. Pay special attention to offset alignment, map length, file handle lifetime, and unmap ownership.

### `tests/test_unit_pool` - pool allocator

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

### `tests/test_unit_random` - OS randomness

Intent: verify the OS CSPRNG is actually wired up - it succeeds on a hosted platform, fills every byte the caller asked for, does not repeat, and is not trivially structured. None of these prove cryptographic strength (nothing a unit test does could), but each catches a real, shipped failure mode.

Sub-checks:

- `proven_random_bytes` succeeds on a hosted platform, and neither the whole buffer nor its tail is left zero (a stub, a wrong length, or an ignored error leaves zeros).
- Two draws differ (a fixed or unseeded generator repeats), and the bytes are neither all-equal (a memset) nor a simple counter.
- `len == 0` is a successful no-op, and two `proven_random_u64` draws differ.
- `proven_random_u64_checked` succeeds and draws with the platform source, refuses a NULL out, and with a failing source installed returns false with `out` 0 while `proven_random_u64` silently returns 0 (RFC-0009 S-002).
- Three job workers make 300,000 draws while the main thread keeps installing one of two sources, each of which checks it was handed its own context: no draw sees a mismatched pair, and under `./nob tsan` there is no race report (RFC-0009 S-004). Installing `NULL` restores the platform default.

Failure tip: inspect `platform/proven_sys_random.c`. A failure here is a missing or wrong OS entropy call - the `getrandom` guard keys on `GRND_NONBLOCK` from `<sys/random.h>`, not on a syscall number that was never included.

### `tests/test_unit_rng` - randomness by use case

Intent: verify the two generators against the standard each is judged by - xoshiro256** on being reproducible and non-degenerate, ChaCha20 on being *actually ChaCha20* - and the helpers on being unbiased where `% n` is not.

Sub-checks:

- xoshiro256**: the same seed replays the same 1000 words; a different seed does not. The seeds callers actually pass (0, 1, 2) are not degenerate - the bit balance over 512 output bits is near half, which a raw-counter seed fails badly. The first state word for seed 0 is SplitMix64(0), the published constant, so the expansion is checked against something the library did not invent.
- ChaCha20: the first 64 bytes of keystream match the standard byte for byte. The expected block came from OpenSSL, which was itself first verified to reproduce RFC 8439 section 2.4.2's official ciphertext - a property test cannot establish that something *is* ChaCha20, and this is the generator that guards secrets. The keystream is also chunking-independent: drawn in pieces of 1, 7, 64, 3, 61 it equals the same bytes drawn at once, which is where a block-boundary bug hides.
- `proven_rng_below`: every draw is strictly below the bound, and 70,000 draws over 7 buckets land near a seventh each. Bound 0 is 0; bound 1 is 0.
- `proven_rng_range`: inside [lo, hi] inclusive; INT64_MIN..INT64_MAX (a span of 2^64-1) neither overflows nor hangs; an inverted range returns `lo`.
- `proven_rng_f64`: always in [0, 1), never 1.0.
- `proven_rng_shuffle`: a permutation at several sizes and element widths (including a 200-byte element, which catches a swap that assumes a word); count 0 and 1 are no-ops; all six orderings of three elements come up, which a biased shuffle cannot manage.
- The trait: a zero-initialised `proven_rng_t` is inert (0, and a fill that fills nothing) rather than a crash; a valid one fills the tail of a length that is not a multiple of 8.

Failure tip: inspect `src/proven/random.c`. A ChaCha keystream mismatch means a rotation, round count, or word order is wrong - it is not ChaCha20 and must not hold a key.

### `tests/test_unit_ring` - bounded ring

Intent: verify fixed-capacity FIFO semantics, wraparound, full/empty detection, and overflow guards.

Sub-checks:

- Creates a ring and verifies initial head, tail, length, and capacity.
- Pushes and pops values in FIFO order.
- Fills the ring and verifies extra push is rejected.
- Pops across physical wraparound boundaries.
- Verifies empty pop rejection.
- Checks integer-overflow bounds for capacity calculations.

Failure tip: inspect `src/proven/ring.c`. The first suspects are head/tail modulo math, `len` updates, and full-vs-empty boundary handling.

### `tests/test_unit_scan` - scanner

Intent: verify scanner parsing for integers, floats, tokens, skip-until operations, format scanning, and fixed-width integer destinations.

Sub-checks:

- Scans unsigned and signed integers.
- Scans hexadecimal integers with `proven_scan_u64_hex` / `proven_scan_i64_hex` and `{:x}` / `{:X}` in a format: both cases, `0x` taken only before a digit, overflow at 2^64 and at the signed limits with the cursor restored, a sign or a `0x` at the end of the view flagged as needing more input, integer widths range-checked, and `{:x}` on a double refused with `PROVEN_ERR_INVALID_FORMAT`.
- Scans positive, negative, and exponent-style floating-point values.
- Scans tokens and string views.
- Skips until substrings and numbers.
- Confirms not-found behavior.
- Scans using `{}` and spec-style format patterns.
- Scans native and fixed-width integer aliases.
- A compound-literal view wrapped in parentheses passes through `proven_scan_fmt` as one argument and scans (the form the headers and manual chapter 1 document for every view-taking macro, RFC-0009 X-008).

Failure tip: inspect `src/proven/scan.c`. The most common bugs are cursor advancement on failure, overflow detection, and accepting invalid trailing characters.

### `tests/test_unit_scan_f64_accuracy` - float scanner accuracy

Intent: verify float scanning preserves exact small values, signed zero, a round-trip style decimal token, exponent extremes, and cursor restoration on malformed input.

Sub-checks:

- Confirms exact bit patterns for `0.0`, `-0.0`, `1.0`, `-1.0`, `0.5`, `0.1`, and `123456789.0`.
- Confirms the parsed bits for `0.30000000000000004` match the source literal.
- Confirms `1.7976931348623157e308`, `2.2250738585072014e-308`, and `4.9e-324` remain finite and stable.
- Confirms `1e309` reports `PROVEN_ERR_OVERFLOW`.
- Confirms malformed input restores the scanner cursor to its original position.
- `tests/test_unit_scan_f64_bounds` covers underflow-to-signed-zero spellings, the true-min half threshold, subnormal-boundary spellings around DBL_MIN, and overflow boundary behavior at the same parser boundary.

Failure tip: inspect `src/proven/scan.c`, especially the decimal mantissa accumulation, exponent scaling, and final finite-value check. If a malformed token leaves the cursor advanced, inspect the failure-atomic rollback path first.

### `tests/test_unit_stream` - writers and readers

Intent: verify one piece of code can move bytes without knowing where they go, and that the sinks refuse rather than truncate.

Sub-checks:

- The same serializer writes into an owned string, a fixed caller buffer, and a file, and all three agree byte for byte.
- A full fixed buffer returns `PROVEN_ERR_OUT_OF_BOUNDS` and records `overflowed` - it does **not** truncate, and a refused write is not partially applied.
- A buffered writer holds bytes until flushed, auto-flushes when it wraps without losing any, and passes a chunk larger than the whole buffer straight through.
- `proven_reader_read_line` handles `\r\n`, empty lines, and **returns the final line even with no trailing newline**.
- A line longer than the reader's buffer is `PROVEN_ERR_OUT_OF_BOUNDS`, not a silently truncated line.
- End of input is `PROVEN_ERR_EOF`, never a zero-byte success.
- Lines arriving one byte per read (RFC-0009 P-104): a CRLF split across arrivals, an empty line, a raw read of three bytes between lines, a line that is completed after a compaction, and a final unterminated line - each exactly once. Then a source whose second read brings a newline and the next two lines at once: each comes back separately (a search position not reset after a line merged them).

Failure tip: inspect `src/proven/stream.c`. Buffering uses caller-supplied memory: there is no hidden global state and no allocation the caller did not ask for.

### `tests/test_unit_stream_u16` - UTF-16 text through writers, readers and the formatter

Intent: verify u16 text goes out through every formatter sink and any writer in each encoding, and comes back line by line from each encoding, with a character split across reads carried.

Sub-checks:

- `PROVEN_ARG` on a `proven_u16str_view_t` renders UTF-8 into a string, a writer and stdout; width counts UTF-8 bytes; an unpaired surrogate fails the format.
- `proven_writer_write_u16` writes UTF-8, UTF-16LE and UTF-16BE byte-exact regardless of host byte order, with the BOM only on request; malformed text writes nothing; `PROVEN_TEXT_AUTO` is refused for writing.
- `proven_u16_reader_t` reads all three encodings through a source that returns one byte per read, explicitly and through `AUTO` with each BOM; no BOM reads as UTF-8; an explicit encoding delivers a BOM as U+FEFF.
- A line that exactly fills the buffer is a line; one unit longer is `OUT_OF_BOUNDS` and stays so; a surrogate pair is never split at the buffer edge; `read` never ends on a high surrogate.
- Malformed input, a lone surrogate, an odd trailing byte and a source ending mid-character each stop the reader with `INVALID_ENCODING` after the valid first line.
- A file round-trips in each encoding through `proven_sysio_u16_lines_open(AUTO)`, and the wrapper may be moved between calls.
- For 2,400 bytes of UTF-16LE with a surrogate pair every seven units - valid, with a lone low surrogate mid-text, and ending in a lone high surrogate - reading with every `cap` from 2 to 9 delivers the same units and ends with the same error as `cap` 512 (RFC-0009 P-103: the reader validates only `cap + 1` units per call; a window one unit too small is caught).

Failure tip: inspect the UTF-16 section of `src/proven/stream.c` (`u16r_decode`, `proven_u16_reader_read_line`) and `render_u16` in `src/proven/fmt.c`. A failure only with the one-byte source is the carry between reads.

### `tests/test_unit_sysio_console` - UTF-8 text through a UTF-16 console

Intent: verify the Windows console edge (`src/proven/proven_internal_console.h`) against a fake UTF-16 console, on every host.

Sub-checks:

- A text written as two writes split at every byte offset, one byte at a time, and through a 5-byte buffered writer arrives as exactly the right code units.
- Malformed UTF-8 stops the write after the valid part; a character still open when the text ends is reported by finish (the flush); a failed console write reports the input bytes that went out, not the units.
- Console reads of 1, 2 or 3 units into destinations of 1 to 10 bytes return exactly the UTF-8 of the console text, and so does the line reader over it, with CR LF removed.
- Ctrl+Z as the first unit of a read is end of input; a lone low surrogate, and a high surrogate followed by the end, are refused after the valid text before them.
- On POSIX the standard streams are never consoles.

Failure tip: inspect `src/proven/proven_internal_console.h`. A failure at one split offset is the write carry; at one destination size, the read carry; at one-unit chunks, the pending high surrogate. The real console is checked on Windows by `b039-console-check.c`.

### `tests/test_unit_sysio_streams` - the standard streams are writers and readers

Intent: verify stdin can be read a line at a time, that a buffered stdout holds its bytes until it is flushed and then emits them in order, and that an unbuffered standard-stream writer is out immediately.

The test puts pipes in place of the **real standard streams** - `dup2` over fd 0 and fd 1 on POSIX, `SetStdHandle` on Windows - so a pass means `proven_sysio_stdin()` / `proven_sysio_stdout()` themselves work - not a stand-in - including the short reads a pipe actually delivers.

Sub-checks:

- Lines from stdin: `first\n`, `second\r\n`, a line with spaces, and a final line with no trailing newline all come back without their newline and without a stray `\r`; the end of input is `PROVEN_ERR_EOF`, not an empty line forever.
- A buffered stdout writes **nothing** to the pipe before `proven_writer_flush`, and after it every buffered byte is out, in order. This assertion is the whole point: `proven_sysio_flush` used to claim to flush a buffer that did not exist, and this is the first time the claim could be tested - because there is finally something to flush.
- The formatter is aimed straight at a standard stream (`proven_fprintln` into the buffered writer), which could not be done before: `proven_fprint` takes a writer, and stdout was not one.
- An unbuffered standard-stream writer is in the pipe with no flush at all.
- A line longer than the buffer is `PROVEN_ERR_OUT_OF_BOUNDS`, never a silently truncated line.
- A null state or an empty buffer is `PROVEN_ERR_INVALID_ARG`.

Failure tip: inspect the standard-stream bridge in `src/proven/sysio.c`. It composes `stream.h`'s writer/reader over a handle parked in caller-owned storage; it re-implements nothing, because a second buffered reader would be a second place for the same bug.

### `tests/test_unit_sysio_env` - sysio and environment

Intent: verify standard stream access, formatter-backed console output, environment lookup, missing-variable errors, and long environment-key handling.

Sub-checks:

- Uses proven sysio/formatter APIs without including `<stdio.h>` directly in the test.
- Prints structured output to prove standard stream wrappers are usable.
- Reads a likely existing environment variable such as `PATH`.
- Confirms a fake environment variable reports failure.
- Creates and reads an environment variable whose key is larger than the old fixed stack limit.

Failure tip: inspect `src/proven/sysio.c` and `platform/proven_sys_env.c`. Long-key failures usually mean a fixed-size C-string conversion path returned. Windows failures may involve UTF-8 to UTF-16 conversion and allocator ownership.

### `tests/test_unit_sysio_scanner` - sysio-backed scanner

Intent: verify scanner behavior over file-backed sysio data instead of only in-memory string views.

Sub-checks:

- Creates a temporary file and writes integer/token content.
- Opens the file for reading.
- Initializes a sysio scanner with an allocator-backed buffer.
- Scans two integers across the file stream and confirms EOF after the final token.
- Verifies `tests/test_unit_sysio_scanner_boundary` resumes across a chunk boundary, refills as needed, and only reports EOF after the final token is consumed.
- Cleans up scanner and file resources.

Failure tip: inspect `src/proven/sysio.c`, `src/proven/scan.c`, and file read wrappers. If in-memory scan tests pass but this fails, suspect buffer refill or file-position behavior, especially at the current-buffer boundary.

### `tests/test_unit_sysio_scanner_init` - sysio scanner init allocator validation

Intent: verify buffered scanner initialization rejects partial allocators and leaves the scanner zero-safe on failure.

Sub-checks:

- Passes an allocator that only exposes `alloc_fn` and expects `PROVEN_ERR_INVALID_ARG`.
- Confirms the partial allocator is never called.
- Confirms a rejected initialization leaves the scanner fields cleared.
- In hosted builds, confirms a valid heap allocator still initializes and deinitializes the scanner normally.

Failure tip: inspect `proven_sysio_scanner_init` in `src/proven/sysio.c`. If a partial allocator is accepted, the full allocator trait check is missing; if the scanner keeps non-zero state after failure, the failure path is not zero-safe.

### `tests/test_unit_time_fmt` - time and formatting integration

Intent: verify time measurement, sleep duration, modern format syntax, datetime formatting, and escaped braces.

Sub-checks:

- Reads monotonic or high-resolution time before and after a short sleep.
- Confirms elapsed nanoseconds are at least approximately the requested sleep.
- Formats positional and automatic arguments.
- Converts the Unix epoch to a datetime and checks year/month.
- Formats a datetime value through `PROVEN_ARG`.
- Verifies `{{` and `}}` produce literal braces.

Failure tip: inspect `platform/proven_sys_time.c` for clock conversion and `src/proven/fmt.c` for datetime formatting. Timing failures can be caused by a broken clock source or by assuming exact scheduling latency.

### `tests/test_unit_time_fmt_u16_parity` - `u16` matches `u8` across the whole `fmt.h` spec grammar

Intent: verify `proven_time_u16_fmt` produces the same text as `proven_time_u8_fmt` (widened to code units) for every field and every fill/align/width spec - right-align, centre, left-align, custom fill - on numeric AND named fields, plus literals and escaping, so no spec is silently dropped.

Failure tip: inspect `proven_time_u16_fmt` in `src/proven/time.c`. It renders through the u8 path (which delegates each field to the `fmt.h` spec engine) and widens the result, rather than hand-rolling a u16 parser that recognised only `:0>N`.

### `tests/test_unit_time_monotonic` - the monotonic clock

Intent: verify `proven_time_monotonic_now` is a clock a duration can be measured with.

Sub-checks:

- A hundred thousand consecutive readings never decrease, and the clock advances while it is read.
- A 30 ms sleep is measured as at least 25 ms and less than a minute. No tighter upper bound is asserted: the scheduler decides when a sleeper runs again.

Failure tip: inspect `proven_sys_time_monotonic_ns` in `platform/proven_sys_time.c`. A decrease means a settable clock is being read; a wrong magnitude means the unit, or the Windows frequency scaling, is wrong.

### `tests/test_unit_net_addr` - net: addresses as text

Intent: verify address literals parse to the bytes they name and print in one canonical form, with no socket opened and nothing asked of the system.

Sub-checks:

- IPv4: accepted literals give the right four bytes; twenty malformed ones - short forms (`127.1`, `2130706433`), leading zeros (`01.2.3.4`), out-of-range parts, stray characters and spaces - are `PROVEN_ERR_INVALID_FORMAT` and leave the output untouched.
- IPv6: nineteen accepted forms (full, compressed at each position, bracketed, with a numeric zone, with an IPv4 tail) print as the RFC 5952 spelling - lowercase, no leading zeros, the longest run of two or more zero groups compressed, the first run on a tie; twenty-five malformed ones are refused. The accepted and refused sets were cross-checked against Python's `ipaddress` module, which differs only in printing an IPv4-mapped address in hex.
- Constructors (`ipv4`, `ipv6`, `loopback`, `any`), equality including the zone, a destination too small (`PROVEN_ERR_OUT_OF_BOUNDS`, nothing reported written), an address of no family.
- Unix-domain paths: kept as given; empty, one byte too long, or holding a NUL are refused.
- `proven_net_resolve` answers a literal itself and refuses an empty name, a NUL in the name and a zero capacity.

Failure tip: inspect `internal_parse_ipv4`, `internal_parse_ipv6` and `internal_put_ipv6` in `src/proven/net.c`.

### `tests/test_unit_net_tcp` - net: TCP on the loopback interface

Intent: verify each row of the stream-socket contract in `net.h`, in one thread on loopback. One thread suffices because a connection completes in the kernel's backlog before `accept` is called.

Sub-checks:

- `listen` on port 0 reports the port the OS chose; `listener_addr` agrees; each end's local address is the other's peer address; `TCP_NODELAY` sets and clears; close resets the value and closing twice is not an error.
- Bytes cross both ways; after `shutdown_write` the peer reads `PROVEN_ERR_EOF` (repeatably, never a zero-byte success) and can still answer; a read into no space succeeds with zero bytes and waits for nothing.
- A read with nothing to read is `PROVEN_ERR_TIMEOUT` after at least 75 ms of an 80 ms deadline; `PROVEN_NET_DONT_WAIT` does not wait; an accept with nobody connecting times out and hands back nothing; the connection carries data after two timeouts; data already waiting is read even with no time to wait.
- `write_all` of 48 MiB into a connection nobody reads is `PROVEN_ERR_TIMEOUT` with a count greater than zero and less than the total; the peer then reads exactly that many bytes, equal to the start of the data, and not one more. A single `write` sends a part and reports its size.
- Connecting to a port that was just released is `PROVEN_ERR_REFUSED` and leaves nothing open.
- A connect in two halves: `proven_net_connect_start` is `PROVEN_OK` or `PROVEN_ERR_AGAIN` with a socket to watch; `proven_net_connect_finish` after it is writable is `PROVEN_OK`, and the result is an ordinary connection; a refused connect is `PROVEN_ERR_REFUSED` from one of the two and leaves nothing open; finishing what is not open is `PROVEN_ERR_INVALID_STATE`.
- A peer that closes with unread data makes the other end read `PROVEN_ERR_RESET`, and a later write there is `PROVEN_ERR_RESET` rather than a signal that kills the process.
- A second listener on a bound address is `PROVEN_ERR_BUSY`; calls on a socket that is not open are `PROVEN_ERR_INVALID_STATE`; an address of no family is `PROVEN_ERR_INVALID_ARG`.
- The same exchange over `::1`, or SKIP where the machine has no IPv6 loopback.
- `localhost` resolves through the system resolver (the only case that reaches `getaddrinfo`; literals never do) to loopback addresses carrying the requested port, one of which can be listened on; a capacity of one yields one address; a name under `.invalid` is `PROVEN_ERR_NOT_FOUND`, or `PROVEN_ERR_TIMEOUT` where no name server answers.
- POSIX only, with the descriptor limit lowered to 64 by `setrlimit`: sockets open until the limit and the next is `PROVEN_ERR_BUSY`, leaving nothing behind; a listener cannot open either; an accept with a connection pending and no descriptor free is `PROVEN_ERR_BUSY` at once rather than a wait, and the same connection is accepted after one descriptor is freed.

Where the environment refuses to open a listening socket at all, the test reports SKIP and passes.

Failure tip: inspect the deadline loop (`internal_wait_one`) in `src/proven/net.c` and the reason mapping in `platform/proven_sys_net.c`. A run that dies with no failure message was killed by `SIGPIPE`.

### `tests/test_unit_net_udp` - net: UDP on the loopback interface

Intent: verify datagrams behave as messages.

Sub-checks:

- Three sends are three receives of matching sizes, each naming the sender; the empty datagram in the middle is a success of zero bytes; a reply to the reported address arrives.
- A ten-byte datagram into four bytes of room is `PROVEN_ERR_OUT_OF_BOUNDS` with the first four bytes, and the next receive is the next datagram.
- An empty socket times out after its deadline; a datagram sent to a port nobody holds does not break a later receive on the sending socket (one deferred refusal report is tolerated, as Linux gives).
- Close resets the value; calls on a closed socket are `PROVEN_ERR_INVALID_STATE`; a Unix-domain address is `PROVEN_ERR_UNSUPPORTED`.

Failure tip: inspect `proven_sys_net_recv_from` in `platform/proven_sys_net.c` - the truncation report and, on Windows, `SIO_UDP_CONNRESET`.

### `tests/test_unit_net_unix` - net: Unix-domain stream sockets

Intent: verify the stream calls over a filesystem path, and what is particular to paths.

Sub-checks:

- A listener on a path reports that path; a second listener on it is `PROVEN_ERR_BUSY`.
- Connect, accept, both directions, a read timeout, half-close and `PROVEN_ERR_EOF` behave as over TCP; `TCP_NODELAY` is `PROVEN_ERR_UNSUPPORTED`.
- After the listener closes the socket file remains: connecting is `PROVEN_ERR_REFUSED` and a new listener is `PROVEN_ERR_BUSY`. With the file removed, connecting is `PROVEN_ERR_NOT_FOUND`.

The file's presence is observed through the sockets, not through `proven_fs_stat`, which does not open a socket file on Windows. SKIP where the address family is absent.

Failure tip: inspect `to_native` and `from_native` in `platform/proven_sys_net.c`.

### `tests/test_unit_net_poll_transport` - net: readiness and the transport interface

Intent: verify `proven_net_poll` predicts the call that will not wait, and that the transport interface carries the same code over memory and over a socket.

Sub-checks:

- Over a transport made of memory: `write_all` continues across two-byte writes, reports the count on a failure part-way, and returns `PROVEN_ERR_IO` for a sink that accepts nothing; the reader adapter feeds a line reader three bytes at a time; `proven_fprint` writes through the writer adapter; shutdown and close reach the transport once, and are no-ops when it has none; an empty transport is invalid.
- A listener is not readable until a client connects, then is, and the accept does not wait; of three sockets only the one that can act is reported; arriving data and end of input both make a connection readable, and the read that follows returns the data or `PROVEN_ERR_EOF`.
- 65 items are `PROVEN_ERR_OUT_OF_BOUNDS` for `proven_net_poll` and accepted by `proven_net_poll_with`; scratch one byte short is refused; an impossible count has no scratch size; a closed socket's handle is invalid; a poll of nothing waits until its deadline.
- A connection as a transport: two formatted lines out, two lines in through the line reader, a 60 ms reader timeout, shutdown read as `PROVEN_ERR_EOF`, and close closing the connection.

Failure tip: inspect `proven_net_poll_with` and the transport functions at the end of `src/proven/net.c`.

### `tests/test_unit_net_selector` - net: the selector

Intent: verify a registered set of sockets and a wait that reports the ready ones, for the system's kind (epoll on Linux) and for the portable kind built on poll - which must answer alike.

Sub-checks, each run for both kinds:

- An empty selector waits out its deadline. Three hundred socket pairs are registered; none idle is reported; eight made readable are reported exactly once each with their own tags; unread, they are reported again (level-triggered); read, they are quiet. Registering twice is `PROVEN_ERR_EXISTS`.
- More ready than fit: a hundred ready sockets and a report of seven. The portable kind reaches all hundred within sixty waits (it rotates its starting point); for either kind, reading each as it is reported yields all hundred exactly once.
- A socket asked nothing is not reported though it has data; asked again with another tag, the new tag comes back. A removed socket is silent; removing or changing what is not there is `PROVEN_ERR_NOT_FOUND`; it can be registered again. A closed peer makes its socket readable and the read says `PROVEN_ERR_EOF`; removed before it is closed, it leaves nothing behind. An idle socket asked about writing is writable and not readable.
- Churn: twenty thousand random registrations, removals and readiness changes; after every ninety-seventh, every registered and ready socket is reported and nothing else is, and the count follows throughout. For the portable kind this is the test of its hash's deletion.
- Null pointers, a zero cap, an invalid allocator and handles that are not open are `PROVEN_ERR_INVALID_ARG`.

Failure tip: inspect the Selector section of `src/proven/net.c` - `proven_net_selector_remove` is where the portable kind repairs its hash - and of `platform/proven_sys_net.c` for the system's kind. The `kqueue` path is compiled and run by nothing this project has. The test opens 600 sockets per kind.

### `tests/test_unit_url` - url: parsing, percent-coding, and a path that stays inside its root

Intent: verify a URL comes apart into the components that were written, and that a request path is decoded once and cannot climb out of its root.

Sub-checks:

- Twenty-one URLs split into scheme, userinfo, host, port, path, query and fragment, with absent told apart from empty; the splits were cross-checked against Python's `urllib.parse.urlsplit`. Twenty-eight texts that are not an absolute URL with an authority (relative references, raw spaces, non-ASCII, bad escapes, bad ports, malformed brackets) are `PROVEN_ERR_INVALID_FORMAT` and leave the output untouched.
- Default and effective ports; scheme comparison without case; request targets in origin-form, absolute-form and `*`, with authority-form, fragments and spaces refused.
- A query walked pair by pair: empty values, a missing `=`, empty pairs skipped, `=` inside a value.
- Percent decoding yields any byte (slash, NUL, 0xff), leaves `+` alone, works in place, and refuses a malformed escape; form decoding reads `+` as a space; the three encoders; all 256 byte values round-trip.
- `proven_url_path_resolve`: twenty-eight paths resolve to the expected clean path, including `%252e%252e` staying a file name; fifteen climbs above the root - with the dots written, encoded, and mixed - are `PROVEN_ERR_PERMISSION`; thirty-two hostile paths (encoded slash, backslash, NUL and other controls, overlong and broken UTF-8, an encoded surrogate, bad escapes) are `PROVEN_ERR_INVALID_FORMAT`; a too-small output is `PROVEN_ERR_OUT_OF_BOUNDS`.
- 200,000 paths generated from a small alphabet of dangerous pieces: every one that resolves is checked, on the output alone, to start at the root and to hold no dot segment, empty segment, backslash or control byte.

Failure tip: inspect `src/proven/url.c`. A path in the hostile tables that resolves is a path that reaches a file outside the root.

### `tests/test_unit_http_head` - http: the head parser

Intent: verify what the HTTP/1.1 head parser accepts and refuses, and the two properties callers build on.

Sub-checks:

- A request head field by field: method, target, version, headers in order with values trimmed, views pointing into the parsed buffer, `head_size` at the start of the body; an unknown method kept as `OTHER`; leading empty lines skipped and counted.
- Every proper prefix of fourteen valid requests and eight valid responses, each parsed from an exact-size copy, is `PROVEN_ERR_NEED_MORE`, and the whole is `PROVEN_OK` at exactly its length.
- Thirty-one malformed requests - bare LF and bare CR, whitespace before the colon, obsolete folding, a missing colon, control bytes, DEL and NUL in a value, doubled spaces, a tab separator, a missing target or version, a lowercase protocol name, junk after the version, a non-token method, non-ASCII in the target - are `PROVEN_ERR_INVALID_FORMAT`; HTTP/2.0, 0.9, 1.2 and 3.0 are `PROVEN_ERR_UNSUPPORTED`.
- Limits: one byte under the head limit with no end is `NEED_MORE`, at the limit it is `PROVEN_ERR_OUT_OF_BOUNDS`; a head of exactly the limit parses and one byte more does not; one field more than the array holds is `OUT_OF_BOUNDS`; a limit of 0 means the 16 KiB default.
- Response heads with and without a reason phrase; fourteen malformed ones refused.
- Header lookup without case, first-match, counting repetitions, tokens across repeated fields and inside lists; method and reason-phrase tables; the keep-alive rule for 1.1 and 1.0.
- 300,000 mutated heads (replace, delete, insert, truncate, biased toward CR, LF, space, colon, NUL) parsed from buffers of exactly their size: the result is always one of five known codes, and every accepted head ends in CRLF CRLF, keeps its views inside the buffer, and has no unpaired CR or LF and no NUL.

A separate differential program (RFC-0010, `rfc-0010-http-diff.py`) compared 20,000 generated heads with Python's `http.server`: every head this parser accepted, Python read identically.

Failure tip: inspect `http_fields` and the two parse functions in `src/proven/http.c`.

### `tests/test_unit_http_body` - http: framing, the body decoder, writers and dates

Intent: verify that the length of a body is never a matter of opinion, that a body decodes the same however it arrives, and that a writer cannot be made to write a line it was not asked for.

Sub-checks:

- Request framing: none, a length (zero, and the largest 64-bit value), chunked in any case with surrounding whitespace. Nineteen ambiguous or malformed framings are `PROVEN_ERR_INVALID_FORMAT`: `Transfer-Encoding` with `Content-Length` in either order, two `Content-Length` fields equal or not, a list, a sign, an inner space, hex, an exponent, a decimal point, empty, letters, overflow, `Transfer-Encoding` on HTTP/1.0, an empty or comma-only or quoted coding. Nine codings other than a single plain `chunked` are `PROVEN_ERR_UNSUPPORTED`.
- Response framing: a length, chunked, until-close; no body for `HEAD`, 1xx, 204, 304 and a successful `CONNECT`; contradictory framing refused even on a bodiless status.
- A length-delimited body and a chunked one (extensions, a trailer, CRLF inside chunk data, the next request behind it) decoded whole, split in two at every position, and in pieces of one to seven bytes: identical payload, identical end position. Each piece is an exact-size copy and each payload is checked to lie inside it.
- The body limit: exact fits, one byte less is `PROVEN_ERR_OUT_OF_BOUNDS`, a `Content-Length` over it is refused at init and a chunk over the remaining allowance when its size is read.
- Twenty malformed chunk framings (no size, non-hex, `0x`, whitespace around the size, a sign, bare LF, wrong data length, seventeen digits, a control byte in an extension, bad trailers) are `PROVEN_ERR_INVALID_FORMAT` whole and byte by byte; the 256-byte chunk-line bound and the trailer bound; `PROVEN_ERR_INVALID_STATE` after any error; `PROVEN_ERR_NEED_MORE` from `body_end` for a body cut short.
- Writers: a request and a chunked response written field by field are byte for byte as expected and parse back, the chunks decoding to the data written. Ten values and six names with CR, LF, NUL, another control, or edge whitespace are `PROVEN_ERR_INVALID_ARG`, as are a target with a space or line break, a reason with a line break, a status outside 100-999 and a zero-size chunk - and through all of it the length does not move and no byte of the buffer is written. Exact-fit and one-byte-short buffers.
- Dates: the RFC 9110 example and five other moments format as expected; the three accepted forms parse to the same instant; the two-digit-year rule; twenty-six invalid texts refused (wrong weekday, 30 February, lowercase, a missing zero, ISO 8601); a leap second accepted; 200,000 random moments across the whole range of `proven_time_t` round-trip; a date past April 2262 is `PROVEN_ERR_OVERFLOW`.

Failure tip: inspect `http_framing_headers`, `proven_http_body_feed`, the writers and the date functions in `src/proven/http.c`.

### `tests/test_unit_http_helpers` - http: URL references, forms, ranges, multipart and authentication

Intent: verify the pure helpers a client and a server need around the codec against the documents that define them, not against themselves.

Sub-checks:

- `proven_url_resolve`: the forty-one normal and abnormal examples of RFC 3986 section 5.4, an absolute reference returned as it is, a base that is not absolute refused, and an output one byte short as `PROVEN_ERR_OUT_OF_BOUNDS`.
- `proven_url_form_append`: separators, `+` for a space, every other byte percent-encoded; a buffer one byte short leaves the length and the bytes before it unchanged.
- Ranges: `Range` written closed and open-ended; twenty-three request values - nine satisfied and clamped, four unsatisfiable (`PROVEN_ERR_OUT_OF_BOUNDS`), two unsupported, eight malformed; `Content-Range` written and parsed back, with an unknown total and with contradictory numbers refused.
- Multipart: the boundary from sixteen bytes is forty header-safe characters; a body of a field and a file is byte for byte as expected; a quote, CR or LF in a name or file name is percent-encoded; an empty name, a bad boundary and a control byte in the type are refused.
- Basic: the RFC 7617 example; a colon in the user name and control bytes refused.
- Digest: challenges parsed from one header value holding several schemes; the strongest algorithm chosen; the answers of RFC 7616 section 3.9.1 (SHA-256 and MD5) and RFC 2617 section 3.5 reproduced exactly; the session variants; a challenge without `qop=auth` or with only unknown algorithms is `PROVEN_ERR_UNSUPPORTED`.

Failure tip: inspect `proven_url_resolve` and `proven_url_form_append` in `src/proven/url.c`, the range and multipart functions in `src/proven/http.c`, and `src/proven/http_auth.c`. A Digest mismatch means the answer disagrees with the RFC's own worked example.

### `tests/test_unit_http_cookie` - http: the cookie jar

Intent: verify a cookie is returned to the host that set it, under the path and the transport it was set for, until it expires - and to nobody else.

Sub-checks:

- Store and return for the same host; nothing for another host, a parent or a child of it.
- `Path` matching on segment boundaries; a cookie with no `Path` takes the request's directory; longer paths come first in the header.
- `Secure` cookies are refused when set over plain HTTP and withheld from it.
- `Max-Age` and `Expires` end a cookie; `Max-Age` wins when both are given; an expired cookie in a `Set-Cookie` deletes the stored one.
- A `Domain` that does not cover the setting host is `PROVEN_ERR_PERMISSION`; one that does is accepted and the cookie stays host-only.
- Replacement by name and path; the count limit evicts the oldest; clear empties the jar.
- Malformed values (no `=`, an empty name, a control byte) are `PROVEN_ERR_INVALID_FORMAT`; an oversized one is `PROVEN_ERR_OUT_OF_BOUNDS`; an output buffer too small is `PROVEN_ERR_OUT_OF_BOUNDS`. The jar is unchanged after each.

Failure tip: inspect `src/proven/http_cookie.c`. A cookie returned for a host that did not set it is a leak; the jar is host-only by design.

### `tests/test_unit_sse` - sse: the event-stream parser

Intent: verify the parser gives the events the WHATWG rules give, however the stream is cut into reads.

Sub-checks:

- Data lines joined with LF; LF, CRLF and CR line endings, with a CRLF split across two feeds counted once; comment lines; a field with no colon; exactly one leading space removed.
- `id` kept across events and ignored when it contains NUL; `retry` accepted as digits only; an event with no data not delivered while its id is remembered; a leading byte-order mark skipped.
- Every split of a sample stream into two feeds, and a byte-by-byte feed, give the same events as one feed.
- A line or an event larger than the work memory is `PROVEN_ERR_OUT_OF_BOUNDS`, and every later feed is `PROVEN_ERR_INVALID_STATE`; work memory under 64 bytes is refused.

Failure tip: inspect `proven_sse_feed` in `src/proven/sse.c`. A difference between split and whole feeds is state lost between calls.

### `tests/test_unit_http_server` - http: the server

Intent: verify what a client sees on the socket, with handlers on the loop thread and with handlers on a job system: every request answered, every wait bounded, nothing ambiguous let through.

Sub-checks:

- Keep-alive: several requests on one connection; `Date` and `Content-Length` written by the server; a second response to one request is `PROVEN_ERR_INVALID_STATE`; the handler sees the client's address.
- Three requests in one write are answered in order, the body of the middle one not mistaken for a request.
- Request bodies by length and chunked; 20000 bytes read through the 4 KiB window; a body the handler does not read is skipped so that the next request is found.
- Responses in pieces: chunked when the length is unknown, checked against the length when it is known; `HEAD` with the headers and no bytes; `204`; HTTP/1.0 with and without `keep-alive`; `Connection: close` with the last chunk still sent; a response shorter than announced cut off by a close; a silent handler answered `500`.
- A handler's own `Content-Length` is `PROVEN_ERR_INVALID_ARG` and a header value with a line break is refused, after which another response can be sent.
- `Expect: 100-continue`: the interim response is sent when the handler first reads, before any body byte (job-system model), and not at all when the handler refuses - the connection is then closed without waiting for a body.
- Eleven requests refused before any handler runs, each with its status and a close: missing or duplicate `Host`, bare LF, a space before the colon, a length with chunked, two lengths, a negative length (`400`); another version (`505`); another transfer coding (`501`); an oversized announced body (`413`); too many fields (`431`). A head that never ends is `431` at the limit; a chunked body past the limit and malformed chunk framing are `413` and `400`, reported to the handler's read.
- Timeouts: half a head is `408` after `head_timeout_ms`; a silent connection and an idle one are closed with nothing sent; half a body is `408` with `PROVEN_ERR_TIMEOUT` to the handler; a client that leaves mid-body is `PROVEN_ERR_RESET`.
- `max_connections` of two holds a third client in the backlog until one leaves; a fifth listener is `PROVEN_ERR_OUT_OF_BOUNDS`.
- With a job system: two handlers are inside at the same time, and a connection handed back by a worker carries the next request.
- `stop` from a handler makes `run` return; `destroy` with a handler still running on a worker returns only after it.
- Three hundred connections at once, in both models: each is answered, all are held open together, another connection is served while they sit idle, none is closed before `idle_timeout_ms` has passed since it was last heard from, and every one is then closed by its own timer with nothing more sent.
- A full job queue: eight held requests against one worker and a queue of two - those that do not fit are answered `503` with a close, the rest are served once the worker is free, and every one is answered.

Failure tip: inspect `src/proven/http_server.c`: `sv_service` for the refusal ladder, `sv_run` for what follows a handler, `proven_http_server_poll` for readiness and deadlines, `sv_job` and `sv_collect_done` for the hand-back between threads. Run under ThreadSanitizer when the job-system half fails alone. Responses are read with this library's own codec, so this test does not show agreement with another implementation.

### `tests/test_unit_http_client` - http: the client

Intent: verify the client against this library's server on another thread, and against scripted peers for what an origin server cannot play: a proxy, a SOCKS5 relay, a server that closes a kept connection.

Sub-checks:

- Configuration refused at creation: no allocator, a proxy scheme other than `http` and `socks5`, a control byte in a credential.
- A `GET` read two bytes at a time and then to `PROVEN_ERR_EOF`; `404` and `204` as responses; requests refused before anything is sent - a relative URL, another scheme, `https` without `tls_wrap`, the four headers the client owns, a line break in a header value, a space in a URL - with the server's request count unchanged; `PROVEN_ERR_REFUSED` and `PROVEN_ERR_NOT_FOUND`.
- Reuse: two requests from one source port; a response finished unread is not reused; a client that keeps no connections uses a new one each time; a kept connection the server closed costs one replay on a new connection.
- Bodies: a form from memory with `Content-Length`; 40000 bytes from a stream sent chunked; both at once refused; a `POST` with no body; `HEAD`; a 300000-byte body the server refuses by its length is answered `413` and the client receives that answer rather than a reset.
- Responses: fifty chunks; a body shorter than announced is `PROVEN_ERR_RESET` after the bytes that came; `read_all` with a limit above and below the size; a head past `max_head_bytes`.
- A server that stops mid-body costs `io_timeout_ms`; `103 Early Hints` is skipped; a server that sends twelve interim responses and no final one is `PROVEN_ERR_INVALID_FORMAT` at the ninth, without waiting for a timeout.
- Redirects: three hops followed, the sixth returned at a limit of five; eight method-and-status rows of RFC 9110 with the body and `Content-Type` kept or dropped; relative `Location` values with dot segments resolved and fragments dropped; a `307` for a stream body returned and a `303` followed; a redirect to `ftp` returned; `Authorization` and `Cookie` withheld from another origin and sent to their own.
- Challenges: nothing volunteered; Basic answered in two requests; Digest preferred when both are offered and verified by the server's own computation, query included; a stale nonce answered in three; a challenge after a cross-origin redirect not answered; a wrong password costs two requests for either scheme.
- A cookie jar filled from `Set-Cookie` with a foreign `Domain` dropped; the caller's own `Cookie` header replaces the jar's. A `206` with its `Content-Range`, and a `416`. An event stream read seven bytes at a time into three events.
- The TLS seam: an `https` URL wrapped once for its host and reused without wrapping again; not shared with `http` to the same port; a redirect from `https` to `http` returned; a refusing wrap is `PROVEN_ERR_UNTRUSTED` with nothing sent.
- Proxies: an HTTP proxy sent the absolute URL without its fragment, with `Proxy-Authorization`; a `CONNECT` tunnel to `host:443` with TLS begun inside it for the origin's name and the proxy's credentials not sent through it; a `407` as `PROVEN_ERR_PERMISSION`; SOCKS5 negotiated byte for byte per RFC 1928 and RFC 1929 with the host as a name; a SOCKS5 refusal as `PROVEN_ERR_REFUSED`.

Failure tip: inspect `src/proven/http_client.c`: `proven_http_client_send` for redirects and challenges, `cl_exchange` for what is written, `cl_connect`, `cl_http_tunnel` and `cl_socks5` for proxies. The proxy targets are under `.invalid`, so `PROVEN_ERR_NOT_FOUND` means the client resolved a name it should have handed to the proxy.

### `tests/test_unit_ws` - ws: the WebSocket codec

Intent: verify the codec against RFC 6455's own examples and against the rules it gives a receiver, with the result independent of how the stream is cut.

Sub-checks:

- Handshake: the key and accept value of section 1.3; five malformed keys refused with nothing written; a made key is the Base64 of its bytes. The RFC's request is an upgrade; twelve requests classified (`NOT_FOUND` for no or another upgrade, `INVALID_FORMAT` for the wrong method, HTTP/1.0, no `Connection: Upgrade`, a missing, repeated or short key, no version; `UNSUPPORTED` for another version); subprotocols offered compared exactly. Ten responses judged, among them a wrong accept value, a subprotocol that was not offered, and an extension.
- Frame headers: the eight examples of section 5.7 parse to the RFC's fields, every proper prefix from an exact-size buffer is `PROVEN_ERR_NEED_MORE`, and the writer reproduces the bytes; the masked `Hello` unmasks, and masking in two pieces with the offset equals masking in one. Ten lengths at the edges of the three encodings, masked and not, written in shortest form and read back, with one byte short `PROVEN_ERR_OUT_OF_BOUNDS`. Twenty-five malformed headers refused: each reserved bit, the ten undefined opcodes, fragmented and long control frames, four lengths not in shortest form, a top bit set. The writer refuses four frames the parser would.
- Close: sixteen codes that may be sent and thirteen that may not; payloads written and read back; no status as an empty payload; refusals for a reason without a code, an unsendable code, 124 bytes of reason, a reason that is not UTF-8, a small buffer; a one-byte payload and code 1005 on the wire.
- Decoder, both directions: a conversation - a message, a fragmented message with a ping and an empty fragment inside, 300 binary bytes, two kinds of empty message, text cut inside a character, a pong, a close - decoded whole, split in two at every byte, and one byte at a time from exact-size copies, with identical transcripts. Bytes after a close frame are `PROVEN_ERR_EOF`.
- Decoder refusals, each however it is cut: twenty-seven streams - wrong masking for the direction, continuation with nothing open, a new message inside an open one, eight close-payload violations, a close reason and eight texts that are not UTF-8 including a character broken across fragments, the message limit whole and across fragments - each with `INVALID_FORMAT`, `INVALID_ENCODING` or `OUT_OF_BOUNDS` as its close code requires. The same bytes pass as binary; the limit is exact and control frames are not counted; an error is repeated and consumes nothing.
- 3000 generated conversations: random messages, fragmented at random with pings between fragments, masked or not, decoded from random pieces - every byte, every message end and every ping accounted for, and no call that consumes nothing and reports nothing.

Failure tip: inspect `src/proven/ws.c`: `proven_ws_frame_parse` for header rows, the checks after the header in `proven_ws_decoder_feed` for stream rows, `ws_text_feed` for UTF-8 across pieces. The refusal tables follow the categories of the Autobahn test suite; the suite itself was not run.

### `tests/test_unit_ws_conn` - ws: connections

Intent: verify WebSocket connections end to end on loopback - this library's two ends with each other in both handler models of the HTTP server, and each end against a peer that breaks the rules.

Sub-checks:

- A conversation: connect with two subprotocols offered and one selected; text and binary echoed at eleven sizes from 1 to 70000 bytes, across the three length encodings and both the 4 KiB send buffer and the 16 KiB read buffer; text beyond ASCII; text that is not UTF-8 and a 126-byte ping refused before sending; a message sent in fragments with a ping inside and a character cut between two of them, received as one, with the pong counted; `PROVEN_ERR_TIMEOUT` at the deadline leaving the connection usable; the close handshake, twice, with the code echoed, `INVALID_STATE` for a later send and `EOF` for a later receive, and the server's end seeing code 1000.
- Messages the server starts: text, 70000 binary bytes, three fragments with a ping between them reassembled; the server's close as `PROVEN_ERR_EOF` with code 1001 and its reason; the server's close completing because the client answered it.
- The message limit: exactly the limit passes; one byte more is `PROVEN_ERR_OUT_OF_BOUNDS`, repeated by every later call, and the sender is sent 1009.
- Hand-off: the handler gives the connection to another thread and returns; three ordinary HTTP requests are served while the WebSocket is open, and it still echoes afterwards.
- No WebSocket there: a 404 as `PROVEN_ERR_REFUSED` with the status reported; a server selecting a subprotocol nobody offered gets `PROVEN_ERR_INVALID_ARG` and may still answer; `wss` without `tls_wrap` as `PROVEN_ERR_UNSUPPORTED`; argument errors; a plain GET to an endpoint is `PROVEN_ERR_NOT_FOUND` to accept with nothing sent.
- A raw client: the 101 carries the accept value of RFC 6455 and names no subprotocol or extension; a frame sent in the same write as the handshake is echoed, unmasked; a ping gets its pong; a close gets a close with the same code and then the connection ends. Five violations answered with close 1002, 1007 or 1009 (the last for a frame only announced); a frame past the limit with 60000 bytes of its payload already sent, whose 1009 must still arrive - the run on Windows is what checks this, since Linux loopback tends to deliver a close that a reset should have destroyed; a reserved bit; a client that vanishes as `PROVEN_ERR_RESET` with 1006; three bad handshakes as 426 naming version 13, and 400.
- A scripted server: a wrong accept value and an unrequested extension fail the connect; a frame in the same read as the 101 is received; what the client sends is a masked frame; a masked frame and invalid UTF-8 from the server end the connection with a masked close of 1002 and 1007; a server that never answers a close costs `close_timeout_ms`.
- Over a socket pair, with no handshake: both directions, the getters on an open connection, and destroy without close seen by the peer as `PROVEN_ERR_RESET` with 1006.

Failure tip: inspect `src/proven/ws_conn.c`: `wc_pump` for receiving, `wc_send_frame` for sending, `wc_fail` for the close sent on a violation; and `proven_http_exchange_upgrade` and `proven_http_client_upgrade` for the hand-over. Both ends share one codec, so this test does not show agreement with another implementation. Run under ThreadSanitizer when only the job-system half fails.

### `tests/test_unit_u16str` - U16 strings

Intent: verify optional UTF-16/code-unit string support and its append policies.

Sub-checks:

- Creates and destroys a U16 string.
- Appends code units into fixed capacity.
- Confirms atomic append failure leaves content and length unchanged.
- Confirms partial append writes the count that fits and reports out-of-bounds.
- Confirms growable append reallocates and completes the write.

Failure tip: inspect `src/proven/u16str.c` and `include/proven/u16str.h`. Treat U16 values as UTF-16 code units, not Unicode scalar values. Check `PROVEN_NO_U16STR` guards if the failure is compile-time.

### `tests/test_unit_utf` - UTF-8 and UTF-16 transcoding

Intent: verify `utf.h` converts every scalar value exactly in both directions, refuses malformed input without writing, and tells input cut mid-character apart from malformed input.

Sub-checks:

- Known text (ASCII, a two-byte letter, Hangul, an emoji that becomes a surrogate pair) converts to the expected units and back, with the size functions agreeing.
- Every scalar value U+0000..U+10FFFF (minus the surrogates) round-trips against a reference encoding.
- UTF-8 validity agrees with an independent code-point formulation over every 1-, 2- and 3-byte input and a sweep of 4-byte inputs; UTF-16 validity agrees with the surrogate rules over every unit alone and before every kind of neighbour. Planted defects in the lead-byte table and the surrogate checks were each caught before the test was trusted.
- `proven_utf8_decode_next` agrees with the same reference over the same inputs: its verdict, its step length (the maximal subpart for malformed input, computed by the reference as the longest prefix it still calls a valid start) and, for a character, a code point whose reference encoding is exactly the bytes consumed. A walk through mixed text visits every byte once and ends exactly at the end. A planted off-by-one in the maximal subpart was caught.
- The malformed forms the standard names (overlong, encoded surrogate, above U+10FFFF, stray continuation, bad lead, bad continuation) stop the conversion exactly where they start.
- Input cut mid-character is `PROVEN_ERR_NEED_MORE` in the partial forms and `PROVEN_ERR_INVALID_ENCODING` in the whole forms; a trailing high surrogate likewise.
- A full output never receives half a character; the atomic forms write nothing on refusal.
- The grow forms append, and roll back length and terminator on malformed input, aliasing input, and an allocator that fails mid-way.

Failure tip: inspect `src/proven/utf.c`. A validity mismatch is a hole in the lead-byte range table (Unicode table 3-7); a rollback failure leaves the destination longer or unterminated.

### `tests/test_unit_u8str_mutation` - U8 string mutation

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

### `tests/test_unit_u8str_split` - splitting a view

Intent: verify `proven_u8str_view_split` / `_split_next` against RFC-0005 table 4.1 and its properties.

Sub-checks:

- Every row of table 4.1: `"a,b,c"` is three fields; no separator is one field; leading, trailing and doubled separators keep empty fields; `""` and a null view are one empty field; a multi-byte separator; leftmost non-overlapping matching (`"aXXXb"` on `"XX"`); empty and null separators yield the whole input once; an ill-formed source is one empty field; NULL arguments yield nothing and consume nothing; a well-formed separator reads back as passed.
- Over 50,000 random inputs: field count equals non-overlapping occurrences + 1 (for a non-empty separator), every field lies inside the source, and an iterator copied part-way continues exactly as the original. Case, field and fork counts are printed so a vacuous pass is visible.

Failure tip: inspect `proven_u8str_view_split_next`; its four steps must stay in RFC-0005's order. A count one short is the dropped tail; a count at the cap is a non-terminating iterator.

### `tests/test_unit_u8str_view_cmp` - view ordering

Intent: verify `proven_u8str_view_cmp` is the order RFC-0005 section 3.4 defines, and `_cmp_ptr` sorts with it.

Sub-checks:

- Table 4.4 and each row's mirror: bytewise, unsigned (`"\xFF"` after `"a"`), a prefix first, embedded NUL as data, ill-formed views as empty.
- Over every string of length 0-3 on `{00, 'a', FF}`: antisymmetric, transitive, and zero exactly when `proven_u8str_view_eq` says equal.
- `proven_array_sort` with `proven_u8str_view_cmp_ptr` sorts an array of views into that order.

Failure tip: a wrong sign on the `\xFF` row is signed comparison; on the prefix rows, the length tie-break.

### `tests/test_unit_u8str_view_ops` - view trim, affixes, reverse search and well-formedness

Intent: verify RFC-0005 tables 4.2, 4.3 and 4.5 row for row, asserting empty results by size, never by pointer.

Sub-checks:

- Trim removes exactly the six ASCII whitespace bytes from the chosen ends, leaves interior whitespace, does not treat a UTF-8 no-break space as whitespace, and treats ill-formed input as empty.
- Prefix and suffix removal return the view unchanged when the affix is absent or longer, and empty when it is the whole view.
- `find_last` counts overlapping occurrences (`"aaa"`/`"aa"` is 1), answers `size` for an empty needle and `NOT_FOUND` otherwise; `contains` equals `find != NOT_FOUND` on every row.
- `is_well_formed` is false only for `{NULL, n > 0}`, and true for an out-of-range slice.

Failure tip: inspect the view vocabulary at the end of `src/proven/u8str.c`. `find_last` at scale is `test_differential_find_last_oracle`.

### `tests/test_unit_float_bits` - float bit extraction

Intent: verify the internal float bit helpers preserve raw IEEE-754 bit patterns for f32 and f64 values, including signed zero, infinities, and NaN payloads.

Failure tip: inspect src/proven/float_decimal.c if the raw byte-copy helpers stop matching the object representation.

### `tests/test_unit_float_exact_range` - float exact-range backend

Intent: verify representative exact-range decimal spellings keep their documented bit patterns without the host strtod fallback.

Failure tip: inspect src/proven/scan.c and the shared float decimal helper if the exact-range backend falls back to host strtod or the corpus drifts.

### `tests/test_unit_float_f32_boundaries` - float32 boundary neighbors

Intent: verify the float32 upgrade and shortest corpora pin the ULP-adjacent neighbors around FLT_MIN and FLT_TRUE_MIN so the parser-driven backend keeps the documented boundary spellings.

Failure tip: inspect tests/test_differential_float_corpus_f64.c and tests/test_unit_float_shortest_roundtrip.c if a float32 boundary-neighbor corpus value disappears or changes spelling.

### `tests/test_unit_float_format_policy` - float format policy scaffold

Intent: verify the new float format policy seam preserves the current simple formatter behavior, rejects unsupported shortest-mode requests, and reports invalid inputs clearly.

Failure tip: inspect src/proven/float_format.c and include/proven/float_format.h if the policy dispatch or fixed formatter helper regresses.

### `tests/test_unit_float_parse_api` - float parse API

Intent: verify the public ASCII float parser and strtod-like wrapper expose consumed-length, endptr, and range signaling over the shared exact backend.

Failure tip: inspect include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if the public parser seam or wrapper contract drifts.

### `tests/test_unit_float_rfc_0001_cases` - RFC-0001 parse audit

Intent: verify the decimal-to-binary64 rewrite still satisfies the explicit named cases from RFC-0001.

Failure tip: inspect RFC-0001, include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if a named RFC audit case fails.

### `tests/test_unit_float_shortest_known` - float shortest known values

Intent: verify the shortest float formatting policy emits the documented exact spellings for representative f64 and f32 values.

Failure tip: inspect src/proven/float_format.c if the shortest-policy output drifts or if RYU requests stop reaching the active backend.

### `tests/test_unit_float_shortest_roundtrip` - float shortest round-trip

Intent: verify shortest float formatting round-trips through host strtod for representative f64 and f32 values.

Failure tip: inspect src/proven/float_format.c if the shortest output stops round-tripping, and keep the host strtod oracle limited to tests.

### `tests/test_unit_float_shortest_scientific_guard` - float shortest scientific guard

Intent: verify the shortest float formatter handles very small finite values by producing a valid shortest candidate instead of an invalid scientific normalization result.

Failure tip: inspect src/proven/float_decimal.c and src/proven/float_format.c if the shortest formatter rejects a tiny finite value or emits an invalid scientific spelling.

### `tests/test_unit_float_shortest_tie_break` - float shortest tie-break corpus

Intent: verify the shortest corpus keeps the 0.001 fixed-versus-scientific tie-break cases pinned for both widths.

Failure tip: inspect tests/test_unit_float_shortest_roundtrip.c and tests/test_differential_float_corpus_f64.c if the tie-break corpus disappears or is renamed.

### `tests/test_unit_mem_copy` - bounded memory copy

Intent: verify proven_mem_copy copies within capacity, rejects overflow without writing, treats a zero-size source as a no-op, and rejects null pointers.

Failure tip: inspect proven_mem_copy in src/proven/memory.c if a copy overflows, writes on rejection, or mishandles empty/null inputs.

### `tests/test_unit_scan_f64_bounds` - float scanner boundary behavior

Intent: verify float scanning treats underflow as signed zero, reports overflow deterministically, and preserves cursor rollback at the true boundary cases.

Failure tip: inspect proven_scan_f64 exponent-to-value handling and final finite checks if a boundary token returns the wrong error or wrong sign.

### `tests/test_unit_sysio_scanner_boundary` - sysio scanner boundary refill

Intent: verify buffered sysio scanning resumes across a chunk boundary, refills as needed, and only reports EOF after the final token is consumed.

Failure tip: inspect proven_sysio_scanner_scan_impl staging, refill handling, and EOF transition behavior when a token reaches the end of the buffer.

### `tests/test_unit_u128_mul` - wide multiply helper

Intent: verify the shared 64x64 to 128-bit multiply helper returns exact high and low halves for representative operands.

Failure tip: inspect src/proven/float_decimal.c if the wide multiply helper stops matching the reference product.

### `tests/test_unit_u8str_borrow` - U8 string borrow (fixed-capacity over caller memory)

Intent: verify proven_u8str_borrow/_reset: fixed-capacity ops and fmt work, growing ops refuse to reallocate caller memory, and destroy is a no-op.

Failure tip: inspect proven_u8str_borrow/_reset and the borrowed-flag guards in reserve/append_grow/replace_at_grow/destroy.

### `tests/test_unit_public_surface_gaps` - the public functions nothing had ever called

Intent: exercise the shipped, documented public API that no test touched - `proven_fs_symlink` (creation, resolution, stat-follows, refusal to clobber), the bounds-checked mem slices (including `offset+size` overflow), the formatter's caller-supplied-scratch path, the mutable map/array lookups (a write through `get_mut` must be visible), `linear_search` on an UNSORTED array, `proven_u16str_create_from_view` (sealed at the right unit index), and the standard-stream bridges that shipped with only their siblings covered.

Failure tip: the gap list was found by diffing every `proven_*` symbol in `include/proven` against every one named in `tests/` or `manual/examples/`. Untested public API is where bugs live, because nothing has ever disagreed with it.

## Contract and hardening tests

The public invariants: misuse, corrupted structs, exhausted allocators, refused input. These say what the library *refuses to do*, which is the half a caller cannot infer from the happy path.

### `tests/test_contract_allocator_dealloc` - allocator deallocation policies

Intent: document and verify the different deallocation policies exposed through the allocator trait.

Sub-checks:

- Allocates from an arena through the generic allocator trait.
- Calls the arena free function and verifies it is intentionally a no-op.
- Resets the arena as the correct lifetime-ending operation.
- Allocates from the heap allocator and frees through the heap trait.

Failure tip: inspect `src/proven/arena.c`, `src/proven/heap.c`, and the allocator trait definition. Do not make arena `free` reclaim individual blocks; that would break the arena lifetime model.

### `tests/test_contract_arena_panic` - arena panic path

Intent: verify panic-on-allocation-failure behavior is deterministic and does not fire on successful arena allocation.

Sub-checks:

- Installs a test panic handler with `proven_set_panic_handler`.
- Allocates successfully with `alloc_or_panic` and confirms no panic occurred.
- Requests more memory than the arena can provide.
- Confirms the panic hook was invoked exactly for the out-of-memory path.

Failure tip: inspect `src/proven/arena.c` and `src/proven/panic.c`. Restore the panic hook carefully in tests so later tests are not affected.

### `tests/test_contract_fmt_failure_policy` - formatter failure policy

Intent: verify formatting append policies are explicit: fixed-capacity atomic, fixed-capacity truncating, and allocator-backed growable.

Sub-checks:

- Appends formatted output with growable allocation.
- Populates a small fixed string.
- Confirms fixed-capacity formatting reports out-of-bounds and leaves the string unchanged.
- Confirms truncating formatting writes the partial count and reports required size.
- Confirms content after truncation matches the expected prefix.
- Checks extremely large padding specs for safe overflow handling.

Failure tip: inspect `src/proven/fmt.c`. Track `written`, `required`, and destination length separately. Atomic failure must not modify the destination.

### `tests/test_contract_fmt_atomic` - the fixed-capacity format is atomic on failure

Intent: verify a failed fixed-capacity format leaves the string byte-for-byte as it was - after an overflow, after a format error discovered halfway through the output, and after an argument-count error - and that it still reports how many bytes it needed.

Failure tip: inspect the single-pass branch of `proven_u8str_fmt_internal` in `src/proven/fmt.c`. It writes as it goes, so atomicity rests on the rollback restoring `internal.len` and resealing the NUL.

### `tests/test_contract_map_hardening` - map borrowed-key hardening

Intent: verify borrowed U8 keys that point into internal map storage are rejected when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Inserts a normal external borrowed key and confirms it still works.
- Constructs a borrowed view that points into the map's own internal storage.
- Expects `PROVEN_ERR_INVALID_ARG` for that internal-storage key when the validation gate is active.

Failure tip: inspect the borrowed-key range guard in `src/proven/map.c` if an internal pointer is accepted or if ordinary borrowed keys stop working.

### `tests/test_contract_pool_misuse` - pool double-free hardening

Intent: verify the pool free trait catches repeated frees when debug validation or `PROVEN_HARDENED` is enabled.

Sub-checks:

- Installs a test panic handler.
- Allocates one fixed-size block through the pool allocator trait.
- Frees the block once successfully.
- Frees the same block again and expects the validation path to reach the panic handler when hardening or debug validation is active.

Failure tip: inspect `src/proven/pool.c`. The repeated-free check must remain gated on debug validation or `PROVEN_HARDENED`, and the test should only require the panic path when that gate is active.

### `tests/test_contract_scan_f64_overflow` - float scanner overflow

Intent: verify a very large floating-point token reports `PROVEN_ERR_OVERFLOW` instead of silently accepting infinity.

Sub-checks:

- Builds an input with roughly 1000 decimal digits.
- Scans it as `f64`.
- Confirms the error is `PROVEN_ERR_OVERFLOW`.

Failure tip: inspect `proven_scan_f64` in `src/proven/scan.c` and math helper behavior in the PAL. Do not accept `inf` as a successful parsed finite value.

### `tests/test_contract_sysio_scan_nonseekable` - non-seekable sysio rejection

Intent: verify one-chunk file scanning rejects pipe/stdin-like inputs before consuming data.

Sub-checks:

- Checks the helper returns `PROVEN_ERR_UNSUPPORTED` for a non-seekable handle.
- Checks the scan destination is left unchanged on the early rejection path.
- Checks the original pipe payload is still readable after the rejected scan attempt.

Failure tip: inspect `src/proven/sysio.c` and make sure the one-chunk scan path probes seekability before reading.

### `tests/test_contract_sysio_scan_truncation` - chunked sysio scan truncation

Intent: verify one-chunk file scanning rejects inputs that exceed the fixed buffer, refuses borrowed string outputs that would escape its local buffer, and leaves the stream reusable after a failed attempt.

Sub-checks:

- Checks a chunk-full literal reports the bounds error used by the one-chunk scan path.
- Checks the file cursor is still usable after the failure.
- Checks the trailing integer is not consumed by the failed scan.
- Checks a string-view destination is refused before reading, stays unchanged, and leaves the input available to a subsequent scalar scan.

Failure tip: inspect `src/proven/sysio.c` and `tests/test_contract_sysio_scan_truncation.c`.

### `tests/test_contract_float_module_layout` - float module scaffold

Intent: verify the shared float helpers live in a dedicated internal translation unit instead of being copied into fmt.c and scan.c.

Failure tip: inspect src/proven/float_decimal.c, src/proven/float_decimal.h, fmt.c, scan.c, and nob.c if the shared decimal helper scaffold regresses.

### `tests/test_contract_public_structs` - public array/map/filesystem contracts

Intent: verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.

Failure tip: inspect public invariant guards in array/map mutation entry points and the filesystem open-flag translation if a corrupt struct or append request slips through.

## Regression tests

One test per defect that actually shipped. Each is named for what broke, not for a version or a number, and each was verified to FAIL against the pre-fix source. A regression test that passes before the fix is not a regression test.

### `tests/test_regression_rng_unseeded` - an unseeded or failed generator is inert

Intent: verify a ChaCha generator that was never usable never hands back bytes that *look* usable. Three defects, found by the standing audit, all with that shape.

Sub-checks:

- `proven_chacha_rng_next(NULL)` is 0. It used to declare an 8-byte scratch, call `_fill` (which returns immediately for a NULL generator, touching nothing), and read the scratch anyway - returning the caller's own stack as randomness. It was the only entry point in the module without a NULL guard.
- A never-seeded, stack-declared generator yields zeros and an **invalid trait**. `used == 0` is exactly what a zero-initialised struct holds, and the fill path read that as "a full block of fresh keystream is ready" - and copied its own uninitialised `block[]` out. A silent stack disclosure.
- A generator whose seeding FAILED stays inert **past the first block**. Zeroing the state was not enough: ChaCha over an all-zero state emits an all-zero *first* block, so "the caller gets zeros" was true for exactly 64 bytes - and then the counter advanced and block 1 was a normal-looking, fixed, publicly derivable keystream.
- The control: a properly seeded generator is valid, produces a real keystream, and is still ChaCha20 byte for byte. The guard changed nothing about the maths.

Failure tip: inspect the `seeded` marker in `proven_chacha_rng_t`. `used` alone cannot encode usability, because a zero-initialised struct is the shape of "never seeded".

### `tests/test_regression_read_line_exact_fit` - a line that fits the buffer is a line, not an error

Intent: verify the reader enforces the rule it documents. It said "a line **longer** than the buffer is `PROVEN_ERR_OUT_OF_BOUNDS`" and enforced something stricter - it refused any line that *filled* the buffer.

It had to, because it asked the wrong question first: it answered "too long" before attempting a fill, since a fill cannot tell "buffer full" from "source ended". But a full buffer means one of three things and only one is an error - the next byte is the newline that ends the line; the source has ended and what is held IS the final line; or the line really is too long. One byte of lookahead tells them apart.

Sub-checks:

- **The data-loss case:** a 4-byte file with no trailing newline, read through a 4-byte buffer, returns its 4 bytes. It used to return `OUT_OF_BOUNDS`, with the entire contents of the file unreachable through this API.
- A line exactly the size of the buffer, terminated by the next byte, is returned - and the stream carries on.
- A CRLF split at the boundary yields the line without its `\r`.
- A line *genuinely* longer than the buffer is still `OUT_OF_BOUNDS`, never a truncated line returned as a success - that is the corruption the check existed to prevent, and the fix must not trade one for the other.
- The ordinary cases keep working at every buffer size that fits them.

Failure tip: inspect the buffer-full branch of `proven_reader_read_line` in `src/proven/stream.c`, and the `peek` / `has_peek` lookahead on `proven_reader_buffered_t`. The looked-at byte belongs to the stream: it is stashed, not dropped.

### `tests/test_regression_read_line_peek_eof` - a stream byte stranded after a too-long line is not lost

Intent: verify that after `proven_reader_read_line` reports `OUT_OF_BOUNDS` (stashing one lookahead byte), a following raw `proven_reader_read` reaches that byte instead of returning a spurious EOF - so a read-to-EOF loop does not silently lose the byte peeked past the over-long line.

Failure tip: inspect `reader_buffered_fill` in `src/proven/stream.c`. It must report whether it made the buffer non-empty (a re-inserted peek byte is progress), not just whether the source handed over new bytes this call. `return r.value > 0` alone stranded the peek at EOF.

### `tests/test_regression_split_empty_sep` - an empty separator ends the split

Intent: pin RFC-0005 section 1.1: a split on an empty or null separator yields exactly one field, the whole input.

Sub-checks:

- For sources `"abc"`, `""` and `",,"`, and for an empty and a null separator, the iterator yields one field. Fields are counted up to a cap, so the hang is reported, not reproduced.

Failure tip: step 2 of `proven_u8str_view_split_next` (the empty-separator case, before any search) is missing or has moved after the search. With it removed, this test fails in bounded time.

### `tests/test_regression_float_exact_pow5` - the exact float fallback uses an exact power of five

Intent: verify an exact halfway value in the `56..350` exponent window breaks to even, and that values just below and just above a rounding boundary there land on the correct double.

Note: the exact big-integer tier is the one that makes "correctly rounded, ties-to-even, bit-identical to a correct `strtod`" true. It built `5^q` above the exact table by shifting a **rounded** Eisel-Lemire table entry, and `5^q` is odd, so the shift was never exact. A differential run against glibc found 2,923 misrounded values - all of them exact ties. The expectations here were verified with exact rational arithmetic, not against a host `strtod`, so the test states what is true rather than what this machine agrees with.

Failure tip: inspect `proven_float_bigint_build_pow5_cached` in `src/proven/float_decimal.c`.

### `tests/test_regression_job_permit_starvation` - job permit starvation deadlock

Intent: verify a permit spent on an empty queue cannot strand the jobs behind it.

A permit on the workers' semaphore used to mean "take exactly one job". That is sound only if a woken worker can always find the job its permit announced, and it cannot: the queue hands out slots in order, so a producer that has claimed slot *n* and not yet published it hides slot *n+1* from every consumer. The worker woken for *n+1* reads an empty queue, spends the permit, and parks - and when *n* is published a moment later there is no permit left to announce the work. Lose enough of those and the queue stops draining; because it never empties, no worker reaches its exit test either, so `close` and `destroy` wait on threads that never finish.

Sub-checks:

- Six rounds of 24 producers - far more than the machine has cores - against a **four-slot** queue, so the window between claiming a slot and publishing it is hit constantly.
- Each round closes and destroys the system, which is where the hang appeared.
- Every accepted job must have run by the time `destroy` returns.
- A watchdog thread turns a deadlock into a reported failure naming the round. Without it the failure has no assertion to fail - the process simply stops, and the whole suite stops with it.

Note: verified to FAIL against the pre-fix source - five deadlocks in five runs, where the stress harness needed heavy background load to hang in 18 runs out of 40. The fix is that a permit now means "there may be work" and a woken worker drains the queue rather than taking one job from it.

Failure tip: a worker is parked while the queue still holds work. Check that the worker loop drains rather than taking a single job per permit.

### `tests/test_regression_float_parse_concurrency` - concurrent float parsing

Intent: verify the public decimal parser has no shared writable state across concurrent Clinger, Eisel-Lemire, subnormal, and exact-fallback conversions.

Sub-checks:

- Runs eight PAL threads through four fixed-bit decimal cases for 2,000 iterations each.
- Keeps all writable result state in a separate context for each thread.
- Checks the consumed length and exact binary64 bits on every conversion.
- Gives TSAN a focused regression for the process-global path counters that previously raced.

Failure tip: run this test under TSAN and inspect `src/proven/float_decimal.c` for global path counters or shared scratch storage.

### `tests/test_regression_scanner_float_split` - a float split across the scanner buffer

Intent: verify a float whose exponent, sign, or mantissa lands on the buffered scanner's refill boundary still scans to its exact value (not a mantissa-only truncation, not a dropped sign that desyncs the stream), across every buffer size, and that genuine garbage is still an error rather than an endless refill.

Failure tip: inspect `proven_scan_f64` in `src/proven/scan.c`. It must flag `needs_more` both when a valid float might still grow and when a FAILED parse left only a float prefix. Found by a fmt -> file -> scanner -> float round-trip.

### `tests/test_regression_scanner_short_read` - the scanner over a pipe

Intent: verify a token split across two pipe writes scans whole, that the rest of the stream stays readable, and that a failed read is `PROVEN_ERR_IO` rather than a clean end of input.

Also: a hexadecimal `0x` that arrives before its digits, and hex digits split across two writes, scan whole through `{:x}` (a planted removal of the `0x` stream signal was caught).

Note: runs on both platforms - an anonymous pipe (`pipe()` or `CreatePipe`) fed by a writer thread. `read()` on a pipe returns whatever has arrived; treating that as EOF truncated the token *and* discarded the rest of the stream. Regular files hide the bug entirely, which is why the whole suite passed.

Failure tip: inspect `scanner_fill` in `src/proven/sysio.c`.

### `tests/test_regression_map_churn` - a map with churn does not grow without bound

Intent: verify a bounded live set with endless insert/remove keeps the capacity bounded, that live keys survive an in-place rehash, that removed keys stay removed, and that a genuinely growing map still grows.

Note: `used` counts tombstones and never falls on its own, so an unconditional doubling grew a steady-state cache forever - 100 live entries reached 33 MB. Not a leak, which is why nothing caught it.

Failure tip: inspect `map_rehash` in `src/proven/map.c`.

### `tests/test_contract_sort_alignment` - the sort never hands the comparator a misaligned element

Intent: verify sorting over-aligned elements passes only correctly-aligned pointers to the caller's comparator, and still sorts.

Note: the check lives in the comparator, so it fails in **every** build mode, not only under UBSan. A contract only one build enforces is a contract that breaks in release.

Failure tip: inspect `insertion_sort` in `src/proven/algorithm.c`.

### `tests/test_unit_fs_walk` - the recursive walk

Intent: verify `proven_fs_walk` reports every entry once in pre-order with the right depth, reports a symlinked directory without descending into it, REPORTS an unreadable directory as an error rather than skipping it, honours `max_depth` while still reporting the boundary directory, and streams a wide directory rather than buffering it.

Note: this test was written **from the contract, before the implementation existed** - the first feature under the rule in `TESTING.md` section 5.1 - and it landed red, in its own commit. It earned its keep immediately: it found the first draft of the contract ("follow symlinked directories, but stop at a cycle") quietly walking all of `/tmp`, and it found the implementation writing a NUL into the middle of a path view the caller was still holding. Neither would have been asked about by a test written afterwards to confirm code that already looked right.

Failure tip: inspect `proven_fs_walk_open/_next/_close` in `src/proven/fs.c`.

### `tests/test_regression_fs_walk_errors` - the walk's error branches and TOCTOU safety

Intent: verify a `readdir()` that fails mid-directory is reported with the last path component as the name and the directory's own depth (not its children's), and that a directory swapped for a symlink at the moment of descent is not followed out of the tree.

Failure tip: inspect the readdir-failure branch of `proven_fs_walk_next` and the fd-relative, `O_NOFOLLOW` descent (`proven_sys_fs_dir_open_at`). Both defects were found by the standing audit and are pinned here against the same fault injection.

### `tests/test_contract_protected_destination` - one rule, every door

Intent: verify that a destination whose owner-write bit is clear is refused by **every** public function that replaces a file, with the same error, leaving the file exactly as it was.

Sub-checks:

- Eight doors, one by one: `proven_fs_open` for WRITE, for WRITE|TRUNC and for APPEND; `proven_fs_write_file`, `_atomic` and `_durable`; `proven_fs_copy`'s destination; and `proven_fs_rename`'s destination. Each must answer `PROVEN_ERR_PERMISSION`, and after each the file must still hold its contents **and** its mode.
- Clearing the mark lets the write through. A refusal that cannot be recovered from is a wall, not a rule.
- `proven_fs_remove` is deliberately **not** covered - deleting a name is a directory operation and POSIX has never let the file's mode have a say in it - but it must still answer `PROVEN_OK` or `PROVEN_ERR_PERMISSION`, never a bare `PROVEN_ERR_IO`.
- A refusal names itself: a missing name is `PROVEN_ERR_NOT_FOUND`, a protected one is `PROVEN_ERR_PERMISSION`.

Note: the list of doors is the point. This test exists because the rule was true of some functions and not others, and no one could see that by reading. Measured before it was one rule, on one platform: `write_file` refused, `write_file_atomic` succeeded, and `copy` succeeded **and left a 0444 file as 0664**. `proven_fs_rename` was the worst of them - it is what the atomic write is built on, so a caller refused by one got the result from the other, and the rule was one line of caller code away from being void.

Skipped as root (file modes refuse root nothing) and on a filesystem that does not honour a `0444` chmod, each with its reason printed.

Failure tip: inspect `internal_refuse_if_protected` in `src/proven/fs.c` and its call sites. Adding a public function that replaces a file means adding a door here.

### `tests/test_regression_fs_backslash_parent` - a backslash in a POSIX filename is not a separator (RFC-0008 H-003)

Intent: verify a durable write syncs the directory the file is actually in, when the filename legally contains a backslash.

Sub-checks:

- A durable write to `<dir>/a\b` succeeds and holds the new contents. It used to return an I/O error - *after* the rename had already published them, so the caller was told the write failed while looking at a file that had been replaced. The parent of that file is `<dir>`; the old rule computed `<dir>/a`, which does not exist.
- The decoy: with `<dir>/a` created as a real directory, the write must still sync `<dir>` and never `<dir>/a`. This is the case a `PROVEN_OK` assertion cannot see at all - the old code returns success and has synced the wrong directory - so the test records which directories were opened and names the one that was synced.
- Ordinary paths are unchanged: a plain slash path syncs the directory before the last slash, and a bare filename syncs `.`.
- A 250-character basename made of `x` and `\` is written atomically. The staging file is `<path>.pvtmpNN` and the basename is trimmed so that name still fits in `NAME_MAX`; under the old rule the basename was measured from the last backslash, so a long name measured as a short one, no trim happened, and the filesystem was handed a name too long to create.

Note: POSIX-only; compiles to a skip on Windows, where a backslash *is* a separator and `proven_fs_sync_dir` is `PROVEN_ERR_UNSUPPORTED` anyway. The observation seam is a definition of `open()` in the test, bound to the platform layer's call by the linker, forwarding to the real `openat` syscall - `proven_fs_sync_dir` opens the directory read-only to fsync it, so the record says what was synced.

What it does not prove: the Windows path rules - drive roots, UNC shares, extended-length paths - have no result here. `proven_fs_is_absolute` is deliberately left accepting Windows spellings everywhere; it classifies a path that may have come from elsewhere rather than resolving one on this machine.

Failure tip: inspect `internal_is_separator` and `internal_parent_dir` in `src/proven/fs.c`.

### `tests/test_regression_job_seq_wrap` - queue sequence comparison at the sign boundary (RFC-0008 H-004)

Intent: verify the job queue decides what to do with a cell by **modular** distance in the unsigned counter type, and that it is unambiguous everywhere the counters can be.

Sub-checks:

- The classifier at the exact state the RFC reproduces: capacity two, an enqueue position at `PTRDIFF_MAX + 1`, a cell one lap behind it. The old code wrote that comparison as `(proven_ptrdiff_t)seq - (proven_ptrdiff_t)pos`, and those two positions cast to `PTRDIFF_MAX` and `PTRDIFF_MIN` - whose difference does not exist in the type, even though the distance being asked about is -1. UBSan reports it, and no concurrency is involved: a legitimately full queue at that boundary is enough.
- Distances of -1, 0 and +1 taken at nine positions, including 0, the sign boundary, and `SIZE_MAX`, so the wrap to zero and the wrap across the sign are both covered.
- The far edges: the largest AHEAD distance is one below half the counter range, and half the range itself already reads as BEHIND. An off-by-one in the sign-bit test shows up here and nowhere else.
- A queue capacity at half the counter range is `PROVEN_ERR_INVALID_ARG` **before** anything is allocated. Past that limit ahead and behind stop being distinguishable, so the limit is a correctness condition, not a resource one. The existing power-of-two and minimum-size guards still hold.
- A source check that neither queue path computes a signed difference again, and that both go through the one shared helper. Two copies of a comparison this easy to get wrong are two chances to get it wrong differently.
- An ordinary capacity-two queue with one worker still accepts work, runs every accepted job exactly once, and shuts down.

Note: the test reads the classifier from `src/proven/proven_internal_jobseq.h` rather than seeding a live queue. Seeding one means compiling a second copy of `job.c` into the test executable, and one implementation compiled twice is a thing that can disagree with itself. The RFC's Appendix B probe does exactly that on purpose, as a one-off reproduction under UBSan; it is not what a registered test should be built on.

Failure tip: inspect `src/proven/proven_internal_jobseq.h` and the two call sites in `src/proven/job.c`.

### `tests/test_regression_fs_private_staging` - staging files are created private (RFC-0008 H-002)

Intent: verify that replacing a 0600 file - atomically or durably - stages the new contents in a file that is *created* 0600, and that a new copy destination is created the same way.

Sub-checks:

- Rewrites a 0600 file with `proven_fs_write_file_atomic` and requires the `.pvtmpNN` staging file to have carried no group or other bits at the instant it was created. It used to be created with `0666 & ~umask` - 0644 under the usual umask - and narrowed a moment later. A `chmod` cannot revoke a descriptor another user opened in that moment, and the private payload is then written through the file that descriptor still points at.
- Repeats it through `proven_fs_write_file_durable`, which shares the implementation.
- Copies a 0600 source to a name that does not exist yet and requires the destination to be created private too, then to end up 0600.
- Pins the unchanged default: a brand-new atomic target is still `0666 & ~umask`. Restrictive creation is for carrying an existing target's mode across, not a new default-permissions policy.
- Repeats the first case under `umask 0000`, where the default creation mode is 0666. A staging file that is still private there proves the mode came from the creating call and not from the process umask, which is shared mutable state the library must not touch.

Note: POSIX-only; compiles to a skip on Windows, whose confidentiality story is ACLs and needs a native test. The observation seam is a definition of `open()` in the test itself, which the linker binds the platform layer's call to; it forwards to the real `openat` syscall and records the mode each created file was born with. That makes the check deterministic - unlike the watcher thread in `test_regression_fs_perms_and_types`, there is no race to win. The test works under the system temporary directory and skips itself, with a reason, on a filesystem whose inherited ACLs do not honour creation modes at all.

What it does not prove: nothing here addresses a hostile writer in the directory, readers who already held the old file open, ACL preservation, or secure erasure.

Failure tip: inspect `internal_write_file_atomic` and `proven_fs_copy` in `src/proven/fs.c`, and the private-create flag in `platform/proven_sys_fs.c`.

### `tests/test_regression_fs_perms_and_types` - filesystem permissions and entry types

Intent: verify a copy carries the source's mode, that an atomic write never exposes its contents under a wider mode, that a symlink and a FIFO are `PROVEN_FS_TYPE_OTHER`, and that syncing a PRIVATE mapping is `PROVEN_ERR_UNSUPPORTED`.

Sub-checks:

- Copies a 0600 file and checks the destination is 0600. It used to be 0644: the destination was created with the process umask and the source's mode was never carried across.
- Runs a watcher thread that stats every temp file *that already holds bytes* during a 16 MiB atomic rewrite of a 0600 target. If any of them is group- or world-readable, the window is open. The temp used to be chmod'd at the end, so the whole payload sat in a 0644 file for the duration of the write.
- Walks a directory holding a dangling symlink, a FIFO and a regular file, and checks the first two are `PROVEN_FS_TYPE_OTHER`. They used to be reported as regular files - files a caller cannot open, or that block forever on a writer who never comes.
- In the same walk (RFC-0009 P-102, where most entries are answered from `d_type` without a stat): the dangling link and a link to the file are marked `is_symlink`, the FIFO and the regular file are not, the regular file carries its real size, a subdirectory is `DIR` and not a symlink, and a symlink to it is `DIR` and marked as one.
- Writes through a PRIVATE mapping, syncs, and requires `PROVEN_ERR_UNSUPPORTED`; then does the same through a SHARED mapping and requires the bytes to be on disk.

Note: POSIX-only; compiles to a skip on Windows. The `close()`-failure defect from the same audit cannot be provoked without an `LD_PRELOAD`, so it is pinned by the `[[nodiscard]]` on `proven_fs_close` instead - the compiler now refuses to let a write path ignore it.

Failure tip: inspect `proven_fs_copy` and `internal_write_file_atomic` in `src/proven/fs.c`, the `is_regular` mapping in `platform/proven_sys_fs.c`, and `proven_mmap_sync`.

### `tests/test_contract_allocator_trait` - the trait means the same thing for every allocator

Intent: verify `alloc(0)`, `realloc(ptr, 0)` and over-aligned allocations answer identically for the heap and the arena, and that shrinking a non-tail block in a *full* arena still succeeds.

Note: the same function body runs against both allocators, which is the whole point of a trait. Before this, `alloc(0)` was `NOMEM` on the heap (a lie - nothing was out of memory) and `PROVEN_OK` with a live pointer on the arena; `realloc(ptr, 0)` returned NULL on one and a live pointer on the other, though the trait documents NULL; and asking an arena to make a block *smaller* could fail with `NOMEM`.

Failure tip: inspect `src/proven/heap.c`, `src/proven/arena.c`, and the contract in `include/proven/allocator.h`.

### `tests/test_regression_stream_partial_write` - partial writes, failed reads, and `{:f}`

Intent: verify a sink that accepts only part of a chunk receives every byte exactly once; that a read failure reaches the caller as `PROVEN_ERR_IO` rather than as a clean end of file; and that `{:f}` forces the fixed form at any magnitude.

Sub-checks:

- Drives a buffered writer over a sink that never accepts more than 700 bytes at a time, and checks the sink receives exactly the 6000-byte payload, in order. The first buffered writer kept the whole buffer after a partial write and re-sent it, so the sink received 10,096 bytes with the first 4096 duplicated.
- Drives `proven_writer_write_partial` over a sink that takes 8 bytes and then fails, and checks the caller is told both facts: the error, and the 8.
- Drives a buffered reader over a source that yields 4 bytes and then fails, and checks the second read reports `PROVEN_ERR_IO`. It used to report a clean EOF, making a file truncated by a disk error indistinguishable from a complete one.
- Checks `{:.1f}` on `1e20` and `{:.8f}` on `1e-7` contain no exponent, and that plain `{}` on `1e20` still chooses the shorter scientific spelling.

Note: all three defects were in code written the same day, and all three passed every test that existed - because every sink the tests used behaved perfectly. The bug in each case was a contract that only a well-behaved sink could honour.

Failure tip: inspect `writer_buffered_flush` and `reader_buffered_fill` in `src/proven/stream.c`, and `never_scientific` in `src/proven/float_format.c`.

### `tests/test_regression_fmt_spec_silently_wrong` - formatter specs that used to be silently wrong

Intent: verify `{:08}` zero-pads instead of eating the `0` as a width digit, and that a spec the argument cannot honour (hex on a double or a string) is rejected rather than ignored.

Note: both defects failed the worst way available - silently. `{:08}` on 42 produced `"      42"` and returned OK; `{:x}` on a double printed `3.500000` and returned OK. A spelling that is accepted and quietly does the wrong thing is worse than one that is rejected.

Failure tip: inspect the spec parser and the applicability guard in `src/proven/fmt.c`.

### `tests/test_regression_fs_copy_to_self` - filesystem self-copy regression

Intent: verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the file.

Sub-checks:

- Creates a source file with known content.
- Attempts to copy the file to the same path and expects `PROVEN_ERR_INVALID_ARG`.
- Reads the file back and verifies size and contents are unchanged.
- Creates a hard link to the same file when supported.
- Attempts to copy across the hard-linked paths and expects failure without corruption.

Failure tip: inspect same-file detection and open/truncate ordering in filesystem copy code. The destination must not be opened with truncation before proving it is not the same file as the source.

### `tests/test_regression_fs_slurp` - filesystem whole-file read/write

Intent: verify whole-file reads go to EOF rather than to a pre-measured size, and that the whole-file write entry points round-trip.

Sub-checks:

- Round-trips a regular file through `proven_fs_write_file` and `proven_fs_read_all`.
- Verifies `proven_fs_write_file` truncates a longer existing file.
- Verifies an empty file reads as `{NULL, 0}` with `PROVEN_OK`.
- Reads a source whose size cannot be known up front (`/proc/self/status`, whose `st_size` is 0) and requires a non-empty result. Skipped where the path is unavailable.
- Verifies `proven_fs_read_all_u8str` is NUL-terminated and valid, including for an empty file.
- Verifies `proven_fs_write_file_atomic` replaces the contents, creates a missing target, and leaves no temp file behind.
- Verifies `proven_fs_read_all_bounded` reads a file of exactly `max_bytes`, refuses one byte more with `PROVEN_ERR_OUT_OF_BOUNDS` without allocating a buffer, reads an empty file under a bound of 0, and - on POSIX - refuses `/dev/zero` at a 4096-byte bound without the buffer growing past it and `/proc/self/status` under a 1-byte bound (RFC-0009 S-003).
- Verifies an invalid allocator and a missing path are rejected as values.

Failure tip: `proven_fs_size` reports 0 for anything that is not a regular file, so the reported size may only seed the read capacity - never bound the read. Inspect `internal_slurp_path` and `internal_read_to_eof` in `src/proven/fs.c`.

### `tests/test_regression_fs_staging_names` - random staging names

Intent: verify that leftover or planted staging files never block an atomic or durable whole-file write (RFC-0009 D-001).

Sub-checks:

- `proven_fs_is_staging_name` accepts `.pvtmp` plus 13 characters from `0-9a-v` and the old `.pvtmp` plus two digits, and refuses a character outside the alphabet, upper case, a wrong length, a bare suffix and an ordinary name.
- With the eight old fixed names `<path>.pvtmp00` .. `07` present (the reproducer), both `proven_fs_write_file_atomic` and `proven_fs_write_file_durable` succeed.
- With 64 more names of the new shape planted, 32 atomic writes in a row succeed, and the planted files keep their contents.
- Afterwards exactly the 72 planted names have the staging shape: no write left its own.
- A write into a missing directory is `PROVEN_ERR_NOT_FOUND` at once, not retried.

Failure tip: inspect `internal_write_file_atomic` and `internal_tmp_bits` in `src/proven/fs.c`. A failure past the planted names means the suffix is predictable again; a higher staging count means a rename or cleanup path lost its temp file.

### `tests/test_regression_fs_refusal_codes` - refusal codes for directory, mode, link and lock calls

Intent: verify that `proven_fs_mkdir`, `proven_fs_rmdir`, `proven_fs_chmod`, `proven_fs_link`, `proven_fs_lock` and `proven_fs_rename` say which refusal they met instead of `PROVEN_ERR_IO` for all of them, and that `proven_fs_mkdir_all` creates a whole path.

Sub-checks:

- The reproducer: the second `proven_fs_mkdir` of one path is `PROVEN_ERR_EXISTS`. So is a mkdir over a file; a mkdir under a missing parent is `PROVEN_ERR_NOT_FOUND`.
- `proven_fs_rmdir` of a missing name is `PROVEN_ERR_NOT_FOUND`; of a directory that is not empty it is `PROVEN_ERR_INVALID_STATE`, as is `proven_fs_remove` of it, and the directory stays.
- POSIX, where a second file system is found (`/dev/shm`, `/tmp`, `/var/tmp`): a hard link and a rename across file systems are `PROVEN_ERR_UNSUPPORTED`, and the rename leaves its source. Skipped with a reason otherwise, and on Windows.
- `proven_fs_chmod`, `proven_fs_link` and `proven_fs_rename` of a missing name are `PROVEN_ERR_NOT_FOUND`; a hard link onto a name that is taken is `PROVEN_ERR_EXISTS`.
- A lock held by someone else and asked for with `wait == false` is `PROVEN_ERR_BUSY`. The first holder is a child process on POSIX, where record locks belong to the process, and a second handle on Windows, where they belong to the handle.
- POSIX, not as root: a mkdir inside a `0555` directory is `PROVEN_ERR_PERMISSION`, and so is `proven_fs_mkdir_all` through it. Skipped with a reason where the filesystem does not honour the mode.
- `proven_fs_mkdir_all` creates four missing levels in one call, is `PROVEN_OK` when repeated, creates one level under an existing parent, accepts a trailing and a doubled separator and `.`, answers `PROVEN_ERR_EXISTS` for a file at the last name or in the middle of the path and leaves the file a file, and answers `PROVEN_ERR_INVALID_ARG` for the empty path.
- `proven_fs_mkdir_all` with an absolute path creates two missing levels and is `PROVEN_OK` when repeated; the root (`/`, or the drive root on Windows) is `PROVEN_OK`. On Windows an extended-length `\\?\` path and a UNC path through the drive's administrative share are created too; the UNC case is skipped with a reason where that share cannot be reached.

Failure tip: inspect `path_refusal` and the `*_checked` functions in `platform/proven_sys_fs.c`, then `internal_err_from_refusal` and `proven_fs_mkdir_all` in `src/proven/fs.c`. `PROVEN_ERR_IO` where a reason is expected means the platform reason was dropped; `PROVEN_OK` over a file means `proven_fs_mkdir_all` stopped asking what is there.

### `tests/test_regression_platform_text` - platform text: environment values and directory names

Intent: verify text that comes from the platform follows the strict-text rule, and that an empty environment value is not an error (RFC-0009 D-002, D-003).

Sub-checks:

- A variable set to the empty string reads as `PROVEN_OK` with length 0; once unset it is `PROVEN_ERR_NOT_FOUND`. (On Windows it read as `PROVEN_ERR_IO`.)
- A value holding a lone surrogate (Windows) or bytes that are not UTF-8 (POSIX) is `PROVEN_ERR_INVALID_ENCODING`; a valid UTF-8 value reads back exactly.
- In a directory holding `good.txt` and an entry whose name is not valid text, `proven_fs_dir_next` reports the bad entry once as `PROVEN_ERR_INVALID_ENCODING` with an empty name and its type filled in, and still lists `good.txt`.
- `proven_fs_list` on that directory is `PROVEN_ERR_INVALID_ENCODING` as a whole.
- `proven_fs_walk_next` reports the bad entry once, naming the directory it is in, and goes on.
- The directory part is skipped, and says so, where the filesystem refuses such a name.

Failure tip: inspect `platform/proven_sys_env.c`, `proven_env_get` in `src/proven/sysio.c`, `proven_sys_fs_dir_step`, and `internal_dir_step_text` and the walk in `src/proven/fs.c`. A `U+FFFD` or raw bytes coming back as `PROVEN_OK` is the defect.

### `tests/test_regression_scanner_rollback` - scanner rollback after a failed scan

Intent: verify a scan that fails on an oversized token restores the stream exactly - dropping no byte and duplicating none.

Sub-checks:

- Scans a token successfully, then snapshots the bytes the scanner holds unconsumed.
- Scans a token too large for the scanner buffer and requires failure.
- Requires the unconsumed bytes after the failure to match the snapshot in both count and content.
- Requires a retry of the oversized token to fail the same way, not succeed from stale bytes.
- Scans the same file with a buffer large enough and requires every token to come back in order.

Failure tip: `scanner_fill` compacts the buffer (it memmoves unconsumed bytes to the front and resets the cursor). A snapshot taken before compaction cannot be written back afterwards without accounting for how far the contents moved. Inspect the rollback in `proven_sysio_scanner_scan_impl`.

### `tests/test_regression_sort_duplicates` - sort on duplicate keys

Intent: verify `proven_array_sort` stays sub-quadratic on duplicate and degenerate input.

Sub-checks:

- 20,000 all-equal keys: comparison count must stay below 40n. A two-way partition that sends equal elements to one side needs ~2x10^8 comparisons here.
- 20,000 keys drawn from 8 distinct values: comparison count below 60n.
- Sorted, reverse-sorted, and organ-pipe orderings: comparison count below 60n each - the classic quicksort killers, bounded by median-of-three plus the heapsort depth fallback.
- 48-byte elements with a payload tied to the key, to catch a torn bulk swap.

Note: this suite counts comparisons, not wall-clock time. A timing threshold is a flaky test on a shared machine; the comparison count is exactly what blew up.

Failure tip: inspect the partition in `src/proven/algorithm.c`. Equal elements must be collected into a run that is final and never recursed into.

### `tests/test_regression_time_fmt_neg_year` - `u8` and `u16` agree on zero-filled negative years

Intent: verify `proven_time_u8_fmt` and `proven_time_u16_fmt` render the same string for a zero-filled negative year - `{year:0>4}` of `-44` is `"-044"` in both, with the sign counted toward the field width like `printf %0Nd` - and that positive years still agree.

Failure tip: inspect `proven_time_u16_fmt` in `src/proven/time.c`. It delegates to the u8 path and widens, so the sign counts toward the pad width. The old hand-rolled u16 path padded to full width THEN prepended the sign, one column wider than the fmt.h-based u8 path.

### `tests/test_regression_base64_decoded_size` - Base64 decode sizing round-trips its own output

Intent: verify `proven_base64_decoded_size` is an upper bound for UNPADDED input too (so the library can decode its own base64url output into a `decoded_size()`-sized buffer), and that `proven_base64_decode` / `proven_hex_decode` refuse a `{out=NULL, out_cap>0}` argument with `INVALID_ARG` rather than storing through NULL, matching the encoders.

Failure tip: inspect `proven_base64_decoded_size` (`(n+3)/4*3`, not `(n/4)*3`) and the NULL-out guards in `src/proven/encode.c`. Found by the standing audit; the unit test missed the sizing by using one oversized buffer.

### `tests/test_regression_v26_05` - v26.05 regressions

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

### `tests/test_regression_v26_07` - v26.07 regressions

Intent: protect the fixed `u8str` NUL-seal, datetime formatting, and pool init defects.

Sub-checks:

- `proven_u8str_reserve` on a zero-initialized string leaves `ptr[len] == 0`, so `proven_u8str_as_cstr` is readable and `proven_u8str_is_valid` accepts it.
- A format that produces no output still leaves the string sealed after its growth allocation.
- A `proven_datetime_t` with year `-1` renders as `-0001-...`, and `INT32_MIN` renders correctly.
- A `proven_pool_init` whose bin allocation fails leaves `bin_cap == 0`, so the free trait cannot write through a null bin.

Note: the string checks allocate from an arena over deliberately poisoned backing memory. Allocators do not return zeroed memory, and on a quiet heap a fresh block is often zero by luck - which is exactly why these defects went unnoticed.

Failure tip: each section names one area - `proven_u8str_reserve` in `u8str.c`, the growth branch and `PROVEN_ARG_DATETIME` case in `fmt.c`, the init ordering in `pool.c`.

## Portability tests

Freestanding builds, cross-target builds, source-level platform contracts, and the build driver's own standard probe. Most of these cannot be *run* on the host, so they check what can be checked: that the code compiles, links where configured, and keeps its platform branches intact.

### `tests/test_portability_source_contracts` - source portability contracts

Intent: guard platform branches and documentation/test-output contracts that may not be executable on the current host.

Sub-checks:

- Checks Windows directory open allocation guards before `FindFirstFileW` use.
- Checks Windows `FILETIME` to Unix time conversion uses the correct epoch delta and underflow guard.
- Checks POSIX mmap stores offsets in `off_t` and rejects truncation.
- Checks Windows append mode uses `FILE_APPEND_DATA` rather than a one-time seek-to-end emulation.
- Checks the Windows fixes from the first native full-suite run (B-035): remove falls back to `RemoveDirectoryW` for an empty directory, both read paths map `ERROR_BROKEN_PIPE` to EOF, positioned I/O requires `FILE_TYPE_DISK`, and pread/pwrite restore the file position.
- Checks Windows environment key conversion sizes the wide buffer dynamically rather than using a fixed 255-byte stack buffer.
- Checks public environment lookup no longer rejects large keys through a fixed C key buffer.
- Checks 32-bit Linux `_llseek` uses a 64-bit result buffer.
- Checks the job system keeps a single admission state and explicit begin/end submit helpers.
- Checks idle workers wait on the PAL counting semaphore, submission and close
  post retained permits, POSIX uses a condition-variable wait, and Windows uses
  a kernel semaphore.
- Checks `nob.c` keeps structured test metadata and emits standard begin, intent, failure-hint, failure, and pass log lines.
- Checks `nob.c` and the test share one preprocessed dependency manifest, and every header in the declared library, PAL, test, and example roots is active in it.
- Checks the test-catalog gate uses the portable directory iterator instead of POSIX-only `dirent.h`.
- Checks this `TEST.md` documents failure tips, sub-checks, and the log format.
- **RFC-0008 H-005**: the Windows rename replaces an existing destination (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`), does not fall back to a cross-volume copy, and does not delete the destination first. `MoveFileW` fails outright when the destination exists, so on Windows the *first* whole-file atomic write to a name succeeded and every write after it failed. Deleting first would open an interval in which the name does not exist, which is the one thing an atomic replacement exists to prevent.
- **RFC-0008 H-006**: the Windows entropy length is planned in chunks the backend accepts instead of being cast whole to `ULONG`, and a failed chunk fails the whole call rather than falling back to a PRNG. Above `ULONG_MAX` that cast narrowed silently - a request for exactly 2^32 bytes asked the OS for **zero** - and success for the short request was returned as success for the whole buffer, leaving untouched bytes to be read as fresh entropy.
- The chunk planner itself is exercised, not just grepped: 0, 1, limit-1, limit, limit+1 and 2*limit+1 against a reduced artificial limit, plus a whole plan walked to check it covers the buffer with no gap and no overlap. A reduced limit is what makes those boundaries testable without allocating gigabytes or constructing a pointer outside a real object.

Note on the two Windows rows: neither is a runtime result. This host has never run a Windows binary. What it has is `./nob cross`, which compiles both Windows targets, and these source contracts. Both defects stay open for native verification.
- Every function named `*_internal` / `*_impl` declared in a public header (from the header manifest, the alias layer aside) is on a closed list of seven and is marked MACRO SUPPORT within the lines above it; all seven are still declared (RFC-0009 X-004; a planted eighth is caught).
- `nob.c` takes its library sources from `build_sources.inc` (listed in the header manifest, so a change invalidates outputs) and keeps no hand-written list of hosted-only sources (RFC-0009 X-005).

Failure tip: source-contract tests should stay narrow. If a source pattern changes legitimately, update the contract to the new safe pattern in the same commit as the source change and explain it in docs.

### `tests/test_portability_cross_compile_smoke` - cross compile smoke

### `tests/test_portability_cross_link_smoke` - cross link smoke

### `tests/test_portability_freestanding_nocrt_link` - freestanding no-CRT link

Intent: prove the freestanding runtime contract (B-034) by linking, for every freestanding cross target, all freestanding library objects with a program that supplies only `memcpy`, `memmove`, `memset` and `memcmp`, using `-nostdlib -nostartfiles -static` and the compiler support library (`-lgcc`). A static link fails on any unresolved symbol, so success means nothing else is needed. The executable is never run.

Failure tip: the linker names the undefined symbol. Route it through the platform layer, or name it as a required service in the freestanding guide - not both silently.

### `tests/test_portability_float` - float portability

Intent: verify scan and format float conversion paths stay double-only and keep target-deterministic behavior without long double dependence.

Failure tip: inspect src/proven/scan.c and src/proven/fmt.c if long double returns, casts, or target-specific float drift reappear.

### `tests/test_portability_nob_std_probe` - build driver standard probe

Intent: verify nob probes -std=c23 first and falls back to -std=c2x when the compiler rejects c23.

Failure tip: inspect nob.c standard-flag selection and toolchain probing if the fallback does not trigger.

### `tests/test_portability_compile_nonet` - PROVEN_NO_NET leaves the socket layer out

Intent: verify the promise `PROVEN_NO_NET` makes to a consumer who wants no sockets. The suite is built without the macro, so nothing else would notice it breaking.

Sub-checks:

- Every source in `build_sources.inc` compiles with the host compiler and `-DPROVEN_NO_NET -Wall -Wextra -Werror`.
- A program that includes `proven.h` and `proven/alias_xcv.h` links against that build and runs, and `net.h` was not included.
- No object of that build defines or references `proven_net_*`, `proven_transport_*`, `proven_sys_net_*` or a socket call.

The fixture runs the host compiler through `/bin/sh` and is skipped on Windows.

Failure tip: inspect the `PROVEN_NO_NET` guards in `include/proven.h`, `src/proven/net.c` and `platform/proven_sys_net.c`.

### `tests/test_portability_nob_clean` - build driver clean

Intent: verify `./nob clean` removes the selected build root and nothing else: `.`, `/`, a path with `..` and a directory without the `.proven-build-root` marker are refused with nothing deleted, and a symlink inside a marked root is removed without its target being entered.

Failure tip: inspect `clean_build_root`, `build_root_path_is_safe` and `remove_tree_no_follow` in nob.c; a surviving target means links are removed, never followed.

## Documentation tests

The documentation is checked by the build, not by eye: every public function has an alias, every example the manual prints is a program that compiles and runs, and no example drifts from its chapter.

### `tests/test_docs_manual_depth` - every module section is documented to depth, not merely mentioned

Intent: a **gate on the shape of a section**. The symbol checks prove a module is *mentioned*; they cannot prove it is *documented*, and that is exactly where the manual failed - the five modules added in the v26.07.13 line each had an intent paragraph and a table, and **not one had a counter-example**. They passed every check that existed and were still half-written.

For each module section registered in the test, it must carry:

- **real prose** - enough words outside the tables and the code fences to actually explain *why* this exists, not just *what* the calls are. Why is the half a reader cannot reconstruct from the header file.
- **a reference table** - what each call does, what it returns, and which one can *fail*.
- **the structures the caller declares** - a `text` listing with the role of each field (and, for caller-owned state, the rule that it must not be copied).
- **a runnable example** - an `<!-- example: -->` marker, so the build compiles and runs it.
- **at least one counter-example** - a `text` block showing the code a reader would actually write and should not.

A section that is legitimately exempt from *structures* or *an example* declares that **in the test, in code, with a reason**. A gate that cannot be argued with is a gate people route around; an exemption that has to be written down is one that has to survive being written down.

Failure tip: the section and the missing element are named. This is `DOCUMENTING.md` section 3 turned from advice into a gate.

### `tests/test_docs_manual_claims` - every factual claim the new chapters make is true

Intent: the manual makes **claims**, and each is a proposition that is either true or false. Prose cannot be test-driven; a claim can be *tested*. You write the assertion the sentence implies, and the build decides whether the sentence is still true.

This is what `test_docs_manual_ch08_contracts` does for the scanner chapter, done for the modules added this cycle. It exists because prose ages worst of anything in a repository: the README said "`proven` exposes no fsync" for a month after `proven_fs_sync` shipped, and nothing objected - because nobody had written down what that sentence was asserting.

Sub-checks (each quotes the claim it tests): the CRC-32 check value the chapter's interoperability promise rests on; chained `crc32_update` equalling the one-shot, because the chapter tells readers they may store the intermediate value and resume; `PROVEN_SHA256_SIZE`; the standard SHA-256 of `"abc"` and its 64-character hex; the streaming digest equalling the one-shot ("depends only on the bytes, never on how they were chunked"); base64url emitting no padding; both alphabets decoding, padded or not; `decoded_size` being an upper bound for unpadded text; a refused encode writing **nothing**; whitespace being `INVALID_ENCODING` rather than skipped; a stray character committing nothing; seed 0 not being degenerate; an unseeded generator presenting an **invalid** trait and yielding zeros; `rng_below` respecting its bound; `rng_f64` never returning 1.0; an inverted range returning `lo`; a line that **exactly fills** the buffer being returned while a longer one is refused.

**The rule for adding to this file:** when you write a sentence a reader could act on - a value, a boundary, a refusal, a guarantee - write the assertion for it here. If you cannot state the assertion, the sentence is too vague to be in the manual.

Failure tip: the claim is named. Either the code changed and the manual did not, or the manual was wrong when it was written - decide which before changing either.

### `tests/test_docs_manual_symbols` - the manual and the headers name the same functions

Intent: verify the two agree in **both** directions, because each direction fails differently and each has already happened here.

Sub-checks:

- **Headers -> manual:** every public function is named somewhere in `manual/`. A function nothing documents is a feature nobody can find - `proven_fs_dir_open/_next/_close`, the streaming directory API and the answer to `proven_fs_list` reading a 50,000-entry directory into 4.2 MB before you see any of it, went undocumented for months and nothing noticed. (The PAL, `proven_sys_*`, is exempt: it is an internal layer for porting, not the API a caller programs against.)
- **Manual -> headers:** the manual does not document a function that does not exist. This is the worse direction - the reader writes the call and the *linker* tells them, which is the moment they stop trusting the manual. Two were live: `proven_sysio_flush`, deleted while the manual went on declaring it as public API in the present tense; and `proven_pool_free`, which never existed at all (the real symbol is a static `proven_pool_free_trait`, and freeing a pool slot goes through the allocator trait).
- Writing a name as a **call** - `proven_x(...)` - is what counts as claiming it exists. A family wildcard (`proven_fs_*`) is not a claim, and a past-tense historical note about a deleted function is not one either.

Failure tip: the name is printed. It is either a function you added without documenting, or one the manual promises and the linker will refuse. See `DOCUMENTING.md`.

### `tests/test_docs_manual_usage` - every public function is shown in working code

Intent: verify the manual **demonstrates** every public function, not merely that it names one.

`tests/test_docs_manual_symbols` is satisfied by a reference-table row, which gives a reader the spelling of a call and nothing else: not when to reach for it, not what its arguments have to be true of, not what its failure means. That is how half the API was documented - 86 of 279 public functions had a table row and no line of code anywhere that used them, `proven_fs_sync_dir` among them, without which the atomic-replace recipe the chapter describes is not actually durable.

Sub-checks:

- Every public function appears in a `manual/examples/*.c` program - each of which the build **compiles and runs** - or in a ```` ```c ```` block, all of which `check_manual_code_blocks` compiles.
- A ```` ```text ```` block does not count. Those are the signature listings and the deliberate counter-examples, and neither demonstrates anything.
- A **macro wrapper counts for the function it expands to**: `PROVEN_ARRAY_PUSH` is how a caller is meant to write `proven_array_push`. The macro bodies are read out of the public headers and followed.
- A **`_Generic` dispatch table does not count**. `PROVEN_ARG` names every argument constructor there is, so following it would mark twenty-one functions as demonstrated by an example that formats one integer. It lists alternatives rather than calling them.
- The PAL (`proven_sys_*`) is exempt, as it is for the naming gate: it is the porting layer, not the API a caller programs against.

Failure tip: the unshown function is named. Put the call in an example where it belongs - or write one - next to the sentence saying why a reader would want it.

### `tests/test_docs_manual_ko` - the Korean edition mirrors the English one

Intent: verify the translation has not quietly fallen behind, and that it keeps the vocabulary promise chapter 0 makes.

`manual-ko/` was checked by nothing. Six gates guard the English chapters - their examples are compiled and run, their symbols matched against the headers, their claims asserted - and the translation beside them was guarded by nobody. So a chapter could gain a whole worked example on one side and not the other and the build would still pass, which is exactly what happened: eleven examples existed in English and in no Korean chapter.

Sub-checks:

- **Structure:** every `manual/x.md` has a `manual-ko/x-ko.md`.
- **Coverage:** every `<!-- example: -->` marker appears in both editions, in either direction. The bodies themselves are checked verbatim by `test_docs_manual_examples`, which reads both directories.
- **Vocabulary:** a Korean chapter that uses an English term must write it at least once paired with its Korean word - `할당자(allocator)`, or the reverse order the glossary rows use. The pairing is required once per chapter, not per occurrence: after the introduction, the bare word is the ordinary way to write it.

Failure tip: the failure names the missing chapter, the example only one edition prints, or the chapter and the exact pair to write. `scripts/sync-manual-examples.py` copies example bodies into both editions.

### `tests/test_docs_test_catalog` - the test catalog matches the build

Intent: verify every test registered in `nob.c` has one entry in this file, every registry contains unique paths, `regression_tests[]` is a hosted subset with exactly the membership listed above, and all published suite, tree, and filename-class counts match the registries and test files.

Failure tip: a failure names the missing test, duplicate path, stale subset member, or stale count. Reconcile this catalog with every registry array and `tests/test_*.c`; do not update a list or total independently. Ten registered tests once had no entry, three full-suite entries were duplicated, the regression list omitted twenty members, and the headline and class counts had drifted because the old gate ignored those values.

### `tests/test_docs_version_sync` - the version string agrees with itself everywhere

Intent: verify `PROVEN_VERSION_STRING` - the source of truth - is `proven_c_lib-v<MAJOR>.<MINOR>.<PATCH>` built from the three number macros, and matches the README (both language halves), TEST.md, the manual headings, chapter 1's `version.h` excerpt (string and all three numbers), and the CHANGELOG's newest `## [x.y.z]` entry. Versions are semantic from v0.0.1 (2026-09-04); the date-based numbers before it are history.

`CHECKLIST.md` has always required these to be updated together, and nothing checked: `version.h` once sat five releases behind the CHANGELOG while the README claimed a third value that matched neither. That is not cosmetic - it is the number a downstream project pins, the number a bug report quotes, and the number that decides whether a fix is in the copy someone is holding.

Failure tip: bump the version in every place the test names.

### `tests/test_docs_alias_completeness` - alias layer completeness

Intent: verify every public `proven_*` function has an `xcv_*` alias in `include/proven/alias_xcv.h`.

Sub-checks:

- Parses every header in `include/proven/` (excluding the alias header itself) for public function declarations.
- Requires each one to appear as the target of an `xcv_*` alias; names any that do not.

Note: `tests/test_docs_alias_smoke` only checks that a hand-picked subset of aliases compiles. It cannot notice a *missing* alias, which is how 25 public functions ended up with none. This test closes that gap: adding a public function without an alias now fails the build.

Failure tip: add `#define xcv_<name> proven_<name>` to `include/proven/alias_xcv.h`, keeping the file alphabetical. A half-covered alias layer fails at the caller's call site, not here.

### `tests/test_docs_alias_smoke` - alias layer smoke

Intent: verify the public XCV alias layer compiles and maps representative aliases to canonical proven APIs.

Sub-checks:

- Uses native integer scan aliases.
- Uses scan function aliases.
- Uses formatting argument aliases.
- Uses macro aliases that are expected to stay available for the alias layer.

Failure tip: inspect `include/proven/alias_xcv.h` and `tests/test_docs_alias_smoke.c`. When public symbols are added, renamed, or removed, update the alias header and this smoke test together.

### `tests/test_docs_manual_ch08_contracts` - manual chapter 8 scanner contracts

Intent: verify every behaviour manual chapter 8 states as fact about the scanner is actually true - error codes, cursor restoration on failure, decimal-only integers (`0x10` is zero), the overflow/underflow asymmetry, and the non-transactional structural scan.

Note: prose is where a contract goes to drift. Chapter 8 makes 18 factual claims about the scanner; this test makes each one executable. A false claim fails the build and names itself.

Failure tip: find the named claim in `manual/manual-08-fmt-scan.md` and decide which side is wrong before changing either.

### `tests/test_docs_manual_examples` - manual examples match the manual

Intent: verify every example the manual prints exists in manual/examples/, matches it verbatim, and that no example file is left unquoted.

Failure tip: the example file is the source of truth: it is compiled and run. Copy its body into the chapter rather than hand-editing the chapter to look right.

## Differential tests

Correctness against an independent oracle - the host libc, or a corpus with known-good answers. These catch what a self-written expectation cannot: a wrong belief held consistently by both the code and its test.

### `tests/test_differential_float_corpus_f32` - float upgrade corpus float32 coverage

Intent: verify the upgrade corpus source also keeps the documented float32 shortest literals pinned alongside the existing float64 cases.

Failure tip: inspect tests/test_differential_float_corpus_f64.c if the float32 corpus section disappears or drifts from the documented literals.

### `tests/test_differential_float_corpus_f64` - float upgrade corpus

Intent: verify the representative exact-range, subnormal-boundary, and shortest-format corpus stays pinned to the documented spellings while the float upgrade remains staged.

Failure tip: inspect src/proven/scan.c and src/proven/float_format.c if a representative corpus value changes bit pattern or shortest spelling.

### `tests/test_differential_float_host_oracle_f32` - float host oracle float32

Intent: verify representative finite float32 fixed-format rendering matches the platform C library on the same inputs without sharing implementation code.

Failure tip: inspect src/proven/float_format.c if the float32 fixed formatter stops matching the host oracle corpus.

### `tests/test_differential_float_host_oracle_f64` - float host oracle

Intent: verify representative finite float parsing and simple fixed-format rendering match the platform C library on the same inputs without sharing implementation code.

Failure tip: inspect src/proven/scan.c and src/proven/float_format.c if the host oracle and library disagree on the representative finite corpus.


## Stress tests

Concurrency under a sanitizer, over enough iterations to make a race likely rather than theoretical.

### `tests/test_differential_find_last_oracle` - find_last against a brute-force oracle

Intent: verify `proven_u8str_view_find_last` against a memcmp-at-every-position oracle across all of its paths.

Sub-checks:

- 120,000 fixed-seed cases: dense 1-4 symbol alphabets, single-byte runs, the periodic `"aab"` haystack and arbitrary bytes; needles of 1, 64 and 65 bytes (the path boundaries), 65-214 bytes (the reverse Two-Way range) and random lengths; needles copied from the haystack so matches occur. The share of cases with a match is printed and must exceed a quarter.
- Planted defects each failed it before it was trusted: unreversed Shift-Or masks, skipping a whole needle past a match, a 65-byte needle sent to Shift-Or (RFC-0005), and for B-024 an inverted Two-Way periodicity flag, a Two-Way result off by one, and an anchored scan that stops at its first failed candidate.

Failure tip: inspect `proven_u8str_view_find_last`. The printed needle length and shape name the path: 1 byte scan; 2-64 on shapes 0-2 backward Shift-Or, on shape 3 the anchored scan; 65+ on shapes 0-2 reverse Two-Way.

### `tests/test_stress_job_concurrency` - job queue stress

Intent: verify the job queue tolerates concurrent producers and a close racing
active submitters, drains every accepted job exactly once, and releases every
parked worker.

Failure tip: run this under TSAN first; inspect queue admission,
publish-before-post ordering, semaphore permit accounting, and close wakeups if
a slot count drifts or a producer stalls.


## Regression subset

`./nob regression`, `./nob regression-asan`, and `./nob regression-ubsan` currently run:

- `tests/test_regression_v26_05`
- `tests/test_unit_map_owned_key`
- `tests/test_contract_map_hardening`
- `tests/test_contract_pool_misuse`
- `tests/test_contract_public_structs`
- `tests/test_regression_fs_copy_to_self`
- `tests/test_regression_fs_slurp`
- `tests/test_regression_fs_staging_names`
- `tests/test_regression_fs_refusal_codes`
- `tests/test_regression_scanner_rollback`
- `tests/test_regression_v26_07`
- `tests/test_regression_sort_duplicates`
- `tests/test_regression_scanner_float_split`
- `tests/test_regression_time_fmt_neg_year`
- `tests/test_unit_time_fmt_u16_parity`
- `tests/test_regression_scanner_short_read`
- `tests/test_regression_float_exact_pow5`
- `tests/test_regression_job_permit_starvation`
- `tests/test_regression_float_parse_concurrency`
- `tests/test_regression_map_churn`
- `tests/test_contract_sort_alignment`
- `tests/test_contract_allocator_trait`
- `tests/test_regression_fs_walk_errors`
- `tests/test_unit_random`
- `tests/test_unit_rng`
- `tests/test_unit_map_keyed`
- `tests/test_unit_hash`
- `tests/test_unit_hash_legacy`
- `tests/test_unit_fs_walk`
- `tests/test_regression_fs_backslash_parent`
- `tests/test_regression_job_seq_wrap`
- `tests/test_regression_fs_private_staging`
- `tests/test_regression_fs_perms_and_types`
- `tests/test_regression_stream_partial_write`
- `tests/test_regression_fmt_spec_silently_wrong`
- `tests/test_portability_source_contracts`

Intent: provide a short feedback loop for bug-fix work without running every hosted example and container test.

What it checks:

- Historical behavioral bugs remain fixed.
- Filesystem destructive edge cases remain protected.
- Float parsing, scanner refill, and formatting regressions remain covered.
- Allocator, map, sort, random, hash, and concurrency contracts stay in the short loop.
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
- `utf.h` converts UTF-8 to raw `proven_u16` units there, as the freestanding guide says it does.
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

`./nob bench-float` runs all three benchmarks - the name is historical; it is not float-only:

- `tests/test_bench_primitives` - times the hashes, the encoders, and the two random generators.
- `tests/test_bench_float_parse_paths` - times the shared float parser, its wrapper, and the host `strtod` on path-oriented decimal corpora, and records dated output.
- `tests/test_bench_float_parse` - times the decimal parser against the host `strtod` on a mixed corpus. Like the round-trip test above, this file existed but was never registered; it is registered now.

Benchmarks are not correctness gates. A timing regression is a signal to investigate, not a build failure; a checksum drift is a correctness signal and must be.

### `tests/test_bench_primitives` - primitive throughput benchmark

Intent: time the hashes (FNV-1a, CRC-32, SipHash-2-4, SHA-256), the encoders (hex, Base64), and the two random generators (xoshiro256\*\*, ChaCha20) over a fixed buffer, folding each output into a checksum so the work is not optimised away.

Failure tip: if a checksum drifts the backend changed behaviour; if a timing regresses, inspect the module named by the backend label. See `primitives-benchmark.md`.

### `tests/test_bench_float_parse_paths` - float parse path benchmark

Intent: compare the shared float parser, wrapper, and host strtod on separate path-oriented decimal corpora and record dated docs output.

Failure tip: inspect src/proven/float_parse.c, src/proven/float_decimal.c, and the path-specific corpus split if the timing harness fails or any checksum drifts.

### `tests/test_bench_float_host` - float engine against the host C library

Intent: time parsing and formatting against `strtod`/`snprintf` on fixed-seed corpora, and check every result against the host in the same run.

- Parse: `%.6g`, shortest (~16 digit), `%.17g` and `%.25g` spellings of a normal-magnitude corpus; bits must equal `strtod`'s. The 25-digit spelling is past what a 64-bit mantissa holds, so it times the bounded long-input path.
- Format: shortest (timed against `%.17g`), `%f` with 6 digits, `%e` with 16, on normal-magnitude and uniform-bit-pattern corpora; `%f`/`%e` bytes must equal `snprintf`'s and shortest must round-trip. Every call's result is checked and the buffer cleared first - the first draft scored a failed call by the previous call's output.
- Rows in the shared benchmark format (`tests/proven_bench.h`) plus a proven/host ratio line per case.

Failure tip: any mismatch fails the run and is a correctness defect in `src/proven/float_*.c`; timing is recorded, not judged (the maintainers' benchmark records).

### `tests/test_bench_job` - job system idle cost and latency

Intent: record the job system's idle cost and wake latency (B-038) in the shared benchmark format.

- Idle CPU over 250 ms for 1, 2, 8 and 32 parked workers.
- Median and p99 submit-to-start latency: every worker parked, a 64-job burst, and saturated by four producers; saturated four-producer throughput.
- Ten thousand no-op jobs are counted; a lost job fails the run.

Failure tip: timings are compared with the budget in the maintainers' benchmark records, not asserted; a lost job is a defect in `src/proven/job.c`.

### `tests/test_bench_float_parse` - float parse benchmark

Intent: time the decimal parser against the host strtod on a mixed corpus and record the result.

Failure tip: inspect src/proven/float_parse.c and src/proven/float_decimal.c if a timing run regresses or a checksum drifts.

## Cross-build matrix

The matrix compiles `tests/test_portability_cross_compile_smoke.c` (or `tests/test_portability_freestanding.c` for freestanding targets) and links it with `tests/test_portability_cross_link_smoke.c`, which exists to prove the objects actually link - a header-only compile check would miss a missing symbol. Freestanding targets are also linked with no C runtime at all (`tests/test_portability_freestanding_nocrt_link.c`, stage `nocrt-link`).

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
- `freestanding-wasm32` through `clang --target=wasm32` (needs `wasm-ld`)

Hosted targets compile all hosted source files and `tests/test_portability_cross_compile_smoke.c`. Freestanding targets compile only freestanding-safe source files and `tests/test_portability_freestanding.c` as a smoke translation unit.

`freestanding-wasm32` differs in two ways, both because bare Clang for wasm32 ships no C library headers and no support library. It does not compile `tests/test_portability_freestanding.c`, which reports through `<stdio.h>`; its consumer-side check is the no-CRT link program, which includes `proven.h` and needs no hosted header. And that link supplies `__multi3` (the 128-bit multiply from compiler-rt's builtins) beside the four memory functions, where the other freestanding targets link `-lgcc`. `wasm-ld` refuses any other undefined symbol, so the link is the same evidence. The module is not executed by `./nob cross`. Because this target has no hosted headers at all, it is also the one that fails when a freestanding source includes one - three did until v0.9.0.

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
- Confirms a significand longer than 19 digits whose 19-digit bounds round alike takes the fast path, and one whose bounds straddle a tie (`9007199254740993.00000000000000000001`) reaches the exact path and rounds up.
- Compares, bit for bit with `strtod`, every digit shape the one-pass reader handles: the 20-digit `u64` edge (`18446744073709551615` and `...616`), zeros before, inside and after the digits, a point at either end, long fractions and extreme exponents.

Failure tip: inspect `include/proven/float_parse.h`, `src/proven/float_parse.c`, and `src/proven/float_decimal.c` if the public parser seam or wrapper contract drifts.

### 38b. `tests/test_unit_float_rfc_0001_cases` - RFC-0001 parse audit

Intent: verify the decimal-to-binary64 rewrite still satisfies the explicit named cases from RFC-0001 (the RFC itself is a maintainers' record, not in the published repository; the cases it names are reproduced in the test).

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

Intent: verify the hosted job queue tolerates concurrent producers and a close
racing active submitters, drains every accepted job exactly once, and releases
every parked worker.

Sub-checks:

- Launches four producer threads against a small bounded queue.
- Submits 1024 jobs per producer while yielding between retries.
- Counts total executions with an atomic counter.
- Uses per-slot atomics to detect duplicate or missing execution.
- Runs a second phase that closes while four producers are still submitting.
- Joins those producers before destroy, then proves every accepted job ran once
  and no job ran twice.

Failure tip: run `./nob tsan` if available and inspect `src/proven/job.c` plus
`platform/proven_sys_thread.c` if counts drift, a wake is lost, or a producer
stalls.

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
./nob clean -build-root build-out/proven_c_lib
./nob strict-error -build-root build-out/proven_c_lib
./nob regression-asan -build-root build-out/proven_c_lib
./nob regression-ubsan -build-root build-out/proven_c_lib
./nob freestanding -build-root build-out/proven_c_lib
./nob cross -build-root build-out/proven_c_lib
```

Optional compiler-specific gate:

```sh
./nob strict-error -cc clang -build-root build-out/proven_c_lib
```
