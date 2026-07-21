# Chapter 1: Foundation — Errors, Types, and Memory Views

**Part II — The vocabulary every program uses. Prerequisite: [Chapter 0](manual-00-start-here.md).**
**After this chapter** you can handle every failure this library reports, describe a region of
memory without losing track of its size, and do arithmetic on sizes that cannot silently wrap.

This chapter covers `types.h`, `error.h`, `memory.h`, `align.h`, `version.h`, and `panic.h` — the
pieces every other chapter is built out of. Nothing here allocates, and nothing here talks to the
operating system. It is the shortest chapter that is genuinely required reading.

## Table of contents

1. [Errors are values](#1-errors-are-values)
2. [The types, and why they are spelled differently](#2-the-types-and-why-they-are-spelled-differently)
3. [Memory views: a pointer and a length, together](#3-memory-views-a-pointer-and-a-length-together)
4. [Size arithmetic that cannot wrap](#4-size-arithmetic-that-cannot-wrap)
5. [Alignment](#5-alignment)
6. [Panic: when there is no one left to return an error to](#6-panic-when-there-is-no-one-left-to-return-an-error-to)
7. [Version macros](#7-version-macros)
8. [Examples and misuse cases](#8-examples-and-misuse-cases)

## 1. Errors are values

### Why this is first

Every other page of this manual assumes you know how failure is reported here, so this is the one
section you cannot skip.

C has never agreed with itself about how a function should report failure. `malloc` returns
`NULL`. `fopen` returns `NULL`. `read` returns `-1`. `strtol` returns `0` and *maybe* sets
`errno`, but only if you cleared `errno` first, because a successful call is permitted to leave
junk in it. `printf` returns a negative number that almost nobody checks. Four conventions, and
the only thing they share is that **ignoring them is silent and legal**:

```text
char *p = malloc(n);
p[0] = 'x';                    /* wrong: malloc returns NULL on failure */

FILE *f = fopen(path, "r");
fread(buf, 1, n, f);           /* wrong: f may be NULL */

long v = strtol(s, NULL, 10);  /* wrong: 0 could be the value or the failure */
```

The deeper problem is not that these are easy to get wrong. It is that **the type does not say
anything is possible.** `char *` is the same type whether or not it can be `NULL`, so nothing —
not the compiler, not a reviewer skimming, not you at 2 a.m. — is prompted to check.

`errno` makes it worse by making the error *global and temporary*. It must be read at exactly the
right moment; any intervening library call may overwrite it; and it is per-thread, so the fix for
one bug class opened another.

### What this library does instead

The error is a return value, and it has a type of its own.

When a function's only outcome is success or failure, it returns `proven_err_t` — a plain enum
where `PROVEN_OK` is `0`:

```text
proven_err_t err = proven_u8str_append(&s, PROVEN_LIT("hello"));
if (!proven_is_ok(err)) { /* handle it */ }
```

When a function has something to give back, the value and the error travel together in one
struct, and **the value is meaningless until the error has been checked**:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
if (!proven_is_ok(s.err)) return;      /* s.value is not a string; do not touch it */
```

This is the same idea as `Result` in Rust or `std::expected` in C++23, expressed in the only way C
allows — a struct returned by value. There is no allocation, no indirection, and no hidden control
flow. What you give up is the ability to skip error handling silently, which is the point.

Three properties are worth stating explicitly because the rest of the library relies on them:

- **`PROVEN_OK` is zero**, so a result struct zero-initialised with `{0}` starts out meaning
  "success, empty".
- **Fallible functions are `[[nodiscard]]`** — a C23 attribute that makes the compiler reject code
  that throws the return value away. You can still ignore an error, but you have to write
  `(void)` in front of the call and thereby say that you meant it.
- **Failure is failure-atomic** unless a function documents otherwise: if an operation fails, it
  changed nothing. A failed append leaves your string exactly as it was.

### Reference

```text
typedef enum {
    PROVEN_OK = 0,
    PROVEN_ERR_NOMEM,
    PROVEN_ERR_OUT_OF_BOUNDS,
    PROVEN_ERR_INVALID_ENCODING,
    PROVEN_ERR_INVALID_ARG,
    PROVEN_ERR_IO,
    PROVEN_ERR_NOT_FOUND,
    PROVEN_ERR_INVALID_STATE,
    PROVEN_ERR_OVERFLOW,
    PROVEN_ERR_UNSUPPORTED,
    PROVEN_ERR_AGAIN,
    PROVEN_ERR_EOF,
    PROVEN_ERR_BUSY,
    PROVEN_ERR_PERMISSION,
    PROVEN_ERR_INVALID_FORMAT
} proven_err_t;
```

| Error | Typical meaning | You usually respond by |
|---|---|---|
| `PROVEN_OK` | Success. | Carrying on. |
| `PROVEN_ERR_NOMEM` | The allocator could not provide memory. | Giving up on this operation; the object is unchanged. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | An index, size, capacity, or range is invalid — including "this does not fit". | Growing the destination, or refusing the input. |
| `PROVEN_ERR_INVALID_ENCODING` | Encoded text failed validation (hex, Base64). | Rejecting the input as malformed. |
| `PROVEN_ERR_INVALID_ARG` | A null pointer, an invalid allocator, an impossible mode, a wrong argument count. | Fixing the call — this one usually means a bug in your code, not bad data. |
| `PROVEN_ERR_IO` | The OS or device failed the operation. | Retrying, or reporting to the user. |
| `PROVEN_ERR_NOT_FOUND` | A file, key, substring, or resource does not exist. | Taking the "absent" branch; often not an error at all. |
| `PROVEN_ERR_INVALID_STATE` | The object's state does not allow this operation. | Fixing the sequence of calls. |
| `PROVEN_ERR_OVERFLOW` | Integer conversion or size arithmetic overflowed. | Refusing the size; see §4. |
| `PROVEN_ERR_UNSUPPORTED` | Unavailable on this platform or build profile. | Taking a different route (e.g. a pipe cannot seek). |
| `PROVEN_ERR_AGAIN` | Not now; retry later. | Retrying, usually after waiting. |
| `PROVEN_ERR_EOF` | End of input. | Stopping the read loop — expected, not exceptional. |
| `PROVEN_ERR_BUSY` | A queue, lock, or resource is busy. | Backing off. |
| `PROVEN_ERR_PERMISSION` | Access denied. | Reporting; retrying will not help. |
| `PROVEN_ERR_INVALID_FORMAT` | A format or scan template is itself malformed. | Fixing the format string — a bug, not bad data. |

```text
static inline int proven_is_ok(proven_err_t err);
#define PROVEN_IS_OK(err) proven_is_ok(err)
```

`proven_is_ok` returns non-zero when `err == PROVEN_OK`. `PROVEN_IS_OK` is the macro spelling for
places where you want it to look like a macro; they do the same thing.

Correct:

```c
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (!proven_is_ok(s.err)) {
    return;   /* nothing was created, so there is nothing to destroy */
}
(void)proven_u8str_append(&s.value, PROVEN_LIT("ready"));
proven_u8str_destroy(alloc, &s.value);
```

Wrong — testing the error as a bare truth value:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (s.err) {
    /* wrong: works today only because PROVEN_OK is 0. It reads as "if there is
       an error" to someone who already knows that, and as nothing at all to
       everyone else. Say what you mean: !proven_is_ok(s.err). */
    return s.err;
}
```

Wrong — the mistake this whole design exists to prevent:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
(void)proven_u8str_append(&s.value, text);   /* wrong: s.value may be garbage */
proven_u8str_destroy(alloc, &s.value);       /* wrong: destroying what was never created */
```

Reading `.value` before checking `.err` is the one error this convention cannot catch for you. The
compiler will not complain — the struct exists either way — so it is worth making a habit of
writing the check on the line immediately after the call.

### Worked example: errors as values

This program is compiled and run by the test suite, so it cannot fall out of date.
It shows the two shapes a fallible call takes — a bare `proven_err_t` when there
is nothing to hand back, and a `proven_result_*_t` when there is — and why the
value inside a result means nothing until you have checked the error beside it.

<!-- example: manual/examples/ex_01_errors.c -->
```c
/*
 * Errors are values in proven: a fallible call hands back either an error or a
 * result, and the compiler makes you look at it. There is nothing to unwind and
 * nothing global to consult.
 */

/* A fallible operation returns proven_err_t when it has no value to give back. */
static proven_err_t write_greeting(proven_u8str_t *out) {
    return proven_u8str_append(out, PROVEN_LIT("hello"));
}

/* When there IS a value, it comes wrapped with the error that guards it. The
 * value is only meaningful once you have checked `err`. */
static proven_result_size_t half(proven_size_t n) {
    proven_result_size_t res = {0};
    if (n % 2 != 0) {
        res.err = PROVEN_ERR_INVALID_ARG;   /* leave res.value at 0: it means nothing */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = n / 2;
    return res;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- checking a plain proven_err_t ------------------------------------ */
    proven_result_u8str_t s = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "creating a 32-byte string should succeed");

    proven_err_t err = write_greeting(&s.value);
    if (!proven_is_ok(err)) {
        /* Nothing was appended, and the string is still valid: proven's
         * grow-style operations are failure-atomic. */
        proven_u8str_destroy(alloc, &s.value);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("hello")),
                    "the greeting should have been appended");

    /* --- checking a result struct ----------------------------------------- */
    proven_result_size_t ok = half(10);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "10 is even, so halving it must succeed");
    EXAMPLE_REQUIRE(ok.value == 5, "half of 10 is 5");

    proven_result_size_t bad = half(7);
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "7 is odd, so halving it must fail");
    /* bad.value is NOT to be read. It is 0 here, but that is an implementation
     * detail of this function, not a promise of the result type. */

    /* --- the error is impossible to drop by accident ----------------------- */
    /* proven_u8str_append is [[nodiscard]], so this would be a compile error:
     *
     *     proven_u8str_append(&s.value, PROVEN_LIT("!"));
     *
     * If you really do want to ignore a failure, you have to say so: */
    (void)proven_u8str_append(&s.value, PROVEN_LIT("!"));

    printf("greeting: %s\n", proven_u8str_as_cstr(&s.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

## 2. The types, and why they are spelled differently

### Why not just use `int` and `char`?

C's built-in types are famously vague. `int` is at least 16 bits, `long` is at least 32, and
`char` may be signed or unsigned depending on the compiler and the target. Code that assumes
otherwise works on your machine and breaks on someone else's, and the break is usually silent.

C99 fixed the width problem with `<stdint.h>` (`int32_t`, `uint64_t` and friends). This library
uses those underneath and gives them its own names, for one reason: **a single spelling that means
the same thing on every target the library supports**, including the freestanding ones where
`<stdint.h>` may be the only header available.

There is a second, subtler reason, and it is the one that matters in practice: `proven_u8` and
`proven_byte_t` are both 8 bits, and they mean different things.

- `proven_u8` is a **number** between 0 and 255. Add it, compare it, print it.
- `proven_byte_t` is a **byte of some object's representation**. It is an alias of
  `unsigned char`, which is the one type C allows you to use to inspect the raw bytes of anything
  without invoking undefined behaviour.

Using `proven_byte_t` for raw memory is not a style preference. C's strict-aliasing rules say that
reading an object through a pointer of the wrong type is undefined, with `unsigned char` as the
explicit exception. Every buffer, view, and hash input in this library is `proven_byte_t` for
exactly that reason.

### Reference

| Name | Meaning | Notes |
|---|---|---|
| `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64` | Signed fixed-width integers | Aliases of the standard fixed-width types. |
| `proven_u8`, `proven_u32`, `proven_u64` | Unsigned fixed-width integers | `proven_u8` is **numeric**; use `proven_byte_t` for raw object bytes. |
| `proven_u16` | 16-bit code unit type | Uses `char16_t` when `<uchar.h>` is available, otherwise `uint_least16_t`. The U16 string APIs use it for UTF-16 code units. |
| `proven_byte_t` | Byte-level object representation | Alias of `unsigned char`; the type you may legally inspect any object's bytes through. |
| `proven_size_t` | Size and index type | Alias of `size_t`. Unsigned — see §4 before you subtract two of them. |
| `proven_ptrdiff_t` | Pointer difference, signed offset | Alias of `ptrdiff_t`. |
| `proven_intptr_t`, `proven_uintptr_t` | Pointer-sized integers | Only for explicit pointer-to-integer work, such as the arena's range checks. |

```text
typedef struct {
    proven_err_t  err;
    proven_size_t value;
} proven_result_size_t;
```

`proven_result_size_t` returns either a size or an error; `value` is valid only when
`err == PROVEN_OK`. It is what you get back from partial appends, file reads and writes, and size
queries — anywhere the answer is "how many", and the operation could fail.

Wrong — treating a byte as a small number's storage:

```text
proven_u8 *raw = (proven_u8 *)&some_struct;   /* wrong type for raw inspection */
for (size_t i = 0; i < sizeof some_struct; i++) hash ^= raw[i];
```

It happens to work on every mainstream compiler, and it is still the wrong type to have written:
`proven_byte_t` is the one with the aliasing guarantee behind it, and using it documents that you
are looking at representation rather than at a number.

## 3. Memory views: a pointer and a length, together

### The problem: pointers forget

A pointer says where something starts and nothing else. Everything C has ever done about the
"where does it end" half has been a convention layered on top:

- **A NUL terminator**, for strings — which costs an *O(n)* scan every time you want the length,
  cannot represent text containing a zero byte, and produces a buffer overrun the moment the
  terminator is missing.
- **A separate length parameter**, for everything else — which works right up until someone passes
  the wrong one, because nothing ties the two arguments together.

```text
void process(const unsigned char *data, size_t len);
process(buf, sizeof buf);        /* fine */
process(buf, sizeof(buf) - 1);   /* also compiles; now the last byte is invisible */
process(other, sizeof buf);      /* also compiles; now it reads past the end */
```

Every one of those is a legal call. The relationship between the pointer and the length exists
only in the programmer's head.

### What this library does instead

A **view** is a pointer and a size in one struct, passed by value. They cannot get separated,
because they are one thing.

```text
typedef struct { const proven_byte_t *ptr; proven_size_t size; } proven_mem_view_t;  /* read-only */
typedef struct {       proven_byte_t *ptr; proven_size_t size; } proven_mem_mut_t;   /* writable  */
typedef struct {       proven_byte_t *ptr; proven_size_t size; } proven_mem_t;       /* owned     */
```

They are two words. Copying one is free, and there is no allocation anywhere in sight. What a view
does *not* do is own anything: it points at bytes that belong to someone else, and it becomes
invalid the moment they do. That rule is contract 2 from [Chapter 0](manual-00-start-here.md#5-the-five-contracts-you-will-meet-on-every-page).

The three spellings differ only in intent, and the intent is the point:

- `proven_mem_view_t` — **borrowed, read-only.** The `const` is on the pointed-to bytes, so the
  compiler enforces it.
- `proven_mem_mut_t` — **borrowed, writable.** You may write through it; you still do not own it.
- `proven_mem_t` — **owned.** Somebody must free this with the allocator that produced it. The
  type does not enforce that; it announces it.

And two result wrappers, for the operations that can fail:

```text
typedef struct { proven_err_t err; proven_mem_mut_t  value; } proven_result_mem_mut_t;
typedef struct { proven_err_t err; proven_mem_view_t value; } proven_result_mem_view_t;
```

`proven_result_mem_mut_t` is what an allocator hands back. `proven_result_mem_view_t` is what a
*checked* slice hands back.

### Slicing, checked and unchecked

Taking a sub-range of a view is the most common thing you will do with one, and it comes in two
forms on purpose:

| API | Purpose | Return |
|---|---|---|
| `proven_mem_view_from_owned(mem)` | Read-only view over an owned block. | `proven_mem_view_t`. |
| `proven_mem_mut_from_owned(mem)` | Writable view over an owned block. | `proven_mem_mut_t`. |
| `proven_mem_view_slice_checked(view, offset, size)` | Sub-view, validated. | `proven_result_mem_view_t`. |
| `proven_mem_view_slice_unchecked(view, offset, size)` | Sub-view, **not** validated. | `proven_mem_view_t`. |
| `proven_mem_mut_slice_checked(mut, offset, size)` | Writable sub-slice, validated. | `proven_result_mem_mut_t`. |
| `proven_mem_mut_slice_unchecked(mut, offset, size)` | Writable sub-slice, **not** validated. | `proven_mem_mut_t`. |
| `proven_range_contains_ptr(base, cap, ptr, size, out_offset)` | Is this pointer range inside that allocation? Integer address comparison, no pointer arithmetic on unrelated pointers. | `_Bool`. |
| `proven_memcmp(s1, s2, size)` | Compare raw memory. | Zero if equal; sign by byte order (unsigned). |
| `proven_mem_copy(dst, dst_cap, src)` | Bounded copy of a view into `dst`. | `PROVEN_OK`; `PROVEN_ERR_OUT_OF_BOUNDS` if it would not fit — **and nothing is written**; `PROVEN_ERR_INVALID_ARG` on a null pointer with a non-zero size. Regions must not overlap. |
| `proven_mem_move(dst, dst_cap, src)` | As `proven_mem_copy`, but the regions may overlap. | As above. |

The checked form's exact behaviour:

- `view.size > 0 && view.ptr == NULL` → `PROVEN_ERR_INVALID_ARG` (a size with no memory behind it
  is a bug, not an empty view).
- The requested range falls outside the view → `PROVEN_ERR_OUT_OF_BOUNDS`.
- `size == 0` → an empty view (`ptr == NULL`, `size == 0`) with `PROVEN_OK`.

**Use the checked form by default.** The unchecked form exists for the case where you have already
proved the range in the lines above — inside a loop whose bounds you computed, for instance — and
the second check would be pure cost. That is a real case, and it is narrower than it feels.

Wrong — unchecked slicing on numbers that came from outside:

```text
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were never validated. This is the buffer
   overrun from the top of section 3, reintroduced through a safer-looking API. */
```

Wrong — rejecting the empty view:

```text
if (!view.ptr) return PROVEN_ERR_INVALID_ARG;   /* wrong: {NULL, 0} is legal */
```

An empty view is a normal value: a zero-length slice, a file with no content, a string that was
just reset. The test for a *malformed* view is a null pointer with a non-zero size:

```c
proven_mem_view_t view = {0};   /* size 0, ptr NULL: a legal empty view */
proven_err_t err = PROVEN_OK;

if (view.size > 0 && !view.ptr) {
    err = PROVEN_ERR_INVALID_ARG;   /* only a null pointer with bytes behind it is a bug */
}
(void)err;
```

## 4. Size arithmetic that cannot wrap

### Why a multiplication needs a function call

`proven_size_t` is unsigned, and unsigned arithmetic in C does not overflow — it *wraps*, silently
and legally, modulo 2^64. That single rule is behind a large fraction of the industry's
allocation bugs:

```text
proven_size_t total = count * sizeof(item_t);   /* wrong: wraps for large count */
void *p = malloc(total);                        /* succeeds, and is far too small */
items[count - 1] = x;                           /* writes way past the end */
```

With `count = 2^61` and a 16-byte item, `total` is `0`. `malloc(0)` succeeds. Every subsequent
write is out of bounds, and nothing anywhere reported an error. The same shape appears whenever a
size is computed from data that came from outside the program — a file header, a network packet,
a user-supplied count.

Subtraction has the mirror problem, and it is easier to hit:

```text
proven_size_t remaining = view.size - offset;   /* wrong when offset > size: a huge number */
```

### What this library does instead

Three macros that do the arithmetic and tell you whether it fit.

```text
#define PROVEN_CKD_ADD(res, a, b)   /* true on overflow */
#define PROVEN_CKD_SUB(res, a, b)
#define PROVEN_CKD_MUL(res, a, b)
```

| Macro | Computes | Returns | Writes `*res` |
|---|---|---|---|
| `PROVEN_CKD_ADD(res, a, b)` | `a + b` | **true on overflow**, false on success | Only when it fits |
| `PROVEN_CKD_SUB(res, a, b)` | `a - b` | **true on overflow** (including unsigned underflow) | Only when it fits |
| `PROVEN_CKD_MUL(res, a, b)` | `a * b` | **true on overflow** | Only when it fits |
| `PROVEN_SIZE_MAX` | — | The largest value a `proven_size_t` can hold | — |

- `res` is a **pointer** to where the answer goes.
- They return **true on overflow**, false on success. That reads backwards the first time; the
  convention comes from C23's `<stdckdint.h>`, which these use when the compiler provides it, and
  from the GCC/Clang `__builtin_*_overflow` family otherwise. Read the call as *"did this
  overflow?"* and it comes out right.
- `PROVEN_SIZE_MAX` is there for the cases where you want to compare against the limit directly
  rather than compute and check.

Correct:

```c
typedef struct { int id; double weight; } item_t;

proven_size_t count = 1024;
proven_size_t total = 0;
proven_err_t err = PROVEN_OK;

if (PROVEN_CKD_MUL(&total, count, sizeof(item_t))) {
    err = PROVEN_ERR_OVERFLOW;   /* refuse to allocate a size that wrapped */
} else {
    proven_result_mem_mut_t block = alloc.alloc_fn(alloc.ctx, total, alignof(item_t));
    err = block.err;
    if (proven_is_ok(err)) {
        alloc.free_fn(alloc.ctx, block.value.ptr);
    }
}
(void)err;
```

Wrong:

```text
proven_size_t total = count * sizeof(item_t);   /* wrong: may wrap silently */
```

You do not need these for every arithmetic expression in your program. You need them wherever a
size is **computed from a value you did not choose yourself** — and that is precisely where the
bugs are.

## 5. Alignment

### What alignment is, and why the library talks about it

Every type in C has an alignment: an address it must start on for the hardware to load it
efficiently, or at all. A `double` typically wants an address divisible by 8. Some architectures
fault on a misaligned load; x86 merely makes it slow; and either way, C says accessing an object
through a misaligned pointer is undefined behaviour.

You have not had to think about this, because `malloc` returns memory suitably aligned for
anything. That guarantee is exactly what disappears the moment you hand out memory yourself — and
handing out memory yourself is what [Chapter 2](manual-02-allocation.md)'s arenas and pools do. An
arena that bumps a pointer by 13 bytes and hands you the result has just given you a misaligned
`double`.

So the helpers here exist for the allocators, and for you if you write one.

### Reference

| Macro | Meaning |
|---|---|
| `PROVEN_DEFAULT_ALIGNMENT` | The default the library uses when you do not say otherwise. Currently 8. |
| `PROVEN_MAX_ALIGN` | `alignof(max_align_t)` — enough for any built-in type. |

| Function | Purpose | Return |
|---|---|---|
| `proven_is_pow2(x)` | Is `x` a non-zero power of two? Alignments must be. | `bool`. |
| `proven_mem_align_up(addr, align)` | Round a size or address up to the next multiple of `align`. | The aligned value, or **0** if `align` is invalid or the rounding overflowed. |
| `proven_uintptr_align_up(addr, align)` | The same, for a `proven_uintptr_t` address. | As above. |

Note the error convention: these return **0 on failure**, because they have no room for an error
code. Zero is never a valid aligned address, so testing for it is unambiguous.

Correct:

```c
typedef struct { int id; double weight; } item_t;

proven_size_t size = 100;
proven_size_t aligned = proven_mem_align_up(size, alignof(item_t));
proven_err_t err = (aligned == 0) ? PROVEN_ERR_OVERFLOW : PROVEN_OK;
(void)err;
```

Wrong — the classic bit-twiddling version:

```text
proven_size_t aligned = (size + align - 1) & ~(align - 1);   /* wrong: may overflow */
```

That line is in a great deal of production code and is correct for every input except the ones
near the top of the range, where `size + align - 1` wraps to a small number and the result is an
address *below* where you started.

## 6. Panic: when there is no one left to return an error to

### Why a library that returns errors has a panic at all

Everything in this chapter has argued that failure should be a value you can inspect. A panic is
the admission that this is not always possible.

Some APIs have no error channel by construction. `proven_arena_alloc_or_panic()` returns a memory
block, not a result, because it exists for the setup phase of a program where an allocation
failure means the program cannot run at all and there is nothing sensible to return. In a
freestanding build there may be no `abort()`, no `stderr`, and nothing to unwind to.

So the library raises a panic by calling `proven_panic()`, which dispatches to a handler you can
replace. It does not call `exit`, print to `stderr`, or assume an operating system.

```text
typedef void (*proven_panic_handler_t)(const char *msg);

void proven_panic(const char *msg);
void proven_set_panic_handler(proven_panic_handler_t handler);
```

| API | Purpose | Notes |
|---|---|---|
| `proven_panic(msg)` | Raise a panic. Dispatches to the installed handler. | Called by the library's `_or_panic` APIs; you can call it too. |
| `proven_set_panic_handler(handler)` | Install a handler. | Pass `NULL` to restore the default. Not thread-safe — install it during start-up, before other threads exist. |
| `proven_panic_handler_t` | `void (*)(const char *msg)` | The handler must not return. |

The default handler traps: `__builtin_trap()` on GCC and Clang, and an infinite loop on any other
compiler. The dispatch goes through a function pointer rather than a weak symbol, so it links the
same way on ELF and on PE/COFF (Windows, mingw-w64) toolchains.

Installing your own (a file-scope function, so this is a listing rather than something you paste
inside another function):

```text
static void my_panic(const char *msg) {
    log_critical(msg);            /* your logger */
    for (;;) {
        /* reset, halt, or wait for a debugger */
    }
}

proven_set_panic_handler(my_panic);   /* pass NULL to restore the default */
```

**A panic handler must not return.** If it does, the `_or_panic` family has nothing to give back
and the validity of its result is not guaranteed.

Wrong — a handler that returns:

```text
static void my_panic(const char *msg) {
    fprintf(stderr, "%s\n", msg);   /* wrong: this returns, and then the caller
                                       proceeds with a block that was never allocated */
}
```

Wrong — reaching for `_or_panic` in ordinary code:

```text
proven_mem_mut_t block = proven_arena_alloc_or_panic(&arena, n);
/* wrong for anything that handles input: an arena that is merely full has just
   killed the process. Use proven_arena_alloc and read the error. */
```

## 7. Version macros

Compile-time identification, for diagnostics and for code that must adapt to the library version.

```text
#define PROVEN_VERSION_STRING "proven_c_lib-v26.07.20e"
#define PROVEN_VERSION_NUM    260720
#define PROVEN_VERSION_SUFFIX "e"
```

`PROVEN_VERSION_STRING` is what you print in a build report or a `--version` flag.
`PROVEN_VERSION_NUM` is `YYMMDD` as an integer, for numeric comparison — `#if PROVEN_VERSION_NUM
>= 260720`. `PROVEN_VERSION_SUFFIX` distinguishes releases made on the same day.

These three are checked against `include/proven/version.h` by the build, so the manual cannot drift
from the header. (It did, once, and this sentence is why it will not again: the string was gated
and the other two were not.)

## 8. Examples and misuse cases

### Safe result handling

```c
proven_byte_t record[16] = {0};
proven_mem_view_t view = { .ptr = record, .size = sizeof record };

proven_result_mem_view_t part = proven_mem_view_slice_checked(view, 4, 8);
if (!proven_is_ok(part.err)) {
    return;   /* the range was not inside the view */
}

proven_byte_t payload[8];
proven_err_t err = proven_mem_copy(payload, sizeof payload, part.value);
(void)err;
```

### Unchecked slicing only after proof

Correct when the caller already proved the range:

```c
proven_byte_t record[16] = {0};
proven_mem_view_t view = { .ptr = record, .size = sizeof record };
proven_size_t offset = 4;
proven_size_t size = 8;

if (offset <= view.size && size <= view.size - offset) {
    proven_mem_view_t part = proven_mem_view_slice_unchecked(view, offset, size);
    (void)part;   /* proved in range: safe to read */
}
```

Note the shape of that test. `size <= view.size - offset` is written that way, rather than the more
natural `offset + size <= view.size`, precisely because of §4: the sum can wrap and the difference
cannot, given the `offset <= view.size` check that precedes it.

Wrong:

```text
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were not validated */
```

### Empty views are allowed

A zero-size view may have a null pointer. Do not reject it blindly.

Wrong:

```text
if (!view.ptr) return PROVEN_ERR_INVALID_ARG; /* wrong for empty views */
```

Better:

```c
proven_mem_view_t view = {0};   /* size 0, ptr NULL: a legal empty view */
proven_err_t err = PROVEN_OK;

if (view.size > 0 && !view.ptr) {
    err = PROVEN_ERR_INVALID_ARG;   /* only a null pointer with bytes behind it is a bug */
}
(void)err;
```

### Where to go next

[Chapter 2](manual-02-allocation.md) is where these types start doing work: allocators produce
`proven_mem_mut_t`, arenas use the alignment helpers from §5, and every growable container in the
library takes the allocator as a parameter for the reasons §1 gave for taking the error as a
return value — the cost should be visible in the signature.
