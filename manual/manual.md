# Proven C Library Complete Manual (v26.07.20b)

This manual is rebuilt from three sources:

1. The current public headers in `include/proven/`.
2. The current implementation and tests in `src/proven/`, `platform/`, and `tests/`.
3. The older developer manual kept in a private workspace outside the repository.

A Korean translation of this manual is in [`manual-ko/`](../manual-ko/manual-ko.md).

The goal is to keep the architectural explanation from the older manual while adding practical examples, return-value rules, ownership rules, and common misuse cases from the current source tree.

## Table of contents

1. [Intent and design philosophy](#1-intent-and-design-philosophy)
2. [Build and include model](#2-build-and-include-model)
3. [Global contracts](#3-global-contracts)
4. [Ownership and destruction matrix](#4-ownership-and-destruction-matrix)
5. [Operation behavior classes](#5-operation-behavior-classes)
6. [Manual chapters](#6-manual-chapters)
7. [Public header map](#7-public-header-map)
8. [Platform support and verification](#8-platform-support-and-verification)

## 1. Intent and design philosophy

`proven` is a compact C23 systems foundation library. It is intended for C programs that want practical infrastructure without hiding memory ownership, error control flow, or platform access behind global state.

It is not a libc replacement. It provides a focused set of allocator-driven memory tools, byte views, containers, strings, formatting, scanning, hashing (FNV, SipHash, CRC-32, SHA-256), hex/Base64 encoding, OS-strength randomness, filesystem helpers, buffered streams, time helpers, memory mapping, stackless coroutine macros, and a bounded job system.

Core design principles:

- C23 first: the build driver uses `-std=c23`.
- Explicit errors: fallible functions return `proven_err_t` or `proven_result_*_t`.
- Explicit ownership: owned objects have clear destroy functions and allocator rules.
- Failure atomicity: grow/realloc-style APIs preserve the old object on allocation failure unless documented otherwise.
- Pointer provenance discipline: raw object access uses `proven_byte_t` and bounded views.
- PAL isolation: hosted OS services live under `platform/` and are called through public wrappers.
- Core containers do not add hidden locks. Shared mutation requires caller synchronization.
- The build system is a single checked-in `nob.c`; tests are plain C executables.

## 2. Build and include model

Build and run the hosted test suite:

```sh
cc nob.c -o nob
./nob build
```

Common validation commands:

```sh
./nob release
./nob strict
./nob strict-error
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob regression-asan
./nob regression-ubsan
./nob freestanding
./nob cross -build-root /home/user/work/build/proven_c_lib
```

Use the umbrella header when you want the full hosted API:

```c
#include "proven.h"
```

Use smaller includes when you want a narrower translation unit:

```c
#include "proven/heap.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
```

A direct hosted application build can follow the same source layout as `nob.c`:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## 3. Global contracts

### 3.1 Result contract

A result value is usable only when `err == PROVEN_OK`.

Correct:

```c
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
if (proven_is_ok(r.err)) {
    /* only now does r.value mean anything */
    proven_mem_mut_t mem = r.value;
    (void)proven_mem_copy(mem.ptr, mem.size, proven_mem_view_from_u8(PROVEN_LIT("hi")));
    alloc.free_fn(alloc.ctx, mem.ptr);
}
```

Wrong (does not compile as shown - the value is read before the error is checked):

```text
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
use_bytes(r.value.ptr, r.value.size); /* wrong: r.err was not checked */
```

### 3.2 Public struct contract

Many public structs expose layout for C usability. This does not mean arbitrary field mutation is supported. Treat direct mutation of internal fields as caller misuse unless the header explicitly allows it.

Wrong:

```text
arr.len = 999;      /* wrong: breaks array invariants */
str.internal.cap=0; /* wrong: breaks string invariants */
```

Use the functions and macros that maintain invariants.

### 3.3 Borrowed view contract

Views do not own memory. A `proven_u8str_view_t`, `proven_u16str_view_t`, `proven_mem_view_t`, or `proven_mem_mut_t` is valid only while the referenced storage remains alive and unmoved.

Wrong:

```text
proven_u8str_view_t v = proven_u8str_as_view(&s);
proven_u8str_append_grow(alloc, &s, PROVEN_LIT("more"));
use_view(v); /* wrong: growth may reallocate s */
```

### 3.4 Allocator contract

A `proven_allocator_t` is valid only when all three function pointers are present. Reallocation must be failure-atomic: if it fails, the old allocation remains valid.

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    /* no usable allocator here: e.g. the freestanding heap stub */
    proven_panic("no heap allocator on this target");
}
```

### 3.5 PAL boundary contract

Code under `src/proven/` should not depend on OS headers directly. Hosted services should route through `platform/proven_sys_*.[ch]` and public wrappers such as `proven_fs_*`, `proven_sysio_*`, `proven_time_*`, and `proven_mmap_*`.

Application code should prefer public APIs. Direct PAL calls are for porting and platform integration.

## 4. Ownership and destruction matrix

There are **two kinds of object** in this library, and the difference decides what you must do
with them.

**Owning objects** hold storage they allocated, and you must destroy them. They are the table
immediately below.

**Caller-owned state objects** allocate nothing. They are scratch structs you declare — usually
on the stack — and hand to a constructor, which returns a small by-value handle that points
*into* them. They have **no destroy function**, because there is nothing to free. What they have
instead is a rule that owning objects do not have, and it is the one that bites:

> **A caller-owned state object must not be copied or moved once a handle has been made from
> it.** The handle holds a pointer into the struct. Copy the struct and the handle still
> addresses the original; let the original go out of scope and the handle points at dead memory.

They are listed in [§4.2](#42-caller-owned-state-no-destroy-do-not-copy).

### 4.1 Owning objects — you must destroy these

| Object | Owns storage | Stores allocator | Destroy function | Notes |
|---|---:|---:|---|---|
| `proven_arena_t` | no — caller owns the backing slice | no | `proven_arena_destroy(&arena)` (no-op) | Bump pointer over caller memory: `alloc` advances an offset, `free` is a no-op, `reset` rewinds to empty. The caller owns/frees the backing block. |
| `proven_pool_t` | yes — items + recycle bin | yes (`base_alloc`) | `proven_pool_destroy(&pool)` | Fixed item size. You use it **through the allocator trait** (`proven_pool_as_allocator`): the trait's `free_fn` returns a slot to the recycle bin for reuse rather than freeing it. There is no `proven_pool_free` — freeing goes through the trait, like every other allocator. |
| `proven_buf_t` | yes | no | `proven_buf_destroy(alloc, &buf)` | Caller must pass the matching allocator. |
| `proven_u8str_t` | yes | no | `proven_u8str_destroy(alloc, &str)` | Always NUL-terminated when valid. |
| `proven_u16str_t` | yes | no | `proven_u16str_destroy(alloc, &str)` | Tracks byte length internally; API length is in `proven_u16` units. |
| `proven_array_t` | yes | yes | `proven_array_destroy(&arr)` or `PROVEN_ARRAY_DESTROY(&arr)` | Pointers into elements may be invalidated by growth. |
| `proven_ring_t` | yes | yes | `proven_ring_destroy(&ring)` or `PROVEN_RING_DESTROY(&ring)` | Fixed capacity; no growth. |
| `proven_map_t` | yes | yes | `proven_map_destroy(&map)` or `PROVEN_MAP_DESTROY(&map)` | Borrowed U8 keys are not copied. |
| `proven_array_t` from `proven_fs_list()` | yes | yes plus owned entry names | `proven_fs_list_destroy(alloc, &list)` | Do not use plain array destroy; entry names need cleanup. |
| `proven_mmap_t` | OS mapping | OS handle state | `proven_mmap_destroy(&map)` | Views into the mapping die with the mapping. |
| `proven_job_sys_t *` | yes | internal | `proven_job_system_close(sys)` then `proven_job_system_destroy(sys)` | Destroy must not race with producers. |

### 4.2 Caller-owned state — no destroy, do not copy

These allocate nothing and free nothing. You declare one, pass its address to a constructor, and
use the small handle you get back. The struct must **outlive the handle**, and must **not be
copied or moved** while the handle is alive.

| State object | Constructed by | The handle it backs | Notes |
|---|---|---|---|
| `proven_sha256_t` | `proven_sha256_init` | (used directly) | A hashing context. Safe to copy *before* you start, meaningless to copy mid-stream unless you intend to fork the hash. |
| `proven_xoshiro256ss_t` | `proven_xoshiro256ss_seed` | `proven_rng_t` | Copying it **clones the sequence** — deliberate and useful for a replay, a bug anywhere else. |
| `proven_chacha_rng_t` | `proven_chacha_rng_seed` / `_seed_from_entropy` | `proven_rng_t` | Copying it clones the keystream: two "independent" tokens become the same token. |
| `proven_writer_buf_t` | `proven_writer_from_buffer` | `proven_writer_t` | **Do not copy.** |
| `proven_writer_u8str_t` | `proven_writer_from_u8str` | `proven_writer_t` | **Do not copy.** The string and allocator must also outlive it. |
| `proven_writer_buffered_t` | `proven_writer_buffered` | `proven_writer_t` | **Do not copy.** You must `proven_writer_flush` before it or its buffer dies. |
| `proven_reader_view_t` | `proven_reader_from_view` | `proven_reader_t` | **Do not copy.** |
| `proven_reader_buffered_t` | `proven_reader_buffered` | `proven_reader_t` | **Do not copy.** Views returned by `proven_reader_read_line` point *into* its buffer. |
| `proven_sysio_std_t` | `proven_sysio_stdout_writer` / `_stderr_writer` / `_stdin_reader` | `proven_writer_t` / `proven_reader_t` | **Do not copy.** Holds the standard handle the writer points at. |
| `proven_sysio_out_t` | `proven_sysio_stdout_buffered` / `_file_buffered` | `proven_writer_t` | **Do not copy.** Must be flushed. |
| `proven_sysio_lines_t` | `proven_sysio_lines_open` / `_stdin_lines` | (used via `proven_sysio_read_line`) | The one exception: `proven_sysio_read_line` re-binds it on every call, so this one **may** be moved. |
| `proven_sysio_scanner_t` | `proven_sysio_scanner_init` | (used directly) | The exception in the other direction: this one **does** own a buffer, and you must call `proven_sysio_scanner_deinit`. |

Wrong — the copy looks harmless and is a use-after-free:

```text
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);

proven_sysio_out_t saved = out;   /* wrong: `w` still points into `out`, not `saved` */
use_elsewhere(&saved);            /* and if `out` goes out of scope, `w` is dangling */
```

Wrong — returning the state by value from a factory function does the same thing:

```text
proven_sysio_out_t make_logger(void) {
    proven_sysio_out_t out;
    proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);
    (void)w;
    return out;   /* wrong: any writer made from `out` addresses this dead frame */
}
```

Correct — the state stays put, and the handle travels:

```c
proven_byte_t buf[512];
proven_sysio_out_t out;                                   /* lives as long as `w` */
proven_writer_t w = proven_sysio_stdout_buffered(&out,
    (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });

(void)proven_fprintln(w, "one syscall, not {}", PROVEN_ARG(100));
(void)proven_writer_flush(w);                             /* or the bytes never happened */
```

## 5. Operation behavior classes

Several APIs intentionally expose three behavior classes:

| Class | Example | Behavior on insufficient capacity |
|---|---|---|
| Atomic fixed-capacity | `proven_u8str_append`, `proven_u16str_append`, `proven_u8str_append_fmt` | Return an error and leave the old object unchanged. |
| Best-effort truncating | `proven_u8str_append_partial`, `proven_u16str_append_partial`, `proven_u8str_append_fmt_trunc` | Write as much as fits, preserve a valid object, report how much was written. |
| Atomic growable | `proven_u8str_append_grow`, `proven_u16str_append_grow`, `proven_u8str_append_fmt_grow` | Grow with an allocator; on allocation failure, leave the old object unchanged. |
| **Refuse, never truncate** | `proven_hex_encode`, `proven_base64_encode`, `proven_base64_decode`, `proven_reader_read_line` | Write **nothing** and return `PROVEN_ERR_OUT_OF_BOUNDS`. A half-encoded string or a shortened line is a wrong answer that looks like a right one, so these do not produce one. Size the buffer with the module's own size function (`proven_base64_encoded_size`, …), not by eye. |

Choose the class deliberately. Do not treat a truncating function as an all-or-nothing function.

Wrong — assuming the truncating and the atomic form behave alike:

```text
/* `_partial` wrote what fit and told you so; the error you did not read is the
   difference between "all of it" and "some of it". */
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the result was the point */
```

## 6. Manual chapters

The detailed reference is split by chapter so it can stay readable and source-grounded.

**New to this library? Start with [Chapter 0](manual-00-start-here.md).** It is the only chapter
that assumes nothing: why the library exists, argued from the C bugs it is answering; a
hello-world program; how to build; the five contracts the rest of the manual takes for granted;
and a glossary plus a libc-to-`proven` table. Everything below assumes it.

0. [Start here: why this exists, hello world, the five contracts, glossary, libc map](manual-00-start-here.md)
1. [Foundation: types, errors, memory, alignment, version, panic](manual-01-foundation.md)
2. [Allocation: allocator trait, heap, arena, pool, byte buffers](manual-02-allocation.md)
3. [Strings and text: U8, U16, formatting, scanning](manual-03-strings-text.md)
4. [Containers and algorithms: array, list, ring, map, sort/search, hashing, encoding](manual-04-containers-algorithms.md)
5. [Hosted services: filesystem, tree walk, streams, sysio, environment, randomness, mmap, time](manual-05-hosted-services.md)
6. [Execution and platform: coroutines, jobs, aliases, PAL, freestanding, cross builds](manual-06-execution-and-platform.md)
7. [Alias index: every `alias_xcv.h` spelling map](manual-07-alias-xcv-index.md)
8. [Formatting and scanning: full `fmt.h` and `scan.h` reference](manual-08-fmt-scan.md)

## 7. Public header map

| Header | Main purpose | Chapter |
|---|---|---|
| `proven.h` | Umbrella include | This file |
| `types.h` | Fixed-width aliases, checked arithmetic, error enum | Chapter 1 |
| `error.h` | Error predicate helpers | Chapter 1 |
| `memory.h` | Byte views, slicing, range checks, memcmp | Chapter 1 |
| `align.h` | Alignment constants and align-up helpers | Chapter 1 |
| `version.h` | Version macros | Chapter 1 |
| `panic.h` | Registerable panic handler | Chapter 1 |
| `config.h` | Compile-time feature toggles (`PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR`, …) | Chapters 1 and 6 |
| `allocator.h` | Allocator trait | Chapter 2 |
| `heap.h` | PAL-backed heap allocator | Chapter 2 |
| `arena.h` | Bump allocator | Chapter 2 |
| `pool.h` | Fixed-size recycler allocator | Chapter 2 |
| `buffer.h` | Fixed-capacity byte buffer | Chapter 2 |
| `u8str.h` | Owned U8 string and borrowed U8 views | Chapter 3 |
| `u16str.h` | Owned U16 string and borrowed U16 views | Chapter 3 |
| `fmt.h` | Structural formatter and format arguments | Chapter 3 |
| `scan.h` | Structural scanner and typed scan destinations | Chapter 3 |
| `float_parse.h` | Locale-free decimal → `double`/`float` parser (`proven_strtod`, `proven_parse_double_ascii`) | Chapter 8 |
| `float_format.h` | `double`/`float` → decimal formatter (fixed `%f`/`%e`, shortest) | Chapter 8 |
| `float_config.h` | Float-engine tuning (`PROVEN_FLOAT_BIGINT_LIMBS`, precision caps) | Chapters 6 and 8 |
| `array.h` | Generic growable vector | Chapter 4 |
| `list.h` | Intrusive doubly-linked list | Chapter 4 |
| `ring.h` | Fixed-capacity FIFO ring | Chapter 4 |
| `map.h` | Open-addressing map | Chapter 4 |
| `algorithm.h` | Array sort and search helpers | Chapter 4 |
| `hash.h` | FNV-1a, SipHash-2-4, CRC-32, SHA-256, by use case | Chapter 4 |
| `encode.h` | Hex and Base64 (standard + URL-safe), bytes to text and back | Chapter 4 |
| `fs.h` | Files, directories, metadata, links, locks, read-all, tree walk | Chapter 5 |
| `stream.h` | Buffered writers, readers, and a line reader — and, through `sysio.h`, the standard streams (hosted-only) | Chapter 5 |
| `sysio.h` | Standard streams as writers/readers, line input from stdin, buffered output, printing, scanning, environment access | Chapter 5 |
| `random.h` | Randomness by use case: xoshiro256** (reproducible), ChaCha20 (cryptographic), the OS CSPRNG, and unbiased range/shuffle helpers. The generators work freestanding; only the OS source is hosted. | Chapter 5 |
| `mmap.h` | Memory-mapped file regions | Chapter 5 |
| `time.h` | Timestamp, datetime, sleep, datetime formatting | Chapter 5 |
| `coro.h` | Stackless coroutine macros | Chapter 6 |
| `job.h` | Bounded worker-thread job system | Chapter 6 |
| `alias_xcv.h` | Optional short alias layer and generated spelling map | Chapters 6 and 7 |

## 8. Platform support and verification

Primary verified hosted target:

- Linux x86_64 with GCC or Clang in C23 mode.

Compile-only cross coverage exists when the corresponding toolchains are installed:

- Linux AArch64.
- Linux ARM hard-float.
- Linux i686 through `i686-linux-gnu-gcc` or `gcc -m32` multilib.
- Windows x86_64 and i686 through MinGW/WinAPI paths.
- ARM Cortex-M freestanding.
- RISC-V ELF freestanding.

The cross matrix checks compilation, public header visibility, and target ABI assumptions. It does not replace runtime validation on the target platform.

Freestanding mode builds a reduced subset with OS-backed services removed. See `manual-freestanding.md` and Chapter 6 for details.
