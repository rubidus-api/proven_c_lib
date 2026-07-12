# Chapter 8: Formatting and Scanning (v26.07.12g)

This chapter is the detailed reference for `fmt.h` and `scan.h`.
Chapter 3 gives the shorter overview and the everyday examples.
This chapter focuses on exact syntax, parameter shapes, return values, and the places where callers usually make mistakes.

## Table of contents

1. [Design model](#1-design-model)
2. [Formatter data model](#2-formatter-data-model)
3. [Formatter constructors and selectors](#3-formatter-constructors-and-selectors)
4. [Format string grammar](#4-format-string-grammar)
5. [Formatting APIs](#5-formatting-apis)
6. [Console print helpers](#6-console-print-helpers)
7. [Scanner data model](#7-scanner-data-model)
8. [Scanner primitive APIs](#8-scanner-primitive-apis)
9. [Scan argument model](#9-scan-argument-model)
10. [Structural scan grammar](#10-structural-scan-grammar)
11. [Scan formatting APIs](#11-scan-formatting-apis)
11.1. [Scan error code guide and recovery](#111-scan-error-code-guide-and-recovery)
12. [Examples and misuse cases](#12-examples-and-misuse-cases)
13. [Freestanding and build-mode notes](#13-freestanding-and-build-mode-notes)

## 1. Design model

The formatting side and the scanning side solve opposite problems.

- Formatting takes typed values and renders text.
- Scanning takes text and writes typed values.

The project keeps both sides intentionally small:

- formatting supports a compact placeholder language, positional reuse, simple alignment, width, and hex rendering for numeric values;
- scanning supports typed destination pointers, strict placeholder counting, and literal matching with whitespace collapsing;
- neither side tries to become a full `printf` or `scanf` clone.

The practical result is that the APIs are easier to reason about than large general-purpose format engines, but the syntax is still expressive enough for common systems-code tasks.

## 2. Formatter data model

### `proven_fmt_result_t`

```text
typedef struct {
proven_err_t  err;
proven_size_t written;
proven_size_t required;
} proven_fmt_result_t;
```

Meaning:

- `err`: the status code for the operation.
- `written`: bytes actually written into the destination.
- `required`: total bytes needed for the full formatted output.

Use `err` first. The other fields are most useful for truncating or partially successful operations.

A successful result looks like this:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
        &s,
        "hello {}",
        PROVEN_ARG("world")
    );
    if (proven_is_ok(r.err)) {
        proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    }

    proven_u8str_destroy(alloc, &s);
}
```

A truncating result can still tell you how much more space you would have needed.
Here the destination is deliberately too small, so `written` stops short of
`required` - and the string is still valid:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 8);   /* on purpose: too small */
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
        &s,
        "name={} score={}",
        PROVEN_ARG("ada"),
        PROVEN_ARG(42)
    );
    /* r.written is what fit; r.required is what the whole output needed. */
    proven_println("wrote {} of {} bytes",
                   PROVEN_ARG(r.written), PROVEN_ARG(r.required));

    proven_u8str_destroy(alloc, &s);
}
```

### `proven_arg_type_t`

```text
typedef enum {
PROVEN_ARG_NONE,
PROVEN_ARG_I32,
PROVEN_ARG_U32,
PROVEN_ARG_I64,
PROVEN_ARG_U64,
#ifndef PROVEN_FMT_NO_FLOAT
PROVEN_ARG_F64,
#endif
PROVEN_ARG_CSTR,
PROVEN_ARG_STR_VIEW,
PROVEN_ARG_DATETIME,
PROVEN_ARG_PTR,
PROVEN_ARG_FN,
} proven_arg_type_t;
```

The formatter currently recognizes these value classes:

- signed 32-bit integers
- unsigned 32-bit integers
- signed 64-bit integers
- unsigned 64-bit integers
- floating-point values, unless `PROVEN_FMT_NO_FLOAT` is defined
- trusted C strings
- borrowed U8 string views
- datetimes
- object pointers
- function pointers

### `proven_arg_t`

```text
typedef struct {
proven_arg_type_t type;
union {
proven_i32 i32;
proven_u32 u32;
proven_i64 i64;
proven_u64 u64;
double f64;
const char *cstr;
proven_u8str_view_t str_view;
proven_datetime_t datetime;
const void *ptr;
void (*fn)(void);
} value;
} proven_arg_t;
```

The union field must match the selected `type`.
Do not manufacture a `proven_arg_t` by writing a mismatched union field and hoping the formatter will guess.

Wrong:

```text
proven_arg_t arg = {0};
arg.type = PROVEN_ARG_I64;
arg.value.u64 = 123; /* wrong: type and union field do not match */
```

Correct:

```c
proven_arg_t arg = proven_arg_i64(123);
(void)arg;   /* pass it to a formatting macro; PROVEN_ARG(arg) accepts it as-is */
```

## 3. Formatter constructors and selectors

### Constructor summary

| API | Parameters | Returns | Intent |
|---|---|---|---|
| `proven_arg_none(void)` | none | `proven_arg_t` | Internal sentinel value. |
| `proven_arg_i32(int v)` | signed integer | `proven_arg_t` | Render as 32-bit signed integer. |
| `proven_arg_u32(unsigned int v)` | unsigned integer | `proven_arg_t` | Render as 32-bit unsigned integer. |
| `proven_arg_i64(long long v)` | wide signed integer | `proven_arg_t` | Render as 64-bit signed integer. |
| `proven_arg_u64(unsigned long long v)` | wide unsigned integer | `proven_arg_t` | Render as 64-bit unsigned integer. |
| `proven_arg_f64(double v)` | floating-point value | `proven_arg_t` | Render as floating-point text, unless float formatting is disabled. |
| `proven_arg_cstr(const char *v)` | trusted live C string | `proven_arg_t` | Render a NUL-terminated C string. |
| `proven_arg_cstr_n(const char *v, proven_size_t max_len)` | possibly bounded C string | `proven_arg_t` | Render only up to `max_len` while searching for NUL. |
| `proven_arg_str_view(proven_u8str_view_t v)` | borrowed U8 view | `proven_arg_t` | Render a borrowed view without assuming NUL termination. |
| `proven_arg_datetime(proven_datetime_t v)` | datetime value | `proven_arg_t` | Render a datetime using the formatter's datetime rules. |
| `proven_arg_ptr(const void *v)` | object pointer | `proven_arg_t` | Render the pointer value. |
| `proven_arg_fn(void (*v)(void))` | function pointer | `proven_arg_t` | Render the raw function-pointer representation. |
| `proven_arg_ucstr(const unsigned char *v)` | unsigned-char string | `proven_arg_t` | Convenience wrapper around `proven_arg_cstr`. |
| `proven_arg_identity(proven_arg_t v)` | existing argument object | `proven_arg_t` | Pass-through helper. |

### `PROVEN_ARG(x)`

`PROVEN_ARG(x)` is the usual entry point.
It uses `_Generic` so the compiler chooses a constructor from the type of `x`.

The current mapping is:

- `_Bool`, `char`, `signed char`, `short`, `int` -> `proven_arg_i32`
- `unsigned char`, `unsigned short`, `unsigned int` -> `proven_arg_u32`
- `long`, `long long` -> `proven_arg_i64`
- `unsigned long`, `unsigned long long` -> `proven_arg_u64`
- `double`, `float` -> `proven_arg_f64`, unless `PROVEN_FMT_NO_FLOAT` is defined
- `const char *`, `char *` -> `proven_arg_cstr`
- `unsigned char *`, `const unsigned char *` -> `proven_arg_ucstr`
- `void *`, `const void *` -> `proven_arg_ptr`
- `proven_u8str_view_t` -> `proven_arg_str_view`
- `proven_datetime_t` -> `proven_arg_datetime`
- `proven_arg_t` -> `proven_arg_identity`

Important consequence:

- `PROVEN_ARG` does not select function pointers.
- Use `PROVEN_ARG_FN(f)` for function pointers.

Wrong:

```text
void helper(void) {}
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(helper)); /* wrong */
```

Correct:

```c
void helper(void);   /* whatever function you want to print the address of */

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_fmt_result_t r = proven_u8str_append_fmt_grow(alloc, &rs.value, "{}",
                                                         PROVEN_ARG_FN(helper));
    (void)r;
    proven_u8str_destroy(alloc, &rs.value);
}
```

### `PROVEN_ARG_FN(f)`

This macro exists so callers can pass function pointers without casting them through `void *`.
It is a small safety wrapper around `proven_arg_fn`.

Example:

```c
void helper(void);

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &s,
        "callback = {}",
        PROVEN_ARG_FN(helper)
    );
    if (!PROVEN_FMT_IS_OK(r)) {
        proven_eprintln("formatting the callback failed");
    }

    proven_u8str_destroy(alloc, &s);
}
```

### `PROVEN_ARG_CSTR_N(v, max_len)`

This macro is the bounded-string helper.
Use it when the source may not be a fully trusted C string, but you still want C-string-like input handling.

It scans for NUL only up to `max_len` and then formats the bounded prefix as a view.

Good use case - `buf` came off the wire, so nothing promises it is NUL-terminated:

```c
char buf[128];                       /* filled from an untrusted source */
(void)proven_mem_copy(buf, sizeof buf, proven_mem_view_from_u8(PROVEN_LIT("payload")));

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &rs.value,
        "payload={}",
        PROVEN_ARG_CSTR_N(buf, sizeof buf)   /* looks for NUL only within 128 bytes */
    );
    (void)r;
    proven_u8str_destroy(alloc, &rs.value);
}
```

Bad use case:

```text
const char *buf = get_network_buffer();
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(buf)); /* wrong if buf is not trusted */
```

### Float formatting note

If `PROVEN_FMT_NO_FLOAT` is defined, float support is removed from the generic selector and the float constructor is not available.
That is a compile-time configuration choice, not a runtime toggle.

The default `{}` rendering of a `double` produces a fixed six-digit fractional
form for finite values and switches to scientific notation when the magnitude is
too large or too small for the compact form. Unlike earlier versions, this output
is now computed by an **exact, integer-only** engine (no `double`/`long double`
approximation): the digits are correctly rounded (round-half-to-even), so `{}`
matches what `printf("%.6f")` / `%.6e` would print on the same value. For a
shortest round-trippable form, or for a precision other than six, use the
`proven_float_format_*` policy API described below.

### Accuracy and limits

- The default `{}` float output uses six fractional digits, correctly rounded to
  nearest with ties to even (matching glibc `%.6f`/`%.6e`). It is exact at any
  magnitude — there is no `long double` and no precision/magnitude ceiling beyond
  the configurable big-integer capacity.
- For round-trip serialization use the **shortest** policy
  (`proven_float_format_options_shortest()`), which emits the shortest decimal
  string that parses back to the exact same value.
- Decimal-to-double scanning is correctly rounded to IEEE-754 binary64 with
  round-to-nearest, ties-to-even, matching the host `strtod` bit for bit. Values
  below the half-way threshold to the smallest subnormal round to signed zero with
  the input sign preserved.
- The parser is layered `Clinger fast path -> Eisel-Lemire -> exact big-integer
  fallback`; every tier is exact and the fallback is the arbiter, so the result is
  correctly rounded for every input. The cached power tables are generated source
  (`scripts/generate_float_decimal_tables.py`).
- The exact-fallback big-integer capacity is tunable with
  `PROVEN_FLOAT_BIGINT_LIMBS` (see `include/proven/float_config.h`) for embedded
  targets; the fast paths never touch the big integer.
- Validation: the formatter and parser are checked exhaustively over all
  4.28 billion finite `binary32` values and over 2.56 billion random `binary64`
  values against the host C library, with zero mismatches. See
  `docs/float-correctness-and-performance.md` for algorithms, methodology, and a
  benchmark against glibc.

### Inside the engine (conceptual)

You do not need any of this to use the API — it is here for readers who want to
know why the output is trustworthy. Full detail is in
`docs/float-correctness-and-performance.md`.

**Parsing (decimal → binary64), three tiers, fastest first.** The result is always
correctly rounded; the tiers are purely a speed staircase, each one only taken when
it can guarantee the exact answer:

1. *Clinger fast path.* When the value has few significant digits and a small
   exponent, both the significand and `10^exp` are exactly representable as
   `double`, so a single rounded multiply/divide is provably correct. Covers most
   everyday numbers.
2. *Eisel-Lemire.* A 64×128-bit fixed-point multiply by a cached power of ten,
   with a check that the result is far enough from a rounding boundary to be
   certain. If the check is inconclusive (the value sits on a halfway tie), it
   falls through.
3. *Exact big-integer fallback.* Builds the value as a ratio of big integers
   (`significand` and `5^q`/`2^q`) and compares against the candidate `double` and
   its neighbor exactly — this is the arbiter that makes ties and subnormals
   correct. A seeded ±16-ULP window keeps the search to a few comparisons. The
   big-integer capacity is bounded by `PROVEN_FLOAT_BIGINT_LIMBS`; the tier is the
   only one that allocates limbs (on the stack), and the fast paths never reach it.

**Formatting (binary64/32 → decimal).** Two engines, no `long double` anywhere:

- *Shortest* (`proven_float_format_options_shortest()`): a **Grisu3** fast path
  produces the minimal round-trippable digits for almost all inputs in ~90 ns; the
  rare cases where Grisu3 cannot prove minimality fall back to an exact **Dragon4**
  (Burger–Dybvig, round-to-even) core. The result is the unique shortest decimal
  that parses back to the same bits.
- *Fixed `%f` / scientific `%e`* (the default `{}` and the fixed options): an exact
  integer engine scales the value by `10^precision` with big-integer
  `mul_pow5`/shift, does an integer `divmod`, and rounds half-to-even — so it
  matches glibc at any precision and magnitude, with no `2^64`/precision ceiling.
  Extreme exponents do real arbitrary-precision work and are correspondingly slower
  (rare in practice).

### Public float parsing APIs

Three entry points, sharing one correctly-rounded backend:

- `proven_scan_f64(scan)` — parse from a `proven_scan_t` cursor; restores the
  cursor on failure. The native, length-bounded path (no NUL terminator needed).
- `proven_parse_double_ascii(view)` — parse one locale-free ASCII token from a
  `proven_u8str_view_t` and report the consumed length.
- `proven_strtod(nptr, endptr)` — a `strtod`-style convenience wrapper over a C
  string: skips leading ASCII whitespace and reports `endptr`.

#### Worked example: parsing

```c
#include "proven/scan.h"
#include "proven/float_parse.h"

/* (1) Native, view-based parsing through a scanner cursor. */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2 rest"));
proven_result_f64_t r = proven_scan_f64(&sc);
if (r.err == PROVEN_OK) {
    /* r.val == 314.159; the cursor now sits at " rest". */
    proven_println("parsed {}", PROVEN_ARG(r.val));
}

/* (2) strtod-style wrapper for C strings. endptr reports where parsing stopped. */
char *end = NULL;
double v = proven_strtod("  -0.5\t", &end);   /* v == -0.5, *end == '\t' */
(void)v;

/* A trailing exponent marker with no digits stops like strtod: "1e" parses 1,
   leaving endptr at 'e'. Inputs with hundreds of significant digits and extreme
   exponents are still rounded correctly via the exact fallback. */
```

#### Worked example: formatting with the policy API

`proven_float_format_f64_policy` / `_f32_policy` write directly into a caller
buffer and report the number of bytes written. They never allocate.

```c
#include "proven/float_format.h"

char buf[64];
proven_size_t n = 0;

/* Shortest round-trippable form: 0.1 -> "0.1" (not "0.10000000000000001"). */
(void)proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);
/* buf == "0.1", n == 3 */

/* Fixed precision (correctly rounded, round-half-to-even). */
proven_float_format_options_t opt = proven_float_format_options_fixed_default();
opt.precision = 2;
(void)proven_float_format_f64_policy(buf, sizeof buf, 3.14159,
    PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, opt, &n);
/* buf == "3.14" */
```

- `PROVEN_FLOAT_FORMAT_POLICY_RYU` selects shortest output; `DEFAULT`/`SIMPLE`
  select the exact fixed-precision path (`%f`, switching to `%e` for very large or
  very small magnitudes).
- Returns `PROVEN_ERR_OUT_OF_BOUNDS` if the buffer is too small (the value is never
  truncated silently) and `PROVEN_ERR_INVALID_ARG` for an unsupported policy.
- The generic `{}` formatter (`proven_u8str_append_fmt*`) uses the `DEFAULT`
  policy internally, so everyday logging needs no direct call to this API.

## 4. Format string grammar

The formatter accepts a deliberately small grammar.

### Replacement fields

Supported forms:

- `{}`: next positional argument
- `{0}`: first user argument
- `{1}`: second user argument
- `{2}`: third user argument
- and so on

The numbering is user-facing and zero-based.
The implementation stores a hidden sentinel at index 0 and maps user index `0` to internal argument slot `1`.

### Escaped braces

- `{{` becomes a literal `{`
- `}}` becomes a literal `}`

### Alignment and width specifiers

The formatter accepts a compact layout spec after `:`:

```text
{:fillalignwidthx}
```

More precisely:

- optional fill character, followed by alignment
- or alignment by itself
- optional decimal width
- optional trailing `x` for hexadecimal numeric rendering

Supported alignment characters:

- `<` left align
- `>` right align
- `^` center align

Default behavior:

- fill = space
- align = right
- width = 0
- hex mode = off

Examples:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;
    (void)proven_u8str_append_fmt_grow(alloc, &s, "{:0>5}", PROVEN_ARG(42));    /* 00042 */
    (void)proven_u8str_append_fmt_grow(alloc, &s, "{:*^10}", PROVEN_ARG("ok")); /* ****ok**** */
    (void)proven_u8str_append_fmt_grow(alloc, &s, "{:.<10}", PROVEN_ARG("x"));  /* x......... */
    proven_u8str_destroy(alloc, &s);
}
```

### Hex mode

A trailing `x` turns on lowercase hexadecimal rendering for numeric arguments.
The formatter does not use uppercase hex and does not add a `0x` prefix for integer values.

Example:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 16);
if (proven_is_ok(rs.err)) {
    /* renders "0xbeef": the 0x is literal text, the {:x} is the hex conversion */
    (void)proven_u8str_append_fmt_grow(alloc, &rs.value, "0x{:x}", PROVEN_ARG(48879));
    proven_u8str_destroy(alloc, &rs.value);
}
```

For signed integers, the numeric value is rendered through the implementation's unsigned conversion path when hex mode is enabled.
That means negative numbers are shown in their unsigned representation rather than as a signed decimal value.

### Width limit and invalid specs

Width parsing is checked.
Very large widths are rejected instead of silently wrapping.
The current parser also rejects unknown format characters.

Wrong:

```text
proven_u8str_append_fmt_grow(alloc, &s, "{:>9999999999}", PROVEN_ARG(123)); /* width too large */
proven_u8str_append_fmt_grow(alloc, &s, "{:q}", PROVEN_ARG(123));            /* invalid spec */
proven_u8str_append_fmt_grow(alloc, &s, "{", PROVEN_ARG(123));               /* invalid format */
```

### What the formatter does not support

Do not expect these features:

- precision fields
- sign flags
- alternate form flags such as `#`
- locale-aware grouping
- nested format language
- Python-style format type families
- full `printf` compatibility

The project intentionally keeps the language small.

### Type-specific rendering notes

- integers render in base 10 unless hex mode is set
- strings and string views are rendered as byte sequences
- datetimes render using the datetime formatter in `time.h`
- object pointers render as pointer text
- function pointers render as raw representation bytes with a function-pointer prefix

That means a spec like `:x` mostly matters for the numeric types.
For strings, views, and datetimes, width and alignment are the important pieces.

## 5. Formatting APIs

### `proven_u8str_fmt_internal(...)`

```text
proven_fmt_result_t proven_u8str_fmt_internal(
proven_allocator_t alloc,
proven_u8str_t *str,
bool trunc,
const char *fmt,
proven_allocator_t scratch,
const proven_arg_t *args,
proven_size_t args_count
);
```

This is the internal formatting engine.
User code should normally call the public macros instead.

Parameters:

- `alloc`: allocator used when the string must grow
- `str`: destination U8 string
- `trunc`: if true, allow best-effort truncation; if false, keep atomic behavior
- `fmt`: format text
- `scratch`: allocator used for temporary alias-patching when needed
- `args`: array of format arguments, including the hidden sentinel at index 0
- `args_count`: total length of `args`, including the sentinel

Return value:

- a `proven_fmt_result_t`

Important rules:

- `args_count` must match the number of placeholders plus the hidden sentinel
- extra unused arguments are an error
- missing arguments are an error
- if the engine detects aliasing between the destination string and a borrowed view argument, it may use the scratch allocator to preserve failure atomicity

### `proven_u8str_append_fmt(str, fmt, ...)`

Atomic formatting into a fixed-capacity string.
If the result does not fit, the function reports failure and leaves the destination unchanged.

Use this when you want all-or-nothing behavior.

### `proven_u8str_append_fmt_trunc(str, fmt, ...)`

Best-effort formatting.
It writes as much as fits and reports how much was written and how much was required.

Use this when partial output is acceptable.

### `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)`

Growable formatting.
It may reallocate the destination string through the supplied allocator.
On allocation failure, the old string remains valid.

Use this when you want the output to fit without manual capacity planning.

### `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)`

Growable formatting with a separate scratch allocator.
This is useful when the argument list contains string views that may alias the destination buffer and temporary patching is needed.

Use a real allocator for both `alloc` and `scratch`.
Do not pass a dead arena or a one-shot temporary buffer unless its lifetime is long enough for the call.

### Float format policy seam

The public float policy header provides an explicit policy layer for float formatting.
It is intentionally small and keeps the exact fixed-precision formatter as the default path.

The main entry points are:

- `proven_float_format_f64_policy(...)`
- `proven_float_format_f32_policy(...)`
- `proven_float_format_options_fixed_default()`
- `proven_float_format_options_shortest()`

Policy notes:

- `PROVEN_FLOAT_FORMAT_POLICY_DEFAULT` and `PROVEN_FLOAT_FORMAT_POLICY_SIMPLE` select the exact fixed-precision output (correctly rounded, round-half-to-even).
- `PROVEN_FLOAT_FORMAT_POLICY_RYU` is the shortest-output policy branch.
- The policy API returns `PROVEN_ERR_INVALID_ARG` for unsupported enum values.
- The policy API returns `PROVEN_ERR_OUT_OF_BOUNDS` when the caller-provided buffer is too small.

Example:

```c
char buf[128];
proven_size_t written = 0;
proven_err_t err = proven_float_format_f64_policy(
    buf,
    sizeof buf,
    0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(),
    &written
);
if (proven_is_ok(err)) {
    /* buf holds the shortest form that parses back to exactly 0.1 */
    proven_println("{}", PROVEN_ARG_CSTR_N(buf, written));
}
```

### `PROVEN_FMT_IS_OK(res)`

A small helper macro for checking `proven_fmt_result_t`.
Use it when you want the intent to stay compact.

Example:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 32);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &s,
        "name={} score={:0>4}",
        PROVEN_ARG("ada"),
        PROVEN_ARG(42)
    );
    if (!PROVEN_FMT_IS_OK(r)) {
        /* the string is untouched: grow-mode formatting is failure-atomic */
        proven_eprintln("formatting failed");
    }

    proven_u8str_destroy(alloc, &s);
}
```

### Console-style helpers

The `sysio` layer provides print helpers that use the same formatter machinery:

- `proven_print(fmt, ...)`
- `proven_println(fmt, ...)`
- `proven_eprint(fmt, ...)`
- `proven_eprintln(fmt, ...)`

They are convenient when you want formatted output directly to stdout or stderr.
They still return `proven_err_t`, so check the result when the output matters.

Example:

```c
if (!proven_is_ok(proven_println("hello {}", PROVEN_ARG("world")))) {
    /* the write to stdout failed - a closed pipe, a full disk */
    proven_eprintln("stdout is not writable");
}
```

## 6. Console print helpers

This section is intentionally short because the detailed I/O API lives in Chapter 5.
The important point for formatter users is that the console helpers share the same argument rules as the string append APIs.

Common mistakes:

- using `PROVEN_SCAN_ARG` with `proven_println`
- assuming `PROVEN_LIT` is needed for every format string
- forgetting that output functions can still fail

## 7. Scanner data model

The scanner is a cursor over bytes you already have. It does not own the text, it
does not copy it, and it does not read from anywhere - `proven_scan_t` is a view
plus an offset, and that is the whole of it.

```text
typedef struct {
    proven_u8str_view_t view;    /* the bytes being read; not owned */
    proven_size_t       cursor;  /* how far in we are */
} proven_scan_t;
```

Two consequences worth stating, because they are what make this different from
`scanf`:

- **The scanner never allocates and never writes into your input.** A scanned word
  comes back as a `proven_u8str_view_t` pointing *into* the original bytes. It is
  valid exactly as long as those bytes are, and no longer. If it must outlive them,
  copy it with `proven_u8str_create_from_view()`.
- **The cursor is yours.** It is a plain field. You may save it, restore it, or step
  it by hand (§12 does exactly that after `proven_scan_skip_until`). Nothing in the
  scanner is hidden from you, so nothing has to be undone for you.

`proven_scan_init()` normalizes a malformed view (`size > 0` with a null pointer) to
an empty one rather than trusting it, so a scanner built from garbage reads as
end-of-input instead of dereferencing.

Each primitive returns a result struct pairing the value with the error that guards
it: `proven_result_i64_t`, `proven_result_u64_t`, `proven_result_f64_t`,
`proven_result_u8str_view_t`. **The value is meaningless unless the error is
`PROVEN_OK`** - the scanner does not use a sentinel value to mean failure, because
every sentinel is also a legitimate input.

## 8. Scanner primitive APIs

```text
void                       proven_scan_skip_whitespace(proven_scan_t *scan);
proven_result_i64_t        proven_scan_i64(proven_scan_t *scan);
proven_result_u64_t        proven_scan_u64(proven_scan_t *scan);
proven_result_f64_t        proven_scan_f64(proven_scan_t *scan);
proven_result_u8str_view_t proven_scan_str(proven_scan_t *scan);
proven_err_t               proven_scan_skip_until(proven_scan_t *scan, proven_u8str_view_t target);
void                       proven_scan_skip_until_number(proven_scan_t *scan);
```

The value-returning ones are `[[nodiscard]]`: a scan whose result you throw away is
a scan you did not need to make.

### Shared behaviour

- **Leading whitespace is skipped** by every value scanner. You do not need to call
  `proven_scan_skip_whitespace()` first; it exists for when you want to position the
  cursor yourself.
- **Scanning stops at the first byte that cannot belong to the value.** `"12abc"`
  yields `12` and leaves the cursor on the `a`. That is not an error - the scanner
  answered the question you asked and left the rest for whoever asks next.
- **On failure the cursor is restored**, so a failed scan is a non-event: you can
  turn around and parse the same position as something else. §12 does this.

### The integer scanners

| Input | `proven_scan_i64` | Why |
|---|---|---|
| `"42"`, `"+42"`, `"-42"` | `OK` - 42, 42, -42 | a sign is part of the number |
| `"9223372036854775808"` | `PROVEN_ERR_OVERFLOW` | one past `INT64_MAX`; it does **not** wrap |
| `"abc"`, `""` | `PROVEN_ERR_INVALID_ARG` | there is no number here |
| `"0x10"` | `OK` - **0**, cursor at 1 | **decimal only**: a zero, followed by text |

That last row is the one that surprises people. `proven_scan_i64` and
`proven_scan_u64` read decimal. There is no hex, no octal, no base prefix. `0x10` is
the integer zero, and `x10` is still in the input.

`proven_scan_u64` means unsigned: `"-1"` is `PROVEN_ERR_INVALID_ARG`, not a wrap to
`18446744073709551615`. A scanner that quietly turns a negative number into a huge
positive one is how a bounds check gets defeated.

### The float scanner

`proven_scan_f64` routes through the same correctly-rounded decimal engine as the
rest of the library: round-to-nearest, ties-to-even, no `long double` anywhere. It
accepts `nan` and `inf`.

Its two boundary behaviours are **deliberately asymmetric**, and the asymmetry is
the point:

- `"1e309"` gives `PROVEN_ERR_OVERFLOW`. There is no correct finite answer, so it
  refuses rather than handing you an infinity you did not ask for.
- `"1e-400"` gives `PROVEN_OK` and `0.0`, with the sign preserved. Underflow to zero
  *is* the correctly rounded answer. Reporting it as an error would mean reporting
  correct arithmetic as a failure.

### Words and navigation

`proven_scan_str` returns the next whitespace-delimited run as a view into the
input. Nothing left but whitespace is `PROVEN_ERR_INVALID_ARG`.

`proven_scan_skip_until(scan, target)` moves the cursor **to** the target, not past
it - you decide how much of it to consume. If the target is not there the result is
`PROVEN_ERR_NOT_FOUND` **and the cursor does not move**: the scanner does not consume
input it failed to navigate.

`proven_scan_skip_until_number` stops at the first digit, or at a sign immediately
followed by a digit. If there is no number it runs the cursor to the end of the
input - so check `scan.cursor < scan.view.size` before assuming there is something
to read.

## 9. Scan argument model

A scan argument is a **typed pointer to your destination**, selected by the compiler:

```text
PROVEN_SCAN_ARG(&x)     /* _Generic on the pointer type */
```

This is where the scanner differs most sharply from `scanf`. There is no format
letter to get wrong, because there is no format letter at all. `%d` against a `long`,
or `%s` against a buffer that is too small, are not mistakes available here: the
destination's type *is* the specification, and a mismatch is a compile error rather
than a corrupted stack.

Supported destinations: `short`, `unsigned short`, `int`, `unsigned int`, `long`,
`unsigned long`, `long long`, `unsigned long long`, `double`, and
`proven_u8str_view_t`.

`PROVEN_SCAN_ARG_LONG(&x)` and `PROVEN_SCAN_ARG_ULONG(&x)` exist for callers who want
to be explicit at the call site; `PROVEN_SCAN_ARG` already handles `long*` and
`unsigned long*`.

**Narrow destinations are range-checked.** Scanning `"70000"` into a `short` is
`PROVEN_ERR_OVERFLOW`, not a truncated `4464`. The value is parsed at 64 bits and
checked against the destination's range before anything is stored.

The `proven_scan_arg_*` constructors are public if you need to build an argument
array by hand, but the macros are what callers use.

## 10. Structural scan grammar

The scan format string is the formatter's, read backwards:

- a placeholder consumes one argument, in order;
- anything else is a **literal that must match the input exactly**.

There are no specs inside a placeholder on the scanning side. Width, fill and
alignment are formatting concerns; the scanner reads what is there.

Whitespace in the format is not special. The value scanners skip leading whitespace
themselves, so a format with a space between two placeholders and one without parse
`"7 8"` identically - the space in the format matches the space in the input, and had
it not been there, the second scanner would have skipped it anyway.

The number of placeholders must equal the number of arguments. Too few values in the
input is an error; **too many is not** (§11.1).

## 11. Scan formatting APIs

```text
proven_scan_fmt(view, fmt, ...)            /* scan a view from the beginning */
proven_scan_fmt_cursor(&scan, fmt, ...)    /* continue from an existing cursor */
proven_err_t proven_scan_fmt_internal(...) /* what the macros expand to */
```

Use `proven_scan_fmt` for a self-contained line. Use `proven_scan_fmt_cursor` when
the scan is one step in a longer walk over the same input: it advances the cursor you
own, so it mixes freely with the primitives of §8.

### 11.1. Scan error code guide and recovery

| Code | What actually happened | What to do |
|---|---|---|
| `PROVEN_OK` | Every placeholder was filled and every literal matched. | Publish the values. |
| `PROVEN_ERR_INVALID_ARG` | The input is not the shape you asked for - a placeholder had no value to read, or the input ran out. | The line does not match. Report it; do not retry the same shape. |
| `PROVEN_ERR_NOT_FOUND` | A **literal** in the format did not match. | The line has a different shape than expected. Try another format, from a saved cursor. |
| `PROVEN_ERR_OVERFLOW` | A number was well-formed but does not fit the destination. | The input may be valid and your destination too narrow, or the input may be hostile. Those are very different situations - tell them apart before widening the type. |
| `PROVEN_ERR_INVALID_FORMAT` | The format string itself is malformed. | A bug in your code, not in the input. |

**The structural scanner is not transactional, and this is the one that bites.**

When a literal fails to match, the placeholders *before* the mismatch have already
been written through. The call returns `PROVEN_ERR_NOT_FOUND` and your destination
holds a value anyway - `id` below is 7, from a call that failed:

```text
int id = -1;
double ratio = -1.0;
proven_err_t err = proven_scan_fmt(line, "id={} XXX={}",
                                   PROVEN_SCAN_ARG(&id), PROVEN_SCAN_ARG(&ratio));
/* err == PROVEN_ERR_NOT_FOUND, and id == 7: it was written before the failure. */
```

So: **on failure, treat every destination as clobbered.** If you need all-or-nothing,
scan into locals and publish them only once the call has succeeded - the worked
example in §12 shows the shape. Alternatively, save `scan.cursor` before the call and
restore it afterwards. The cursor is a plain field, and that is deliberate.

**Trailing input is not an error.** Scanning one placeholder against `"7 8"` succeeds
with the value 7 and leaves `8` unconsumed. The scanner matched what you asked for and
stopped; it does not police what you did not ask about. If the whole line must be
consumed, check the cursor yourself:

```text
if (scan.cursor != scan.view.size) { /* there is unparsed input left */ }
```

## 12. Examples and misuse cases

### Worked example: formatting a line and scanning it back

Compiled and run by the test suite. It formats into a stack-borrowed string (atomic on
overflow) and into an allocator-backed one (grows), uses a bounded argument for
untrusted bytes, and then parses the line back - with the float round-tripping exactly.

<!-- example: manual/examples/ex_08_fmt_scan.c -->
```c
/*
 * Formatting and scanning are the two halves of the same idea: `{}` renders typed
 * values into text, and the scanner reads text back into typed destinations. Both
 * are type-checked at the call site (_Generic picks the constructor), so there is
 * no format-string/argument mismatch to get wrong at runtime.
 *
 * The choice that matters is *where the bytes go*:
 *
 *   append_fmt       - fixed capacity, atomic. Too long? Nothing is written and
 *                      you get PROVEN_ERR_OUT_OF_BOUNDS. No allocator involved,
 *                      so it works on a stack buffer.
 *   append_fmt_grow  - allocator-backed. Grows to fit; on allocation failure the
 *                      string is left exactly as it was.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- fixed capacity: no allocator, no allocation ------------------------ */
    /* borrow wraps caller memory, so this string lives entirely on the stack. `cap`
     * includes the NUL, so 32 bytes hold 31 of content. Nothing to destroy. */
    proven_byte_t stack_buf[32];
    proven_u8str_t fixed = proven_u8str_borrow(stack_buf, sizeof stack_buf);

    proven_fmt_result_t r = proven_u8str_append_fmt(&fixed, "port={}", PROVEN_ARG(8080));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a short line should fit in 32 bytes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "the fixed-capacity append should have rendered the port");

    /* Atomic means atomic: an append that does not fit changes nothing. The string
     * is still valid and still holds what it held before - no truncated tail to
     * clean up. (Use append_fmt_trunc if a truncated tail is what you want.) */
    proven_fmt_result_t too_long = proven_u8str_append_fmt(
        &fixed, " and a great deal more text than will ever fit here {}", PROVEN_ARG(1));
    EXAMPLE_REQUIRE(too_long.err == PROVEN_ERR_OUT_OF_BOUNDS, "the overlong append must fail");
    EXAMPLE_REQUIRE(too_long.required > too_long.written, "it reports what it would have needed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "a failed atomic append must leave the string untouched");

    /* --- specs: fill, align, width, hex ------------------------------------- */
    proven_result_u8str_t created = proven_u8str_create(alloc, 8);   /* deliberately small */
    EXAMPLE_REQUIRE(proven_is_ok(created.err), "creating the output string should succeed");
    if (!proven_is_ok(created.err)) return 1;
    proven_u8str_t out = created.value;

    /* grow reallocates as needed, so the initial capacity is a hint, not a limit.
     * `{:0>4}` = fill '0', align right, width 4. `{:x}` = lowercase hex, no 0x. */
    r = proven_u8str_append_fmt_grow(alloc, &out, "id={:0>4} tag={:*^9} addr=0x{:x}",
                                     PROVEN_ARG(7),
                                     PROVEN_ARG(PROVEN_LIT("ok")),
                                     PROVEN_ARG(48879));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the growing append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("id=0007 tag=***ok**** addr=0xbeef")),
                    "fill/align/width/hex should render exactly this");
    printf("%s\n", proven_u8str_as_cstr(&out));

    /* --- untrusted text is bounded, never trusted to be NUL-terminated ------ */
    /* PROVEN_ARG on a char* means "walk it until a NUL turns up" - fine for a
     * literal, a buffer-overread for anything that came off a socket. This buffer
     * has no NUL at all; PROVEN_ARG_CSTR_N stops at the length instead, so it reads
     * only what actually exists. Use it for anything you did not create yourself. */
    const char untrusted[4] = {'a', 'b', 'c', 'd'};   /* no terminator, on purpose */
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "payload={}",
                                     PROVEN_ARG_CSTR_N(untrusted, sizeof untrusted));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the bounded append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("payload=abcd")),
                    "the bounded argument should render its whole 4 bytes and stop");

    /* --- format a record, then scan it back --------------------------------- */
    proven_i64 sensor_id = 42;
    double reading = 3.14159;

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "{} {} {}",
                                     PROVEN_ARG(sensor_id),
                                     PROVEN_ARG(PROVEN_LIT("boiler")),
                                     PROVEN_ARG(reading));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "formatting the record should succeed");
    printf("record: %s\n", proven_u8str_as_cstr(&out));

    /* One scanner over one view. Each call advances the cursor past what it
     * consumed, so the calls compose left to right - and each one can fail
     * independently, which is the difference between a parser and a guess. */
    proven_scan_t sc = proven_scan_init(proven_u8str_as_view(&out));

    proven_result_i64_t id = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(id.err), "the first field should parse as an integer");
    EXAMPLE_REQUIRE(id.val == sensor_id, "the integer should round-trip");

    /* scan_str returns a view *into the scanned string* - it copies nothing and
     * owns nothing, so it is only valid while `out` is. */
    proven_result_u8str_view_t name = proven_scan_str(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "the second field should parse as a word");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(name.val, PROVEN_LIT("boiler")), "the name should round-trip");

    proven_result_f64_t temp = proven_scan_f64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(temp.err), "the third field should parse as a float");

    /* Exactly equal, not approximately: the scanner is correctly rounded, so it
     * returns the nearest double to the text - and the text the formatter produced
     * (six fractional digits) names this value unambiguously. Bit-for-bit, this is
     * the same double we started with. For a value that needs more than six
     * fractional digits, format it with the shortest policy
     * (proven_float_format_options_shortest) and the same round-trip holds. */
    EXAMPLE_REQUIRE(temp.val == reading, "the float must round-trip exactly, not approximately");

    /* The input is fully consumed: nothing was silently left on the table. */
    proven_result_i64_t extra = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(!proven_is_ok(extra.err), "there should be nothing left to scan");

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
```

### Worked example: the scanner's error codes, and recovering from them

Compiled and run by the test suite. Every code in the table above is provoked on
purpose here, including the non-transactional failure - because a contract you have
only read is a contract you have not learned.

<!-- example: manual/examples/ex_08_scan_recovery.c -->
```c
/*
 * The scanner's error codes, and how to recover from them.
 *
 * The scanner is not scanf. It never writes through a pointer it was not given,
 * it never guesses a width, and it tells you which of several different things
 * went wrong. That last part only helps if you know what the codes mean - so
 * this program provokes each one on purpose.
 */

static proven_u8str_view_t v(const char *s) {
    return proven_u8str_view_from_cstr(s);
}

int main(void) {
    /* --- the primitives restore the cursor when they fail ----------------- */
    /* A failed scan is a non-event: the cursor is where it was, so you can try
     * to parse the same position as something else. */
    {
        proven_scan_t sc = proven_scan_init(v("abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG, "'abc' is not an integer");
        EXAMPLE_REQUIRE(sc.cursor == 0, "a failed integer scan leaves the cursor alone");

        /* So the same position can be read as a word instead. */
        proven_result_u8str_view_t w = proven_scan_str(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(w.err) && proven_u8str_view_eq(w.val, PROVEN_LIT("abc")),
                        "the same bytes parse fine as a word");
    }

    /* --- a number that does not fit is OVERFLOW, not a wrapped value ------ */
    {
        proven_scan_t sc = proven_scan_init(v("9223372036854775808"));   /* INT64_MAX + 1 */
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_OVERFLOW, "one past INT64_MAX must not wrap");
        EXAMPLE_REQUIRE(sc.cursor == 0, "the cursor is restored on overflow too");
    }

    /* --- but a float that underflows is NOT an error ---------------------- */
    /* Too large is OVERFLOW; too small is zero, with the sign kept. That
     * asymmetry is deliberate - underflow to zero is the correctly rounded
     * answer, while overflow has no correct finite answer at all. */
    {
        proven_scan_t big = proven_scan_init(v("1e309"));
        proven_result_f64_t b = proven_scan_f64(&big);
        EXAMPLE_REQUIRE(b.err == PROVEN_ERR_OVERFLOW, "1e309 does not fit a double");

        proven_scan_t tiny = proven_scan_init(v("-1e-400"));
        proven_result_f64_t t = proven_scan_f64(&tiny);
        EXAMPLE_REQUIRE(proven_is_ok(t.err), "1e-400 underflows, which is not an error");
        EXAMPLE_REQUIRE(t.val == 0.0, "it rounds to zero");
    }

    /* --- the integer scanners are decimal only ---------------------------- */
    /* "0x10" is not sixteen. It is a zero, followed by text the scanner has not
     * been asked to look at. This surprises people, so it is worth knowing. */
    {
        proven_scan_t sc = proven_scan_init(v("0x10"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 0, "0x10 scans as the integer 0");
        EXAMPLE_REQUIRE(sc.cursor == 1, "and the cursor stops before the 'x'");
    }

    /* --- scanning stops at the first byte that cannot belong to the value -- */
    {
        proven_scan_t sc = proven_scan_init(v("12abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 12, "12abc yields 12");
        EXAMPLE_REQUIRE(sc.cursor == 2, "and leaves 'abc' for whoever asks next");
    }

    /* --- unsigned means unsigned ------------------------------------------ */
    {
        proven_scan_t sc = proven_scan_init(v("-1"));
        proven_result_u64_t n = proven_scan_u64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG,
                        "-1 is rejected rather than wrapping to a huge unsigned value");
    }

    /* --- navigating to a value: skip_until ------------------------------- */
    /* skip_until leaves the cursor ON the target, not past it, so you decide
     * how much of it to consume. */
    {
        proven_scan_t sc = proven_scan_init(v("port=8080"));
        proven_err_t err = proven_scan_skip_until(&sc, PROVEN_LIT("="));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the '=' is there");
        EXAMPLE_REQUIRE(sc.cursor == 4, "the cursor sits on the '=' itself");

        ++sc.cursor;                                  /* step over it */
        proven_result_i64_t port = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(port.err) && port.val == 8080, "the port parses");

        /* Not finding it is NOT_FOUND, and the cursor does not move - the
         * scanner does not consume the input it failed to navigate. */
        proven_scan_t sc2 = proven_scan_init(v("port=8080"));
        proven_err_t missing = proven_scan_skip_until(&sc2, PROVEN_LIT("#"));
        EXAMPLE_REQUIRE(missing == PROVEN_ERR_NOT_FOUND, "there is no '#'");
        EXAMPLE_REQUIRE(sc2.cursor == 0, "and the cursor stayed put");
    }

    /* --- the structural scanner ------------------------------------------- */
    {
        int id = 0;
        double ratio = 0.0;
        proven_u8str_view_t name = {0};

        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5 name=ada"),
                                           "id={} ratio={} name={}",
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio),
                                           PROVEN_SCAN_ARG(&name));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line matches the shape");
        EXAMPLE_REQUIRE(id == 7 && ratio == 0.5, "the values land in the right places");
        EXAMPLE_REQUIRE(proven_u8str_view_eq(name, PROVEN_LIT("ada")), "including the word");
    }

    /* --- the structural scanner is NOT transactional ---------------------- */
    /*
     * This is the one that bites. When a literal fails to match, the scan
     * returns an error - but the placeholders BEFORE the mismatch have already
     * been written through. `id` is 7 even though the call failed.
     *
     * So: on failure, treat every destination as clobbered. If you need
     * all-or-nothing, scan into locals and only publish them once the call
     * succeeded, which is what the code below does.
     */
    {
        int id = -1;
        double ratio = -1.0;
        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5"),
                                           "id={} XXX={}",       /* the literal is wrong */
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NOT_FOUND, "the literal 'XXX=' is not in the input");
        EXAMPLE_REQUIRE(id == 7, "and yet id was already written: the scan is not atomic");

        /* The safe shape: scan into locals, publish on success. */
        int good_id = 0;
        double good_ratio = 0.0;
        int published_id = -1;
        proven_err_t ok = proven_scan_fmt(v("id=7 ratio=0.5"), "id={} ratio={}",
                                          PROVEN_SCAN_ARG(&good_id), PROVEN_SCAN_ARG(&good_ratio));
        if (proven_is_ok(ok)) published_id = good_id;
        EXAMPLE_REQUIRE(published_id == 7, "publish only what a successful scan produced");
    }

    /* --- running out of input, and having input left over ------------------ */
    {
        int a = 0, b = 0;
        proven_err_t short_input = proven_scan_fmt(v("5"), "{} {}",
                                                   PROVEN_SCAN_ARG(&a), PROVEN_SCAN_ARG(&b));
        EXAMPLE_REQUIRE(!proven_is_ok(short_input), "two placeholders, one value: that fails");

        /* Trailing input is NOT an error. The scanner matched what you asked for
         * and stopped; it does not police what you did not ask about. If the
         * whole line must be consumed, check that yourself. */
        int only = 0;
        proven_scan_t sc = proven_scan_init(v("7 8"));
        proven_err_t err = proven_scan_fmt_cursor(&sc, "{}", PROVEN_SCAN_ARG(&only));
        EXAMPLE_REQUIRE(proven_is_ok(err) && only == 7, "the first value scans");
        EXAMPLE_REQUIRE(sc.cursor < sc.view.size, "and '8' is still sitting there, unconsumed");
    }

    /* --- narrow destinations are range-checked ---------------------------- */
    {
        short small = 0;
        proven_err_t err = proven_scan_fmt(v("70000"), "{}", PROVEN_SCAN_ARG(&small));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_OVERFLOW,
                        "70000 does not fit a short, and the scanner says so rather than truncating");
    }

    return EXAMPLE_OK();
}
```

### Misuse: assuming `0x10` is sixteen

It is zero. The integer scanners are decimal only, and `x10` is still in the input. If
you need hex, you are writing that digit loop yourself.

### Misuse: treating trailing input as an error

It is not one. One placeholder against `"7 8"` succeeds. Check the cursor if you care.

### Misuse: trusting destinations after a failed scan

They are clobbered. See §11.1.

### Misuse: keeping a scanned word after its input is gone

`proven_scan_str` returns a **view into the input**, not a copy. When the buffer goes,
so does the word. Copy it with `proven_u8str_create_from_view()` if it has to outlive
the bytes it came from.

## 13. Freestanding and build-mode notes

The scanner is core: it does no I/O, allocates nothing, and touches no platform layer,
so it is available in a freestanding build exactly as it is in a hosted one.

The one build-mode dependency is float. The freestanding build sets
`PROVEN_FMT_NO_FLOAT` (which compiles out the float *formatter*) along with
`PROVEN_NO_U16STR`. `proven_scan_f64` pulls in the decimal parsing engine
(`float_parse.c` and `float_decimal.c`), which is integer-only - no `long double`, no
libm, no soft-float helper calls - but it is not free in code size. On a target where
that matters and you do not need to read floats, simply do not call it: nothing else
in the scanner references the float path.

`proven_sysio_scanner_*` (Chapter 5) is a **different** thing: a buffered scanner over
a file, hosted-only, because it does I/O. The scanner described in this chapter reads
bytes you already have.
