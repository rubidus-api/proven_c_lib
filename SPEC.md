# proven C Library Specification (v26.05.16)

## 1. Scope

`proven` is a C23 systems library for explicit memory management, checked data movement, PAL-isolated system access, and compact infrastructure types. It is designed around pointer provenance, strict aliasing awareness, failure-atomic memory operations, and explicit error propagation.

The library is not a general replacement for libc. It provides a focused set of primitives for projects that want small, predictable C components with clear ownership and platform boundaries.

## 2. Design principles

1. C23 first: build with `-std=c23`.
2. Explicit ownership: callers pass allocators and destroy owned objects.
3. Explicit errors: fallible APIs return `proven_err_t` or `proven_result_*_t`.
4. Provenance-aware memory: raw object access uses `proven_byte_t` and memory views.
5. PAL isolation: OS and C runtime calls are centralized under `platform/`.
6. Failure atomicity: realloc-style operations preserve the old value on failure.
7. No hidden global state in core containers.
8. No external build system: `nob.c` is the build driver.
9. No external test framework: tests are plain C executables.
10. Optional ergonomics: `alias_xcv.h` provides short-form aliases.

## 3. Dependency layers

The intended dependency order is:

1. Foundation: `types`, `error`, `align`, `memory`.
2. Allocation: `allocator`, `heap`, `arena`, `pool`.
3. Buffers and strings: `buffer`, `u8str`, `u16str`.
4. Collections: `array`, `list`, `ring`, `map`, `algorithm`.
5. PAL-backed services: `fs`, `time`, `mmap`, `sysio`.
6. Execution helpers: `coro`, `job`.
7. Text utilities: `fmt`, `scan`.
8. Optional alias layer: `alias_xcv`.

Lower layers must not depend on higher layers.

## 4. Public headers

- `proven.h`: umbrella include.
- `types.h`: fixed-width aliases, size types, checked arithmetic, error enum.
- `error.h`: error predicates.
- `memory.h`: immutable and mutable memory views.
- `align.h`: alignment helpers.
- `allocator.h`: allocator trait.
- `heap.h`: PAL-backed heap allocator.
- `arena.h`: bump allocator.
- `pool.h`: fixed-size recycler.
- `buffer.h`: byte buffer operations.
- `u8str.h`: UTF-8-oriented byte string container and views.
- `u16str.h`: `char16_t` string container and views.
- `array.h`: generic growable array macros and functions.
- `list.h`: intrusive doubly-linked list.
- `ring.h`: bounded ring buffer.
- `map.h`: open-addressing hash map.
- `algorithm.h`: sorting and search helpers.
- `fs.h`: filesystem API.
- `time.h`: time API.
- `fmt.h`: formatter API.
- `mmap.h`: memory mapping API.
- `sysio.h`: console and environment I/O helpers.
- `job.h`: bounded job system.
- `scan.h`: structural scanner.
- `coro.h`: stackless coroutine macros.
- `panic.h`: panic and invariant helpers.
- `version.h`: version macros.
- `alias_xcv.h`: optional `xcv_` alias layer.

## 5. Implementation units

The current hosted build compiles these implementation files:

- `src/proven/memory.c`
- `src/proven/arena.c`
- `src/proven/pool.c`
- `src/proven/buffer.c`
- `src/proven/heap.c`
- `src/proven/u8str.c`
- `src/proven/u16str.c`
- `src/proven/array.c`
- `src/proven/ring.c`
- `src/proven/map.c`
- `src/proven/algorithm.c`
- `src/proven/fs.c`
- `src/proven/time.c`
- `src/proven/fmt.c`
- `src/proven/mmap.c`
- `src/proven/sysio.c`
- `src/proven/job.c`
- `src/proven/scan.c`
- `src/proven/panic.c`

PAL units:

- `platform/proven_sys_mem.[ch]`
- `platform/proven_sys_fs.[ch]`
- `platform/proven_sys_time.[ch]`
- `platform/proven_sys_env.[ch]`
- `platform/proven_sys_thread.[ch]`
- `platform/proven_sys_io.[ch]`
- `platform/proven_sys_math.[ch]`

## 6. Type model

- Fixed-width aliases use project names: `proven_i8`, `proven_u8`, `proven_i16`, `proven_u16`, `proven_i32`, `proven_u32`, `proven_i64`, `proven_u64`.
- `proven_u16` is based on `char16_t`.
- `proven_byte_t` is `unsigned char` and is the byte-level object representation type.
- `proven_size_t` maps to `size_t`.
- `proven_ptrdiff_t` maps to `ptrdiff_t`.
- `proven_intptr_t` and `proven_uintptr_t` are available for explicit pointer-integer use.
- `uintptr_t` support is required for the current arena range model.

## 7. Error and result model

`proven_err_t` includes success, allocation failure, bounds failure, invalid encoding, invalid argument, I/O failure, not found, invalid state, overflow, unsupported operation, retry, EOF, busy, permission failure, and invalid format.

Fallible value-returning APIs use result structs. A result contains an error code and a value field. The value is only meaningful when the error is `PROVEN_OK`.

Control flow must stay visible. Use explicit `if (!proven_is_ok(err))` checks.

## 8. Memory and allocation model

- `proven_mem_view_t` is a non-owning read-only byte view.
- `proven_mem_mut_t` is a non-owning mutable byte view.
- `proven_allocator_t` contains context plus `alloc`, `realloc`, and `free` function pointers.
- Allocator `realloc` must leave the old allocation valid on failure.
- The heap allocator routes through PAL memory functions.
- The arena allocator is linear and does not reclaim individual allocations.
- The pool allocator recycles fixed-size blocks.

## 9. Container model

- Arrays are generic growable vectors with allocator-backed storage.
- Lists are intrusive and do not allocate nodes.
- Rings are bounded circular buffers.
- Maps use open addressing and tombstones.
- Algorithms operate on explicit views and element sizes.
- Public structs expose layout for C usability, but callers must preserve documented invariants.

## 10. String and formatting model

- `PROVEN_LIT` creates compile-time string views without runtime length scanning.
- U8 and U16 strings distinguish borrowed views from owned containers.
- String and formatting APIs follow three behavior classes where applicable:
  - fixed-capacity atomic operations,
  - best-effort truncating operations,
  - growable atomic operations.
- Formatting and scanning avoid direct `stdio` dependency in core logic.
- Long and unsigned long scan destinations are handled directly through dedicated scan argument wrappers.

## 11. Filesystem, mmap, and system I/O

Hosted system services go through PAL-backed APIs. The public layer exposes file operations, directory operations, metadata, memory mapping, console output, and environment access. Zero-length read or write operations should succeed with zero bytes processed without requiring non-null buffers. Append opens should preserve platform append semantics where available, and environment access must not impose a small fixed key-size limit.

## 12. Concurrency model

- Core containers do not add hidden locks.
- Shared mutable containers require caller synchronization.
- Coroutine support is stackless and macro-based.
- The job system uses bounded queueing and atomic coordination for its scheduler internals.
- Job-system destruction must not race with producers.

## 13. Freestanding model

`PROVEN_FREESTANDING` builds a reduced subset with OS-backed services removed. Freestanding mode also disables float formatting and U16 string support in the current build configuration through `PROVEN_FMT_NO_FLOAT` and `PROVEN_NO_U16STR`.

Freestanding checks are build-time compatibility checks, not a claim that every hosted module is available without an OS.

## 14. Build specification

The build driver is compiled and invoked as follows:

```sh
cc nob.c -o nob
./nob build
```

Supported commands include `build`, `debug`, `release`, `strict`, `strict-error`, `asan`, `ubsan`, `tsan`, `freestanding`, `cross`, `regression`, `regression-asan`, `regression-ubsan`, `clean`, and `help`.

`-build-root <dir>` redirects generated output. Use `/mnt/ai-share/build/proven_c_lib` on shared build servers.

## 15. Verification specification

The hosted full run currently includes 32 executable tests. The freestanding run includes 5 freestanding tests. Regression commands run the focused regression subset. `./nob cross` performs compile-only target checks and skips missing optional compilers.

Required validation before release:

```sh
./nob clean
./nob strict-error
./nob regression-asan
./nob regression-ubsan
./nob freestanding
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Use Clang validation when available:

```sh
./nob strict-error -cc clang
```

## 16. Supported targets

Primary target:

- Linux x86_64 with GCC or Clang and C23 support.

Experimental or partial targets:

- Linux AArch64.
- Linux ARM hard-float.
- Linux i686 via either `i686-linux-gnu-gcc` or `gcc -m32` multilib.
- Windows through WinAPI PAL paths, normally checked with MinGW cross compilers.
- macOS through POSIX-like PAL paths.
- MSVC when required C23 features and checked arithmetic support are available.
- Freestanding C environments for the reduced subset, including ARM Cortex-M and RISC-V ELF compile-only checks when toolchains are installed.

## 17. Non-goals

- No C++ API layer.
- No mandatory dependency on CMake, Make, Meson, or npm.
- No hidden garbage collection.
- No implicit process-wide allocator.
- No promise that internal public-struct fields may be mutated arbitrarily by callers.
- No Wine-based validation requirement for Windows behavior.
