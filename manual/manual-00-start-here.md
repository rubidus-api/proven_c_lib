# Chapter 0: Start Here

**Part I — Start here.** No prerequisites beyond one introductory C book.
**After this chapter** you can build a program against `proven`, read any other chapter, and look
up a term you do not recognise.

## Table of contents

1. [Who this manual is for](#1-who-this-manual-is-for)
2. [Why this library exists](#2-why-this-library-exists)
3. [Your first program](#3-your-first-program)
4. [Building and including](#4-building-and-including)
5. [The five contracts you will meet on every page](#5-the-five-contracts-you-will-meet-on-every-page)
6. [Intent and design philosophy](#6-intent-and-design-philosophy)
7. [Build and include model](#7-build-and-include-model)
8. [Global contracts](#8-global-contracts)
9. [Ownership and destruction matrix](#9-ownership-and-destruction-matrix)
10. [Operation behavior classes](#10-operation-behavior-classes)
11. [Manual chapters](#11-manual-chapters)
12. [Platform support and verification](#12-platform-support-and-verification)
13. [Appendix B: glossary](#13-appendix-b-glossary)
14. [Appendix C: public header map](#14-appendix-c-public-header-map)
15. [Appendix D: the libc map](#15-appendix-d-the-libc-map)
16. [Where to go next](#16-where-to-go-next)

---

## 1. Who this manual is for

This manual assumes you have finished one introductory C book. Concretely, it assumes you are
comfortable with variables and control flow, functions, arrays, `struct`, pointers and `*`/`&`,
`malloc` and `free`, `printf`, `char *` strings with `strlen`, and compiling a program with
`gcc main.c -o main`. If all of that is familiar, you have enough.

It does **not** assume you have met ownership as a discipline, borrowed versus owned data, arenas
or pools, function-pointer tables used as an interface, C23 attributes, undefined behaviour as
something a compiler actively exploits, alignment beyond "it works", atomics, or the idea that a
library might *refuse* an operation instead of doing its best. Every one of those is explained
where it is first used, and every one is in the glossary in §13.

This is not a C tutorial. It will not explain what a pointer is. It will explain, at length, why
this library hands you a `struct` containing an error instead of setting `errno`, because that is
a design decision you are entitled to disagree with, and you cannot disagree with a decision
nobody explained.

---

## 2. Why this library exists

C gives you almost nothing and trusts you completely. That is its great strength — there is no
runtime, no hidden allocation, no cost you did not write — and it is why C is still the language
of operating systems, embedded devices and everything that has to be small and predictable.

It is also why the same five bugs have been shipping for fifty years. This library is a set of
answers to those five bugs. Each answer costs something, and this section says what.

### The string functions do not know how big anything is

```text
char buf[64];
strcpy(buf, name);            /* wrong: how long is name? strcpy never asks */
strcat(buf, ", welcome!");    /* wrong: and how much room is left now? */
```

`strcpy` receives a destination pointer and a source pointer. Nothing in that signature carries
the size of the destination, so nothing can check it. The function will happily write the 200th
byte into a 64-byte buffer, and what it corrupts is whatever the compiler happened to put next —
often the return address. This is not a rare mistake by careless people; it is the single most
exploited class of bug in the history of the language, and the API makes it the *default*
behaviour.

`strncpy` is the traditional answer and it is a trap of its own: it does not always NUL-terminate,
so the "safe" version silently produces a string that is not a string.

**What this library does instead.** A string carries its length. `proven_u8str_view_t` is a
pointer *and* a size, together, always. An append checks the destination's capacity because it
knows the capacity, and when the text does not fit it **returns an error and writes nothing** —
it does not truncate, because a truncated path is a wrong path and a truncated command is a
different command. See [Chapter 3](manual-03-strings-text.md).

**What it costs.** Every string is two words instead of one, and you cannot pass a `proven` string
straight to a `printf("%s")` without asking for a NUL-terminated form.

### Nothing makes you check for failure

```text
char *p = malloc(n);
p[0] = 'x';                   /* wrong: malloc returns NULL when it fails */
```

`malloc` reports failure by returning `NULL`, and C will not say a word if you never look. The
same is true of `fopen`, of `realloc`, of every function that returns a sentinel. The error is
*available*; noticing it is optional.

`errno` is worse, because it is a global that survives the call. You must check it at exactly the
right moment, before any other library call has a chance to overwrite it, and you must remember
that a successful call is allowed to set it to garbage.

**What this library does instead.** A function that can fail returns its error *as a value*, and
when it also has a result, the two come back together in one `struct`:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
if (!proven_is_ok(s.err)) return 1;      /* s.value means nothing until you check */
```

Functions whose only job can fail are marked `[[nodiscard]]`, which means the compiler refuses to
build code that throws the error away. You can still ignore it deliberately — `(void)` in front
of the call — and having to type that is the point. See [Chapter 1](manual-01-foundation.md).

**What it costs.** More `if`s. There is no exception mechanism to jump you out of a deep failure,
so error paths are visible in the shape of the code. That visibility is the feature.

### `printf` believes whatever you tell it

```text
printf("%d\n", 3.0);          /* wrong: %d with a double. This compiles. */
printf("%s\n", 42);           /* wrong: and this one crashes */
```

The format string is checked by nothing at runtime. Modern compilers warn about literal formats,
which helps exactly until the format is a variable, and then you are back to a function that
reads whatever bytes the varargs stack happens to hold, in whatever shape the string demanded.

**What this library does instead.** `{}` is a placeholder with no type in it, and the type comes
from the argument, resolved at compile time by `_Generic`:

```text
proven_println("{} is {}", PROVEN_ARG(name), PROVEN_ARG(count));
```

There is no `%d`-versus-`double` mismatch available, because you never wrote the type twice. See
[Chapter 3 §3](manual-03-strings-text.md) for the tutorial and [Chapter 8](manual-08-fmt-scan.md)
for the full grammar.

**What it costs.** `PROVEN_ARG` around each argument, and a format language that is not the one
in your fingers.

### Who frees this?

```text
char *s = build_message();    /* do I free this? the type does not say */
```

A `char *` returned from a function might be freshly allocated, might point into a caller's
buffer, might be a string literal in read-only memory, or might be a pointer into a static buffer
that the next call will overwrite. The type is identical in all four cases. The answer lives in
documentation, and documentation drifts.

**What this library does instead.** Ownership is in the type name and in the signature. A
`proven_u8str_t` is **owned** — you got it from a `_create`, and you must `_destroy` it with the
same allocator. A `proven_u8str_view_t` is **borrowed** — it points at bytes someone else owns,
you never destroy it, and it stops being valid when the owner does. Any function that might
allocate takes the allocator as a parameter, so a signature without an allocator cannot allocate.

**What it costs.** Two types where C has one, and the discipline of asking "who owns this?" at
every boundary — which you were paying anyway, just later and in a debugger.

### The comparison function nobody can typecheck

```text
qsort(a, n, sizeof *a, cmp);  /* cmp takes const void*; get it wrong and it is UB */
```

`qsort` takes a comparator through a `void *` interface, so a comparator with the wrong parameter
types still compiles. The classic version of this bug is comparing the pointers instead of what
they point at, and it produces a program that runs, sorts nothing correctly, and never crashes.

**What this library does instead.** The same `void *` shape — this is C, there is no other way —
but the library documents the contract precisely, gives you working comparators to copy, and
`proven_array_sort` is an introsort with an *O(n log n)* guarantee rather than a quicksort that
degrades to *O(n²)* on the input an attacker chooses. See
[Chapter 4](manual-04-containers-algorithms.md).

### The bytes have a type, even when you did not choose one

You think of memory as bytes. C's abstract machine does not: it treats memory as *typed*, and
reading the same bytes through pointers of two different types is undefined behaviour — which the
compiler is allowed to exploit, silently, only when optimisations are on. This is called **strict
aliasing**, and it is the trap under every hand-written parser that reads a byte buffer through
pointers of different widths:

```text
void *buf = malloc(8);
uint32_t *w = buf;      /* the same memory, seen as 32-bit — no cast, no warning */
uint16_t *h = buf;      /* the same memory, seen as 16-bit */
*w = 0xAAAAAAAAu;
*h = 0x1234;            /* change the low half */
printf("%08x\n", *w);   /* wrong to expect aaaa1234: at -O2 this prints aaaaaaaa */
```

Compiled at `-O0` it prints `aaaa1234`; at `-O2` it prints `aaaaaaaa`, because the compiler assumed
a `uint16_t` write and a `uint32_t` read cannot touch the same memory and dropped the write. No
warning is given, and the program passed every test you ran in a debug build. This is the class of
bug the Linux kernel avoids by compiling with `-fno-strict-aliasing` — a whole flag, for one rule.

**What this library does instead.** Raw memory is `proven_byte_t`, an alias of `unsigned char` —
the one type the rule explicitly exempts, because the standard lets you inspect any object's bytes
through it. The ordinary API never quietly reinterprets your bytes as a wider type, so the bug
above cannot be written through it. (Strict aliasing has a subtler sibling, *provenance*, that the
library is named after; [Chapter 6 §3](manual-06-execution-and-platform.md) and the project README
cover it.)

### What this library is not

It is not a framework, and it does not want to own your `main`. It has no global state to
initialise, starts no threads, registers no `atexit` handler, and allocates nothing you did not
hand it an allocator for. Every module is usable on its own. Most of it runs with no operating
system at all — see [freestanding mode](manual-freestanding.md).

| The problem | What C gives you | What `proven` gives you | Cost |
|---|---|---|---|
| Buffer overruns | `strcpy`, `strcat` — no size anywhere | Views carry a length; writes refuse rather than truncate | Two words per string |
| Unchecked failure | `NULL` returns and `errno` | Errors returned as values, `[[nodiscard]]` on the ones you must not drop | More `if`s |
| Format mismatch | `printf` trusts the format string | `{}` with the type taken from the argument | `PROVEN_ARG` at each call |
| Unclear ownership | `char *` means four different things | Owned and borrowed are different types | Two types instead of one |
| Hidden allocation | Anything may call `malloc` | Only functions taking an allocator can allocate (one bounded exception: `proven_println` on an over-long line) | The parameter is on the signature |
| Bytes with a hidden type | Reinterpreting memory through a wider pointer is UB the optimiser exploits | Raw bytes go through `proven_byte_t`, the type the rule exempts | — |

---

## 3. Your first program

This is the whole of it. Every line is one of the contracts in §5, and the build compiles and runs
this exact file, so it cannot quietly stop being true.

<!-- example: manual/examples/en/ex_00_hello.c -->
```c
/*
 * The first program. It is deliberately small, and every line of it is one of
 * the five contracts you will meet on every page of this manual.
 *
 * Compare it with the C you already know:
 *
 *     char buf[64];
 *     strcpy(buf, name);          <- how big is name? strcpy does not ask.
 *     strcat(buf, ", welcome!");  <- and now? strcat does not ask either.
 *     printf("%s\n", buf);
 *
 * That program is correct until the day `name` is longer than you assumed, and
 * then it is a security advisory. The version below cannot do that: every write
 * knows the size of its destination, and every operation that could fail hands
 * you back an error you are not allowed to ignore silently.
 */

int main(void) {
    /* (1) You pass the allocator in. The library never reaches for a global
     *     malloc behind your back, so you always know who allocated what. */
    proven_allocator_t alloc = proven_heap_allocator();

    /* (2) Anything that can fail returns its error WITH its value. There is no
     *     errno to remember to check, and `greeting.value` means nothing until
     *     you have looked at `greeting.err`. */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* (3) A view is borrowed text that knows its own length. PROVEN_LIT builds
     *     one from a literal at compile time - no strlen scan happens here. */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* (4) The append refuses rather than truncates. If "hello, " and the name
     *     did not fit in the 64 bytes asked for above, this returns
     *     PROVEN_ERR_OUT_OF_BOUNDS and writes nothing - it never quietly stores
     *     half a word and lets you carry on. */
    proven_err_t err = proven_u8str_append(&greeting.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, name);
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, PROVEN_LIT("!"));

    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &greeting.value);
        return 1;
    }

    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&greeting.value),
                                         PROVEN_LIT("hello, world!")),
                    "the three appends should have built the whole greeting");

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* (5) You created it with `alloc`, so you destroy it with the SAME `alloc`.
     *     Owning things are destroyed exactly once; borrowed things - like
     *     `name` above - are never destroyed at all. */
    proven_u8str_destroy(alloc, &greeting.value);

    return EXAMPLE_OK();
}
```

`EXAMPLE_REQUIRE` and `EXAMPLE_OK` are not part of the library — they come from
`manual/examples/example.h` and exist so the build can check that every example in this manual
still does what the text says it does. Your own program would use neither.

Three details worth pausing on, because they recur everywhere:

- **`proven_u8str_create(alloc, 64)` asks for 64 bytes of capacity**, and that capacity includes
  the NUL terminator. It does not grow on its own. `proven_u8str_append` is the fixed-capacity
  form: it fails when the text does not fit. When you want growth, you call
  `proven_u8str_append_grow` and pass the allocator, and the signature tells you at a glance which
  one you are using.
- **`PROVEN_LIT("hello, ")` costs nothing at runtime.** It is a compile-time `sizeof` on a string
  literal. `proven_u8str_view_from_cstr(s)` exists for the case where the text comes from
  elsewhere, and that one really does scan for the NUL — the two are spelled differently because
  one is free and the other is *O(n)*.
- **The `destroy` takes the allocator again.** The string does not remember which allocator made
  it; you must pass the same one. That keeps the string small and makes the dependency visible,
  and it is a real hazard — see §5.

---

## 4. Building and including

There is one header for everything:

```text
#include "proven.h"
```

It pulls in the whole public API. If you prefer to include only what you use, every module has its
own header under `include/proven/` — `#include "proven/u8str.h"`, `#include "proven/fs.h"` and so
on — and §14 below maps every header to the chapter that documents it.

Compiling by hand, which is all this library needs:

```text
gcc -std=c2x -Iinclude -Iplatform your_program.c src/proven/*.c platform/*.c -o your_program
```

`src/proven/` is the portable library. `platform/` is the small layer that talks to the operating
system — the **PAL**, or platform abstraction layer. Everything that makes a syscall lives there
and nowhere else, which is why the rest of the library can be compiled for a target with no
operating system at all.

The repository builds itself with a C program rather than a build system:

```text
cc -std=c2x -o nob nob.c     # build the build driver, once
./nob build                  # compile everything and run the whole test suite
./nob release                # the same, optimised
```

`./nob` with no arguments lists the other modes — sanitizers, freestanding, cross-compilation and
the benchmark. There is no `make`, no CMake, and nothing to install.

**A C23 compiler is required.** The library uses C23 features deliberately, `[[nodiscard]]` most
visibly. GCC 13+, Clang 16+ and recent MSVC all work; the build driver probes for `-std=c23` and
falls back to `-std=c2x` for slightly older compilers.

---

## 5. The five contracts you will meet on every page

These five rules explain most of the library's shape. Each is stated once here and assumed
everywhere else. §8 below gives the formal versions; these are the plain-language ones.

| # | Contract | In one line | Chapter |
|---|---|---|---|
| 1 | **Errors are values** | Fallible calls return `proven_err_t`, or a `{ err, value }` struct | [1](manual-01-foundation.md) |
| 2 | **Views are borrowed** | A view is a pointer + a length into memory someone else owns | [3](manual-03-strings-text.md) |
| 3 | **Allocation is a parameter** | If a function can allocate, it takes an allocator; if it cannot, it does not | [2](manual-02-allocation.md) |
| 4 | **Caller-owned state must not be copied** | Some structs point into themselves; copying them creates dangling pointers | [5](manual-05-hosted-services.md) |
| 5 | **Refuse, never truncate** | An operation that does not fit fails and writes nothing | [3](manual-03-strings-text.md) |

### 1. Errors are values

Every fallible function hands the error back. When there is nothing else to return it is a bare
`proven_err_t`; when there is a value, the value and the error travel together and the value is
meaningless until you have checked the error. `PROVEN_OK` is zero, and `proven_is_ok(err)` reads
better than `err == 0`.

Wrong — reading the value before the error:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
proven_u8str_append(&s.value, text);   /* wrong: s.value is garbage if create failed */
```

The whole point of pairing them is that the value is only valid on the success path. Check first,
every time.

### 2. Views are borrowed

`proven_u8str_view_t` is a pointer and a size. It owns nothing, allocates nothing, and is free to
copy — but it stops being valid the moment the thing it points into is destroyed or moved.

Wrong — a view that outlives what it points at:

```text
proven_u8str_view_t name;
{
    proven_result_u8str_t owned = proven_u8str_create(alloc, 16);
    (void)proven_u8str_append(&owned.value, PROVEN_LIT("temp"));
    name = proven_u8str_as_view(&owned.value);
    proven_u8str_destroy(alloc, &owned.value);   /* the bytes are gone */
}
use(name);                                       /* wrong: dangling view */
```

This is the same lifetime bug as a dangling pointer, and it is worth stating separately because a
view *looks* like a value. It is not; it is a pointer wearing a struct.

### 3. Allocation is a parameter

Read the signature. `proven_u8str_append(str, data)` cannot allocate, so it fails when the text
does not fit. `proven_u8str_append_grow(alloc, str, data)` takes an allocator, so it can grow.
Nothing in this library calls `malloc` behind your back, which is what makes it usable in an
arena, in a pool, or on a device with no heap.

The corollary is the hazard: **you must destroy with the allocator you created with.** The object
does not remember.

Wrong — mismatched allocators:

```text
proven_result_u8str_t s = proven_u8str_create(arena_alloc, 64);
proven_u8str_destroy(heap_alloc, &s.value);   /* wrong: heap free on arena memory */
```

Nothing checks this today. It is heap corruption that surfaces later, somewhere else.

### 4. Caller-owned state must not be copied

Some objects are structs you own and pass by pointer — buffered writers, line readers, directory
iterators. Several of them contain a pointer to one of their own fields, so copying the struct
copies a pointer that still points at the *original*.

Wrong — copying a state struct:

```text
proven_writer_buf_t a = ...;
proven_writer_buf_t b = a;          /* wrong: b's internals still point into a */
```

§9.2 below lists all sixteen of these types. The rule is simple: create it where it lives,
pass `&it`, and do not assign it.

### 5. Refuse, never truncate

When a result does not fit, this library fails the operation and leaves the destination unchanged.
It does not write "as much as fits". A truncated path opens the wrong file, a truncated command
runs the wrong command, and a truncated number is a different number — and every one of those is
worse than an error you can see.

Where truncation genuinely is what you want, there is a separate, differently named function that
tells you how much it wrote:

```text
proven_result_size_t r = proven_u8str_append_partial(&s, huge);
/* r.value is how many bytes were actually appended. Reading it is the point. */
```

Wrong — assuming the two behave alike:

```text
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the count WAS the result */
```

---

Everything up to here is the plain-language introduction. The rest of this chapter is the
reference the other chapters lean on: the library's intent and build model stated formally, the
global contracts, the ownership matrix, the behaviour classes, the reading order, platform
support, and the appendices. It used to be a document of its own, the manual's front page; it
lives here now so that the front page is nothing but the table of contents.

## 6. Intent and design philosophy

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

## 7. Build and include model

### There is nothing to install

This library has no `configure`, no CMake, no package to fetch, and no shared object to place
somewhere. It is C source: you compile it into your program alongside your own files.

That is a deliberate choice and it costs something. You do not get a system package, and updating
means pulling new source rather than bumping a version constraint. What you get is that the library
cannot be a different version than the one you are looking at, cannot pick up a build flag you did
not choose, and cannot fail to link because a distribution built it with different options. For a
library that is meant to run on hosted systems *and* on bare metal, "compile it with your program"
is the only model that works in both places.

Two directories matter. `src/proven/` is the portable library — no OS calls anywhere. `platform/`
is the small layer that makes syscalls, and it is the only part a new target has to replace. A
freestanding build simply leaves the hosted files out; see [the freestanding
guide](manual-freestanding.md).

The build driver, `nob.c`, is a C program rather than a build system, and you compile it the same
way you compile everything else here. It is checked into the repository, so there is no bootstrap
step and no version of it to install either.

**A C23 compiler is required** — GCC 13+, Clang 16+, or recent MSVC. The driver probes `-std=c23`
and falls back to `-std=c2x` for compilers that still use the transitional spelling.

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
./nob cross -build-root build-out/proven_c_lib
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

## 8. Global contracts

### 8.1 Result contract

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

### 8.2 Public struct contract

Many public structs expose layout for C usability. This does not mean arbitrary field mutation is supported. Treat direct mutation of internal fields as caller misuse unless the header explicitly allows it.

Wrong:

```text
arr.len = 999;      /* wrong: breaks array invariants */
str.internal.cap=0; /* wrong: breaks string invariants */
```

Use the functions and macros that maintain invariants.

### 8.3 Borrowed view contract

Views do not own memory. A `proven_u8str_view_t`, `proven_u16str_view_t`, `proven_mem_view_t`, or `proven_mem_mut_t` is valid only while the referenced storage remains alive and unmoved.

Wrong:

```text
proven_u8str_view_t v = proven_u8str_as_view(&s);
proven_u8str_append_grow(alloc, &s, PROVEN_LIT("more"));
use_view(v); /* wrong: growth may reallocate s */
```

### 8.4 Allocator contract

A `proven_allocator_t` is valid only when all three function pointers are present. Reallocation must be failure-atomic: if it fails, the old allocation remains valid.

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    /* no usable allocator here: e.g. the freestanding heap stub */
    proven_panic("no heap allocator on this target");
}
```

### 8.5 PAL boundary contract

Code under `src/proven/` should not depend on OS headers directly. Hosted services should route through `platform/proven_sys_*.[ch]` and public wrappers such as `proven_fs_*`, `proven_sysio_*`, `proven_time_*`, and `proven_mmap_*`.

Application code should prefer public APIs. Direct PAL calls are for porting and platform integration.

## 9. Ownership and destruction matrix

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

They are listed in [§4.2](#92-caller-owned-state--no-destroy-do-not-copy).

### 9.1 Owning objects — you must destroy these

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

### 9.2 Caller-owned state — no destroy, do not copy

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

## 10. Operation behavior classes

### One operation, three honest answers

"Append this text to that string" has three defensible behaviours when the text does not fit, and
most libraries pick one and hide the choice. This library exposes all three, gives them different
names, and puts the difference in the signature — because which one you want depends on what the
text *is*, and only the caller knows that.

Consider appending to a filesystem path:

- If the path does not fit, **truncating is catastrophic.** `documents/report.pdf`
  becomes `documents/rep`, which is a different, possibly existing, file. The operation must fail
  and change nothing.
- Now consider appending to a log line. If it does not fit, **truncating is fine** — you would
  rather have most of the message than none of it — as long as you are told how much was written.
- And appending to a buffer you are building up, where you would simply like it to **grow**.

The three classes below are those three answers. The one you get is decided by the function name
and the presence of an allocator parameter, never by a flag or a global:

- `proven_u8str_append(str, data)` — no allocator, so it cannot grow: **refuses**.
- `proven_u8str_append_partial(str, data)` — returns a count: **truncates and tells you**.
- `proven_u8str_append_grow(alloc, str, data)` — takes an allocator: **grows**.

The default across the library is the first one, and [Chapter 0
§5](#5-the-five-contracts-you-will-meet-on-every-page) explains why: a
truncated path opens the wrong file, a truncated command runs the wrong command, and a truncated
number is a different number.

Wrong — ignoring the count from the truncating form:

```text
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the count WAS the answer */
```

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

## 11. Manual chapters

**Just finished an introductory C book? Start with the
[tutorial](manual-t-tutorial.md).** It is six short programs, each introducing exactly one idea,
ending with the greeting program from this chapter read line by line. This chapter shows that program
on its first page and it carries five new ideas at once; the tutorial hands them over one at a time.

**Otherwise start with this chapter.** It is the only chapter
that assumes nothing: why the library exists, argued from the C bugs it is answering; a
hello-world program; how to build; the five contracts the rest of the manual takes for granted;
and a glossary plus a libc-to-`proven` table. Every other chapter assumes it.

### The reading order

The chapters are grouped into parts, and the parts are ordered so that each one only needs the
ones before it. That order is not the same as the header dependency graph, and it is not
arbitrary: strings need allocators, containers need allocators, and hosted services need strings,
so the sequence below is what the material itself requires.

The chapter *numbers* are stable identifiers, not a reading sequence. Two of them are out of
order on purpose: the alias index is an appendix you look things up in, and Chapter 8 is a
reference you read after Chapter 3 has introduced the subject.

| Part | Read | Prerequisites | You can then |
|---|---|---|---|
| **I — Start here** | [tutorial](manual-t-tutorial.md) → [0](manual-00-start-here.md) | One introductory C book | Build against the library and read anything below |
| **II — The vocabulary every program uses** | [1](manual-01-foundation.md) → [2](manual-02-allocation.md) → [3](manual-03-strings-text.md) | Chapter 0 | Handle errors as values, own memory deliberately, hold text safely |
| **III — Data structures** | [4](manual-04-containers-algorithms.md) | Part II | Arrays, maps, lists, rings, sorting, searching, hashing, encoding |
| **IV — Text in and out** | [8](manual-08-fmt-scan.md) | Chapter 3 §3–§4 | Format and parse anything, and teach the formatter your own types |
| **V — Talking to the operating system** | [5](manual-05-hosted-services.md) | Part II | Files, directories, streams, standard I/O, time, randomness, mapping |
| **VI — Going further** | [6](manual-06-execution-and-platform.md) → [freestanding](manual-freestanding.md) | Parts II–V | Coroutines, jobs, thread-safety, bare metal, cross builds |
| **Appendices** | [A](manual-07-alias-xcv-index.md), [B](#13-appendix-b-glossary), [C](#14-appendix-c-public-header-map), [D](#15-appendix-d-the-libc-map) | — | Look things up |

### The chapters

- [**Tutorial**: the library in six short programs, one new idea at a time](manual-t-tutorial.md) — *Part I, optional on-ramp*
0. [**Start here**: why this exists, hello world, the five contracts, glossary, libc map](manual-00-start-here.md) — *Part I*
1. [**Foundation**: types, errors, memory views, alignment, version, panic](manual-01-foundation.md) — *Part II*
2. [**Allocation**: heap, arena, pool, byte buffers, and the allocator trait](manual-02-allocation.md) — *Part II*
3. [**Strings and text**: U8, U16, and an introduction to formatting and scanning](manual-03-strings-text.md) — *Part II; the tutorial half of the text material*
4. [**Containers and algorithms**: array, list, ring, map, sort/search, hashing, encoding](manual-04-containers-algorithms.md) — *Part III*
5. [**Hosted services**: filesystem, tree walk, streams, sysio, environment, randomness, mmap, time](manual-05-hosted-services.md) — *Part V*
6. [**Execution and platform**: coroutines, jobs, thread-safety, aliases, PAL, cross builds](manual-06-execution-and-platform.md) — *Part VI*
7. [**Appendix A — Alias index**: every `alias_xcv.h` spelling](manual-07-alias-xcv-index.md) — *reference only; not reading material*
8. [**Formatting and scanning**: the full `fmt.h` and `scan.h` reference](manual-08-fmt-scan.md) — *Part IV; the reference half of the text material*

**Chapters 3 and 8 both cover the formatter and the scanner, and the division is deliberate.**
Chapter 3 introduces them alongside strings, with the everyday cases and enough to be productive.
Chapter 8 is the complete reference: the full format grammar, every argument constructor, the
scanner's error codes and recovery rules, and how to teach the formatter a type of your own. Read
Chapter 3 first; reach for Chapter 8 when you need the exact behaviour of a specifier or a
failure.

## 12. Platform support and verification

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

## 13. Appendix B: glossary

Terms this manual uses as if they were ordinary words. They are not ordinary C words, and every
one of them is load-bearing.

Two rules the chapters follow, so that nothing here has to be guessed at:

- **No private vocabulary.** Where a widely used word exists for an idea, the manual uses that
  word. Where the library has invented something, it is defined here.
- **Every abbreviation is written out the first time it appears in a chapter**, and every one of
  them is listed below as well.

The Korean edition adds one more: an English term appears with its original spelling in
parentheses the first time each chapter uses it — 뷰(view) — so a reader who learned the idea in
English can find it, and a reader who did not is never handed an untranslated word.

| Term | Meaning here |
|---|---|
| **owned** | You are responsible for destroying it. Comes from a `_create`, goes to a `_destroy`, exactly once. |
| **borrowed** | Points at memory someone else owns. Never destroyed. Valid only while the owner is. |
| **view** | A borrowed pointer + length pair, e.g. `proven_u8str_view_t`. Copyable, non-owning, no allocation. |
| **allocator** | A value carrying four things: a context pointer and three function pointers (alloc, realloc, free). Passed by value into anything that may allocate. |
| **arena** | An allocator that hands out memory by bumping a pointer through one block. Individual frees do nothing; you reset or destroy the whole arena at once. Fast, and perfect for "many small things with the same lifetime". |
| **pool** | An allocator for many objects of one fixed size, with a free list, so freeing really does recycle a slot. |
| **trait** | A struct of function pointers used as an interface — C's answer to a virtual table. `proven_allocator_t`, `proven_writer_t` and `proven_rng_t` are traits. Not a C keyword; borrowed terminology. |
| **PAL** | Platform abstraction layer: the code under `platform/` that makes actual syscalls. The only OS-dependent part. |
| **freestanding** | A build with no operating system and no libc — bare metal. `PROVEN_FREESTANDING` selects it. |
| **failure atomicity** | If an operation fails, it changes nothing. A failed grow leaves your old data intact and valid. |
| **provenance** | Which allocation a pointer came from. C's optimizer assumes pointers from different allocations never overlap; violating that is undefined behaviour, not merely surprising. Chapter 6 covers it. |
| **UB** (undefined behaviour) | Not "unpredictable output" — the standard imposes no requirement at all, and optimizers are allowed to assume it never happens. This is why UB can delete your `if`. |
| **`[[nodiscard]]`** | A C23 attribute. The compiler errors if you throw the return value away. Used on every function whose error you must not drop. |
| **fixed-capacity** | Will not grow. Fails when full. Takes no allocator. |
| **growable** | Will reallocate when full. Takes an allocator. Always spelled `_grow` in the name. |
| **CSPRNG** | Cryptographically secure pseudo-random number generator: output an attacker cannot predict even after seeing earlier output. |
| **intrusive** | The list's links live *inside* your struct rather than in separately allocated nodes. No allocation per element. |
| **code unit** | One element of an encoding: a byte in UTF-8, a 16-bit value in UTF-16. Not a character — one character can take several. |
| **code point** | One character's number in Unicode. A code point takes one to four bytes in UTF-8, and one or two code units in UTF-16 — which is why counting either one is not counting characters. |
| **API** (application programming interface) | The set of functions and types a library exposes for other programs to call. In this library, everything declared in `include/proven/`. |
| **hosted** | A build that has an operating system and a C standard library under it — the opposite of *freestanding*. |
| **heap** | The general-purpose pool of memory `malloc` hands out from. `proven_heap_allocator()` is the allocator that uses it. |
| **slice** | A borrowed pointer + length pair you may **write** through, e.g. `proven_mem_mut_t`. A *view* is the read-only form of the same idea. |
| **result** | A small struct carrying an error code and a value together, e.g. `proven_result_u8str_t`. The value means nothing until the error beside it has been checked. |
| **panic** | The deliberate stop taken when a failure has no caller to return to. `proven_set_panic_handler()` chooses what happens; the default stops the program. |
| **reserve** | Raise a container's capacity **now**, so later growth does not reallocate. Saves copying on the heap, and dead storage in an arena. |
| **dangling** | A pointer to memory that has been freed or moved. Using one is undefined behaviour; the usual cause here is holding a pointer across a call that may reallocate. |
| **use-after-free** | Reading or writing through a dangling pointer. The sanitizers below detect it. |
| **sanitizer** | A compiler mode that adds run-time checks: **ASan** (AddressSanitizer) finds memory errors, **UBSan** (UndefinedBehaviorSanitizer) finds undefined behaviour, **TSan** (ThreadSanitizer) finds data races. `./nob asan`, `./nob ubsan`, `./nob tsan`. |
| **partial write / short read** | A single write or read that moved *fewer* bytes than asked for. Normal, not an error — and treating a short read as end of input is the classic way to lose the tail of a file. |
| **EOF** (end of file) | There is no more input. Reported as `PROVEN_ERR_EOF`, never as a zero-byte success, so it cannot be confused with "nothing arrived yet". |
| **flush** | Push a buffered writer's accumulated bytes onward. Nothing in this library flushes on your behalf at exit. |
| **back-pressure** | Slowing a producer down because the consumer cannot keep up. `proven_writer_write_partial()` is the call that lets you notice and react. |
| **durability** | The guarantee that data survives a power cut. Reaching the operating system is not enough: `proven_fs_sync()` puts a file's bytes on the storage device, and `proven_fs_sync_dir()` does the same for a rename. |
| **atomic rename** | Replacing a file by renaming a finished temporary over it. A reader sees the whole old file or the whole new one, never a half-written mixture. |
| **advisory lock** | A lock that excludes only the processes that also ask for it (`proven_fs_lock()`). It is a convention between cooperating programs, not access control. |
| **hard link** | A second **name** for the same file. There is no original; the data lives until the last name is removed. Same filesystem only. |
| **symbolic link** | A small file that holds a path. It may cross filesystems, and it may point at nothing — following it then fails. |
| **memory mapping** | Making a file's contents appear at an address, so the processor reads it as memory (`mmap.h`). **SHARED** mappings write back to the file; **PRIVATE** ones are *copy-on-write*: the writes exist in your process and nowhere else. |
| **copy-on-write** | Sharing memory until somebody writes, at which point that writer gets a private copy. What `PROVEN_MMAP_PRIVATE` does. |
| **cursor** | The scanner's position in the text it is reading (`proven_scan_t.cursor`). Scanning advances it; `proven_scan_skip_*` moves it deliberately. |
| **locale** | The system's idea of local conventions — including whether the decimal separator is `.` or `,`. This library's number parsing is **locale-free**: a comma is never a decimal point, on any machine. |
| **entropy** | Genuinely unpredictable bits, from the operating system or from hardware. A generator is *seeded* from entropy; a clock or a counter is not entropy, however random it looks. |
| **seed** | The starting value of a generator. The same seed replays the same sequence — essential for a reproducible test, and fatal for a key. |
| **PRNG / CSPRNG** | A pseudo-random number generator computes a sequence from a seed. A **C**ryptographically **S**ecure one (CSPRNG) is additionally unpredictable to an attacker who has seen earlier output. |
| **HashDoS** | An attack that feeds a hash table keys chosen to collide, turning *O(1)* lookups into *O(n²)*. `proven_map_create()` defends against it with a keyed hash; `proven_map_create_trusted()` opts out for keys you choose yourself. |
| **open addressing** | The map's layout: entries live in one flat bucket array and a collision moves to the next slot, rather than following a chain of separately allocated nodes. |
| **tombstone** | The marker left where a map entry was removed, so lookups keep probing past it. Tombstones count towards the load factor, which is why heavy remove traffic still triggers a rehash. |
| **rehash** | Rebuilding the bucket array at a new size. It invalidates every pointer a previous `get_mut` returned, which is why you hold the key rather than the pointer. |
| **checksum** | A short value that detects accidental corruption — `proven_crc32()`. It detects accidents, never tampering. |
| **digest / hash** | A fixed-size value computed from data. **SHA-256** is a cryptographic digest (tamper-evident); **FNV-1a** and **SipHash-2-4** are table hashes, and only SipHash is keyed. |
| **hex / Base64 / Base64URL** | Ways to write bytes as text. Hex is two characters per byte; Base64 packs three bytes into four characters with `+ / =`; Base64URL uses `- _` and no padding, so it is safe in a URL or filename. |
| **padding** | The `=` characters Base64 adds so the output length is a multiple of four. Base64URL leaves them out. |
| **BMP** (Basic Multilingual Plane) | The first 65,536 Unicode code points. A character outside it — an emoji, many rarer CJK characters — needs two UTF-16 code units. |
| **NUL terminator** | The zero byte that marks the end of a C string. A *view* does not have one, which is exactly why it carries a length instead. |
| **shortest round trip** | Printing the fewest digits that read back as exactly the same floating-point value. `PROVEN_FLOAT_FORMAT_MODE_SHORTEST` asks for it; it is what a serialiser wants. |
| **dispatch macro** | A macro built on C11 `_Generic` that picks a function from an argument's type — `PROVEN_ARG(x)` and `PROVEN_SCAN_ARG(&x)`. It chooses among the named constructors; it is not itself one. |
| **identity constructor** | `proven_arg_identity()` / `proven_scan_arg_identity()`: they take an argument that has already been built and pass it through, so macro-driven code can accept one. |
| **scratch allocator** | An allocator passed for temporary working memory only, separate from the one that owns the result — e.g. the `scratch` parameter of `proven_map_set_with_scratch()`. |
| **stackless coroutine** | A function that can suspend and resume without a stack of its own: its state lives in a struct you hold. `coro.h` implements it with a switch, so it costs no thread and no allocation. |
| **bounded queue** | A queue with a fixed capacity that refuses when full rather than growing. The job system uses one, so a producer that outruns the workers is told so instead of exhausting memory. |
| **monotonic clock** | A clock that only moves forward, for measuring how long something took. Distinct from the wall clock, which can jump when the system time is corrected — measuring a duration with the wall clock is how a negative elapsed time happens. |

---

## 14. Appendix C: public header map

### How to find things

There are 35 public headers and one umbrella. `#include "proven.h"` pulls in everything and is
what the examples in this manual do; including individual headers is for when you care about
compile time or want the dependency to be visible in the file.

Two things this table tells you that the file names do not:

- **Which chapter documents it.** Every header has exactly one chapter that explains it, and the
  build enforces that every public function is named somewhere under `manual/` — so if a symbol is
  not in the chapter you expect, it is in the manual somewhere and this map says where.
- **Whether it survives a freestanding build.** The headers assigned to Chapter 5 are the hosted
  ones: they need a filesystem, standard streams, a clock, virtual memory or threads. Everything
  else compiles with no operating system. [The freestanding
  guide](manual-freestanding.md) has the authoritative per-module table.

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

## 15. Appendix D: the libc map

If you already write C, this is the fastest way in. Every row links to the chapter that explains
the trade.

| You would write | Use instead | Why it differs |
|---|---|---|
| `malloc` / `free` | `proven_heap_allocator()` + `_create` / `_destroy` | The allocator is a parameter, so the same code runs on an arena or a pool. [Ch 2](manual-02-allocation.md) |
| `strcpy`, `strcat` | `proven_u8str_append`, `_append_grow` | Sizes are known, so overruns are refused instead of performed. [Ch 3](manual-03-strings-text.md) |
| `strlen` | `view.size` | The length is already there; nothing scans. [Ch 3](manual-03-strings-text.md) |
| `strcmp` for equality | `proven_u8str_view_eq` | Works on text with embedded NULs, and does not walk past the end. [Ch 3](manual-03-strings-text.md) |
| `strstr` | `proven_u8str_view_find` | Returns an index or `PROVEN_INDEX_NOT_FOUND`; the search is not naive. [Ch 3](manual-03-strings-text.md) |
| `strtok` | *(no equivalent yet)* | `strtok` mutates its input and cannot be nested. A view-based splitter is designed in `docs/RFC-0002`. |
| `printf` | `proven_println("{}", PROVEN_ARG(x))` | Types come from the arguments, not from the format string. [Ch 8](manual-08-fmt-scan.md) |
| `sprintf` | `proven_u8str_append_fmt` | Writes into a sized destination and refuses to overrun it. [Ch 8](manual-08-fmt-scan.md) |
| `sscanf` | `proven_scan_*`, `proven_scan_fmt` | Reports which field failed and where the cursor stopped. [Ch 8](manual-08-fmt-scan.md) |
| `strtod` | `proven_parse_f64_ascii` | Correctly rounded, locale-independent, no `errno`. [Ch 8](manual-08-fmt-scan.md) |
| `fopen` / `fread` / `fclose` | `proven_fs_open`, `_read`, `_close`, or `proven_fs_read_all_u8str` | Explicit errors, no hidden buffering, one call for whole files. [Ch 5](manual-05-hosted-services.md) |
| `fgets` | `proven_sysio_read_line`, `proven_reader_read_line` | A line that exactly fills the buffer is returned, not lost. [Ch 5](manual-05-hosted-services.md) |
| `qsort` | `proven_array_sort` | Introsort: *O(n log n)* guaranteed, not quicksort's worst case. [Ch 4](manual-04-containers-algorithms.md) |
| `bsearch` | `proven_array_binary_search` | Same shape, same comparator contract. [Ch 4](manual-04-containers-algorithms.md) |
| `rand` | `proven_xoshiro256ss_*` or `proven_random_bytes` | Reproducible and fast, or unpredictable and secure — you pick, deliberately. [Ch 5](manual-05-hosted-services.md) |
| `time` / `clock` | `proven_time_now`, `proven_time_breakdown` | Nanoseconds, explicit about wall clock versus monotonic. [Ch 5](manual-05-hosted-services.md) |
| `assert` | `proven_panic` + a panic hook | Works in freestanding builds and is overridable. [Ch 1](manual-01-foundation.md) |

---

## 16. Where to go next

The chapters are ordered so that each one only needs the ones before it.

| Part | Read | For |
|---|---|---|
| **I** | This chapter | The contracts and the vocabulary |
| **II** | [1](manual-01-foundation.md) → [2](manual-02-allocation.md) → [3](manual-03-strings-text.md) | Errors, memory, and text: what every program uses |
| **III** | [4](manual-04-containers-algorithms.md) | Arrays, maps, lists, rings, sorting, hashing, encoding |
| **IV** | [8](manual-08-fmt-scan.md) | Formatting and scanning in full, once Chapter 3 has introduced them |
| **V** | [5](manual-05-hosted-services.md) | Files, streams, standard I/O, time, randomness, mapping |
| **VI** | [6](manual-06-execution-and-platform.md) → [freestanding](manual-freestanding.md) | Coroutines, jobs, thread-safety, bare metal, cross builds |
| **Appendices** | [A: alias index](manual-07-alias-xcv-index.md), B and D above | Looking things up |

If you are an experienced C programmer in a hurry, read §15 above, then
[Chapter 1](manual-01-foundation.md), then whichever chapter covers the thing you need. If you are
newer, read Parts I and II in order — they are short, and everything later assumes them.
