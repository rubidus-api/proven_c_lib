# Proven C library

**English** · [한국어](README-ko.md)

> A C23 systems library built on one idea: **memory should know where it came from.**

You have finished a C book. You know pointers, `malloc`, `printf`, and `char *`. And then you
wrote your first real program and discovered the part the book did not cover — that `strcpy` has
no idea how big your buffer is, that `malloc` returning `NULL` is something you simply have to
remember, and that `printf("%d", 3.0)` compiles.

`proven` is what those problems look like when someone answers them one at a time, in plain C,
without a framework. It is the everyday layer C projects end up rewriting: allocators you pass in,
strings that carry their own length, containers, formatting and scanning, files, hashing,
randomness — with ownership and failure visible in every signature.

**New here? Start with [Chapter 0](manual/manual-00-start-here.md).** It assumes nothing beyond
one introductory C book, and it is the only document in this repository written to be read rather
than looked up.

- Version: proven_c_lib-v26.07.20f · Standard: C23 · License: MIT
- Repository: https://github.com/rubidus-api/proven_c_lib

---

## The name: provenance

**Provenance** is an art-world word. It means the documented history of an object: where it came
from, who owned it, how it got here. A painting without provenance may be genuine, but nobody can
prove it.

C's memory model uses the word in almost exactly that sense, and it is easier to *see* than to
define. Here is a complete program. Two `int` pointers; the only difference between them is where
each one came from:

```c
#include <stdio.h>
#include <string.h>
int y = 2, x = 1;                       /* the compiler is likely to place these adjacently */
int main(void) {
    int *p = &x + 1;                    /* derived from x — its address is one past x */
    int *q = &y;                        /* derived from y — a different object */
    if (memcmp(&p, &q, sizeof p) != 0)  /* go on only when the two pointers hold the */
        return 0;                       /* identical address, checked bit for bit */
    *p = 11;                            /* store 11 through p */
    printf("*p = %d, *q = %d\n", *p, *q);
}
```

```text
gcc -O1 :  *p = 11, *q = 11     # same address; both read back 11
gcc -O2 :  *p = 11, *q = 2      # same address — yet *p is 11 and *q is 2
```

Read that `-O2` line again. `p` and `q` hold the **identical address** — `memcmp` compared the raw
bytes of the two pointers and only let the program continue when they matched. You store `11`
through `p`. Then dereferencing `p` gives `11` and dereferencing `q` gives `2`. **One address, two
values.** It is not a race, not uninitialised memory, not undefined *output* — the program is
deterministic and prints this every run. The compiler knows `p` was derived from `x`, assumes a
store through it cannot reach `y`, and keeps `y` in a register; so `*p` and `*q` refer to different
things even though the addresses are equal to the last bit.

Notice what is *not* going on here. Forming `&x + 1` is perfectly legal — C specifically lets you
make a pointer one past the end of an object. Nothing is "out of bounds" in a way you would catch
by reading the code. The surprise is entirely in the last step: two pointers can be bit-for-bit
the same address and still not be the same pointer, because each carries the identity of the object
it came from. That identity is its **provenance**, and the compiler treats it as real even where
the address does not distinguish them. (GCC does warn about the `&x + 1` store here — and then
miscompiles it anyway.)

This is not a language-lawyer curiosity. It is where a great deal of optimisation comes from, and
it is a class of bug that a debugger cannot show you, because the miscompilation happens before
the debugger sees anything. The formal model is real, active work: ISO WG14's provenance Technical
Specification ([N2577](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2577.pdf) →
[N3005](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3005.pdf), with the **PNVI-ae-udi** model
the committee voted for). It is a TS pending publication rather than part of C23 proper, but the
direction is settled.

### Two rules, not one: strict aliasing and provenance

It is tempting to lump provenance together with its older, more famous sibling — **strict
aliasing** — as "the compiler being clever." They do share a root cause: C's abstract machine
models memory more strictly than the hardware does. But they are *separate rules* that answer
different questions, and, as we will see, the compiler flag that switches one off leaves the other
fully in force.

| | strict aliasing | provenance |
|---|---|---|
| The question it asks | what **type** is stored at this address? | which **object** may this pointer reach? |
| Age | old — C89/C99 "effective types" | new — WG14 TS (N3005), pending publication |
| What the compiler assumes | two pointers of incompatible types never refer to the same object | a pointer derived from one object cannot access another |
| How you trip it | read memory through the wrong type (type punning) | offset or launder a pointer past its object, then use it where another object sits |
| The blessed escape hatch | access raw bytes through `unsigned char` — this is exactly `proven_byte_t` | keep pointer arithmetic inside one object; don't rebuild pointers from integers |

Both are easy to trip, both are correct at `-O0` and wrong at `-O2` — the worst possible failure
mode, because it survives every test you ran in a debug build. You have already met the provenance
one: the `*p = 11, *q = 2` program at the top of this section is exactly it — two `int *` pointers,
no type punning, wrong only under optimisation. Its strict-aliasing twin trips **without even a
warning**, and it is the shape of every hand-written parser and serialiser: a byte buffer read
through pointers of two different widths.

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(void) {
    void *buf = malloc(8);
    uint32_t *w = buf;      /* the same memory, seen as 32-bit */
    uint16_t *h = buf;      /* the same memory, seen as 16-bit — no cast, no warning */
    *w = 0xAAAAAAAAu;
    *h = 0x1234;            /* change the low half */
    printf("%08x\n", *w);   /* did that write show up? */
    free(buf);
}
```

```text
gcc -O0 :  aaaa1234     # the 16-bit write shows
gcc -O2 :  aaaaaaaa     # the write vanished — the compiler assumed h and w cannot overlap
```

And the detail that proves these are two rules and not one:

```text
                     -O2      -O2 -fno-strict-aliasing
strict-aliasing bug  broken   fixed
provenance bug       broken   STILL broken   <- this was never aliasing
```

`-fno-strict-aliasing` is the flag large C projects — the Linux kernel among them — reach for to
make the first class of bug go away. It does nothing for the second, because provenance is a
different rule with no such off switch. You cannot opt out of it; you can only avoid tripping it.

That is the argument for the whole library in miniature: two invisible rules, no warnings, wrong
only under optimisation, and only one of them with an escape flag. The library's answer is to make
the correct thing the default thing. Raw bytes go through `proven_byte_t`, the one type strict
aliasing exempts, so the first bug above cannot be written through the ordinary API. Lengths travel
with pointers, so arithmetic has no reason to wander off the end of an object. Nothing is laundered
through an integer.

**One honest exception, and it is the reason this library aims where it does.** The intrusive list
recovers the enclosing object from a pointer to one of its members — the classic `container_of`,
here `(type *)((proven_byte_t *)ptr - offsetof(type, member))`. That idiom is everywhere real C
lives, the Linux kernel most of all, and it is precisely the case the strictest readings of the
object model have never comfortably blessed: a pointer whose origin is the *member* is used to reach
the *whole struct*. So `proven` does **not** claim strict-provenance purity — it could not, and
offer intrusive containers at the same time. It defends the settled, universally agreed undefined
behaviour that every C programmer already respects and that sanitizers actually check, and it treats
the unsettled frontier — where a dominant, battle-tested technique sits at odds with the strict
model — honestly, as unsettled. Provenance is the direction the library leans, not a finished
guarantee it pretends to hold. Getting that boundary right, and being plain about which side a given
API is on, is itself part of what the project is trying to work out.

### Why the library is named after it

**The name is `proven` because of *provenance*, not because of *prove*.**

I came to these ideas late. I had written C for a while with the ordinary mental model — memory is
bytes, a pointer is an address — and then I read about strict aliasing, and then about pointer
provenance, and the ground moved. It was not that I had been writing subtly wrong code and got
away with it, though I probably had been. It was the realisation that **C's rules about memory are
considerably stricter than the model I had been carrying in my head**, and that the gap between
the two is exactly where the bugs nobody can reproduce come from.

The obvious response is to learn the rules properly and apply them by hand. I want to be honest
about why that was not enough for me: **I do not know these rules well, and I cannot reliably keep
them in my head while writing ordinary code.** Effective types, when a cast is laundering
provenance, which escape hatches are blessed and which merely happen to work today — this is
genuinely hard, and being told to "just be careful" is not a strategy.

So the response became the opposite one. If I cannot hold the rules reliably, then the rules
should live somewhere other than my attention: **in a library that makes the correct thing the
easy thing, and in a set of conventions that are visible in every signature.** Raw bytes go
through `proven_byte_t`, because that is the type the standard actually blesses for inspecting
representation. Lengths travel with pointers, so there is no arithmetic to get wrong. Sizes go
through checked macros, because silent wraparound is a rule I will forget. None of that requires
me to be careful in the moment; it requires me to be careful once, here.

That is what the name records. Not that the code is proven, but that it is built around
*provenance* — the idea that memory should carry the history of where it came from, and that a
library should not make you track that by hand.

`proven_c_lib` was built with AI as a collaborator, which is part of the same thought: the
explicitness that helps a person who cannot hold the whole memory model in mind is the same
explicitness that lets a language model produce code that is correct for reasons visible in the
call, rather than correct by accident.

**And then the coincidence.** *Proven* also means tested, demonstrated, shown to be true — which
is a better fit than anything I planned, given what the repository turned into: 170 registered
tests, every manual example compiled and run by the build, and documentation gated so it cannot
claim a function that does not exist. The two words are not related. *Provenance* is from Latin
*provenire*, to come forth; *proven* is from *probare*, to test. Two different roots that happen
to land on the same seven letters, and the accident describes the project better than the
intention did.

---

## C is not portable assembly

There is a way of talking about C that treats it as a thin, honest layer over the machine: bytes
are bytes, a pointer is an address, casting is free, and the standard is a formality that gets in
the way of people who know what the hardware does. That view produces clever code — integers and
pointers mixed freely, aliasing tricks, unions used as reinterpret casts — and it was defensible
in 1980, when compilers translated more or less literally.

It is wrong now, and it is worth being precise about *why*, because the reason is not that
compilers became hostile.

**C's abstract machine has always been stricter than the hardware.** Three examples, all from the
standard rather than from anyone's opinion:

- **Effective types and strict aliasing.** An object's stored value may only be accessed through
  an lvalue of compatible type — with a deliberate exception for character types. Memory in C's
  model *has a type*. Assembly has no such concept.
- **Provenance**, above. Hardware sees an address; C sees an address *and where it came from*.
- **Undefined behaviour is not "whatever the machine does".** It means the standard imposes no
  requirement at all, and optimisers are permitted to assume it never happens — which is how UB
  can delete an `if` that was clearly written to prevent it.

So the old programs did not stop being correct. They were never correct; they merely worked,
because nothing was exploiting the freedom the standard had granted all along.

The honest summary is that **C is permissive about what you can write and strict about what it
promises.** The "portable assembly" view conflates the two. And C does provide escape hatches —
inspecting bytes through `unsigned char`, type punning through `memcpy`, `uintptr_t` round trips —
but they are *narrow and specified*, not a general licence.

This library takes that seriously rather than working around it. Raw bytes are `proven_byte_t`
(an alias of `unsigned char`, the one type you may legally inspect any object through). Views
carry a pointer and a length instead of a pointer and a hope. Size arithmetic goes through checked
macros because unsigned overflow wraps silently and legally. None of this is defensive
programming; it is programming the language that is actually specified.

---

## Where systems languages are going

The last decade produced a rough consensus about strings and memory, arrived at independently by
several languages. It is worth seeing, because `proven` is C's version of the same answers.

### Strings: length beside the pointer

The NUL-terminated string was a 1970s decision to save one byte per string. The bill has been
enormous: length is an *O(n)* search, text cannot contain a zero byte, and a missing terminator is
a buffer overrun that nothing detects.

Almost every alternative rediscovers the same fix — **keep the length next to the pointer**:

| | Owning | Borrowed |
|---|---|---|
| **Pascal** (1970s) | length-prefixed string | — |
| **C++17** | `std::string` | `std::string_view` |
| **Rust** | `String` | `&str` |
| **Zig** | `std.ArrayList(u8)` | `[]const u8` (a slice: pointer + length) |
| **Go** | — | `string`, `[]byte` |
| **`proven`** | `proven_u8str_t` | `proven_u8str_view_t` |

Pascal got there first with a length prefix. C++17's `string_view` popularised the *borrowed*
half — the observation that most functions want to *read* text, not own it, and that copying a
string to pass it is the most common needless allocation in a program. Rust and Zig built the same
split into the type system from day one.

The second half of the idea matters as much as the first: **owned and borrowed are different
types**. `char *` means four different things — freshly allocated, pointer into a caller's buffer,
a string literal in read-only memory, a static buffer the next call will overwrite — and the type
cannot tell you which. Splitting them puts the answer in the signature.

### Memory: the allocator is a parameter

`malloc` is a global. Nothing in a signature says whether a function allocates, you cannot change
the strategy for one part of a program, you cannot test the failure path without intercepting it
globally, and you cannot use it at all where there is no heap.

Zig's answer — an `Allocator` interface passed explicitly to anything that allocates — is now the
clearest statement of the alternative, and it is the one `proven` follows. Once allocation is a
parameter, three strategies become interchangeable at the call site:

| | How it works | Free | Use it when |
|---|---|---|---|
| **Heap** | `malloc`/`free` behind the interface | Yes | The general case. |
| **Arena** | Bump a pointer through one block | **No** — a no-op; you reset the whole thing | Many allocations that die together: one request, one frame, one parse. |
| **Pool** | Fixed-size slots with a free list | Yes, into the list | Many objects of one size, created and destroyed continuously. |

An arena turns ten thousand `free` calls into one `reset`, and it is the reason the same code that
runs on Linux runs on a microcontroller with no heap: put an arena over a `static` array and
nothing else changes.

### Modern C already has the tools

The pieces are in the language now, and most C code has not caught up:

- **C99** — designated initializers, compound literals: option structs instead of ten-parameter
  functions.
- **C11** — `_Generic`, which is how `{}` gets a value's type from the argument instead of from a
  format string.
- **C23** — `[[nodiscard]]` (used 172 times here, so the compiler refuses code that drops an
  error), `<stdckdint.h>` for checked arithmetic, `constexpr`, `typeof`, `nullptr`.

Andre Weissflog's [*Modern C for C++ Peeps*](https://floooh.github.io/2019/09/27/modern-c-for-cpp-peeps.html)
and Luca Sas's ACCU 2021 talk **Modern C and What We Can Learn From It**
([video](https://www.youtube.com/watch?v=QpAhX-gsHMs) ·
[slides](https://accu.org/conf-docs/PDFs_2021/luca_sass_modern_c_and_what_we_can_learn_from_it.pdf))
are the best short introductions to this style. This library was read against that talk in
[`docs/RFC-0002`](docs/RFC-0002-view-vocabulary-and-splitting.md) — a useful exercise, because the
result was mostly a list of things already done and one genuine gap.

---

## What this library is for

**To replace the tired parts of the standard library, from the bottom up — without excluding it.**
`strcpy`, `strtok`, `sprintf`, `errno`, `qsort`, `rand`, `atoi`: each is answered here by something
sized, checked, and explicit. But `proven` is not a libc replacement and does not want your
`main`. It has no global state, starts no threads, registers no `atexit` handler, and allocates
nothing you did not hand it an allocator for. Use one module, or all of them, beside whatever else
you already link. [Appendix D](manual/manual-00-start-here.md#7-appendix-d-the-libc-map) maps the
libc call you know to what to use instead — and why it differs.

**To be pleasant for a person and safe for a person working with an AI.** Explicit is more
verbose, and that is the trade. What it buys is that every important fact is *local*: whether a
call allocates is in its parameter list, whether it can fail is in its return type, and whether it
grows is in its name. That is exactly the kind of context that neither a tired human nor a
language model reliably reconstructs from surrounding code — and it is why the documentation is
gated by the build rather than kept up to date by intention.

**To test whether modern C actually holds up.** That is the honest third reason. This is a
working experiment in whether C23, used deliberately and without a framework, can carry a real
support layer — and the experiment is run in public: every design decision is an RFC in
[`docs/`](docs/), including the ones that were wrong and got corrected.

---

## Why people reach for it

- Ownership is explicit, so it is easy to see who allocates and who frees.
- Fallible APIs return results instead of silently hiding failure, and the important ones are
  `[[nodiscard]]` so the compiler will not let you drop the error.
- Operations **refuse rather than truncate**: a path that does not fit is an error, not a shorter
  path that opens a different file.
- Reallocation-style operations stay failure-atomic where documented — a failed grow leaves your
  data intact.
- Borrowed views are a different type from owning objects.
- Decimal/float conversion is correctly rounded (bit-for-bit equal to the host `strtod`/`snprintf`),
  checked exhaustively for `binary32`, and on typical data faster than glibc.
- Hosted OS access is isolated behind the PAL in `platform/`, so freestanding builds drop it
  cleanly.
- The build system is one C file, `nob.c`. No CMake, no Meson, no npm, no external test framework.

That makes it useful for command-line tools, embedded-adjacent code, experiments that may later
need a stricter platform boundary, and C projects that want a compact support layer without giving
up control.

## Quick start

```sh
cc nob.c -o nob     # build the build driver, once
./nob build         # compile everything and run the whole test suite
```

Use a different compiler if needed:

```sh
./nob strict-error -cc clang
```

Common checks:

```sh
./nob release
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob bench-float
./nob freestanding
./nob cross -build-root /home/user/work/build/proven_c_lib
```

Running `./nob` without arguments prints the full command list.

## A small example

This example creates an owned UTF-8 string, appends to it with the formatter, prints it, and then destroys it with the same allocator family.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r =
        proven_u8str_create_from_view(alloc, PROVEN_LIT("hello"));
    if (!proven_is_ok(r.err)) {
        return 1;
    }

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

Build it against the hosted sources:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## Containers without hidden ownership

`proven_array_t` is a growable contiguous array. It stores the allocator trait used for growth and destruction, so the call site stays honest about memory ownership.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
    if (!proven_is_ok(r.err)) {
        return 1;
    }

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

The same pattern appears across strings, maps, buffers, arenas, pools, and filesystem helpers: create with an allocator, check the result, keep borrowed pointers short-lived, and destroy owned objects deliberately.

## Text formatting with bounded input

`PROVEN_ARG("literal")` is convenient for trusted NUL-terminated strings. For text from outside your program, prefer bounded views or bounded C-string arguments so formatting does not search past the bytes you meant to expose.

```c
const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
```

The formatter has three useful modes for owned strings:

- `proven_u8str_append_fmt`: fixed-capacity and atomic.
- `proven_u8str_append_fmt_trunc`: fixed-capacity and truncating.
- `proven_u8str_append_fmt_grow`: allocator-backed and atomic.

## Whole files, and sorting that cannot be made quadratic

Reading a file whole is the operation most programs actually want, so it is one call:

```c
proven_result_u8str_t src = proven_fs_read_all_u8str(alloc, PROVEN_LIT("main.c"));
if (!proven_is_ok(src.err)) return src.err;
proven_u8str_view_t text = proven_u8str_as_view(&src.value);   /* NUL-terminated, no second copy */
proven_u8str_destroy(alloc, &src.value);
```

`proven_fs_read_all` / `proven_fs_read_all_u8str` read to EOF rather than to a pre-measured size. The file's reported size only seeds the capacity, so a regular file still costs one allocation and one pass - but a source whose size cannot be known up front (a pipe, a FIFO, a `/proc` entry) is read correctly rather than coming back empty, and a file that grows mid-read is not silently truncated.

Writing is symmetric. `proven_fs_write_file` creates or truncates; `proven_fs_write_file_atomic` writes a sibling temp file and renames it over the target, so a concurrent reader sees either the whole old file or the whole new one, and the target's permissions are preserved (a `0600` file is not republished as `0644`). It is atomic with respect to readers; durability across power loss is a separate, explicit ask - `proven_fs_sync` (fsync) and `proven_fs_write_file_durable`, which does the three steps in the only order that works: sync the temp file, rename, then sync the directory.

`proven_array_sort` is an introsort, and two of its properties are worth stating because they are the ones that bite:

- **O(n log n) is a guarantee, not a typical case.** A heapsort fallback past a bounded recursion depth is what makes it one. A sort whose worst case can be reached by an adversarial ordering is a denial of service in any program that sorts data it did not author.
- **Duplicate keys are the fast case.** Elements equal to the pivot are collected into a run that is final and never recursed into, so all-equal input costs a single pass. Low-cardinality keys - a status column, an enum, a bucket id - are what callers actually sort by, and they are exactly what a naive partition degrades on.

## Hashes, tokens, and text you can put in a URL

There is no single "hash" and no single "random". Which one is correct depends on what you are doing with the result, and reaching for the wrong one gives you a program that is either needlessly slow or quietly insecure. Both modules are organised so the choice is made once you name the job — and so the wrong choice is hard to make by accident.

| Your job | Use |
|---|---|
| Hash keys into **your own** table (trusted input) | `proven_hash_bytes` — FNV-1a, fast |
| Hash keys from **untrusted** input | `proven_hash_keyed` — SipHash. (`proven_map` already does this for you: string-key maps are HashDoS-resistant by default.) |
| Detect **corruption** on disk or in transit | `proven_crc32` — interoperates with gzip/zlib/PNG |
| **Fingerprint** content — dedup, "same file?" | `proven_sha256` — the only one safe against a *deliberately* forged match |
| A key, a token, a nonce | `proven_random_bytes` (the OS CSPRNG), or a `proven_chacha_rng_t` seeded from it |
| A **reproducible** run — a simulation, a test | `proven_xoshiro256ss_t`. Fast, replays exactly from its seed, and **never** for a secret |

```c
/* A URL-safe session token: strong bytes, then text that needs no escaping. */
proven_byte_t raw[16];
proven_byte_t token[32];
proven_size_t n = 0;

if (proven_random_bytes(raw, sizeof raw) &&
    proven_is_ok(proven_base64url_encode(
        (proven_mem_view_t){ raw, sizeof raw }, token, sizeof token, &n))) {
    proven_println("token: {}", PROVEN_ARG_CSTR_N((const char *)token, n));
}
```

`encode.h` is the other half of this: `hex`, `base64`, and `base64url` (no padding, nothing to escape). The decoders **validate the whole input before writing a byte** — a stray character is `PROVEN_ERR_INVALID_ENCODING`, never a read past the end or a silently short result — and the output size is a function you call, not a number you remember.

The generators and hashes are pure arithmetic, so they work on a bare-metal target too. On a board with no OS, hand the library its hardware entropy once (`proven_random_set_source`) and the cryptographic generator works with no operating system at all.

## Streams: a line from stdin, and printing without a syscall per line

A writer is a byte sink; a reader is a byte source. Both are small vtables passed by value, like the allocator — so one `serialize(writer, value)` works over memory, a file, or a standard stream, and the formatter can be aimed at any of them.

```c
/* Read stdin a line at a time. One buffer, no allocation per line. */
proven_byte_t buf[4096];
proven_sysio_lines_t lines;
if (proven_is_ok(proven_sysio_stdin_lines(&lines,
        (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }))) {
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(line.err)) break;   /* a line longer than `buf` is refused, not truncated */
        proven_println("{}", PROVEN_ARG(line.val));
    }
}
```

The view points *into* your buffer and is valid until the next call — which is what makes a million lines cost one buffer instead of a million allocations. Copy it if you need to keep it.

Buffer the output side and a thousand small prints cost one syscall instead of a thousand — but **you must flush**: there is no hidden global buffer, so there is also no destructor and no `atexit` handler to flush it behind your back. The direct calls (`proven_println`, `proven_eprintln`) stay unbuffered for exactly that reason: what they write is on its way out before they return.

## Correct, fast number conversion

Decimal-to-`double` parsing and `double`/`float`-to-decimal formatting are
correctly rounded (round-to-nearest, ties to even) and produced by an
integer-only engine — no `long double`. The parser is bit-for-bit identical to
the host `strtod`; fixed `%f`/`%e` output matches the host `snprintf`; and a
shortest mode emits the minimal round-trippable string.

```c
#include "proven/scan.h"
#include "proven/float_format.h"

/* Parse: correctly rounded, NUL terminator not required (length-bounded view). */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2"));
double v = proven_scan_f64(&sc).val;            /* 314.159 */

/* Format shortest: the minimal string that round-trips back to the same value. */
char buf[64];
proven_size_t n = 0;
proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);  /* buf == "0.1" */
```

How far this is checked, stated plainly:

- Exhaustive: all 4,278,190,080 finite `binary32` values, zero mismatches against
  the host C library (shortest round-trip + minimality, parser vs `strtod`).
- Large-scale: 2,560,000,000 random `binary64` values, zero mismatches (this sweep
  found and fixed one real formatting defect — see the doc).
- Speed vs glibc 2.41 on this machine (x86-64): faster at parsing typical numbers
  and at shortest formatting (~4-5x); `%f`/`%e` are faster at normal magnitudes and
  slower at extreme magnitudes, where the engine does exact arbitrary-precision work.

Methodology, algorithms, and the full benchmark are in
[`docs/float-correctness-and-performance.md`](docs/float-correctness-and-performance.md).

## The platform boundary

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

Cross compilation shows that headers, source visibility, ABI assumptions, and compile-time platform branches line up. It does not replace runtime tests on the target machine.

## The modules

- Foundation: `types`, `error`, `memory`, `align`, `version`, `config`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- Hashing and encoding: `hash` (FNV-1a, SipHash-2-4, CRC-32, SHA-256), `encode` (hex, Base64, Base64URL).
- Randomness: `random` (xoshiro256** reproducible, ChaCha20 cryptographic, unbiased range/shuffle helpers, and a pluggable entropy source — the OS CSPRNG by default, a board's hardware TRNG on bare metal).
- Hosted services: `fs`, `stream`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## What it is not

`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.

It is also worth saying where the platform boundary **stops**, because otherwise you find out by running into it. The PAL covers memory, the filesystem, time, memory mapping, environment variables, console I/O and threads. It does **not** cover process control (`fork` / `exec` / pipes), terminal control (raw mode, job control), or networking — a program whose substance is one of those will reach for POSIX or Win32 directly, and the "no platform `#ifdef`s" property does not extend to it.

The `hash` module does provide cryptographic and non-cryptographic hashes (SHA-256 alongside FNV, SipHash, and CRC-32) and `random` provides OS-strength bytes, but `proven` is not a cryptography library: deliberate non-goals, so you do not go looking, are signatures, key exchange, password hashing / KDFs, authenticated encryption, and TLS — along with path manipulation, argument parsing, and a logging framework.

## Using it in a real project

These notes come from building a small terminal text editor (`prov`) on top of `proven`. They are one data point, recorded plainly rather than as a sales pitch.

What it bought:

- **Testability.** Threading `proven_allocator_t` through every module let the whole editor core run under ASan/UBSan and leak detection, including randomized edit-vs-model cross-checks. This was the largest practical gain.
- **Fewer unchecked-error and string bugs.** `proven_err_t` with `[[nodiscard]]` makes an ignored error a compile-time signal; bounded/owning `u8str` and the typed formatter removed the `snprintf` format/overflow bug class. The editor core stayed libc-free apart from its `main` entry point.
- **Portability.** The `fs` / `time` / `mmap` / terminal PAL let one codebase build for Linux x86_64/arm64/armhf and Windows x64; a file browser worked on both through `proven_fs_list` / `proven_fs_stat` / `proven_time_breakdown` with no platform `#ifdef`s in the editor.

What you accept, and should not expect:

- **It is not a performance win.** Replacing libc `memmove` with `proven_mem_move` was benchmark-neutral; the editor's 5–50× edit speedups came entirely from its own data structures (incremental line index, piece coalescing), not from the library.
- **Vendoring discipline.** A copy-only, do-not-patch integration means library gaps are filed upstream rather than fixed in place. Early editor work hit three such gaps (a Windows panic-symbol link failure, a missing fixed-capacity string constructor, and absent owner/group in `fs` stat); they were resolved or deferred upstream, not patched downstream.
- **Pervasive coupling.** Passing the allocator and Result types everywhere is a deliberate commitment. It pays off on a long-lived, multi-platform codebase and is heavier than warranted for throwaway code.
- **A young library has gaps.** Expect to occasionally find a missing primitive and to fill or report it. The value is concentrated in safety, testability, and portability — not in convenience or raw speed.

## Documentation

- User manual: `manual/manual.md` (chapters under `manual/`)
- Korean manual (한국어 매뉴얼): `manual-ko/manual-ko.md`
- Freestanding guide: `manual/manual-freestanding.md`
- Float correctness and performance: `docs/float-correctness-and-performance.md`
- Case study, language toolchain: `docs/case-study-lowent.md`
- Primitive throughput (hash/encode/random): `docs/primitives-benchmark.md`
- Test matrix: `TEST.md`
- Changelog: `CHANGELOG.md`
- Contributor checklist: `CHECKLIST.md`

## Status

The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. Sanitizer, regression, freestanding, and cross compile checks are driven by `nob.c`. Optional cross targets are checked when the corresponding toolchains are present. The build driver probes `-std=c23` first and falls back to `-std=c2x` when needed.

Cross compilation is compile-time coverage only. It checks that the headers, PAL splits, and target-specific branches compile together; it does not replace runtime validation on each target.

Borrowed views require caller-managed lifetime. Public structs expose layout for C use, but callers should not manually corrupt them; validation helpers exist for defensive checks.

Strict pointer provenance is not fully claimed here. The library is designed to avoid common C undefined-behavior hazards under documented contracts on conventional hosted-systems C implementations.

## Author and license

Developed by rubidus-api.
Email: rubidus@gmail.com
License: MIT License. See `LICENSE`.
