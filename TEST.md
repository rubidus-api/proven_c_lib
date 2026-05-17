# proven Test Matrix (v26.05.16)

This document describes the current test coverage for the `proven` C library. Tests are plain C executables built and run by `nob.c`. No external test framework is required.

## Running tests

Compile the build driver:

```sh
cc nob.c -o nob
```

Full hosted debug run:

```sh
./nob build
```

Strict run:

```sh
./nob strict-error
```

Sanitizer runs:

```sh
./nob asan
./nob ubsan
./nob tsan
```

Focused regression runs:

```sh
./nob regression
./nob regression-asan
./nob regression-ubsan
```

Freestanding subset run:

```sh
./nob freestanding
```

Cross compile-only matrix:

```sh
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Missing optional compilers are skipped. Compile errors fail the command.

Clean generated output:

```sh
./nob clean
```

## Hosted test executables

The hosted full run currently builds and executes 32 tests:

1. `test_phase1`: memory slice views and mutable slices.
2. `test_foundation`: checked arithmetic and basic result handling.
3. `test_phase2`: alignment helpers.
4. `test_phase3`: error and result primitives.
5. `test_phase4`: arena creation, aligned allocation, and exhaustion behavior.
6. `test_phase5`: buffer and U8 string basics.
7. `test_phase6_pool`: fixed-size pool allocation and reuse.
8. `test_dealloc`: allocator free routing and arena no-op free behavior.
9. `test_phase7_u8str_mut`: U8 string mutation and operation policy checks.
10. `test_phase8_array`: generic growable array behavior.
11. `test_phase9_list`: intrusive list and container-of behavior.
12. `test_phase10_ring`: bounded ring wrapping and reuse.
13. `test_phase11_map`: open-addressing map insertion, deletion, tombstones, and growth.
14. `test_phase12_algorithm`: sorting and binary search helpers.
15. `test_phase13_fs`: file write/read operations and absolute path classification.
16. `test_phase14_fs_advanced`: directory lifecycle and listing behavior.
17. `test_phase15_fs_security`: filesystem metadata and permission behavior.
18. `test_phase16_time_fmt`: time and formatting integration.
19. `test_phase17_mmap`: memory-mapped file access.
20. `test_phase17_u16str`: U16 string behavior.
21. `test_phase18_sysio`: system I/O and environment access, including long environment keys.
22. `test_phase19_coro`: stackless coroutine state progression.
23. `test_phase20_job`: job-system scheduling and concurrent work execution.
24. `test_phase21_scan`: scanner token and number extraction.
25. `test_phase22_fmt_best_effort`: fixed, truncating, and growable formatting behavior.
26. `test_scan_overflow_f64`: floating-point scanner overflow handling.
27. `test_sysio_scanner`: scanner behavior over sysio-backed data.
28. `test_regression_v26_05`: versioned regression coverage for map, string, and formatting issues.
29. `test_regression_fs_copy_to_self`: copy-to-self and hardlink regression coverage.
30. `test_regression_source_contracts`: source-contract checks for platform portability regressions.
31. `test_arena_panic`: arena invariant and panic-path coverage.
32. `test_alias_smoke`: public alias-layer smoke coverage.

## Regression subset

`./nob regression`, `./nob regression-asan`, and `./nob regression-ubsan` currently run:

- `test_regression_v26_05`
- `test_regression_fs_copy_to_self`
- `test_regression_source_contracts`

Use this subset for fast confirmation after bug fixes. Run the full test set before release.

## Freestanding tests

`./nob freestanding` currently uses:

- `test_freestanding_heap_stub`
- `test_compile_freestanding`
- `test_compile_nofloat`
- `test_compile_nou16str`
- `test_freestanding`

The freestanding run builds the reduced `PROVEN_FREESTANDING` configuration. It excludes hosted filesystem, mmap, sysio, thread, environment, time, and job-system dependencies in the current build script.

## What each mode checks

- `build`: debug compile and hosted full test execution.
- `release`: optimized compile and hosted full test execution.
- `strict`: hosted full run with extra warnings.
- `strict-error`: hosted full run with warnings as errors.
- `asan`: hosted full run with AddressSanitizer.
- `ubsan`: hosted full run with UndefinedBehaviorSanitizer.
- `tsan`: hosted full run with ThreadSanitizer.
- `freestanding`: reduced subset with `PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR`, and `-ffreestanding`.
- `cross`: compile-only matrix for optional native, Linux cross, WinAPI MinGW, and freestanding embedded toolchains.

## Change policy

When behavior changes:

1. Add or update the narrowest test first.
2. Confirm the new test fails before the implementation change when practical.
3. Implement the change.
4. Run the narrow test through `nob.c`.
5. Run `./nob build`.
6. Run sanitizer or freestanding modes when the changed area requires them.
7. Update this file if the test matrix changes.

## Release validation

Recommended release gate:

```sh
./nob clean
./nob strict-error
./nob regression-asan
./nob regression-ubsan
./nob freestanding
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Optional compiler-specific gate:

```sh
./nob strict-error -cc clang
```
