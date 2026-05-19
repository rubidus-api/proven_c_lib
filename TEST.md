     1|# proven Test Matrix (v26.05.19f)
     2|
     3|This document describes how the `proven` test suite is organized, what each test is intended to validate, what each test checks internally, and where to start when a failure occurs. Tests are plain C executables built and run by `nob.c`. No external test framework is required.
     4|
     5|## Table of contents
     6|
     7|- [Running tests](#running-tests)
     8|- [Log format](#log-format)
     9|- [Test modes](#test-modes)
    10|- [Hosted test executables](#hosted-test-executables)
    11|- [Regression subset](#regression-subset)
    12|- [Freestanding tests](#freestanding-tests)
    13|- [Cross compile-only matrix](#cross-compile-only-matrix)
    14|- [Failure triage workflow](#failure-triage-workflow)
    15|- [Change policy](#change-policy)
    16|- [Release validation](#release-validation)
    17|
    18|## Running tests
    19|
    20|Compile the build driver first:
    21|
    22|```sh
    23|cc nob.c -o nob
    24|```
    25|
    26|Run the full hosted debug suite:
    27|
    28|```sh
    29|./nob build -build-root /home/user/work/build/proven_c_lib
    30|```
    31|
    32|Run the warnings-as-errors gate:
    33|
    34|```sh
    35|./nob strict-error -build-root /home/user/work/build/proven_c_lib
    36|```
    37|
    38|Run sanitizer modes:
    39|
    40|```sh
    41|./nob asan -build-root /home/user/work/build/proven_c_lib
    42|./nob ubsan -build-root /home/user/work/build/proven_c_lib
    43|./nob tsan -build-root /home/user/work/build/proven_c_lib
    44|```
    45|
    46|Run focused regression modes:
    47|
    48|```sh
    49|./nob regression -build-root /home/user/work/build/proven_c_lib
    50|./nob regression-asan -build-root /home/user/work/build/proven_c_lib
    51|./nob regression-ubsan -build-root /home/user/work/build/proven_c_lib
    52|```
    53|
    54|Run freestanding checks:
    55|
    56|```sh
    57|./nob freestanding -build-root /home/user/work/build/proven_c_lib
    58|```
    59|
    60|Run cross compile-only coverage:
    61|
    62|```sh
    63|./nob cross -build-root /home/user/work/build/proven_c_lib
    64|```
    65|
    66|Missing optional cross compilers are skipped. A compiler that exists but cannot build the target probe is skipped with a warning. A real compile error in an available target fails the command.
    67|
    68|Clean generated output:
    69|
    70|```sh
    71|./nob clean
    72|```
    73|
    74|## Log format
    75|
    76|`nob.c` prints structured metadata before every hosted or freestanding test executable is linked and run. `./nob cross` uses the same shape for each available cross target, with `path=cross/<target-name>`. The build driver also prints build-level begin, environment, phase, source, test, summary, fail, and pass lines so platform setup problems on Windows/MSYS2 are visible instead of looking like a silent no-op.
    77|
    78|Standard build-driver lines:
    79|
    80|```text
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

    85|
    86|Standard build-run lines:
    87|
    88|```text
    89|[PROVEN][TEST][BEGIN] path=<test executable path> title=<short title>
    90|[PROVEN][TEST][INTENT] <what this executable validates>
    91|[PROVEN][TEST][FAIL_HINT] <where to start if this executable fails>
    92|[PROVEN][TEST][PASS] path=<test executable path>
    93|```
    94|
    95|Standard failure lines:
    96|
    97|```text
    98|[PROVEN][TEST][FAIL] path=<test executable path> stage=<link|install|run>
    99|[PROVEN][TEST][FAIL_HINT] <stage-specific or test-specific debugging hint>
   100|```
   101|
   102|Standard assertion lines printed by `tests/proven_test.h`:
   103|
   104|```text
   105|[PROVEN][CHECK][FAIL] file=<source file> line=<line>
   106|[PROVEN][CHECK][COND] <failed C condition>
   107|[PROVEN][CHECK][INTENT] <why this check exists>
   108|[PROVEN][CHECK][FAIL_HINT] <what to inspect first>
   109|```
   110|
   111|Standard informational and pass lines printed by test executables:
   112|
   113|```text
   114|[PROVEN][TEST][INFO] <message>
   115|[PROVEN][TEST][PASS] <message>
   116|[PROVEN][SECTION][BEGIN] name=<sub-test name>
   117|[PROVEN][SECTION][INTENT] <sub-test intent>
   118|[PROVEN][SECTION][FAIL_HINT] <sub-test failure hint>
   119|```
   120|
   121|The older test files still use many direct `PROVEN_TEST_INFO` calls. Those messages now share the `[PROVEN][TEST][INFO]` prefix. New or substantially edited tests should prefer `PROVEN_TEST_SECTION(name, intent, hint)` for each logically separate sub-check group.
   122|
   123|## Test modes
   124|
   125|### `build`
   126|
   127|Intent: compile the hosted library in debug mode and run the complete hosted runtime suite.
   128|
   129|What it checks:
   130|
   131|- All hosted source files compile together with the public headers.
   132|- All hosted test executables link against the same object set.
   133|- The full set of runtime behavior checks succeeds without sanitizer instrumentation.
   134|
   135|Failure tip: start from the first `[PROVEN][TEST][FAIL]` line. If the stage is `link`, inspect the immediately preceding compiler or linker diagnostic. If the stage is `run`, inspect the test-specific failure hint and the failing assertion.
   136|
   137|### `release`
   138|
   139|Intent: compile the hosted library with optimization and run the complete hosted runtime suite.
   140|
   141|What it checks:
   142|
   143|- Optimized builds do not rely on debug-only initialization or timing.
   144|- Undefined behavior that is hidden in debug mode is less likely to survive optimization.
   145|- Public headers and implementation still agree under `-O3`.
   146|
   147|Failure tip: compare with `./nob build`. A failure only in release mode often means undefined behavior, invalid aliasing, stale borrowed views after reallocation, or missing initialization.
   148|
   149|### `strict`
   150|
   151|Intent: run the hosted suite with extra compiler warnings enabled.
   152|
   153|What it checks:
   154|
   155|- The code remains warning-clean under the compiler's normal warning policy.
   156|- Suspicious conversions, unused variables, and portability risks are visible before release.
   157|
   158|Failure tip: warnings are not fatal in this mode, but they should still be treated as defects. Use the warning location and then rerun `strict-error` after fixing it.
   159|
   160|### `strict-error`
   161|
   162|Intent: make warnings fatal and run the hosted suite.
   163|
   164|What it checks:
   165|
   166|- The codebase is warning-clean enough to be used as a dependency in strict C projects.
   167|- Header changes do not introduce warnings into tests that include the public API.
   168|
   169|Failure tip: fix the first compiler warning as a source issue, not by suppressing it globally. If a warning is target-specific, isolate it behind the relevant PAL or feature guard.
   170|
   171|### `asan`
   172|
   173|Intent: run the hosted suite under AddressSanitizer.
   174|
   175|What it checks:
   176|
   177|- Heap use-after-free, double free, buffer overflow, stack overflow, and some leak paths.
   178|- Allocator trait routing for heap, arena, pool, arrays, maps, and growable strings.
   179|
   180|Failure tip: read the ASan stack trace first. For growable containers, suspect stale views or stale element pointers after reallocation. For arenas, suspect capacity arithmetic or alignment rounding.
   181|
   182|### `ubsan`
   183|
   184|Intent: run the hosted suite under UndefinedBehaviorSanitizer.
   185|
   186|What it checks:
   187|
   188|- Signed integer overflow, invalid shifts, misaligned access, invalid casts in instrumented code, and other UB classes supported by the compiler.
   189|- C23 checked arithmetic wrappers are not bypassed in overflow-prone paths.
   190|
   191|Failure tip: the failing expression is usually the bug. Do not paper over it with casts unless the conversion is explicitly proven and guarded.
   192|
   193|### `tsan`
   194|
   195|Intent: run the hosted suite under ThreadSanitizer.
   196|
   197|What it checks:
   198|
   199|- Data races in the job system and any hosted code that uses atomics or worker threads.
   200|- Worker startup, queue submission, shutdown, and exactly-once job execution under instrumentation.
   201|
   202|Failure tip: if TSAN reports a race, inspect `src/proven/job.c` and `platform/proven_sys_thread.c` first. Make the memory ordering and ownership contract explicit before changing code.
   203|
   204|### `regression`, `regression-asan`, `regression-ubsan`
   205|
   206|Intent: run only focused historical regressions, optionally with sanitizers.
   207|
   208|What it checks:
   209|
   210|- Previously fixed bugs stay fixed.
   211|- Source-contract checks remain synchronized with portability expectations.
   212|- Bug-fix verification is faster than a complete suite run.
   213|
   214|Failure tip: do not delete a regression because it feels narrow. It exists because the same class of bug already happened. Read the corresponding section below and preserve the contract.
   215|
   216|### `freestanding`
   217|
   218|Intent: compile and run the reduced `PROVEN_FREESTANDING` configuration.
   219|
   220|What it checks:
   221|
   222|- Hosted-only modules are excluded.
   223|- `PROVEN_FMT_NO_FLOAT` and `PROVEN_NO_U16STR` builds still work.
   224|- Core allocator-backed containers, formatting without floats, scanning, and algorithms work without OS-backed services.
   225|
   226|Failure tip: any dependency on filesystem, mmap, sysio, environment, time, threads, or hosted heap is suspicious in this mode. Keep the freestanding subset small and explicit.
   227|
   228|### `cross`
   229|
   230|Intent: compile the library and smoke tests for every available target compiler.
   231|
   232|What it checks:
   233|
   234|- Public headers are portable across hosted Linux, Windows MinGW, and freestanding embedded targets that exist on the build server.
   235|- Missing optional compilers are skipped; real compile failures fail the run.
   236|
   237|Failure tip: identify the target name in the log, then check whether the failure is from compiler availability, sysroot usability, or actual source incompatibility. Cross compilation does not replace runtime testing on the target.
   238|
   239|## Hosted test executables
   240|
   241|The hosted full run currently builds and executes 32 tests.
   242|
   243|### 1. `tests/test_phase1` - memory byte views
   244|
   245|Intent: verify the fixed-width integer aliases, semantic pointer/offset types, alignment helpers, and the first memory slice/view contracts.
   246|
   247|Sub-checks:
   248|
   249|- Confirms `proven_u8`, `proven_i8`, `proven_u16`, `proven_i16`, `proven_u32`, `proven_i32`, `proven_u64`, and `proven_i64` have the expected byte widths.
   250|- Confirms semantic pointer/size/offset types are usable for memory calculations.
   251|- Exercises default alignment logic.
   252|- Builds a memory core structure and confirms pointer and size fields remain exact.
   253|
   254|Failure tip: start in `include/proven/types.h`, `include/proven/align.h`, and `include/proven/memory.h`. A width failure usually means a typedef or platform feature branch changed. An alignment failure usually means the helper no longer implements power-of-two alignment correctly.
   255|
   256|### 2. `tests/test_foundation` - foundation primitives
   257|
   258|Intent: verify the core error and checked-arithmetic assumptions used by all higher-level modules.
   259|
   260|Sub-checks:
   261|
   262|- Confirms `PROVEN_IS_OK` and `proven_is_ok` classify success and failure correctly.
   263|- Confirms checked add detects overflow and preserves the wrapped C result where the C23 checked-arithmetic API says it should.
   264|- Confirms checked subtract detects underflow.
   265|- Confirms checked multiply detects overflow and succeeds for safe products.
   266|- Confirms simple result structs can carry both an error and a value.
   267|
   268|Failure tip: inspect `include/proven/error.h` and the `PROVEN_CKD_*` definitions in `include/proven/types.h`. If this test fails, avoid debugging later modules until the foundation behavior is fixed.
   269|
   270|### 3. `tests/test_phase2` - memory slicing
   271|
   272|Intent: verify owned memory can be exposed as immutable and mutable views and sliced without losing pointer or length identity.
   273|
   274|Sub-checks:
   275|
   276|- Creates raw byte storage and wraps it in the owned memory abstraction.
   277|- Converts owned memory to a read-only view and checks pointer, size, and byte contents.
   278|- Converts owned memory to a mutable view and checks that writes through the mutable view are visible through the original buffer and read-only view.
   279|- Slices read-only and mutable views and checks offset, length, and shared backing storage.
   280|
   281|Failure tip: inspect `src/proven/memory.c` and `include/proven/memory.h`. Most failures here are offset arithmetic mistakes, accidental copies instead of views, or unchecked slice preconditions used with the wrong ranges.
   282|
   283|### 4. `tests/test_phase3` - error and result primitives
   284|
   285|Intent: verify the explicit error/result style has stable semantics and no hidden control flow.
   286|
   287|Sub-checks:
   288|
   289|- Confirms `PROVEN_OK` is accepted as success.
   290|- Confirms representative failures such as `PROVEN_ERR_NOMEM` are rejected as success.
   291|- Builds a successful memory result and verifies both the error and value fields.
   292|- Builds a failed memory result and verifies the error and null value fields.
   293|
   294|Failure tip: inspect `include/proven/error.h` and generated/result typedefs. Do not change enum values or result layouts without updating every call site, alias, manual, and test that relies on them.
   295|
   296|### 5. `tests/test_phase4` - arena allocator
   297|
   298|Intent: verify bump-allocation behavior, alignment, exhaustion, reset, realloc, and zero-copy semantics for the arena allocator.
   299|
   300|Sub-checks:
   301|
   302|- Initializes an arena over a fixed backing buffer.
   303|- Allocates default-aligned and explicitly 32-byte-aligned blocks.
   304|- Confirms out-of-memory requests fail instead of overwriting the backing buffer.
   305|- Confirms reset releases the whole arena lifetime at once.
   306|- Exercises reallocation and verifies zero-copy or migration semantics according to arena constraints.
   307|
   308|Failure tip: inspect `src/proven/arena.c`, especially offset rounding, overflow checks, and reset behavior. Under ASan, any failure is likely a true bounds or lifetime bug.
   309|
   310|### 6. `tests/test_phase5` - buffer and U8 string basics
   311|
   312|Intent: verify fixed-capacity buffers, U8 string views, literal construction, append behavior, C-string conversion, and bounds defense.
   313|
   314|Sub-checks:
   315|
   316|- Creates a buffer through an allocator and appends data.
   317|- Verifies `PROVEN_LIT` computes literal sizes without including the NUL terminator.
   318|- Confirms buffer append rejects out-of-bounds writes.
   319|- Creates a U8 string and appends multiple fragments.
   320|- Converts a slice to a heap-owned C string.
   321|- Confirms C-string termination, equality helpers, and integer overflow guards.
   322|
   323|Failure tip: inspect `src/proven/buffer.c` and `src/proven/u8str.c`. Off-by-one capacity mistakes usually show up here first, especially around the extra NUL byte for C-string compatibility.
   324|
   325|### 7. `tests/test_phase6_pool` - pool allocator
   326|
   327|Intent: verify the fixed-size pool allocator enforces item-size constraints and recycles freed blocks through a bounded LIFO bin.
   328|
   329|Sub-checks:
   330|
   331|- Initializes a pool for `proven_u64` sized blocks.
   332|- Confirms an allocation request with the wrong size is rejected.
   333|- Allocates several blocks through the pool and checks fallback allocation when the bin is empty.
   334|- Frees blocks and checks `bin_len` growth up to capacity.
   335|- Confirms freeing beyond bin capacity falls back to the underlying allocator.
   336|- Reallocates and verifies LIFO pointer reuse.
   337|- Tears down the pool without leaking bin storage.
   338|
   339|Failure tip: inspect `src/proven/pool.c`. Wrong-size requests should not be silently accepted. Bin overflow must never lose ownership of the block being freed.
   340|
   341|### 8. `tests/test_dealloc` - allocator deallocation policies
   342|
   343|Intent: document and verify the different deallocation policies exposed through the allocator trait.
   344|
   345|Sub-checks:
   346|
   347|- Allocates from an arena through the generic allocator trait.
   348|- Calls the arena free function and verifies it is intentionally a no-op.
   349|- Resets the arena as the correct lifetime-ending operation.
   350|- Allocates from the heap allocator and frees through the heap trait.
   351|
   352|Failure tip: inspect `src/proven/arena.c`, `src/proven/heap.c`, and the allocator trait definition. Do not make arena `free` reclaim individual blocks; that would break the arena lifetime model.
   353|
   354|### 9. `tests/test_phase7_u8str_mut` - U8 string mutation
   355|
   356|Intent: verify U8 string search, slicing, replacement, insertion, removal, and the three append policies: atomic fixed-capacity, partial fixed-capacity, and growable.
   357|
   358|Sub-checks:
   359|
   360|- Finds substrings from different offsets and reports `PROVEN_INDEX_NOT_FOUND` for missing needles.
   361|- Checks starts-with, ends-with, and slice equality.
   362|- Performs same-length, shrinking, and growing `replace_at` operations.
   363|- Inserts and removes byte ranges.
   364|- Replaces the first matching substring.
   365|- Confirms fixed-capacity append fails atomically when full.
   366|- Confirms partial append writes as many bytes as possible and reports the written count.
   367|- Confirms growable append reallocates and completes the operation.
   368|
   369|Failure tip: inspect `src/proven/u8str.c`. For failures after a reallocation path, assume saved views or C-string pointers are stale unless proven otherwise. For fixed-capacity failures, check whether the operation is documented as atomic or partial.
   370|
   371|### 10. `tests/test_phase8_array` - growable array
   372|
   373|Intent: verify generic array allocation, validation, push/pop, growth, migration, element access, and arena-backed use.
   374|
   375|Sub-checks:
   376|
   377|- Initializes an array with a typed element size and initial capacity.
   378|- Checks validation catches corrupted length/capacity state.
   379|- Pushes typed values through macros.
   380|- Forces growth and verifies data migrated correctly.
   381|- Pops values and checks boundary rejection on empty pop.
   382|- Checks invalid get/set ranges.
   383|- Creates an arena-backed array to ensure allocator independence.
   384|
   385|Failure tip: inspect `src/proven/array.c`. Growth failures usually mean element-size multiplication, capacity doubling, or realloc failure-atomic behavior changed. Remember that pointers into array storage are invalid after growth.
   386|
   387|### 11. `tests/test_phase9_list` - intrusive list
   388|
   389|Intent: verify zero-allocation intrusive list behavior and container-of usage.
   390|
   391|Sub-checks:
   392|
   393|- Initializes an empty sentinel list.
   394|- Appends embedded nodes from caller-owned structs.
   395|- Iterates in reverse and sums payload values.
   396|- Removes nodes while iterating.
   397|- Reads first and last entries through container-of style access.
   398|
   399|Failure tip: inspect `include/proven/list.h`. Intrusive lists do not own node storage. A failure usually means `next`/`prev` linkage was corrupted or a detached node was reused incorrectly.
   400|
   401|### 12. `tests/test_phase10_ring` - bounded ring
   402|
   403|Intent: verify fixed-capacity FIFO semantics, wraparound, full/empty detection, and overflow guards.
   404|
   405|Sub-checks:
   406|
   407|- Creates a ring and verifies initial head, tail, length, and capacity.
   408|- Pushes and pops values in FIFO order.
   409|- Fills the ring and verifies extra push is rejected.
   410|- Pops across physical wraparound boundaries.
   411|- Verifies empty pop rejection.
   412|- Checks integer-overflow bounds for capacity calculations.
   413|
   414|Failure tip: inspect `src/proven/ring.c`. The first suspects are head/tail modulo math, `len` updates, and full-vs-empty boundary handling.
   415|
   416|### 13. `tests/test_phase11_map` - hash map
   417|
   418|Intent: verify open-addressing map behavior for integer and U8 string keys, including tombstones, growth, and scratch allocation.
   419|
   420|Sub-checks:
   421|
   422|- Creates an integer-key map and confirms capacity normalization.
   423|- Inserts, retrieves, updates, and deletes entries.
   424|- Confirms deletion reduces live length and leaves tombstones usable.
   425|- Creates a U8 string-key map and inserts enough entries to force growth.
   426|- Verifies all expected string keys remain reachable after rehash.
   427|- Tracks scratch allocation during safe rehash paths.
   428|
   429|Failure tip: inspect `src/proven/map.c`. Check hash/equality callbacks, tombstone reuse, threshold calculation, and whether borrowed keys or value pointers are being used after rehash.
   430|
   431|### 14. `tests/test_phase12_algorithm` - algorithms
   432|
   433|Intent: verify generic sort and binary search helpers using both scalar and struct comparators.
   434|
   435|Sub-checks:
   436|
   437|- Sorts an integer array and verifies ascending order.
   438|- Binary-searches for an existing and a missing integer.
   439|- Sorts structs using a comparator that sorts by score descending and ID ascending.
   440|- Verifies comparator tie-breaking order.
   441|
   442|Failure tip: inspect `src/proven/algorithm.c`. Comparator return convention must stay consistent: callers expect negative, zero, and positive values to drive ordering.
   443|
   444|### 15. `tests/test_phase13_fs` - basic filesystem
   445|
   446|Intent: verify hosted file open, write, read-all, size queries, and absolute-path classification.
   447|
   448|Sub-checks:
   449|
   450|- Opens a temporary file for create/write/truncate.
   451|- Writes known content and checks byte count.
   452|- Reads the whole file and checks size and byte equality.
   453|- Reopens the file and queries its size.
   454|- Verifies absolute path classification for POSIX, drive-letter Windows paths, UNC paths, and extended Windows paths.
   455|
   456|Failure tip: inspect `src/proven/fs.c` and `platform/proven_sys_fs.c`. If only Windows path cases fail, check path-prefix parsing rather than POSIX filesystem behavior.
   457|
   458|### 16. `tests/test_phase14_fs_advanced` - advanced filesystem
   459|
   460|Intent: verify directory lifecycle, nested file creation, rename/move, listing, sorting expectations, and cleanup.
   461|
   462|Sub-checks:
   463|
   464|- Removes stale test directories from earlier failed runs.
   465|- Creates a directory.
   466|- Creates multiple files inside it.
   467|- Renames/moves one file.
   468|- Lists directory entries into a library array.
   469|- Confirms expected files are present.
   470|- Releases listed strings and removes test files/directories.
   471|
   472|Failure tip: inspect `platform/proven_sys_fs.c` for directory iteration and path handling. On failure, check whether cleanup from a previous run left permissions or stale entries behind.
   473|
   474|### 17. `tests/test_phase15_fs_security` - filesystem metadata and permissions
   475|
   476|Intent: verify hosted permission and locking-related filesystem behavior stays explicit and isolated behind the PAL.
   477|
   478|Sub-checks:
   479|
   480|- Creates a temporary file.
   481|- Changes permissions to read-only and back when the platform supports it.
   482|- Opens files for write where needed.
   483|- Acquires and releases advisory locks where supported.
   484|
   485|Failure tip: inspect `platform/proven_sys_fs.c`. Permission and locking semantics are OS-dependent; keep differences in PAL code and avoid assuming POSIX behavior on every target.
   486|
   487|### 18. `tests/test_phase16_time_fmt` - time and formatting integration
   488|
   489|Intent: verify time measurement, sleep duration, modern format syntax, datetime formatting, and escaped braces.
   490|
   491|Sub-checks:
   492|
   493|- Reads monotonic or high-resolution time before and after a short sleep.
   494|- Confirms elapsed nanoseconds are at least approximately the requested sleep.
   495|- Formats positional and automatic arguments.
   496|- Converts the Unix epoch to a datetime and checks year/month.
   497|- Formats a datetime value through `PROVEN_ARG`.
   498|- Verifies `{{` and `}}` produce literal braces.
   499|
   500|Failure tip: inspect `platform/proven_sys_time.c` for clock conversion and `src/proven/fmt.c` for datetime formatting. Timing failures can be caused by a broken clock source or by assuming exact scheduling latency.
   501|
   502|### 19. `tests/test_phase17_mmap` - memory mapped files
   503|
   504|Intent: verify hosted memory mapping rejects invalid flags and exposes file bytes through mapped memory.
   505|
   506|Sub-checks:
   507|
   508|- Creates a test file with known content.
   509|- Rejects invalid mmap flag combinations: zero flags, private plus shared, zero protection, unknown protection bits, and misaligned offsets.
   510|- Maps a file range.
   511|- Verifies mapped bytes match expected content.
   512|- Modifies mapped memory and syncs/unmaps it.
   513|- Reads the file back to verify the modification reached disk when mapping mode requires it.
   514|
   515|Failure tip: inspect `src/proven/mmap.c` and `platform/proven_sys_fs.c`. Pay special attention to offset alignment, map length, file handle lifetime, and unmap ownership.
   516|
   517|### 20. `tests/test_phase17_u16str` - U16 strings
   518|
   519|Intent: verify optional UTF-16/code-unit string support and its append policies.
   520|
   521|Sub-checks:
   522|
   523|- Creates and destroys a U16 string.
   524|- Appends code units into fixed capacity.
   525|- Confirms atomic append failure leaves content and length unchanged.
   526|- Confirms partial append writes the count that fits and reports out-of-bounds.
   527|- Confirms growable append reallocates and completes the write.
   528|
   529|Failure tip: inspect `src/proven/u16str.c` and `include/proven/u16str.h`. Treat U16 values as UTF-16 code units, not Unicode scalar values. Check `PROVEN_NO_U16STR` guards if the failure is compile-time.
   530|
   531|### 21. `tests/test_phase18_sysio` - sysio and environment
   532|
   533|Intent: verify standard stream access, formatter-backed console output, environment lookup, missing-variable errors, and long environment-key handling.
   534|
   535|Sub-checks:
   536|
   537|- Uses proven sysio/formatter APIs without including `<stdio.h>` directly in the test.
   538|- Prints structured output to prove standard stream wrappers are usable.
   539|- Reads a likely existing environment variable such as `PATH`.
   540|- Confirms a fake environment variable reports failure.
   541|- Creates and reads an environment variable whose key is larger than the old fixed stack limit.
   542|
   543|Failure tip: inspect `src/proven/sysio.c` and `platform/proven_sys_env.c`. Long-key failures usually mean a fixed-size C-string conversion path returned. Windows failures may involve UTF-8 to UTF-16 conversion and allocator ownership.
   544|
   545|### 22. `tests/test_phase19_coro` - stackless coroutine
   546|
   547|Intent: verify coroutine macros preserve caller-owned state across yields and complete after multiple resume calls.
   548|
   549|Sub-checks:
   550|
   551|- Defines a simulated network fetcher that yields across multiple phases.
   552|- Repeatedly resumes the coroutine from a main loop.
   553|- Confirms final payload value is set after completion.
   554|- Confirms the main loop observed multiple ticks rather than one blocking call.
   555|
   556|Failure tip: inspect `include/proven/coro.h`. Coroutine state must live in caller-owned storage and must not be reset between resumes.
   557|
   558|### 23. `tests/test_phase20_job` - job system
   559|
   560|Intent: verify the hosted worker-thread job system executes submitted jobs exactly once and shuts down cleanly.
   561|
   562|Sub-checks:
   563|
   564|- Initializes a job system with four workers and a 1024-entry queue.
   565|- Dispatches 1000 jobs.
   566|- Uses atomics to count total executed jobs.
   567|- Uses indexed atomics to detect duplicate or missing job execution.
   568|- Shuts down workers and flushes synchronization barriers.
   569|
   570|Failure tip: inspect `src/proven/job.c` and `platform/proven_sys_thread.c`. For races, run `./nob tsan`. Check admission state, sequence counters, queue claim/commit ordering, and shutdown wakeups.
   571|
   572|### 24. `tests/test_phase21_scan` - scanner
   573|
   574|Intent: verify scanner parsing for integers, floats, tokens, skip-until operations, format scanning, and fixed-width integer destinations.
   575|
   576|Sub-checks:
   577|
   578|- Scans unsigned and signed integers.
   579|- Scans positive, negative, and exponent-style floating-point values.
   580|- Scans tokens and string views.
   581|- Skips until substrings and numbers.
   582|- Confirms not-found behavior.
   583|- Scans using `{}` and spec-style format patterns.
   584|- Scans native and fixed-width integer aliases.
   585|
   586|Failure tip: inspect `src/proven/scan.c`. The most common bugs are cursor advancement on failure, overflow detection, and accepting invalid trailing characters.
   587|
   588|### 25. `tests/test_phase22_fmt_best_effort` - formatter failure policy
   589|
   590|Intent: verify formatting append policies are explicit: fixed-capacity atomic, fixed-capacity truncating, and allocator-backed growable.
   591|
   592|Sub-checks:
   593|
   594|- Appends formatted output with growable allocation.
   595|- Populates a small fixed string.
   596|- Confirms fixed-capacity formatting reports out-of-bounds and leaves the string unchanged.
   597|- Confirms truncating formatting writes the partial count and reports required size.
   598|- Confirms content after truncation matches the expected prefix.
   599|- Checks extremely large padding specs for safe overflow handling.
   600|
   601|Failure tip: inspect `src/proven/fmt.c`. Track `written`, `required`, and destination length separately. Atomic failure must not modify the destination.
   602|
   603|### 26. `tests/test_scan_overflow_f64` - float scanner overflow
   604|
   605|Intent: verify a very large floating-point token reports `PROVEN_ERR_OVERFLOW` instead of silently accepting infinity.
   606|
   607|Sub-checks:
   608|
   609|- Builds an input with roughly 1000 decimal digits.
   610|- Scans it as `f64`.
   611|- Confirms the error is `PROVEN_ERR_OVERFLOW`.
   612|
   613|Failure tip: inspect `proven_scan_f64` in `src/proven/scan.c` and math helper behavior in the PAL. Do not accept `inf` as a successful parsed finite value.
   614|
   615|### 27. `tests/test_scan_f64_accuracy` - float scanner accuracy

Intent: verify float scanning preserves exact small values, signed zero, a round-trip style decimal token, exponent extremes, and cursor restoration on malformed input.

Sub-checks:

- Confirms exact bit patterns for `0.0`, `-0.0`, `1.0`, `-1.0`, `0.5`, `0.1`, and `123456789.0`.
- Confirms the parsed bits for `0.30000000000000004` match the source literal.
- Confirms `1.7976931348623157e308`, `2.2250738585072014e-308`, and `4.9e-324` remain finite and stable.
- Confirms `1e309` reports a deterministic out-of-range error.
- Confirms malformed input restores the scanner cursor to its original position.

Failure tip: inspect `src/proven/scan.c`, especially the decimal mantissa accumulation, exponent scaling, and final finite-value check. If a malformed token leaves the cursor advanced, inspect the failure-atomic rollback path first.

### 28. `tests/test_sysio_scanner` - sysio-backed scanner

Intent: verify scanner behavior over file-backed sysio data instead of only in-memory string views.

Sub-checks:

- Creates a temporary file and writes integer/token content.
- Opens the file for reading.
- Initializes a sysio scanner with an allocator-backed buffer.
- Scans two integers and a word from the file stream.
- Verifies `tests/test_sysio_scanner_boundary` rejects a token that reaches the end of the current buffer and leaves the file handle reusable.
- Cleans up scanner and file resources.

Failure tip: inspect `src/proven/sysio.c`, `src/proven/scan.c`, and file read wrappers. If in-memory scan tests pass but this fails, suspect buffer refill or file-position behavior, especially at the current-buffer boundary.

### 29. `tests/test_regression_v26_05` - v26.05 regressions
   630|
   631|Intent: protect historically fixed issues in map rehashing, formatting, scanning, aliasing, and environment handling.
   632|
   633|Sub-checks:
   634|
   635|- Map self-payload rehash: inserting a value pointer that points inside the map must not corrupt the new value during rehash.
   636|- Map existing-key update before rehash: updating an existing key must not incorrectly grow or lose the value.
   637|- Map large-value rehash allocation tracking: rehash must allocate/free scratch exactly as expected for large values.
   638|- Formatter self `STR_VIEW` grow: formatting from a view into the destination must handle aliasing safely.
   639|- Formatter self `CSTR` grow invalid-arg: unsafe self C-string aliasing should be rejected.
   640|- Formatter huge argument index: very large explicit indexes must fail safely.
   641|- Formatter many args without alias: large argument arrays must not overflow stack or internal accounting.
   642|- Formatter many args with alias scratch: alias-safe scratch paths must allocate and release scratch correctly.
   643|- Scanner invalid cursor: invalid scan state must not be treated as success.
   644|- Buffer append overlap: `test_phase5` checks that `proven_buf_append()` preserves overlapping source views with move semantics instead of corrupting the appended bytes.
   645|- Array/string self-alias grow: grow operations must not corrupt when source and destination overlap in documented ways.
   646|- `PROVEN_ARG_CSTR_N` safety bounds: C-string-with-length arguments must respect the caller-supplied bound.
   646|- Environment large value: environment values larger than a small stack buffer must be read through dynamic allocation.
   647|
   648|Failure tip: this file is intentionally a set of historical tripwires. Do not collapse it into broad smoke coverage. Read the failing sub-check name printed in the log and inspect the corresponding source module.
   649|
   650|### 30. `tests/test_regression_fs_copy_to_self` - filesystem self-copy regression
   651|
   652|Intent: verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the file.
   653|
   654|Sub-checks:
   655|
   656|- Creates a source file with known content.
   657|- Attempts to copy the file to the same path and expects `PROVEN_ERR_INVALID_ARG`.
   658|- Reads the file back and verifies size and contents are unchanged.
   659|- Creates a hard link to the same file when supported.
   660|- Attempts to copy across the hard-linked paths and expects failure without corruption.
   661|
   662|Failure tip: inspect same-file detection and open/truncate ordering in filesystem copy code. The destination must not be opened with truncation before proving it is not the same file as the source.
   663|
   664|### 31. `tests/test_regression_source_contracts` - source portability contracts
   665|
   666|Intent: guard platform branches and documentation/test-output contracts that may not be executable on the current host.
   667|
   668|Sub-checks:
   669|
   670|- Checks Windows directory open allocation guards before `FindFirstFileW` use.
   671|- Checks Windows `FILETIME` to Unix time conversion uses the correct epoch delta and underflow guard.
   672|- Checks POSIX mmap stores offsets in `off_t` and rejects truncation.
   673|- Checks Windows append mode uses `FILE_APPEND_DATA` rather than a one-time seek-to-end emulation.
   674|- Checks Windows environment key conversion sizes the wide buffer dynamically rather than using a fixed 255-byte stack buffer.
   675|- Checks public environment lookup no longer rejects large keys through a fixed C key buffer.
   676|- Checks 32-bit Linux `_llseek` uses a 64-bit result buffer.
   677|- Checks the job system keeps a single admission state and explicit begin/end submit helpers.
   678|- Checks `nob.c` keeps structured test metadata and emits standard begin, intent, failure-hint, failure, and pass log lines.
   679|- Checks this `TEST.md` documents failure tips, sub-checks, and the log format.
   680|
   681|Failure tip: source-contract tests should stay narrow. If a source pattern changes legitimately, update the contract to the new safe pattern in the same commit as the source change and explain it in docs.
   682|
   683|### 31. `tests/test_arena_panic` - arena panic path
   684|
   685|Intent: verify panic-on-allocation-failure behavior is deterministic and does not fire on successful arena allocation.
   686|
   687|Sub-checks:
   688|
   689|- Overrides the weak panic handler with a test hook.
   690|- Allocates successfully with `alloc_or_panic` and confirms no panic occurred.
   691|- Requests more memory than the arena can provide.
   692|- Confirms the panic hook was invoked exactly for the out-of-memory path.
   693|
   694|Failure tip: inspect `src/proven/arena.c` and `src/proven/panic.c`. Restore the panic hook carefully in tests so later tests are not affected.
   695|
   696|### 32. `tests/test_alias_smoke` - alias layer smoke
   697|
   698|Intent: verify the public XCV alias layer compiles and maps representative aliases to canonical proven APIs.
   699|
   700|Sub-checks:
   701|
   702|- Uses native integer scan aliases.
   703|- Uses scan function aliases.
   704|- Uses formatting argument aliases.
   705|- Uses macro aliases that are expected to stay available for the alias layer.
   706|
   707|Failure tip: inspect `include/proven/alias_xcv.h` and `tests/test_alias_smoke.c`. When public symbols are added, renamed, or removed, update the alias header and this smoke test together.
   708|
   709|### 33. `tests/test_fmt_f64_accuracy` - float formatter accuracy

Intent: verify fixed-point rounding, scientific carry, and special-value text for floating-point formatting.

Sub-checks:

- Checks normal-path rounding to six fractional digits.
- Checks carry from the fractional tail into the integer part.
- Checks scientific notation carry around the mantissa boundary.
- Checks NaN and infinity text stay stable.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_fmt_f64_accuracy.c`.

### 34. `tests/test_fmt_fastpath` - formatter truncation comparison

Intent: compare truncating fixed-capacity formatting against the growable reference path for exact-fit, truncation, malformed format, and excess-argument cases.

Sub-checks:

- Checks exact-fit truncation output matches the reference path.
- Checks over-capacity truncation keeps the same prefix bytes and counts.
- Checks excess-argument validation.
- Checks malformed-format validation.

Failure tip: inspect `src/proven/fmt.c` and `tests/test_fmt_fastpath.c`.

### 35. `tests/test_sysio_scan_nonseekable` - non-seekable sysio rejection

Intent: verify one-chunk file scanning rejects pipe/stdin-like inputs before consuming data.

Sub-checks:

- Checks the helper returns `PROVEN_ERR_UNSUPPORTED` for a non-seekable handle.
- Checks the scan destination is left unchanged on the early rejection path.
- Checks the original pipe payload is still readable after the rejected scan attempt.

Failure tip: inspect `src/proven/sysio.c` and make sure the one-chunk scan path probes seekability before reading.

### 36. `tests/test_sysio_scan_truncation` - chunked sysio scan truncation

Intent: verify one-chunk file scanning rejects inputs that exceed the fixed buffer and leaves the stream reusable after a failed attempt.

Sub-checks:

- Checks a chunk-full string token reports the bounds error used by the one-chunk scan path.
- Checks the file cursor is still usable after the failure.
- Checks the trailing integer is not consumed by the failed scan.

Failure tip: inspect `src/proven/sysio.c` and `tests/test_sysio_scan_truncation.c`.

## Regression subset
   710|
   711|`./nob regression`, `./nob regression-asan`, and `./nob regression-ubsan` currently run:
   712|
- `tests/test_regression_v26_05`
- `tests/test_regression_public_contracts`
- `tests/test_regression_fs_copy_to_self`
- `tests/test_regression_source_contracts`
   716|
   717|Intent: provide a short feedback loop for bug-fix work without running every hosted example and container test.
   718|
   719|What it checks:
   720|
   721|- Historical behavioral bugs remain fixed.
   722|- Filesystem destructive edge cases remain protected.
   723|- Source-level portability contracts and test-log/documentation contracts remain present.
   724|
Failure tip: a regression failure is usually more specific than a full-suite failure. Use the sub-check name and preserve the regression unless the underlying public contract is intentionally changed and documented.

### 34. `tests/test_regression_public_contracts` - public array/map/filesystem contracts

Intent: verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.

Sub-checks:

- Calls array reserve and push on an intentionally corrupted public struct and expects `PROVEN_ERR_INVALID_ARG`.
- Calls array destroy on a corrupted struct and checks that the visible fields are cleared after best-effort cleanup.
- Calls map reserve and set on an intentionally corrupted public struct and expects `PROVEN_ERR_INVALID_ARG`.
- Calls map destroy on a corrupted struct and checks that the visible fields are cleared after best-effort cleanup.
- Opens a file with append-plus-create, writes through the returned handle, and confirms POSIX append mode keeps write intent explicit.
- Confirms append plus truncation is rejected as `PROVEN_ERR_INVALID_ARG`.

Failure tip: inspect `src/proven/array.c`, `src/proven/map.c`, `src/proven/fs.c`, and `platform/proven_sys_fs.c`. If a corrupted struct reaches an allocator callback or append behaves like read-only open, the public contract guard is missing.

## Freestanding tests
   728|
   729|`./nob freestanding` currently builds the library with `PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR`, and `-ffreestanding`, then runs five reduced tests.
   730|
   731|### `tests/test_freestanding_heap_stub`
   732|
   733|Intent: verify the hosted heap allocator is not accidentally available in freestanding mode.
   734|
   735|Sub-checks:
   736|
   737|- Calls `proven_heap_allocator()` under `PROVEN_FREESTANDING`.
   738|- Confirms the returned allocator is invalid because no default OS heap exists.
   739|- Skips with an info line when compiled outside freestanding mode.
   740|
   741|Failure tip: inspect `src/proven/heap.c` and platform heap guards. Freestanding code must not silently pull a hosted allocator.
   742|
   743|### `tests/test_compile_freestanding`
   744|
   745|Intent: verify the reduced freestanding core can compile and link.
   746|
   747|Sub-checks:
   748|
   749|- Includes the public headers under freestanding flags.
   750|- Links against the reduced object set selected by `nob.c`.
   751|
   752|Failure tip: inspect `nob.c` source exclusion lists and public header feature guards. A hosted-only declaration may have leaked into freestanding builds.
   753|
   754|### `tests/test_compile_nofloat`
   755|
   756|Intent: verify `PROVEN_FMT_NO_FLOAT` removes floating-point formatting dependencies without breaking the rest of formatting.
   757|
   758|Sub-checks:
   759|
   760|- Builds with no-float formatting enabled.
   761|- Links the formatter without float-specific argument handling.
   762|
   763|Failure tip: inspect `include/proven/fmt.h` and `src/proven/fmt.c` for unguarded `float`, `double`, or math-helper references.
   764|
   765|### `tests/test_compile_nou16str`
   766|
   767|Intent: verify `PROVEN_NO_U16STR` removes optional U16 string support without breaking core headers and linking.
   768|
   769|Sub-checks:
   770|
   771|- Builds the umbrella header and reduced object set with U16 support disabled.
   772|- Links successfully without `src/proven/u16str.c`.
   773|
   774|Failure tip: inspect `include/proven.h`, `include/proven/u16str.h`, aliases, and the `nob.c` freestanding source list.
   775|
   776|### `tests/test_freestanding`
   777|
   778|Intent: verify the actual freestanding core runtime behavior, not just compilation.
   779|
   780|Sub-checks:
   781|
   782|- Initializes allocator-backed arrays and sorts them.
   783|- Binary-searches sorted array data.
   784|- Exercises intrusive lists.
   785|- Exercises bounded rings.
   786|- Exercises hash maps.
   787|- Exercises U8 strings and no-float formatting.
   788|- Exercises scanner and scan-format behavior.
   789|- Uses a deterministic panic hook for failure paths.
   790|
   791|Failure tip: inspect only freestanding-safe modules first: memory, arena, pool when included, buffer, U8 string, array, ring, map, algorithm, scan, fmt without float, panic, and non-hosted math helpers. Any filesystem, mmap, sysio, environment, time, thread, or hosted heap dependency is a portability regression.
   792|
   793|## Cross compile-only matrix
   794|
   795|`./nob cross` builds object files and smoke tests for available target compilers. The matrix currently includes:
   796|
   797|- `native-gcc-hosted` through `gcc`
   798|- `native-clang-hosted` through `clang`
   799|- `linux-aarch64-hosted` through `aarch64-linux-gnu-gcc`
   800|- `linux-armhf-hosted` through `arm-linux-gnueabihf-gcc`
   801|- `linux-i686-hosted` through `i686-linux-gnu-gcc`
   802|- `linux-i686-multilib-hosted` through `gcc -m32`
   803|- `windows-x86_64-winapi` through `x86_64-w64-mingw32-gcc`
   804|- `windows-i686-winapi` through `i686-w64-mingw32-gcc`
   805|- `freestanding-arm-cortex-m4` through `arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb`
   806|- `freestanding-riscv64-elf` through `riscv64-elf-gcc`
   807|- `freestanding-riscv64-unknown-elf` through `riscv64-unknown-elf-gcc`
   808|
   809|Hosted targets compile all hosted source files and `tests/test_cross_compile_smoke.c`. Freestanding targets compile only freestanding-safe source files and `tests/test_freestanding.c` as a smoke translation unit.
   810|
   811|Failure tip: if the log says the compiler is missing, fix the build-server toolchain rather than the library. If the target probe fails, check sysroot, multilib headers, or target flags. If a later library source file fails, treat it as a real portability bug.
   812|
   813|## Failure triage workflow
   814|
   815|1. Find the first `[PROVEN][TEST][FAIL]` line. Later failures can be cascading noise.
   816|2. Note the `stage` field.
   817|   - `link`: inspect the compiler/linker diagnostic immediately above the failure.
   818|   - `install`: inspect build-root permissions or stale locked output files.
   819|   - `run`: inspect the failing executable's assertion output.
   820|3. Read the `[PROVEN][TEST][INTENT]` and `[PROVEN][TEST][FAIL_HINT]` lines printed before the executable ran.
   821|4. If an assertion failed, read `[PROVEN][CHECK][COND]`, `[PROVEN][CHECK][INTENT]`, and `[PROVEN][CHECK][FAIL_HINT]`.
   822|5. Re-run the narrowest affected command first. For a regression, use `./nob regression`. For a freestanding failure, use `./nob freestanding`. For job/thread failures, use `./nob tsan` when available.
   823|6. After a fix, run at least `./nob build`, plus `strict-error`, sanitizer, freestanding, or cross modes appropriate to the touched area.
   824|
   825|## Change policy
   826|
   827|When behavior changes:
   828|
   829|1. Add or update the narrowest test first.
   830|2. Confirm the new test fails for the expected reason when practical.
   831|3. Implement the change.
   832|4. Make test output explain the intent and failure hint for the changed behavior.
   833|5. Run the narrow test through `nob.c`.
   834|6. Run `./nob build`.
   835|7. Run sanitizer, freestanding, or cross modes when the changed area requires them.
   836|8. Update this file if the test matrix, sub-checks, or failure guidance changes.
   837|
   838|## Release validation
   839|
   840|Recommended release gate:
   841|
   842|```sh
   843|./nob clean
   844|./nob strict-error -build-root /home/user/work/build/proven_c_lib
   845|./nob regression-asan -build-root /home/user/work/build/proven_c_lib
   846|./nob regression-ubsan -build-root /home/user/work/build/proven_c_lib
   847|./nob freestanding -build-root /home/user/work/build/proven_c_lib
   848|./nob cross -build-root /home/user/work/build/proven_c_lib
   849|```
   850|
   851|Optional compiler-specific gate:
   852|
   853|```sh
   854|./nob strict-error -cc clang -build-root /home/user/work/build/proven_c_lib
   855|```
   856|