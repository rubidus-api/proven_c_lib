# Project Updates and Changelog
v26.05.18

## Overview

**Project Core:** `proven` (C23 Library)
**Latest Version:** `v26.05.18`

This file serves as the definitive record of all modifications, enhancements, and additions made to the **proven** library. All changes must be appended here chronologically to maintain a transparent history of the project's evolution.

**Note on Historical Notes:** Older entries may refer to legacy API names (e.g., `append_view` instead of `append_grow`). These are retained for historical accuracy. Refer to the Developer Manual for current naming conventions.

## Status: v26.05.18 (Latest)

### README markdown cleanup and source-sync refresh
*   **Landing Page Rewrite**: Reworked `README.md` to read as a concise GitHub front page: project intent, practical advantages, quick start, and small source-based examples.
*   **Markdown Hygiene**: Removed stale or awkward escaping and tightened section flow so the README renders cleanly as Markdown instead of looking like an exported draft.
*   **Source Sync**: Rechecked the README examples and module summary against the public headers and current build/test surface.

### Docs-site retirement and version-source cleanup
*   **Version Source of Truth**: Declared `include/proven/version.h` as the single source for version macros and synchronized the visible version string to `v26.05.18`.
*   **Docs Consolidation**: Folded the archived guidance from `docs/` and `docs/ai/` into `AGENTS.md` so the working rules live in one place.
*   **Durable Notes**: Added `MEMORY.md` for stable repository facts and `CHECKLIST.md` for recurring bug lessons and prevention rules.
*   **Path Hygiene**: Reaffirmed that public docs, comments, and help text must not expose private host paths or share-specific account details.
*   **Frontend Cleanup**: Retired the optional frontend presentation and the root package wrapper so the repository now focuses on the C library, its build driver, tests, and manuals.
*   **Version Sync**: Updated the visible version markers in root docs and manuals to match `v26.05.18`.

## Status: v26.05.16 (Archive)

### mmap Flag Validation
*   **Safety Enhancement:** Added strict flag validation in `proven_mmap_create()`. Memory mapping now enforces that either `PROVEN_MMAP_PRIVATE` or `PROVEN_MMAP_SHARED` must be provided, while explicitly rejecting conflicting bounds and bit combinations.
*   **Protection Validation:** Enhanced `prot` evaluation to strictly reject any undefined bit flags outside the bounds of `READ/WRITE/EXEC` to establish clear POSIX mappings mapping semantics and prevent potential edge cases.
*   **Test Cases:** Added verification tests in `test_phase17_mmap.c` asserting correct evaluation and rejection logic for invalid initialization paths with error code `PROVEN_ERR_INVALID_ARG`.

### Structural Invariant Validation
*   **API Additions:** Introduced `proven_array_is_valid`, `proven_u8str_is_valid`, `proven_map_is_valid`, and `proven_ring_is_valid` functions.
*   **Safety Policy:** Manually mutating internal fields of public structures (e.g., modifying `len` or `cap`) is officially documented as equivalent to Undefined Behavior (UB), emphasizing architectural immutability rules. Test logic was extended simulating these boundary assertions.

### Arena & Growable Container Optimization
*   **API Additions:** Implemented explicit capacity reservation APIs `proven_array_reserve`, `proven_u8str_reserve`, and `proven_map_reserve` to decouple intentional structural allocations from organic growth policies. Included alias macro `proven_map_create_with_capacity`.
*   **Documentation Focus:** Expanded `PROVEN_MANUAL.md` explicitly detailing semantic pitfalls where combining an `arena` with auto-growing containers results in abandoned memory because `free()` acts as a no-op within an `arena` configuration block. Pre-allocation via `_reserve` functions is highly encouraged.

### Job System Safety & API Refinement
*   **API Segregation:** Segregated the monolithic `proven_job_system_shutdown()` into a more explicit `proven_job_system_close()` (halting new submissions) and `proven_job_system_destroy()` (thread joining and resource deallocation). This strictly enforces submission limits prior to dismantling and structurally prevents UAF race conditions.

### Pool Allocator Safety
*   **Debug Defenses:** Injected `#ifndef NDEBUG` guards into `proven_pool_free_trait` enforcing pointer alignment validations and mitigating double-free flaws globally via `proven_panic_handler` verification.

## Status: v26.05.10i (Archive)

### Environment Variable Fix
*   **PAL-isolated Error Resolution**: Updated `proven_sys_env_get` to return `proven_err_t` instead of a boolean, allowing distinction between `PROVEN_ERR_NOT_FOUND` and `PROVEN_ERR_OUT_OF_BOUNDS` (overflow). 
*   **Large Variable Handling**: Modified `proven_env_get` to handle environment variables larger than the initial 4KB stack buffer by dynamically allocating sufficient memory based on the size reported by the platform layer.

## Status: v26.05.10h (Archive)

### Format System Safety Enhancements
*   **Security Update**: Updated `PROVEN_ARG_CSTR` documentation regarding safety issues with unchecked null-terminator search. Standardized `PROVEN_ARG_STR_VIEW` as the recommended safer alternative.
*   **Bounded String Argument**: Added `PROVEN_ARG_CSTR_N` (and `proven_arg_cstr_n`) to safely bounds-check NUL-terminated C-strings up to a specified maximum length. Added safety-bound regression tests in `test_regression_v26_05.c`.

## Status: v26.05.10g (Archive)

### Compile Error Fix
*   **Compile Error Resolution**: Fixed an error in `tests/test_sysio_scanner.c` to appropriately handle `proven_fs_open` and `proven_fs_write` return types (`proven_result_file_t` and `proven_result_size_t` respectively). Updated structural scanner string extraction to use `PROVEN_SCAN_ARG(&word)` with string views to align correctly with macros.

## Status: v26.05.10f (Archive)

### Compile Error Fix
*   **Compile Error Resolution**: Fixed an error in `tests/test_scan_overflow_f64.c` where `PROVEN_LIT` was improperly used with a stack allocated variable array. Replaced with `proven_u8str_view_from_cstr`.

## Status: v26.05.10e (Archive)

### Memory Allocation Result Signature Fix
*   **Compile Error Resolution**: Fixed an error in `proven_sysio_scanner_init` (`src/proven/sysio.c`) where `alloc.alloc_fn` was incorrectly processed as a raw pointer instead of a `proven_result_mem_mut_t` struct, resolving clang build failures in strict mode.

## Status: v26.05.10d (Archive)

### PAL Configuration & Compilation Fixes
*   **Include Path Standardization**: Updated `nob.c` to explicitly include the `platform/` directory (`-I./platform`) in all build modes.
*   **PAL Math Integration**: Refactored `src/proven/scan.c` and `platform/proven_sys_math.c` to use standardized relative includes (`#include "proven_sys_math.h"`), resolving header resolution failures in strict clang environments.

## Status: v26.05.10c (Archive)

### Test Standardization & Output Refinement
*   **Test Framework Migration**: Completed the library-wide migration of all test phases and regression tests to the standardized `PROVEN_TEST_INFO` and `PROVEN_TEST_ASSERT` format.
*   **Legacy Test Cleanup**: Refactored `test_foundation.c` and `test_arena_panic.c` to use `proven_test.h`, eliminating local redundant macro definitions and `printf` dependencies.
*   **Test Robustness**: Fixed build errors in `test_regression_fs_copy_to_self.c` related to incorrect macro usage (`PROVEN_MEM_VIEW` replaced with `proven_mem_view_from_u8`).
*   **Version Synchronization**: Updated version strings to `v26.05.10` across `version.h`, `SPEC.md`, `AGENTS.md`, `CHANGELOG.md`, and `index.html`.

## Status: v26.05.08aa (Archive)

### Release Blocker Resolution & Freestanding Hardening
*   **Freestanding Build Fixes**: Resolved critical compilation failures in `./nob freestanding` mode.
    *   Renamed invalid `PROVEN_MAX_ALIGN_T` to `PROVEN_MAX_ALIGN` in `test_freestanding.c` and `manual/manual-freestanding.md`.
    *   Corrected `PROVEN_ARG` usage in `test_freestanding.c` by ensuring appropriate formatting macros are utilized for primitive types.
    *   Added verification for `proven_scan_fmt` within the freestanding test suite.
*   **Alias Synchronicity**: Purged stale scan aliases from `alias_xcv.h` and integrated modern `XCV_SCAN_ARG_TYPE_*` mappings. Verified parity via `test_alias_smoke.c`.
*   **Build Engine Robustness**: 
    *   Updated `nob.c` to conditionally exclude `-pthread` and enforce `-static` for freestanding linking, preventing dynamic library leakage on bare-metal targets.
    *   Enforced `-Werror` across freestanding builds to proactively capture UB-prone patterns.
    *   Synchronized the build command hashing logic to reflect these linking flag modifications.
*   **Documentation Audit**: 
    *   Refined technical language in `panic.h` and `arena.h` expressing explicit non-return policies for production panic handlers.
    *   Synchronized version strings (`v26.05.08aa`) across `SPEC.md`, `AGENTS.md`, `CHANGELOG.md`, `index.html`, and `alias_xcv.h`.

## Status: v26.05.08z (Archive)

### Phase 8: Alias Sync and Native Integer Enhancements
*   **Alias Alignment**: Purged stale internal alias macros (`xcv_scan_arg_cstr_buf`, `xcv_scan_fmt_impl`, etc.) from `alias_xcv.h` preventing undeclared identifier warnings. Integrated missing `XCV_SCAN_ARG_TYPE_*` integer enumeration mappings.
*   **Smoke Validation**: Established `tests/test_alias_smoke.c` executing exported `xcv_` function pointers to verify header integrity in `nob.c`.
*   **Safety Assurance**: Fortified `test_phase21_scan.c` with 32-bit/64-bit bounds validation using `LONG_MAX` to verify native long overflow rollbacks.
*   **Documentation Matrix**: Added integer scan descriptions to `PROVEN_MANUAL.md` for `PROVEN_SCAN_ARG(&x)` behavior.

## Status: v26.05.08y (Archive)

### Phase 7: User-Facing Documentation
*   **Manual Supplement**: Created the freestanding manual, now stored as `manual/manual-freestanding.md`, describing requirements, execution constraints, combinations, and matrices for embedded usage.
*   **Readme Integration**: Integrated freestanding goals into existing architectural README objectives displaying lightweight cross-platform behavior.
*   **Version Update**: Advanced version numbering sequentially to `v26.05.08y`.

## Status: v26.05.08x (Archive)

### Phase 6: Cross-Compile Verification Script
*   **Documentation Artifact**: Created `tests/freestanding_compile_check.sh` script invoking `arm-none-eabi-gcc` for freestanding compilation.
*   **Validation**: Confirmed all non-excluded `.c` files generate `.o` files correctly without stdlib inclusion.
*   **Manual**: Updated freestanding manual material, now stored as `manual/manual-freestanding.md`, indicating execution syntax and documentational architecture of the cross-check script.

## Status: v26.05.08w (Archive)

### Phase 5: Comprehensive Freestanding Evaluation (Integration)
*   **Zero-Dependency Validation**: Established `tests/test_freestanding.c` decoupled from external POSIX/libc dependencies. Bound assertions to native compiler hooks via local `__builtin_trap()`.
*   **Self-Contained Data Modules Validation**: Executed tests for `array`, `list`, `ring`, `map`, `coro`, `u8str`, `fmt`, `algorithm`, and `scan` using a 64KB static, aligned byte array via `proven_arena_t`.
*   **Build-System Engine Injection**: Incorporated `tests/test_freestanding` to exclusively parse under the established `freestanding` compiler directives within the build dispatcher mechanism inside `nob.c` (`-ffreestanding`, `-DPROVEN_FREESTANDING`, explicitly disabling floats/UTF-16 encoding extensions simultaneously). 
*   **Version Advance**: Iterated to `v26.05.08w`.

## Status: v26.05.08v (Archive)

### Panic-on-OOM Allocator Variant (Additive)
*   **Panic Handler Hook**: Defined `proven_panic_handler()` in `panic.h`, providing a weakly linked hook to `__builtin_trap()`. Embedded environments can implement system reset or watchdog routines.
*   **Additive Panic Variants**: Injected `proven_arena_alloc_or_panic()` and `proven_arena_alloc_aligned_or_panic()` directly bound to the global handler hook into `arena.h`.
*   **Documentation Check**: Added details of this subset inside `PROVEN_MANUAL.md`. By retaining `Result` return signatures for core processes, the implementation conforms with Result guidelines (AGENTS.md Rule 4) while granting constrained environments a safe hardware exit trap.
*   **TDD Evaluation**: Conceived explicit failure case evaluation mechanisms embedded inside `tests/test_arena_panic.c`, accurately tracking intercept states through handler overriding logic.
*   **Version Update**: Updated version index to `v26.05.08v`.

## Status: v26.05.08u (Archive)

### Module Slimming Macros
*   **Disabled Float/Double Formatting (`PROVEN_FMT_NO_FLOAT`)**:
    *   Conditionally excluded `PROVEN_ARG_F64` from `proven_arg_type_t` and `proven_arg_f64()` constructor in `fmt.h`.
    *   Stripped the IEEE 754 branch inside `fmt.c`, resulting in smaller object size.
    *   Removed `float` and `double` entries from ` PROVEN_ARG()` `_Generic` selection table, producing natural compile-time errors if used under this mode.
*   **Disabled UTF-16 Encoding Module (`PROVEN_NO_U16STR`)**:
    *   Wholly deactivated definitions inside `u16str.h` and routines in `u16str.c`.
    *   Appropriately gated references to `u16str` within the `time` formatter module.
*   **TDD for Permutations**: Crafted `test_compile_freestanding.c`, `test_compile_nofloat.c`, and `test_compile_nou16str.c` included under the freestanding configuration to confirm the soundness of the header inclusion under disparate macro toggles.
*   **Version Bump**: Accelerated to `v26.05.08u`.

## Status: v26.05.08t (Archive)

### Bare-Metal Heap Allocator Stubs
*   **Heap Stub Implementation**: Modified `proven_heap_allocator` to return `(proven_allocator_t){0}` when compiled under `PROVEN_FREESTANDING`, neutralizing dynamic heap calls on MCU targets.
*   **Allocator Validation**: Validated that `proven_alloc_is_valid()` effectively detects and rejects the stubbed zero-initialized allocator structure.
*   **Documentation & TDD**: Added `tests/test_freestanding_heap_stub.c` mapped correctly into the freestanding `./nob` runner, and explicitly updated module manuals documenting this behavior contract.
*   **Version Synchronization**: Incremented the project version to `v26.05.08t`.

## Status: v26.05.08s (Archive)

### Tier 2 (Freestanding) Compilation Logic
*   **Build-Time Exclusions Engine**: Configured `nob.c` to identify `--freestanding` logic and dynamically bypass all POSIX/OS PAL source exclusions alongside modules relying on file I/O operations (`fs`, `sysio`, `mmap`, `job`).
*   **Freestanding Subroutines**: Provided raw inline C looping substitutes inside `proven_sys_mem.h` and Unix time stub mappings inside `proven_sys_time.h` triggered by `-DPROVEN_FREESTANDING`.
*   **Compiler Pipeline Integration**: Integrated `-ffreestanding` and `-DPROVEN_FREESTANDING` for isolated compilation boundaries.
*   **Version Synchronization**: Incremented the project version to `v26.05.08s` summarizing initial Phase 1 execution.

## Status: v26.05.08r (Archive)

### Bare-Metal & Embedded Specifications
*   **Freestanding Architecture**: Updated architectural specifications and manuals to formally define the `PROVEN_FREESTANDING` build mode for bare-metal MCU targets.
*   **Slimming & Overrides**: Outlined the `PROVEN_FMT_NO_FLOAT` and `PROVEN_NO_U16STR` slimming macros alongside the panic-on-OOM allocator variant (`proven_panic_handler`) for constrained environments.
*   **Version Synchronization**: Incremented the project version to `v26.05.08r` to mark the beginning of Tier 2 freestanding support implementation.

## Status: v26.05.08q (Archive)

### Scanner & Test Refinements
*   **Enum Clarity**: Renamed `PROVEN_SCAN_ARG_*` enum constants to `PROVEN_SCAN_ARG_TYPE_*` to eliminate semantic collisions with function-like macro names.
*   **Documentation Alignment**: Updated documentation for `PROVEN_SCAN_ARG_LONG` explicit macro aliases to accurately reflect their direct storage behavior. Added explicit documentation clarifying that scanning into native types larger than 64 bits is limited to the `proven_i64`/`proven_u64` parsing range.
*   **Test Expansion**: Added comprehensive `tests` to verify native integer scanning across all integer types, including overflow behavior and correct rollback of the scanner cursor.
*   **Version Synchronization**: Incremented the project version to `v26.05.08q`.

## Status: v26.05.08p (Archive)

### Scanner & API Convergence
*   **Native Integer Scanning**: Refactored `PROVEN_SCAN_ARG` and the underlying scan engine to support native C integer types (`short`, `int`, `long`, `long long`, etc.).
*   **LP64/LLP64 Safety**: The implementation now stores scanned values directly into native pointers after rigorous range checks, addressing platform-dependent underlying type aliasing (e.g., `long` vs `int64_t`).
*   **Version Synchronization**: Incremented the project version to `v26.05.08p` across all headers, specifications, manuals, and build metadata.

## Status: v26.05.08l (Archive)

### Build System & Documentation
*   **Linker Customization Support:** Added `-ld` flag and `LD` environment variable support to `nob.c` for specifying custom linking tools.
*   **Help Message Polish:** Expanded the build system help message (`./nob -h`) with detailed descriptions of commands, options, and environment variables.
*   **Version Update:** Incremented version to `v26.05.08k` and synchronized all architectural metadata.

## Status: v26.05.08j (Archive)

### Build System & Safety
*   **Strict Error Resolution:** Fixed an `unused-variable` compiler error in `src/proven/u16str.c` specifically involving `alias_offset`.
*   **Checked Arithmetic Enforcement:** Enhanced overflow detection in `proven_u16str_concat` using `PROVEN_CKD_MUL` for data size calculations.

## Status: v26.05.08i (Archive)

### Documentation & Usability Refinement
*   **Phase R5 Enhancements (Usability Audit):** Conducted a comprehensive documentation and API usability audit to improve developer onboarding and safety.
    *   **Allocator Ownership Matrix:** Integrated a definitive ownership table in `PROVEN_MANUAL.md` clarifying which containers store allocators versus those requiring external management.
    *   **Internal API Warnings:** Documented strict warnings regarding the usage of `_internal` suffixed functions to prevent misuse of macro backends.
    *   **Borrowed Key Safety:** Reinforced documentation for `PROVEN_KEY_TYPE_U8_BORROWED` in Maps, explicitly detailing lifetime requirements and address stability constraints.
    *   **Platform Support Tiers:** Formalized the "Tiered Support" model in `README.md` and `PROVEN_MANUAL.md`, designating Linux x86_64 as Tier 1 and other environments (Windows, ARM) as Experimental/Community supported.

## Status: v26.05.08g (Archive)

### Build System Refinement
*   **nob.c Structure Refactoring (Phase R4):** Refactored `nob.c` build script to improve maintainability and reduce duplication. Extracted core build logic into specialized helper functions:
    *   `hash_cmd`: Centralized command-line hashing for change detection.
    *   `write_cmdhash`/`read_cmdhash`: Unified persistent hash management.
    *   `output_file_valid`: Consolidated validation for objects and executables.
    *   `compile_object_tmp`/`link_executable_tmp`: Abstracted stage-based build steps.

## Status: v26.05.08f (Archive)

### Robustness & Regression Testing
*   **Regression Test Expansion (`tests/test_regression_v26_05.c`):** Integrated 9 critical regression cases covering:
    *   Map self-payload rehash safety with scratch allocator support.
    *   Map existing key update behavior preventing premature growth.
    *   Format engine self-aliasing C-string protection returns `PROVEN_ERR_INVALID_ARG`.
    *   Scanning engine boundary safety for invalid cursors.
    *   Array and String self-aliased growth safety.
*   **Testing Philosophy Documentation (`docs/TESTING.md`):** Formalized the distinction between Phase tests (broad features) and Regression tests (specific narrow bug prevents).

## Status: v26.05.08e (Archive)

### Core Refinements & Maintainability
*   **Buffer Range Helper Centralization (Phase R2):** Introduced `proven_bufref_t` and related inline capture/rebase helpers in `proven_internal_memrange.h` to abstract out self-alias bounding logic across the core library.
*   **Format Engine Phase R1 Refactoring (`fmt.c`):** Extract internal parser and patching buffer logic (`proven_fmt_parse_arg_index`, `proven_fmt_arg_patch_prepare`, `proven_fmt_arg_patch_apply`) into clearly separated helper functions to improve structural clarity and mitigate format-spec parsing complexities, while maintaining the public API identically.

## Status: v26.05.08c (Archive)

### Signature Modernization & Safety
*   **Macro Signature Alignment (`map.h`):** Reordered `scratch` parameter in `PROVEN_MAP_SET_WITH_SCRATCH_INT` and `PROVEN_MAP_SET_WITH_SCRATCH_U8_BORROWED` to reside at the end of the required parameters before varargs, creating a uniform footprint identical to the core functions.
*   **Format Engine Optimization (`fmt.h`, `fmt.c`):** Introduced `proven_u8str_append_fmt_with_scratch` allowing formatter expansion while delegating large parameter arrays (> 16) self-aliasing patched resolution buffers into explicit scratch allocators avoiding arena fragmentation.
*   **Documentation Refinements (`PROVEN_MANUAL.md`):** Clarified the failure-atomic behavioral model underlying string resizing during self-aliasing formatting expansions resolving any ambiguity in API contracts.

## Status: v26.05.08b (Archive)

### Memory & Policy Refinements
*   **Formatting Patch Optimization (`fmt.c`):** Refined the patch buffer allocation logic to only perform heap allocations when a self-alias is actually detected during growth, reducing fragmentation in arena-backed contexts.
*   **Map Safety & Cleanliness (`map.c`):** 
    *   Added explicit key-type validation to `proven_map_create` to prevent initialization with invalid enums.
    *   Hardened `proven_map_destroy` to perform a full zero-initialization of the header after freeing internal blocks, preventing stale handle usage.
*   **Documentation Bolstering:** Added specific warnings regarding `U8_BORROWED` key lifetimes and updated the platform support matrix to reflect Tiered support (Tier 1: Linux/x86_64, Tier 2: Windows/ARM/MSVC).

## Status: v26.05.08a (Archive)

### Formatting Engine Safety
*   **Stack Overflow Mitigation (`fmt.c`):** Resolved a critical write-overflow vulnerability in `proven_u8str_fmt_internal`. Previously, formatting workloads with `args_count > 16` combined with string growth but no self-aliases would erroneously write beyond the `stack_patched[16]` buffer.
*   **Atomic Patching Logic:** Refactored the reallocation-patching pipeline to compute `needs_patch` and `found_any_alias` state transitions before attempting buffer copies.
*   **Failure-Atomic Allocation:** Enforced pre-allocation of the heap-based argument patch buffer whenever `args_count > 16` during growth phases, ensuring successful pointer translation even in low-memory scenarios.

### Map Engine Cleanup
*   **Redundant Offset Removal (`map.c`):** Excised an unused `offset` variable in `proven_map_set_with_scratch`, passing `NULL` to the range-containment helper to streamline execution paths and resolve potential static analysis warnings.

## Status: v26.05.08 (Archive)

### Core Map Engine Robustness
*   **Update Logic Generalization:** Integrated `proven_sys_mem_move` (PAL-isolated) within `map_insert_no_grow` to handle overlapping element updates and preserve aliasing integrity during key-value mutations.
*   **Input Validation Guard:** Implemented explicit `NULL` verification for payloads inside `map_insert_no_grow` to prevent undefined behavior during low-level memory migrations.

### Build System & Verification
*   **Command Hashing Integrity:** Reinforced `nob.c` with explicit null-termination for command-line string builders before hashing, resolving potential out-of-bounds reads and ensuring bit-exact rebuild triggers.
*   **Test Diagnostic Modernization:** Replaced implicit library `printf` calls with PAL-isolated `proven_eprintln` within `test_phase11_map.c`, maintaining the project's CRT-isolated strategy.

## Status: v26.05.07n (Archive)

### Architecture Formulation
*   **Map Scratch Extraction:** Exported the capability to provide alternative allocators (`proven_map_set_with_scratch`) used during map insertion workloads requiring self-payload memory extraction prior to bounds migrations.
*   **Aliased Element Safeties:** Centralized logic governing payload self-referential tracking within the primary map engine bounding scratch allocations to internal `mem_move` bypasses eliminating memory bloats under arena workflows utilizing repeated key mutations.

## Status: v26.05.07l (Archive)

### Architecture & Robustness Enhancements
*   **Map Element Overlap Safety (`map.c`):** Implemented directional byte copier logic within `map_set()` handling cases where `element` overlaps existing map values (e.g. self-payload references). Maintains pointer structural integrity avoiding libc `memmove` dependency inside the core.
*   **Rehash Memory Policy Documentation:** Clarified limitations of `map_set()` arena retention behavior when inserting large multi-KB struct types during self-aliased operations structurally resolving unrecoverable bounds exhaustion patterns.
*   **Backend Support Policy:** Stated explicitly in `README.md` that Linux ARM/AArch64 raw syscalls implementation is currently experimental.
*   **Documentation Site Cleansing:** Clarified within `README.md` that the optional frontend documentation builder stayed separate from the independent C library engine.

## Status: v26.05.07k (Archive)

### Compilation & Warning Fixes
*   **Map Payload Reference (`map.c`):** Fixed compiler targeting error mapping to an undeclared `map_find_payload` usage. Implemented explicit static forward declaration ensuring proper scoping for nested lookup implementations.
*   **Seek Unused Result (`sysio.c`):** Addressed a `[-Wunused-result]` warning generated under clang verification targeting the `[[nodiscard]]` execution pattern attached to `proven_sys_io_seek_relative`. The resulting variable is captured to map fail states into process pipelines.

## Status: v26.05.07j (Archive)

### Documentation & Policy Updates
*   **MSVC Support Policy:** Explicitly updated documentation (`README.md`) to state that MSVC is officially unsupported and will only be considered once Microsoft fully implements the C23 standard (including features like `<stdckdint.h>`).

## Status: v26.05.07i (Archive)

### Robustness & Safety Policy Hardening
*   **Format Index Type Integer Overflow (`fmt.c`):** Changed the internal argument index type in `fmt_run` from `int` to `proven_size_t` resolving an integer overflow edge-case that could occur when processing large placeholder counts under `aarch64` and typed bounds contexts.
*   **Syscall EINTR Clamping (`proven_sys_io.c`):** Fixed raw syscall `EINTR` loop logic covering ARM and AArch64. When `-EINTR` occurred in the raw syscall inline assembly (`swi/svc`), the returned error code accidentally overwrote the persistent file descriptor variable. The logic has been rewritten so that real file descriptors use separate variables and get freshly loaded prior to every retry.
*   **Fmt String Reallocation Atomicity (`fmt.c`):** Enforced a pure failure-atomic transaction inside `append_fmt_grow`. Dynamic allocation of the patched string arguments structure (triggering when aliases occur and arg count is > 16) is definitively checked and executed *prior* to `str` array scaling `realloc`. This prevents scenarios where memory growth causes partial structure state corruption if subsequent heap scaling fails.
*   **FS Open Mode Verification (`proven_sys_fs.c`):** Updated invalid/unknown mode representations within `proven_sys_fs_open()` from returning ambiguous fallback POSIX values directly into hard failing returning safely nullified `(proven_sys_file_handle_t){.fd = -1, .handle = NULL}` across environments.

## Status: v26.05.07h (Archive)

### Build System & Core Improvements
*   **Command Hash Verification (`nob.c`):** Migrated from purely timestamp-based recompilation evaluation to explicit command hashing using `.cmdhash`. The build engine dynamically enforces full rebuilds across all targets whenever internal compile/link flags or variables modify the execution signature.
*   **SysIO All/Once Semantics (`sysio.c`, `proven_sys_io.h`):** Forked foundational `proven_sys_io_write` and `proven_sys_io_read` into `_once` (partial) and `_all` variants inside the PAL layer. Refactored the core library `sysio.c` to bind to the `_all` patterns to maintain cross-platform standard IO payload guarantees without manual app-layer loop wrappers.
*   **SysIO Buffer Evaporation Prevention (`sysio.c`, `scan.c`):** Solved the critical data loss (evaporation) vulnerability in `proven_sysio_scan_chunk_impl`. Previously, unparsed bytes from the 4096-byte chunk read were permanently dropped from unbuffered streams. Introduced `proven_sys_io_seek_relative` to the PAL layer (leveraging `lseek`/`SetFilePointerEx`) enabling precise, failure-atomic rewinding of the OS cursor by the exact unconsumed byte offset immediately post-scan.

### Build System Reliability
*   **Object Compilation Protection (`nob.c`):** Applied the temporary file renaming pattern (`.tmp` → final) to `.o` object generation. This prevents interrupted compilations from leaving behind 0-byte or corrupted object files that trigger false "up-to-date" states on subsequent builds.
*   **Object Output Validation**: The build engine now verifies that an object file is non-zero in size before skipping compilation (`proven_output_is_valid_object`).
*   **Compiler Path Sanitization:** Explicitly sanitized the `compiler_exe` variable when constructing the build directory name (replacing `/`, `\` with `_`). This fixes directory creation failures when custom compiler paths (e.g., `CC=/usr/bin/clang`) are used.

### IO Layer Consistency
*   **Raw Syscall Protective Caps (`proven_sys_io.c`):** Implemented an explicit `0x7ffff000u` size cap on all raw Linux system calls (`read` and `write` loops across x86_64, i386, aarch64, arm). This mirrors the Windows `DWORD` constraint and ensures stability for ultra-large IO operations directly to the kernel.
*   **Once-Semantics Clarification:** Restructured documentation in `PROVEN_MANUAL.md` to explicitly annotate `proven_sys_io_read/write` as utilizing "read-once/write-once" semantics (partial transfers), distinguishing them definitively from the full-loop mechanisms in higher-level FS layers.

### Documentation & Maintenance
*   **PAL Thread Allocations:** Documented in `README.md` the internal exception allowing PAL thread lifecycle functions to utilize OS heap allocations for metadata tracking, without violating the strict allocator trait mandates of core data structures.
*   **Demo Artifact Cleanup:** Removed unnecessary AI Studio framework dependencies to keep the optional demo UI lightweight, and clarified the site's optional nature in `README.md`.

## Status: v26.05.07f (Archive)

### Build System & Windows Stability
*   **Target Extension Correction (`nob.c`):** Fixed a critical path issue where executables linked with `gcc/clang` on Windows lacked the `.exe` extension, leading to "File not found" errors during test execution. Standardized `exe_ext` to `.exe` for all Windows toolchains.
*   **Atomic Link Renaming (`nob.c`):** Updated the build engine to link to a temporary `.tmp` file and only rename it to the final executable on success. This prevents corrupted or zero-byte files from being treated as "up-to-date" after interrupted builds.
*   **Output Validation:** Added `proven_output_is_valid_executable` to verify that built artifacts are non-zero in size and have correct execution bits before skipping build stages.
*   **Advanced Flag Hashing:** Expanded the build directory hashing to encompass the full set of global compiler flags (`-std`, `-I`, feature macros), ensuring transparent rebuilds when any part of the build command changes.

### IO & Memory Architecture
*   **Unified IO Primitives (`proven_sys_fs.c`, `proven_sys_io.c`):** Standardized both the specialized and file-system IO primitives to align with the core's "once" (partial transfer) semantics. This removes redundant low-level loops, deferring high-level logic (full-write loops) to the `fs_write_all` and `sysio` layers.
*   **Windows IO Hardening:** Implemented explicit `0x7FFFFFFF` caps on Windows `ReadFile` and `WriteFile` calls to safely handle large buffer sizes without `DWORD` truncation issues.

### Documentation & Maintenance
*   **PAL Reference Section:** Updated the Developer Manual with a section for the Platform Abstraction Layer (PAL), documenting unbuffered IO semantics and thread management policies.
*   **Memory Mandate Exceptions:** Explicitly documented the PAL's internal use of system heap allocation for thread handles as a justified exception to the allocator-trait mandate.
*   **README Cleanup:** Fixed duplicate headers, corrected broken code blocks, and added portability notes for `PROVEN_ARG_FN`.

## Status: v26.05.07e (Archive)

### Platform & OS Robustness
*   **Unified POSIX I/O (`proven_sys_io.c`):** Unified macOS, BSD, and Linux to use POSIX file descriptors and `read/write` syscalls. Removed invalid `fflush` calls in `proven_sys_io_flush` for non-Windows platforms, eliminating potential crashes when using raw fds.
*   **Build Isolation (`nob.c`):** Enhanced the build system with directory hashing (`build/<compiler>-<base_mode>-<hash>/`). This ensures absolute isolation of object files even when switching between different compilers or flag configurations.
*   **Regression Suite (`tests/test_regression_v26_05.c`):** Introduced a specialized regression test covering:
    *   Map self-aliased rehash safety.
    *   Map large-value allocation tracking under self-aliased insertion.
    *   String own-view growth safety (aliasing during `append_fmt`).
    *   Boundary checks for exceptionally large formatting argument indices.
*   **Naming Sanitization:** Renamed `_impl` suffixes to `_internal` in `fmt.c`, `scan.c`, `u8str.c`, and `sysio.c` to clarify the internal-only nature of certain base functions.

### Core Logic & Safety
*   **Map Resiliency (`map.c`):** Fixed an edge case where empty maps could prematurely rehash. Simplified FNV hashing parameters using `sizeof` checks instead of preprocessor macros for improved portability.
*   **Overflow Policy (`types.h`):** Mandated C23 `<stdckdint.h>` or compiler built-ins for checked arithmetic, enforcing a strict `#error` on unsupported platforms to prevent silent safety regressions.

## Status: v26.05.07b (Archive)

### API Sanitization & Consistency
*   **Public/Internal Boundary:** Renamed `proven_u8str_fmt_impl` and `proven_scan_fmt_impl` to `_internal` to clearly distinguish them from public macros. Integrated mandatory `args != NULL` validation in the internal formatting and scanning kernels.

## Status: v26.05.07a (Archive)

### Platform & Build Refinements
*   **SysIO Handle Portability:** Removed architecture guards within the `proven_sys_io_handle_t` union to ensure consistent struct layout regardless of the active compiler branch.
*   **Build Engine Performance:** Optimized `nob.c` to use mode-specific build subdirectories for parallelizing builds across different optimization profiles.

## Status: v26.05.05k (Archive)

### Safety & UAF Prevention
*   **String Alias UB Fix (`u8str.c`):** Removed potential Undefined Behavior in `proven_u8str_replace_at` by replacing relational pointer comparisons (`>=`, `<`) with the `proven_range_contains_ptr` helper. This ensures alias detection even when comparing pointers from unrelated memory allocations, adhering to ISO C requirements.
*   **Format C-String Alias Protection (`fmt.c`):** Implemented a mandatory check for self-aliased C-string arguments in `proven_u8str_append_fmt_grow`. If a `PROVEN_ARG_CSTR` points into the string's internal buffer and a reallocation is required, the function now explicitly returns `PROVEN_ERR_INVALID_ARG` to prevent use-after-free, aligning with the documented safety policy.

## Status: v26.05.05j (Archive)

### Data Consistency & Safety
*   **Map Self-Alias Fix (`map.c`):** Resolved a critical use-after-free vulnerability in `proven_map_set`. Passing a pointer to an element already stored within the map (self-aliasing) could trigger a dangling pointer access if the map performed a rehash/reallocation before insertion. The fix now explicitly detects self-aliasing using `proven_range_contains_ptr` and caches the element in a temporary buffer before triggering growth.

## Status: v26.05.05h (Archive)

### Type Safety & Compliance
*   **Compile Error Fix (`types.h`):** Defined `PROVEN_SIZE_MAX` explicitly as `((proven_size_t)SIZE_MAX)` in `types.h` to address undefined macro errors during strict-error compilation in platform layers.
*   **Self-Alias Checks (`memory.h`, `fmt.c`, `array.c`, etc):** Addressed a potential Undefined Behavior with pointer comparisons by introducing `proven_range_contains_ptr`, evaluating both boundaries.
*   **Time & Date Logic (`time.c`):** Fixed negative calculation errors in `calc_weekday` avoiding modulo regressions for negative years.
*   **Platform Specific Operations (`proven_sys_fs.c`):** Synchronized non-regular file querying behaviors across Windows and POSIX variants safely truncating unsupported large byte-sizes utilizing explicit `PROVEN_SIZE_MAX` limits.
*   **Documentation Refinements (`README.md`, `docs/`):** Completed cleanup on residual internal todo notes and improved platform isolation wording in `docs/` matching structural capabilities without overstating them.

## Status: v26.05.05g (Archive)

### Documentation & Compliance Hardening
*   **Official License (`LICENSE`):** Added the formal MIT License file to the repository root.
*   **Terminology Refinement (`SPEC.md`, `AGENTS.md`, `README.md`, `manual/`):** Conducted a project-wide technical documentation audit to replace all overreaching "independent" or "isolated" environmental claims with more precise descriptions such as "PAL-isolated", "stdio-minimized", and "centralized platform access".
*   **Restrained Technical Language Policy:** Updated the `AGENTS.md` guidelines to strictly forbid exaggerated claims (e.g., "immunity", "perfect") in favor of balanced technical assertions (e.g., "designed to", "verified for").

### Type Safety & Compliance
*   **Time Module u16 Formatting Buffer (`time.c`):** Fixed an issue in `proven_time_u16_fmt` where zero padding specifiers (e.g., `:0>2`) were parsed incorrectly due to a ':' character offset mismatch. This fixes test regressions in `test_phase16_time_fmt.c`.
*   **Sign Conversion Fix (`fs.c`):** Corrected a sign conversion error in `proven_fs_lock` where a `proven_err_t` enum was being used to store an `int` lock type. This resolves `-Wsign-conversion` warnings during compilation.
*   **PAL File Handle Portability (`proven_sys_fs.c`):** Enforced correct platform-specific handle field designations (`.handle` for Windows, `.fd` for POSIX) instead of the invalid shared `.internal` field on failures, addressing compilation failures under strict C23 flags.

## Status: v26.05.05f (Archive)

### Handle Architecture & POSIX Robustness
*   **Handle Representation (`fs.h`, `proven_sys_fs.h`, `proven_sys_io.h`):** Replaced the `fd + 1` integer-to-pointer casting hack with an explicit `struct` containing a union or platform-specific members (`int fd` on POSIX, `void *handle` on Windows). This aligns the library with strict provenance and C23 pointer safety guidelines.
*   **File System Listing Fix (`proven_sys_fs.c`):** Corrected the POSIX implementation of `proven_sys_fs_dir_next` which was erroneously returning 0 for regular file sizes. It now utilizes `fstatat` to report accurate byte counts during directory listing.
*   **Safe Size Semantics (`proven_sys_fs.c`):** Hardened `proven_sys_fs_size` and `proven_sys_fs_stat` (POSIX) with explicit overflow guards and negative size checks. Refined the policy to return `PROVEN_ERR_OVERFLOW` for files exceeding `PROVEN_SIZE_MAX` and 0 for non-regular files when size queries are ambiguous.
*   **Formatting Fix (`time.c`):** Fixed a regression in `proven_time_u16_fmt` where unknown placeholders with format specifiers (e.g., `{foo:0>2}`) were losing their colon in the output.

## Status: v26.05.05d (Archive)
## Status: v26.05.05c (Archive)

## Status: v26.05.05b (Archive)

### Critical Bugfixes & Final Verification
*   **Windows FS Security (`proven_sys_fs.c`):** Verified that `proven_sys_fs_rmdir()` and `proven_sys_fs_mkdir()` correctly manage memory without double-free vulnerabilities on WideChar conversions.
*   **Arithmetic Error Contract (`u8str.c`, `arena.c`, `map.c`):** Standardized error propagation rules. Calculation overflows now consistently return `PROVEN_ERR_OVERFLOW`, while capacity exhaustion returns `PROVEN_ERR_OUT_OF_BOUNDS`. 
*   **MMap Memory Protection (`mmap.c`):** Injected mandatory `offset` validation against file boundaries. Attempting to map beyond the file size now triggers an explicit `PROVEN_ERR_OUT_OF_BOUNDS` logic gate before kernel handoff, preventing platform-specific bus errors.
*   **Unicode Bridge Integrity (`proven_sys_fs.c`):** Adjusted the Win32 `dir_next` iterator to terminate explicitly with `false` if direct path decoding fails, rather than masking failures with empty strings.

## Status: v26.05.04a (Archive)

### Error and Robustness Improvements
*   **Directory Scanning (`proven_sys_fs.c`):** Fixed Win32 directory iterator to cleanly terminate (return false) upon filename decode failure, preventing empty/corrupt UTF-8 strings from reaching the platform abstraction outputs.
*   **Hash Maps (`map.c`):** Fixed arithmetic integer bounds checks during internal capacity and hashing operations. Internal `PROVEN_CKD_MUL` and `PROVEN_CKD_ADD` sequence limits properly relay `PROVEN_ERR_OVERFLOW`.
*   **Filesystem (`fs.h`):** Explicitly documented the error logic of `proven_fs_read_all()` where a failed final memory shrink cleanly returns the correctly sized logical bytes while preserving the original chunked allocation natively.
*   **String Expansion (`u8str.c`):** Adjusted exact edge-case logic during `replace_at` calculations for error classification precision (`PROVEN_ERR_OVERFLOW` instead of `PROVEN_ERR_OUT_OF_BOUNDS`).

## Status: v26.05.04 (Archive)

### Robustness & Safety Policy Hardening
*   **FS & IO Subsystems (`sys_io.c`, `sys_fs.c`, `fs.c`, `mmap.c`):** Migrated `proven_sys_fs_size` and `proven_sys_io_write` to use the explicit `proven_sys_result_size_t` Result types, guaranteeing safe failure propagation instead of masking underlying `errno` failures as zero reads/writes. Upgraded `proven_mmap_create()` to clearly distinguish between genuine empty files and invalid file handles or size query bounds failures. 
*   **String & Arena Arithmetic Hardening (`u8str.c`, `arena.c`):** Hardened numeric bounds checking limits on allocations. Overflowing requested slice lengths during `PROVEN_CKD_ADD` limits now correctly surface `PROVEN_ERR_OVERFLOW` directly instead of conflating to `PROVEN_ERR_OUT_OF_BOUNDS` or `PROVEN_ERR_NOMEM`.
*   **Documentation Policies & Lifetimes (`README.md`, `PROVEN_MANUAL.md`):** Updated operational lifetime limits across `README.md` including boundaries on `proven_job_system_shutdown()` races against active submitters and queue wraparound. Further clarified the reliable allocate-copy-free semantics in standard heap memory relocations.

## Status: v26.05.03ad (Archive)

### Bugfixes
*   **String Fixes (`u16str.c`):** Fixed a compilation error where an implicit `MIN` macro was used in `proven_u16str_append_partial` calculation. Replaced with explicit literal math matching the `u8str` implementation.

## Status: v26.05.03ac (Archive)

### Core Refinements & Hardening
*   **String Subsystems (`u8str.c`, `u16str.c`):** Unified error propagation strategies. Overflow during string creation and append arithmetic now explicitly returns `PROVEN_ERR_OVERFLOW`. Hardened `append_partial` with mandatory invariant validations (`current_len < cap`) and checked arithmetic for terminal-null indexing.
*   **PAL & System Safety (`sys_io.c`):** Added implicit `NULL` handle protection to `proven_sys_io_flush` ensuring stability when facing uninitialized stream handles on Windows and fallback stdio paths.
*   **Memory Alignment (`align.h`, `arena.c`):** Introduced `proven_uintptr_align_up` to provide robust, type-correct address alignment independent of `size_t` limits. Refactored the Arena allocator to leverage this distinct address alignment utility.
*   **Documentation (`PROVEN_MANUAL.md`):** Explicitly documented the 4096-byte chunk scanning limit for `sysio_scan_chunk_impl`. Clarified the Atomic Reliability policy for heap relocations (allocate-copy-free) and established strict external synchronization requirements for Job System shutdowns in the manual.

## Status: v26.05.03ab

### Foundation & PAL Hardening
*   **Job System Lifecycle Safety (`job.c`, `job.h`):** Fixed a sys leak where `proven_job_system_init` returned upon internal `PROVEN_CKD_MUL` queue capacity arithmetic overflow without dismantling previously allocated sys structures. Transitioned queue tracking limits internally directly mapping strictly against bounded `PROVEN_ERR_OVERFLOW` instead of bounded metrics. Further documented and bound external submission lifecycles strictly stating `proven_job_system_shutdown()` mapping limits bounding concurrent `proven_job_submit()` race occurrences entirely enforcing strict external shutdown limits.
*   **PAL Boundary Enforcements (`sys_fs.c`, `fs.c`):** Integrated internal parameter validations directly into PAL invocations (`proven_sys_fs_open`, `proven_sys_fs_stat`) trapping edge-case direct NULL string manipulations mapping correctly trapping `out_stat` validation issues. Refined Manual instructions detailing `platform/` structures representing isolated system limits internally decoupled from global system contexts.
*   **Thread Safety (`sys_thread.c`):** Discarded unsafe C-union Windows function casting models substituting explicit structural `proven_win_thread_start_t` trampling functions matching strictly bound `LPTHREAD_START_ROUTINE` representations cleanly dropping non-compatible signature mappings.

## Status: v26.05.03aa

### Core Systems & Architecture Improvements
*   **System Integrity (UTF-8 Path Handling):** Enhanced `utf8_to_wide_alloc` in the Windows platform layer to explicitly trap invalid UTF-8 characters (`MB_ERR_INVALID_CHARS`), replacing unsafe fallback behaviors with rigorous `NULL` propagation.
*   **System Resiliency (Long Path Enforcement):** Upgraded `proven_sys_fs` native OS functions (like `proven_sys_fs_dir_next`) to dynamically reallocate unbounded UTF-8 translation boundaries resolving potential buffer exhaustion vectors alongside core namespace (`\\?\`) injections to combat `MAX_PATH` truncation limitations.
*   **Scanner API Bounds Verification:** Hardened boundary documentation on `proven_sysio_scan_chunk_impl` to explicitly denote its operational mode as a 4096-byte chunked reader intended for interactive constraints, avoiding accidental misuse in full-stream scanning contexts.
*   **Memory Safety (Arena Alignment Math):** Corrected alignment logic in `proven_arena.c` using bounded `PROVEN_CKD_ADD` checked operations alongside `proven_uintptr_t` preventing catastrophic underflow and unsafe mathematical truncations when cross-verifying offset limits.
*   **Memory Defenses (U8 string bounds):** Injected interior `NUL` byte rejection directly into `proven_u8str_view_to_cstr()`, explicitly killing unsafe conversion of embedded C-strings and maintaining `proven_result_cstr_t` structural transparency.
*   **Thread Safety (POSIX Architecture):** Migrated POSIX thread handles (`proven_sys_thread_t`) away from strict compiler-defined integer-casting to explicit dynamically allocated `proven_posix_thread_box_t` structures enforcing long-term global platform interoperability over varying `pthread_t` runtime footprints.
*   **Null I/O Policy Stabilization:** Unified zero-size data stream behavior in `proven_sys_fs_read/write` to cleanly accept logical lengths while strictly checking target `buf` pointer legitimacy to close POSIX API inconsistencies.
*   **API Defenses (Slicing Mutations):** Hardened structural contracts inside `memory.h` via `_checked` bounds-verifying view manipulators (`proven_mem_view_slice_checked`, `proven_mem_mut_slice_checked`), demoting dangerous legacy slice logic into explicit `_unchecked` legacy signatures.

## Status: v26.05.03x

### Critical Security & Robustness Updates
*   **Security (NUL byte injection):** Strengthened C-string bridges `internal_view_to_cstr` and `proven_env_get` to definitively reject interior NUL bytes (`\0`), preventing traversal mismatches on Windows & POSIX targets.
*   **Safety (Allocator Crash Fix):** Hardened `proven_u8str_view_to_cstr()` to execute strict `proven_alloc_is_valid()` before allocation, explicitly fixing a system crash trigger on zero-initialized `(proven_allocator_t){0}` calls.
*   **System Integrity (File Mode Truncation):** Corrected `proven_fs_open()` platform-flags mode mapping so `PROVEN_FS_WRITE` only flags (`r+b`) no longer implicitly destruct existing file streams without `PROVEN_FS_CREATE` or `PROVEN_FS_TRUNC`.
*   **Format Constraints (CPU DoS Protection):** Rewrote `proven_scan_f64()` float exponent parsing mechanisms to disable generic whitespace skipping and explicitly limit unbounded parsed `e` exponents dynamically restricting iterative multiplication attacks.

### Engine Hardening
*   **POSIX Completeness (`sys_thread.h`):** Changed POSIX threaded operations to allocate standard boxed structs dynamically, effectively preventing arbitrary `intptr_t` representation failures on opaque `pthread_t` runtime structures.
*   **UB Defenses (`arena.c`):** Migrated external pointer comparisons in `proven_arena_realloc_aligned()` towards bounded algebraic integer conversions (`proven_uintptr_t`). It leverages numeric operations with tested bounds controls rejecting pointer overflows, avoiding C pointer arithmetic Undefined Behavior.
*   **OS Resilience (`sys_fs.c`, `sysio.c`):** Verified Windows OS string conversion failures explicitly mapping bad Unicode bounds, and hardened System IO interfaces mapping numeric `PROVEN_ERR` correctly catching partial byte-write operations and specific UNIX file descriptor streams efficiently.

### Critical Bug Fixes & C23 Hardening
*   **Scanner Generic Typematch (`scan.h`):** Rewrote `PROVEN_SCAN_ARG` macro. Migrated from internal typedefs to C-native explicit primitive integers (`int`, `long`, `long long`, etc.) to absolutely eliminate identically-compatible type duplication in `_Generic` evaluations across varying architectures (such as 64-bit LP64 where `int64_t` aliases `long`).
*   **Result Pattern Correction (`u16str.h`):** Fixed `proven_result_u16str_t` field order to align with library-wide `err` first convention, preventing memory misalignment on result checks.
*   **Mmap Zero-Copy View (`mmap.h`):** Implemented `proven_mmap_as_view()` to enable direct binary parsing from mapped memory as requested in specifications.
*   **C23 Macro Standardization (`sysio.h`):** Migrated `proven_println` and `proven_eprintln` from GCC-specific `##__VA_ARGS__` to standard C23 `__VA_OPT__` logic.
*   **Memory Safety (`proven_sys_mem.c`):** Added explicit `size == 0` guards in PAL allocation to prevent implementation-defined UB.
*   **Type Integrity & Portability:**
    *   Updated `proven_coro_t` to utilize `proven_i32` providing strict 4-byte consistency across platforms.
    *   Hardened `PROVEN_SCAN_ARG` with explicit `sizeof(long)` checks in `_Generic` to ensure 64-bit integer extraction integrity on Windows x64 vs Linux.
*   **Documentation Synchronization:** Updated `SPEC.md` to reflect all 25 implementation stages and standardized library versioning to `v26.05.03u` across all manuals and headers.

## Status: v26.05.03t

### Release Orchestration & Workflow Formalization
*   **AI Agent Workflow (`AGENTS.md`):** Formalized the "Release Workflow" (Section 13), defining explicit multi-file synchronization and cleanup tasks triggered by release requests.
*   **Test Pipeline Documentation (`TEST.md`):** Performed a library-wide audit of the TDD verification pipeline. Synchronized phase counts and descriptions to accurately reflect the 25 distinct test modules being executed by `nob.c`.
*   **Alias System Maintenance (`include/proven/alias_xcv.h`):** Verified and updated the `xcv_` ergonomic alias layer to ensure 100% parity with the latest core library primitives.
*   **Documentation Synchronization:** Updated `index.html`, `SPEC.md`, `README.md`, and all manuals to the latest version string.

## Status: v26.05.03s

### Documentation Softening & Build Mode Hardening
*   **Documentation Refinement (`index.html`, `SPEC.md`, `manual/PROVEN_MANUAL.md`, `AGENTS.md`):**
    - Performed a comprehensive library-wide documentation review to soften aggressive and absolute claims, favoring academically defensible and precise technical phrasing.
    - Replaced terms like "absolute independence" with "PAL-isolated independence," "perfect memory control" with "robust memory control," and "verify architectural integrity" with "stress-test architectural integrity."
    - Refined string "state transitions" predictability language from "absolute" to "high predictability." 
*   **Build System Enhancements (`nob.c`):**
    - Introduced a dedicated `strict-error` build mode (`./nob strict-error`) which enforces `-Werror` across library and test compilation, ensuring that warnings are treated as fatal errors during CI/CD or strict local development.
*   **Scanner Robustness (`src/proven/scan.c`):**
    - Hardened `proven_scan_f64` exponent parsing logic. Fixed a regression where invalid exponent strings (e.g., `1e`, `1e+`) would incorrectly succeed or leave the cursor in an intermediate state.
    - Implemented a complete cursor rollback mechanism for `proven_scan_f64` so that any failure during the parsing stages (mantissa, decimal, or exponent) restores the scanner to its exact state prior to the float parse attempt.
*   **Sanitizer Verification:**
    - Confirmed that the entire test suite (22 phases) completes successfully under `asan`, `ubsan`, and `tsan` diagnostic modes without regressions.

## Status: v26.05.03r

### Memory Safety & API Consistency Hardening
*   **Allocator NULL-Safety (`src/proven/pool.c`, `include/proven/arena.h`):** 
    - Hardened `proven_pool_as_allocator` and `proven_arena_as_allocator` to return a zeroed `(proven_allocator_t){0}` when the source object is `NULL`. This prevents accidental crashes where `proven_alloc_is_valid()` would previously return true but the underlying `alloc_fn` would segfault on null context access.
    - Added explicit `NULL` checks to pool trait functions (`alloc_fn`, `free_fn`) to ensure robust behavior even if the interface is bypassed.
*   **Scanner Semantics Optimization (`src/proven/scan.c`):**
    - Refined `proven_scan_i64` to strictly forbid whitespace between the sign (`+/-`) and digits, aligning with standard parsing expectations (e.g., `-5` is valid, `- 5` is now rejected).
    - Implemented a rollback mechanism for `proven_scan_i64` that restores the scanner's cursor to its previous state upon failure, ensuring predictable error recovery during complex format parsing.
*   **`proven_scan_fmt` Validation (`src/proven/scan.c`):**
    - Hardened `proven_scan_fmt` to return `PROVEN_ERR_INVALID_ARG` if the number of placeholders in the format string does not match the number of arguments provided, maintaining parity with the `fmt` engine's strictly-validated argument policy.
*   **Platform Portability & Warnings (`platform/proven_sys_time.c`):**
    - Resolved strict-mode warnings related to signedness conversions and potential range loss in time calculation and integer formatting logic.
*   **Documentation Alignment:**
    - Updated `TEST.md` to clarify the `CRT/PAL-isolated` architectural strategy.
    - Updated the landing page (`index.html`) to reflect the completion of 22 TDD phases and refined technical language.

## Status: v26.05.03q

### Hardened Argument System & ABI Stability
*   **Argument System Hardening (`include/proven/fmt.h`, `include/proven/sysio.h`, `src/proven/sysio.c`):** 
    - Resolved a critical stack-integrity issue (detected via ASAN) where compound literal argument sets were potentially being improperly accessed or aligned across variadic boundaries.
    - **Constructors Refactored**: All `proven_arg_t` inline constructors (e.g., `proven_arg_i32`, `proven_arg_cstr`) now utilize explicit field-by-field initialization instead of compound literals to ensure precise stack-frame placement and alignment guarantees.
    - **Macro Hardening**: Upgraded `proven_print`, `proven_eprint`, and string formatting macros to use `const proven_arg_t[]` and the standard C23 `__VA_OPT__(,)` pattern, ensuring absolute robustness in optional argument handling.
    - **ABI Stability & Handle Isolation**: Refactored `proven_sysio_print_impl` and related scanning functions to pass the internal file handle via a generic `void*`. This eliminates potential struct-passing overhead or alignment mismatches during variadic expansion while preserving CRT isolation.
*   **Documentation:** Synchronized all project documentation (Spec, Manual, Landing Page) to reflect this stability improvement.

## Status: v26.05.02v

### Sanitizer Support Expansion
*   **Nob Build System (`nob.c`):** Officially introduced `ubsan` and `tsan` build modes.
    - `ubsan`: Enables UndefinedBehaviorSanitizer using `-fsanitize=undefined` (and `-fno-omit-frame-pointer` for GCC/Clang).
    - `tsan`: Enables ThreadSanitizer using `-fsanitize=thread`.
*   **Documentation:** Updated all manuals and specifications to reflect the availability of these new diagnostic modes.

## Status: v26.05.02u

### Formalized Three-Tier Append & Formatting Policy
*   **Three-Tier Structural Consolidation (`include/proven/u8str.h`, `include/proven/fmt.h`, `src/proven/u8str.c`, `src/proven/fmt.c`):** Officially codified and implemented three distinct modification patterns to eliminate "Partial-Write-on-Failure" ambiguity across the entire library.
    1.  **Atomic Fixed-Capacity (`append`, `append_fmt`):** Strictly fails with `PROVEN_ERR_OUT_OF_BOUNDS` and leaves the original string **untouched** if space is insufficient.
    2.  **Best-Effort / Truncating (`append_partial`, `append_fmt_trunc`):** Appends as much as geometrically possible while maintaining valid null-termination. Returns `PROVEN_ERR_OUT_OF_BOUNDS` if truncation occurred.
    3.  **Atomic Growable (`append_grow`, `append_fmt_grow`):** Requires an allocator; guarantees either a full successful write or zero modification on allocation failure (`PROVEN_ERR_NOMEM`).
*   **U16 Parity (`include/proven/u16str.h`, `src/proven/u16str.c`):** Mirrored identical policies for the `u16str` system, providing `append_partial` and `append_grow`.
*   **Verification:**
    - Created `test_phase22_fmt_best_effort.c` to rigorously validate these behaviors in the formatting engine.
    - Updated `test_phase7_u8str_mut.c` and `test_phase17_u16str.c` to verify atomic vs. partial append logic.

## Current Status: v26.05.02

### Dynamic Alias Header and Strict Type Adherence
*   **Alias Generator (`include/proven/alias_xcv.h`):** Created a dynamic generation script `gen_aliases.mjs` and the resulting `alias_xcv.h` file. Automatically scans the entire library for all identifiers prefixed with `proven_` and `PROVEN_`, exporting identical `xcv_` and `XCV_` equivalents logic for flexible namespace overriding via search-and-replace.
*   **Time Module Locale Constraint (`include/proven/time.h`):** Upgraded `proven_time_locale_t` struct to formally recognize `proven_u8str_view_t` directly for all string slices instead of raw `const char *` pointers.

## Status: v26.05.01m

### Encoding-Agnostic U8 / U16 Redefinition
*   **Agnostic Core (`include/proven/u8str.h`, `include/proven/u16str.h`):** Explicitly redefined string objects (`u8str`, `u16str`) to operate as strictly encoding-agnostic containers. 
    *   `u8str` handles operations mathematically in increments of unsigned 8-bit `u8` items.
    *   `u16str` handles operations in increments of unsigned 16-bit `u16` items, terminating via `(u16)0`.
    *   All prior assumptions dictating "UTF-8" or "UTF-16LE" have been stripped out from the architecture. Encoding validation and unicode translation logic is now strictly delegated to higher-level frameworks atop the core library.

## Status: v26.05.01l

### String & Format Best-Effort Failure Policy and PAL Optimization
*   **Best Effort on Failure (`src/proven/u8str.c`, `src/proven/u16str.c`, `src/proven/fmt.c`):** Pivoted from "Wipe on Failure" back to "Best Effort Partial Write" to maximize utility while maintaining raw speed and state determinism.
    *   If capacity overflow occurs during appending, mutation, or formatting operations, the string now truncates safely taking in as much data as geometrically possible before halting.
    *   This guarantees we utilize buffer space optimally without costly reallocation tracking.
*   **Platform Abstraction Layer (`platform/proven_sys_mem.h`, `platform/proven_sys_mem.c`):** Added direct wrappers for memory copying.
    *   Added `proven_sys_mem_copy` and `proven_sys_mem_move` mirroring `memcpy` and `memmove`.
    *   Refactored `u8str`, `u16str`, and internal PAL block moves to completely eliminate byte-sized linear `for` loops in favor of optimized PAL block transfers.

## Status: v26.05.01k

### Performance & Safety Convergence
*   **Direct-Write with Rollback (`src/proven/fmt.c`):** Refined the formatting engine to use a performance-first "Direct-Write" approach.
    *   Data is rendered directly into the target buffer without intermediate pre-calculation passes.
    *   **Atomic Rollback**: Introduced a state-rollback mechanism that restores the string's length and null-termination if a capacity overflow occurs, preventing partial data leaks while maximizing throughout.
*   **Documentation:** Updated the Developer Manual to formally define the Direct-Write and Rollback consistency models for string and formatting operations.

## Status: v26.05.01j

### String Atomic Growth Policy
*   **Atomic Append Protocol (`src/proven/u8str.c`, `src/proven/u16str.c`):** Pivoted from "Best Effort" to "Atomic Failure" policy. 
    *   If capacity is insufficient and cannot be grown (no allocator or allocation failure), the string remains **entirely unmodified**, and the function returns `PROVEN_ERR_OUT_OF_BOUNDS`.
    *   Eliminated partial write risks, ensuring deterministic state on failure.
*   **Documentation & Verification:**
    *   Updated `u8str.h`, `u16str.h`, and the Developer Manual to reflect this strict behavioral guarantee.
    *   Updated mutation and formatting tests to explicitly verify that original string data is preserved exactly as it was prior to a failed append.

## Status: v26.05.01i

### "Best Effort" Protocol Evolution
*   **Safety Guard (`src/proven/u8str.c`, `src/proven/u16str.c`):** Updated `append_view` functions for both UTF-8 and UTF-16 systems to gracefully detect invalid or missing objective allocators. They now automatically fallback to the strict Best Effort (no-growth) mode instead of attempting a null pointer call.
*   **Fixed-Capacity Formatting (`include/proven/fmt.h`):** Introduced the `proven_u8str_format` macro. This allows developers to format data directly into existing string buffers using the Best Effort protocol (partial write on overflow) without requiring an allocator.
*   **Documentation:** Revised the Developer Manual to codify the Best Effort behavior across all string and formatting operations.

## Status: v26.05.01h

### Unicode Extensions
*   **UTF-16LE String System (`include/proven/u16str.h`, `src/proven/u16str.c`):** Introduced a specialized string system for UTF-16 Little Endian encoding. 
    *   Implements `proven_u16str_t` and `proven_u16str_view_t`.
    *   Adheres to the "Best Effort" protocol: `append` perform partial writes on overflow, while `append_view` attempts reallocation.
*   **Documentation:** Updated the Developer Manual with physical struct layouts for the new U16 types.

## Status: v26.05.01g

### String Buffer Policies
*   **Best Effort Strategy (`src/proven/u8str.c`):** Refined string append logic to implement the "Best Effort" protocol. 
    *   `proven_u8str_append` (no-allocator) now writes as many bytes as will fit within capacity before returning `PROVEN_ERR_OUT_OF_BOUNDS`. 
    *   `proven_u8str_append_view` (allocator-aware) attempts reallocation first, but handles allocator failures gracefully by falling back to the partial write mechanism.
*   **Documentation & Verification:** Synchronized `u8str.h` and the Developer Manual to clearly define this behavioral guarantee. Updated `test_phase7_u8str_mut.c` to explicitly verify partial write data integrity.

## Status: v26.05.01f

### Core System Simplification
*   **Buffer Purity (`include/proven/buffer.h`):** Removed `flags` field from `proven_buf_t` to maintain structural simplicity and architectural honesty as requested.
*   **Explicit Fixed-Capacity Logic (`include/proven/u8str.h`):** Transitions "Fixed Mode" from a state-based flag to an explicit function-based approach. Removed `proven_u8str_create_fixed`.
*   **Append Protocols:** Documented and verified that `proven_u8str_append` inherently acts as the strict fixed-capacity variant (no growth), while `proven_u8str_append_view` serves as the growth-enabled variant. Updated tests accordingly.

## Status: v26.05.01e

### Documentation & Policy Updates
*   **Structure Transparency (`manual/PROVEN_MANUAL.md`):** Injected a comprehensive "Core Structure Definitions" appendix detailing the memory layout and field definitions for `proven_mem_t`, `proven_buf_t`, `proven_u8str_t`, and `proven_datetime_t`.
*   **Agent Guidelines (`AGENTS.md`):** Established the "Structure Transparency Policy," mandating synchronized documentation of physical struct layouts whenever core types are modified or introduced.
*   **Version Synchronization:** Updated version constants to `v26.05.01e` across the entire repository (Headers, Spec, Manual, Guidelines, and Web Interface).

## Status: v26.05.01d

### String System Updates
*   **Fixed Mode Implementation (`include/proven/buffer.h`, `src/proven/u8str.c`):** Introduced "Fixed Mode" for strings. Added `flags` field to `proven_buf_t` and implemented `proven_u8str_create_fixed`.
*   **Append Logic:** Updated `proven_u8str_append_view` to strictly respect the `PROVEN_BUF_FIXED` flag, preventing any reallocations and returning `PROVEN_ERR_OUT_OF_BOUNDS` when the capacity is exceeded.
*   **Documentation & Testing:** Updated `PROVEN_MANUAL.md` and `u8str.h` with documentation for Fixed Mode. Added comprehensive verification tests in `tests/test_phase7_u8str_mut.c`.

## Status: v26.05.01c
*   **Time Custom Formatter (`include/proven/time.h`, `src/proven/time.c`):** Built implementation of `proven_time_format()` providing custom `strftime`-style string mappings for `{year}`, `{Month}`, `{hour:0>2}` syntax layouts.
*   **Locale Systems (`proven/time.h`):** Created `proven_time_locale_t` managing zero-dependency dictionary strings for cross-language adaptations bypassing state-heavy `<locale.h>`. Defaulting to `proven_time_locale_en` statically.
*   **Documentation (`manual/PROVEN_MANUAL.md`):** Revised output syntax instructions displaying `{month:0>2}` padding usage for strftime functionalities locally. Extended test suits (`test_phase16_time_fmt.c`).

## Status: v26.05.01b

### Format System & Time Updates
*   **Time Module (`include/proven/time.h`, `src/proven/time.c`):** Integrated native `weekday` detection (0-6) representing Sunday through Saturday into `proven_datetime_t` alongside exposing public `proven_time_month_names` and `proven_time_weekday_names` array subsets globally replacing CRT requirements.
*   **Documentation (`manual/PROVEN_MANUAL.md`):** Extended documentation expanding detailed No-CRT formatter combination capacities showcasing exact index positioning combined with specification formatting strings. Injected Datetime output string array usage showcasing `April, Apr, Thu, Thursday` translation paths avoiding POSIX locale complexities.
*   **Testing (`tests/test_phase16_time_fmt.c`):** Built localized datetime arrays test checking string mapping output precision against UNIX Epoch references.

## Status: v26.05.01a

### Versioning Rules
*   **Documentation (`AGENTS.md`):** Updated versioning rules so minor iterations within the same day append an alphabetical suffix like Excel columns (e.g., `v26.05.01a`, `v26.05.01b`, `v26.05.01z`, `v26.05.01aa`). Updated project documentation files to reflect version `v26.05.01a`.

## Status: v26.05.01

### Testing Framework
*   **Testing Core (`tests/proven_test_fw.h`):** Created a unit test framework tracking pass/fails to assert expectations without relying on implicit exits. Designed utilizing C macros evaluating directly within `main()`.
*   **Foundation Testing (`tests/test_foundation.c`):** Integrated `proven_is_ok`, `PROVEN_IS_OK`, the `PROVEN_CKD_ADD/SUB/MUL` arithmetic macros, and `Result` struct handling directly against the new test framework for Phase 1 robustness.
*   **Build Pipeline (`nob.c`):** Injected the `tests/test_foundation.c` pathway inside the execution cycle for autonomous build validation.

## Status: v26.04.25

### Improvements & Robustness Audit (Latest)
*   **Build System:** Transitioned from `.flags` file tracking to mode-specific build directories (`build/debug`, `build/release`, `build/asan`) to isolate object files and prevent compiler flag contamination. Re-added `asan` configuration target.
*   **Platform/FS:** Updated `proven_sys_fs_read/write` to return `proven_sys_result_size_t`, enabling precise error propagation.
*   **Platform/Mem:** Fixed `proven_sys_mem_alloc` to enforce `alignof(max_align_t)` as the minimum alignment floor and added overflow guards for padding calculations.
*   **Arena Allocator:** Enforced power-of-two alignment checks and hardened `arena_realloc_aligned` against capacity-related edge cases.
*   **Hash Map:** Resolved `next_pow2` overflow vulnerabilities; added alignment validation on creation; and hardened `rehash` logic to handle bucket migration failures.
*   **Formatting (FMT):** Implemented bounds checking for format arguments, returning `PROVEN_ERR_INVALID_ARG` if an index is out of bounds.
*   **UTF-8 Strings:** Hardened `cstr_len` against pointer arithmetic pitfalls and added `PROVEN_DEFAULT_ALIGNMENT` (8) to `align.h` for consistent memory behavior.
*   **Job System:** Added validation to prevent initialization with zero worker threads.
*   **Documentation:** Softened tone in `README.md` to reflect a more professional engineering philosophy. Updated version references across `SPEC.md`, `AGENTS.md`, and `index.html`.

### System Capabilities Evolution & Fixes
*   **File System (`fs.h`, `fs.c`):** Resolved the hardcoded 512-byte path limit restriction. Remodeled the entire high-level FS API (`proven_fs_open`, `proven_fs_remove`, `proven_fs_mkdir`, etc.) to accept `proven_allocator_t scratch`, implementing the planned allocator-based path conversion mechanism for dynamically handling arbitrarily long file paths.
*   **Testing:** Updated all phases involving filesystem APIs (`test_phase13_fs.c`, `test_phase14_fs_advanced.c`, `test_phase15_fs_security.c`, `test_phase17_mmap.c`) to correctly supply the local heap allocator to satisfy the new API requirements.

---

The project has reached version `v26.04.25`.

### Core Architecture & Implementation

*   **Platform Abstraction Phase (`/platform/`):**
    *   Implemented `proven_sys_env` for environment abstraction.
    *   Implemented `proven_sys_fs` and `proven_sys_io` for cross-platform file system and input/output operations.
    *   Implemented `proven_sys_mem` for core OS memory allocation isolation.
    *   Implemented `proven_sys_thread` for multithreading primitives.
    *   Implemented `proven_sys_time` for exact OS time querying.

*   **Memory Management (`/include/proven/allocator.h`):**
    *   Established polymorphic `proven_allocator_t` trait.
    *   Integrated diverse memory strategies: Arena (`arena.c`), Heap (`heap.c`), and Pool (`pool.c`) allocators.

*   **Data Structures (`/src/proven/`):**
    *   **Array (`array.c`):** Dynamic contiguous storage.
    *   **List (`list.c`):** Linked list data structures.
    *   **Map (`map.c`):** Generic associative array functionality mapping keys to values.
    *   **Ring (`ring.c`):** Ring buffer implementations.

*   **Utilities & Services:**
    *   **Time & Formatting:** Developed `time.c` and `fmt.c` for standardized runtime and string serialization capabilities.
    *   **String Manipulation:** Developed UTF-8 safe string processing logic in `u8str.c`.
    *   **I/O & File System (`fs.c`, `sysio.c`):** Wrapped low-level OS file descriptors with safety bounds.
    *   **Scanning & Algorithms (`scan.c`, `algorithm.c`):** Common sorting, searching, and boundary scanning features.
    *   **Advanced Control Flow (`coro.c`, `job.c`):** Support for advanced asynchronous logic, coroutines, and automated job systems.
    *   **Memory mapping (`mmap.c`):** Managed OS memory mapping logic.

*   **Test Driven Development (TDD):**
    *   21 comprehensive testing phases mapped and executing via `./nob` build system inside `/tests/`.
    *   Phase tests progress from memory subsystems to complex map layouts, filesystem, and coroutine testing.

---

### [v26.04.24] - Refinements based on ChatGPT Review
*   **Build Pipeline:**
    *   Fixed `nob.c` to use `Nob_File_Paths` instead of undefined `Nob_String_Array` to resolve critical compilation failure.
*   **Memory Management (`align.h`, `arena.c`):**
    *   Added validation for power-of-two in `proven_mem_align_up` to prevent potential underflow/overflow bounds calculations.
    *   Corrected shrink calculation flaw internally in `proven_arena_realloc_aligned` removing unstable `diff` mathematical logic.
*   **Allocator Validation:**
    *   Implemented `proven_alloc_is_valid` constraint ensuring all required function pointers (`alloc_fn`, `realloc_fn`, `free_fn`) are non-null across structures like `array.c`.
*   **Array (`array.h`, `array.c`):**
    *   Replaced cryptic `.internal` member with distinct `.data` memory pointer and robust `.cap` explicitly preserving limits preventing capacity overflow risks.
    *   Split generic `proven_array_get` into const-correct `proven_array_get_mut` and `proven_array_get` enforcing memory safety via immutability.
    *   Ensured full zero-initialization on `destroy` resolving dangling pointer possibilities.
*   **Open-Addressing Hash Map (`map.h`, `map.c`):**
    *   Resolved string key ownership ambiguity by renaming `PROVEN_MAP_INIT_U8` to `PROVEN_MAP_INIT_U8_BORROWED` to clarify memory behavior visually.
    *   Fixed fatal tombstone degradation issues tracking exact `.used` counters combining both occupied elements and tombstones preventing loop failures.
    *   Split `get` into `get_mut` and const-correct `get`.
    *   Resolved 32-bit architectural portability issue modifying `next_pow2` preventing `UINTPTR_MAX` overflow.
    *   Refactored `map_rehash` directly isolating migration in isolated instances rather than risky in-place operations preventing nested partial allocation panic states.
*   **File System (`fs.c`, `sys_fs.c`, `sys_io.c`):**
    *   Fixed cross-platform casting issue casting handle `fd` variables into `void*`. Values are now sequentially incremented by 1 on instantiation mapped securely against `NULL` comparisons on zero-descriptors (stdin/stdout).
    *   Implemented true file partial iteration fixing loop constraints inside raw binary blob reading (`proven_fs_read_all`).
*   **Error Management (`error.h`):**
    *   Added core library expanded unified error descriptors capturing complex error hierarchies (`PROVEN_ERR_INVALID_STATE`, `PROVEN_ERR_OVERFLOW`, `PROVEN_ERR_UNSUPPORTED`, `PROVEN_ERR_AGAIN`, `PROVEN_ERR_EOF`, `PROVEN_ERR_BUSY`, `PROVEN_ERR_PERMISSION`).
*   **Algorithm & Tests (`algorithm.c`, `test_phase14_fs_advanced.c`, `test_phase8_array.c`, `test_phase11_map.c`):**
    *   Updated `proven_array_t` accessor references from using the legacy `internal.ptr` structure directly mapped now securely to `.data`.
    *   Updated `test_phase8_array.c` arrays properties reference variables to strictly match `.data` and `.cap` API boundaries mirroring identical const qualifiers checking explicitly.
    *   Updated `test_phase11_map.c` string retrieval variable explicitly specifying `const` pointers resolving warnings.
*   **Build Pipeline & Guidelines (`nob.c`, `AGENTS.md`):**
    *   Removed improper generation of `include/proven/nob.h` artifact inside `nob.c`.
    *   Added explicit constraint in `AGENTS.md` mandating that both `nob.h` and `nob.c` must permanently remain at the root of the repository.
    *   Eradicated underlying Singleton architectures replacing globals directly with context-encapsulated `proven_job_sys_t` strictly matching `Zero Global State` architectural rules.
