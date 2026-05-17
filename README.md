# Proven C Library

Version: proven_c_lib-v26.05.16  
Standard: C23  
License: MIT  
Repository: https://github.com/rubidus-api/proven_c_lib

`proven` is a small C23 systems library for projects that want explicit ownership, explicit errors, and predictable platform boundaries. It provides allocator-based memory tools, provenance-aware byte views, growable containers, strings, formatting, scanning, filesystem and console helpers, memory mapping, stackless coroutines, and a bounded job system.

The library is not a libc replacement. It is a compact foundation layer for C programs that need practical infrastructure without hiding allocation, error flow, or OS access behind global state.

## Why use it

- C23-first source with checked arithmetic through `PROVEN_CKD_*`.
- Explicit `proven_err_t` and `proven_result_*_t` return values.
- A single allocator trait used by arrays, maps, strings, buffers, arenas, pools, and filesystem helpers.
- Failure-atomic grow/realloc-style operations where old objects stay valid on allocation failure.
- PAL-isolated hosted services for heap, filesystem, time, environment, console I/O, threads, and mmap.
- Borrowed views are represented explicitly, so ownership is visible at call sites.
- Plain C tests and a single `nob.c` build driver. No mandatory CMake, Make, Meson, or test framework.
- Compile-only checks for Linux cross targets, MinGW/WinAPI, ARM Cortex-M, and RISC-V ELF when toolchains are installed.
- Reduced freestanding configuration for OS-free or libc-minimal targets.

## Quick start

```sh
cc nob.c -o nob
./nob build
```

Running `./nob` without arguments prints help. Common commands:

```sh
./nob build
./nob release
./nob strict-error
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob freestanding
./nob cross -build-root /mnt/ai-share/build/proven
```

Use another compiler:

```sh
./nob strict-error -cc clang
```

## Minimal example

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r =
        proven_u8str_create_from_view(alloc, PROVEN_LIT("hello"));
    if (!proven_is_ok(r.err)) return 1;

    proven_u8str_t s = r.value;
    proven_fmt_result_t fr =
        proven_u8str_append_fmt_grow(alloc, &s, ", {}", PROVEN_ARG("world"));
    if (!proven_is_ok(fr.err)) {
        proven_u8str_destroy(alloc, &s);
        return 1;
    }

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    proven_u8str_destroy(alloc, &s);
    return 0;
}
```

For a simple application build, either adapt the source list from `nob.c` or compile the hosted sources directly:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## Main modules

- Foundation: `types`, `error`, `memory`, `align`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Hosted services: `fs`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional aliases: `alias_xcv`.

## Core contracts

- Check every fallible return before using its value.
- Destroy owned objects with the matching allocator or the container-specific destroy function.
- Do not mutate public struct internals unless a header explicitly says it is allowed.
- Do not keep pointers or views into growable objects across calls that may reallocate.
- `PROVEN_ARG_CSTR` requires a trusted, live, NUL-terminated pointer. Prefer bounded views for untrusted text.
- `PROVEN_KEY_TYPE_U8_BORROWED` map keys are borrowed. The key bytes must outlive the map entry.
- Cross compilation checks headers, source visibility, and target ABI assumptions. It does not replace runtime tests on the target.

## Documentation

- User manual: `manual/manual.md`
- Freestanding guide: `manual/FREESTANDING.md`
- Specification: `SPEC.md`
- Test matrix: `TEST.md`
- Architecture and requirements: `docs/`

## Status

The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. The build system also has compile-only coverage for Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows targets, ARM Cortex-M freestanding, and RISC-V ELF freestanding when the corresponding toolchains are present.

## Author and license

Developed by rubidus-api.  
Email: rubidus@gmail.com  
License: MIT License. See `LICENSE`.
