# proven Agent Guide (v26.05.16)

This file defines the working rules for humans and AI agents editing the `proven` C library.

## Project facts

- Language: ISO C23.
- License: MIT.
- Repository directory: `/mnt/ai-share/proven_c_lib` on Hermes, `/home/hermes/ai-share/proven_c_lib` on arch-dev.
- Shared build root: `/mnt/ai-share/build/proven_c_lib` on Hermes, `/home/hermes/ai-share/build/proven_c_lib` on arch-dev.
- Build system: root-level `nob.c` with `nob.h`.
- Public API: `include/proven/*.h` and umbrella header `include/proven.h`.
- Implementation: `src/proven/*.c`.
- Platform boundary: `platform/proven_sys_*.[ch]`.
- Tests: plain C files in `tests/`, built and run by `nob.c`.
- Current version string: `proven_c_lib-v26.05.16`.

## Directory map

- `include/proven.h`: umbrella public include.
- `include/proven/`: public headers for types, errors, memory, allocators, containers, I/O, scanning, formatting, jobs, coroutines, and aliases.
- `src/proven/`: portable implementation files. Do not include OS headers here.
- `platform/`: PAL implementations for heap, filesystem, time, environment, threads, console I/O, and math.
- `tests/`: self-contained executable tests and compile checks.
- `manual/`: user and freestanding manuals.
- `docs/`: architecture, requirements, security, testing, decisions, and AI workflow notes.
- `docs-site/`: optional documentation site. It is not needed to build the C library.
- `build/`: generated output. Do not edit by hand.

## Required commands

Compile the build driver first:

```sh
cc nob.c -o nob
```

Use explicit commands. Running `./nob` without arguments prints help.

```sh
./nob build              # debug build and full hosted test run
./nob release            # optimized build and full hosted test run
./nob strict             # warnings enabled
./nob strict-error       # warnings as errors
./nob asan               # AddressSanitizer run
./nob ubsan              # UndefinedBehaviorSanitizer run
./nob tsan               # ThreadSanitizer run
./nob regression         # regression-only debug run
./nob regression-asan    # regression-only ASan run
./nob regression-ubsan   # regression-only UBSan run
./nob freestanding       # freestanding subset compile/run checks
./nob cross              # compile-only cross-target matrix
./nob clean              # remove build output
```

Useful options:

```sh
./nob build -cc clang
./nob strict-error -cc clang
./nob build -cflags "-DNAME=value"
./nob build -ldflags "-lm"
./nob build -build-root /mnt/ai-share/build/proven_c_lib
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
./nob build -f
```

Cross-platform policy:

- Use `./nob cross` for compile-only coverage of optional toolchains.
- The cross matrix skips missing compilers and fails on real compile errors.
- Current target names cover native GCC/Clang, Linux AArch64, Linux ARM hard-float, Linux i686 via either `i686-linux-gnu-gcc` or `gcc -m32`, WinAPI x86_64/i686 via MinGW, ARM Cortex-M freestanding, and RISC-V ELF freestanding.
- Runtime tests still require a matching runner or host. Cross compilation is not a replacement for runtime validation.
- Keep build outputs outside source trees when using shared servers: `-build-root /mnt/ai-share/build/proven_c_lib`.

## Documentation rules

- Write all documentation and comments in English.
- Keep wording short, technical, and verifiable.
- Do not use emojis or pictograms.
- Do not overpromise. Prefer `designed to`, `tested for`, `currently supports`, and `PAL-isolated`.
- Update `README.md`, `SPEC.md`, `TEST.md`, and the relevant manual when behavior changes.
- If a public structure layout changes, update `manual/manual.md`.
- If the version changes, keep `include/proven/version.h`, root docs, manuals, and `docs-site/index.html` consistent.

## C rules

- Target C23. Use `-std=c23` through `nob.c`.
- Keep public typedef names ending in `_t`, except short fixed-width aliases such as `proven_u8` and `proven_i32`.
- Use lower snake case for functions and variables.
- Use upper snake case for macros.
- Use `proven_size_t` for sizes and indexes.
- Use `proven_ptrdiff_t` for pointer differences and signed offsets.
- Use `proven_byte_t` for byte-level object representation access.
- Avoid VLAs in library code.
- Guard size and offset arithmetic with `PROVEN_CKD_ADD`, `PROVEN_CKD_SUB`, or `PROVEN_CKD_MUL` when overflow is possible.
- Treat direct mutation of public struct internals as caller misuse unless the header explicitly allows it.

## Provenance and aliasing rules

- Do not cast object pointers through unrelated types to read or write stored values.
- Use byte views for raw memory inspection and copying.
- Prefer non-overlap operations unless the API is explicitly documented to handle overlap.
- Keep borrowed views and borrowed keys valid for the full documented lifetime.
- Do not keep views into growable containers across calls that can reallocate.
- Preserve failure-atomic behavior for reallocation-style APIs: on failure, the old object remains valid.

## Error handling rules

- Functions that can fail return `proven_err_t` or a `proven_result_*_t` value.
- Do not use hidden control-flow macros for error propagation.
- Check errors explicitly:

```c
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, size, align);
if (!proven_is_ok(r.err)) return r.err;
```

- Use `goto cleanup_*` for non-memory system resources.
- Do not use non-standard `defer` extensions.
- Memory ownership must be clear at the call site.

## Allocator rules

- Core systems depend on `proven_allocator_t`, not on a concrete allocator.
- Arena, pool, and heap allocators are implementations behind the allocator trait.
- Heap access must route through PAL code.
- Arena frees are no-ops by design.
- Temporary buffers used during persistent arena mutations should use explicit scratch allocation when available.

## PAL boundary rules

- `src/proven/` must not include OS headers directly.
- Isolate libc, POSIX, Windows, thread, filesystem, time, console, environment, and heap calls under `platform/`.
- Keep hosted-only modules out of the freestanding subset.
- `PROVEN_FREESTANDING` disables OS-backed services and builds the reduced core.

## Test-first workflow

- Add or update tests before changing behavior.
- Put unit and regression tests in `tests/`.
- Keep tests plain C. Do not add an external test framework.
- Run the narrow test first, then at least `./nob build`.
- For memory-sensitive changes, run `./nob asan` and `./nob ubsan`.
- For job or atomic changes, run `./nob tsan` when supported.
- For public header or configuration changes, run `./nob freestanding` when relevant.

## Git workflow

- This project is a Git repository. Keep source history in the project-local `.git/` directory.
- Commit every completed source, test, build-script, or documentation change after verification.
- Use short imperative commit subjects that describe the user-visible change.
- Do not commit generated build output, the local `nob` executable, logs, temporary test files, or environment files.
- If a GitHub remote is configured, treat local commits as the source of truth until they are explicitly pushed.

## Public API synchronization

- When adding, renaming, or removing a public symbol, update:
  - the public header,
  - the implementation,
  - tests,
  - `include/proven/alias_xcv.h` when the symbol belongs in the alias layer,
  - `tests/test_alias_smoke.c`,
  - README or manual text if user-visible.

## Release checklist

- Update `include/proven/version.h`.
- Synchronize version strings in root docs, manuals, changelog, and docs site.
- Run `./nob clean` followed by `./nob strict-error`.
- Run sanitizer or regression commands appropriate to the change.
- Check that generated `build/` files are not treated as source.
- Keep the root `nob.c` and `nob.h` in place.

## Web footer rule

`docs-site/index.html` must keep this footer information visible:

- Creator: `rubidus-api`
- Email: `rubidus@gmail.com`
- GitHub: `https://github.com/rubidus-api/proven_c_lib/`
- License: MIT License
