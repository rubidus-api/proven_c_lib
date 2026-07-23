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
6. [Appendix B: glossary](#6-appendix-b-glossary)
7. [Appendix D: the libc map](#7-appendix-d-the-libc-map)
8. [Where to go next](#8-where-to-go-next)

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
where it is first used, and every one is in the glossary in §6.

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

<!-- example: manual/examples/ex_00_hello.c -->
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
on — and `manual.md` §7 maps every header to the chapter that documents it.

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
everywhere else. `manual.md` §3 gives the formal versions; these are the plain-language ones.

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

`manual.md` §4.2 lists all sixteen of these types. The rule is simple: create it where it lives,
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

## 6. Appendix B: glossary

Terms this manual uses as if they were ordinary words. They are not ordinary C words, and every
one of them is load-bearing.

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

---

## 7. Appendix D: the libc map

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

## 8. Where to go next

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

If you are an experienced C programmer in a hurry, read §7 above, then
[Chapter 1](manual-01-foundation.md), then whichever chapter covers the thing you need. If you are
newer, read Parts I and II in order — they are short, and everything later assumes them.
