# Proven C library

Version: proven_c_lib-v26.05.16  
Standard: C23  
License: MIT  
Repository: https://github.com/rubidus-api/proven_c_lib

`proven` is a small C23 foundation library for systems programs that should stay readable after the first month. It gives you strings, arrays, maps, formatting, scanning, filesystem helpers, memory mapping, time, stackless coroutines, and a bounded job system, while keeping the important choices visible: which allocator is used, who owns memory, which errors can occur, and where the program crosses into the operating system.

The goal is not to make C look like another language. The goal is to make practical C less repetitive without hiding the parts that matter.

## why it exists

A lot of C projects grow the same private support code: a string type, a vector, a map, an arena, a few wrappers around files and time, a formatter that does not drag `printf` everywhere, and a test runner nobody wants to maintain.

`proven` packages those pieces as one small layer with a few rules:

- Ownership is explicit. If an object owns memory, the allocator used to create it is part of the story.
- Errors are values. Fallible functions return `proven_err_t` or `proven_result_*_t`.
- Reallocation-style operations are failure-atomic where documented. If growth fails, the old object remains valid.
- Borrowed views are named as views. They do not pretend to own bytes.
- Raw memory uses byte views instead of type-punning through unrelated pointers.
- Hosted OS access is isolated behind the PAL layer in `platform/`.
- The build is one C file: `nob.c`. No mandatory CMake, Make, Meson, npm, or external test framework.
- Freestanding builds can use the reduced core without pulling in hosted filesystem, console, thread, mmap, or environment services.

That makes the library useful for ordinary command line tools, embedded-adjacent code, experiments that may later need a stricter platform boundary, and C projects that want enough infrastructure to move quickly without losing control.

## quick start

```sh
cc nob.c -o nob
./nob build
```

Common checks:

```sh
./nob strict-error
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob freestanding
./nob cross -build-root /mnt/ai-share/build/proven_c_lib
```

Use another compiler:

```sh
./nob strict-error -cc clang
```

Running `./nob` without arguments prints the full command list.

## first program

This example uses the heap allocator, creates an owned UTF-8 byte string, grows it through the formatter, prints it, and then destroys it with the same allocator family.

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

Build it against the hosted library sources:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## containers without hidden ownership

`proven_array_t` is a growable contiguous array. It stores the allocator trait used for growth and destruction, so the call site stays honest about allocation.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
    if (!proven_is_ok(r.err)) return 1;

    proven_array_t numbers = r.value;

    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 10))) goto fail;
    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 20))) goto fail;

    const int *first = PROVEN_ARRAY_GET(&numbers, int, 0);
    if (!first) goto fail;

    proven_println("first = {}", PROVEN_ARG(*first));
    PROVEN_ARRAY_DESTROY(&numbers);
    return 0;

fail:
    PROVEN_ARRAY_DESTROY(&numbers);
    return 1;
}
```

The same pattern appears across strings, maps, buffers, arenas, pools, and filesystem helpers: create with an allocator, check the result, keep borrowed pointers short-lived, destroy owned objects deliberately.

## text formatting with bounded input

`PROVEN_ARG("literal")` is convenient for trusted NUL-terminated strings. For text from outside your program, prefer bounded views or bounded C-string arguments so formatting does not search past the bytes you meant to expose.

```c
const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
```

The formatter has three useful modes for owned strings:

- `proven_u8str_append_fmt`: fixed-capacity and atomic.
- `proven_u8str_append_fmt_trunc`: fixed-capacity and truncating.
- `proven_u8str_append_fmt_grow`: allocator-backed and atomic.

## platform boundary

Portable implementation files live in `src/proven/`. OS and C runtime calls are isolated under `platform/`:

- heap allocation
- filesystem operations
- time
- environment access
- console I/O
- threads
- memory mapping
- math helpers where needed

This split keeps the core library easier to audit and gives ports a clear place to work. Hosted Linux is the primary runtime target today. The build also has compile-only checks for optional targets when the toolchains are installed: Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows x86_64/i686, ARM Cortex-M freestanding, and RISC-V ELF freestanding.

Cross compilation proves that headers, source visibility, ABI assumptions, and compile-time platform branches line up. It does not replace runtime tests on the target machine.

## main modules

- Foundation: `types`, `error`, `memory`, `align`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Hosted services: `fs`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## what it is not

`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.

## documentation

- User manual: `manual/manual.md`
- Freestanding guide: `manual/manual-freestanding.md`
- Specification: `SPEC.md`
- Test matrix: `TEST.md`
- Architecture and requirements: `docs/`

## status

The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. Sanitizer, regression, freestanding, and cross compile checks are driven by `nob.c`. Optional cross targets are checked when the corresponding toolchains are present.

## author and license

Developed by rubidus-api.  
Email: rubidus@gmail.com  
License: MIT License. See `LICENSE`.
