# Chapter 8: Formatting and Scanning (v26.09.02b)

**Part IV — Text in and out. Prerequisite: [Chapter 3](manual-03-strings-text.md) §3–§4.**
**After this chapter** you can format any value, teach the formatter a type of your own, parse
input with the cursor under your control, and recover from a partial parse.

This chapter is the **reference half** of the text material; [Chapter 3](manual-03-strings-text.md)
is the tutorial half. Chapter 3 introduces the formatter and the scanner beside the string types
and gives you enough to be productive. This chapter gives the exact syntax, every parameter shape,
every return value, and the places where callers usually go wrong. If you are meeting `{}` for the
first time, read Chapter 3 first — this one assumes you have.

## Table of contents

1. [Design model](#1-design-model)
2. [Formatter data model](#2-formatter-data-model)
3. [Formatter constructors and selectors](#3-formatter-constructors-and-selectors)
4. [Format string grammar](#4-format-string-grammar)
5. [Formatting APIs](#5-formatting-apis)
5.1. [Formatting a type of your own](#51-formatting-a-type-of-your-own)
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

### Why the type is never written twice

Both halves of this chapter exist to eliminate one thing: a place where the programmer states a
type that the compiler cannot check against the value.

`printf("%d", x)` states it twice — once as `%d`, once by passing `x` — and varargs erases the
second, so nothing can compare them. `scanf("%d", &x)` is worse: the format decides both how to
parse *and* what to write through the pointer, so a mismatch corrupts memory rather than printing
nonsense. Both are the same defect from opposite directions, and both compile silently.

Here the placeholder carries **no type at all**. `{}` marks a position; the type comes from
`PROVEN_ARG(x)` on the formatting side and `PROVEN_SCAN_ARG(&x)` on the scanning side, both
resolved by `_Generic` at compile time against the argument's static type. The spec after `:`
controls presentation only — width, fill, alignment, precision, base — never interpretation. There
is no `%d`-versus-`double` to get wrong because you never wrote a type in the string.

The second design decision is that **neither side owns a buffer**. Formatting appends into a
destination you supply and refuses when it does not fit; scanning reads from a view you supply and
moves a cursor you can read. Nothing here allocates unless you hand it an allocator, which is what
lets the whole chapter work in a freestanding build (§13).

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
| `proven_arg_bool(bool v)` | boolean | `proven_arg_t` | Render `true` / `false` as words, not as `1` / `0`. |
| `proven_arg_char(char v)` | a character | `proven_arg_t` | Render the **character**. This is why a `char` VARIABLE renders as a character while the literal `PROVEN_ARG('Z')` still renders `90`: in C, `'Z'` has type `int`, and no amount of `_Generic` can tell it apart from the number 90. |
| `proven_arg_custom(const void *v, proven_fmt_custom_fn fn)` | any type at all | `proven_arg_t` | Render a type the library has never heard of, through a function you supply. See [Formatting a user-defined type](#51-formatting-a-type-of-your-own). |

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

/* Always scientific - this is what {:e} selects. Six fractional digits by default,
   a signed two-digit-minimum exponent: exactly what printf's %e prints. */
proven_float_format_options_t sci = proven_float_format_options_scientific();
sci.precision = 2;
(void)proven_float_format_f64_policy(buf, sizeof buf, 42.0,
    PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, sci, &n);
/* buf == "4.20e+01" - where fixed would give "42.00" and shortest "42" */
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

### The layout spec

```text
{:[[fill]align][sign][#][0][width][.precision][type]}
```

Every part is optional, and the order is the one the rest of the world uses, so a
spec copied from Python or Rust means here what it means there.

| Part | Values | What it does |
|---|---|---|
| `align` | `<` `>` `^` | left, right, centre. Default `>`. |
| `fill` | any character, before an align | the pad character. Default space. |
| `sign` | `+` or a space | force a sign on a non-negative number, or reserve a space for one. |
| `#` | | alternate form: `0x`, `0X`, `0o`, `0b` prefixes. Integers only. |
| `0` | | zero-fill. `{:08}` on 42 is `00000042`. |
| `width` | digits, up to 10000 | minimum field width. |
| `.precision` | `.N`, up to 60 | decimals. **Floats only.** |
| `type` | `x X o b d` (int), `f g e` (float) | base and case; `f` fixed, `g` shortest round-trip, `e` scientific (printf `%e`). |

Two things are worth knowing because they used to be false:

- **A leading `0` is zero-fill, not the first digit of the width.** `{:08}` produced
  `"      42"` — space-padded, no error — until v26.07.12f. An explicit fill still
  wins: `{:*>08}` pads with `*`.
- **Zero-padding goes between the sign and the digits.** `{:+08}` on 42 is
  `+0000042`, never `0000+42`. Padding is part of the number, and a number's sign
  comes first.

### Floats

`{}` gives six decimals, correctly rounded. `{:.3}` gives three, `{:.0}` gives none,
`{:e}` forces scientific notation - mantissa, `precision` fractional digits (six by
default), a signed two-digit-minimum exponent, correctly rounded, exactly as printf's `%e` -
which is the form `{:f}` and `{:g}` do not reach: `{:f}` never shows an exponent, and `{:g}`
uses one only when it is shorter. `{:f}` forces the fixed form, and `{:g}` gives the shortest representation that
round-trips.

Until v26.07.12i **none of these existed**: every float came out with exactly six
decimals, forever. The exact engine could always do all of it — the `{}` grammar
simply could not reach it. The visible cost was that a float column could not be
aligned, because `12.5` rendered nine characters wide and `100.0` rendered ten:

```c
proven_byte_t buf[64];
proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);
(void)proven_u8str_append_fmt(&line, "{:>9.2}", PROVEN_ARG(12.5));    /* "    12.50" */
```

### A spec the argument cannot honour is an error

`{:x}` on a double, `{:.2}` on an integer, `{:f}` on an integer, `{:#}` on a string:
all `PROVEN_ERR_INVALID_FORMAT`.

They used to be *ignored* — `{:x}` on a double printed `3.500000` and reported
success. The caller asked for something, got something else, and was told it had
worked. That is the worst available outcome, and it is worse than refusing.

### `char` and `bool`

`PROVEN_ARG('Z')` renders `Z`, and a `bool` renders `true` or `false`. Both used to
go through the integer path, so a character printed as `90` and there was no way to
emit one at all — the ASCII column of a hex dump had to be built by hand in a
separate buffer and passed as a string. An uppercase hex dump is now one loop:

```c
proven_byte_t hexbuf[64];
proven_u8str_t hexline = proven_u8str_borrow(hexbuf, sizeof hexbuf);
unsigned char byte = 0xde;
(void)proven_u8str_append_fmt(&hexline, "{:02X} ", PROVEN_ARG((unsigned)byte));
(void)proven_u8str_append_fmt(&hexline, "{}", PROVEN_ARG((char)'.'));
```

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

### 5.1. Formatting a type of your own

`PROVEN_ARG` is built on `_Generic`, and `_Generic` can only dispatch on types it was
told about at compile time. It cannot be told about yours. So the formatter's argument
set — integers, floats, strings, pointers, datetimes — was a **closed** one: a `rect_t`,
a `uuid_t`, a `vec3_t` could not be passed to `{}` at all.

The two ways around it were both bad. Pre-format the value into a scratch string and
pass *that*: an allocation and a copy for every value, in the logging path, which is the
one path that must keep working when allocation is exactly what has failed. Or print the
fields one at a time and give up on ever aligning the column.

`PROVEN_ARG_OF(&obj, render)` is the door:

```c
proven_err_t render(proven_fmt_sink_t out, const void *obj);
```

Three things follow from the shape of that signature:

- **The renderer gets a sink, not a buffer.** It does not need to know how much room
  there is, and it cannot overflow anything. It emits with `proven_fmt_put`.
- **It composes.** The renderer may call the formatter again — into a stack buffer,
  with no allocator — and hand the result to the sink. A type whose fields are
  themselves user types nests naturally.
- **Width, fill and alignment work.** The formatter runs the renderer **twice**: once
  against a counting sink to learn how wide the output is, then once for real, with the
  padding applied around it. This is why `{:>10}` lines up a column of your type exactly
  as it lines up a column of ints — and it is why the renderer must be **deterministic**
  and must not mutate `obj`. If the two passes disagree, the formatter returns
  `PROVEN_ERR_INVALID_ARG` rather than emit a field of the wrong width into an aligned
  column and let you find out later.

What the formatter will **not** do is guess. `{:x}` on a rectangle, `{:.2}` on a UUID,
`{:+}` on a matrix — the library has no idea what those would mean for your type, and
so it refuses them with `PROVEN_ERR_INVALID_FORMAT`. Inventing a plausible answer and
reporting success is how a formatter starts lying; a type letter you did not ask for is
not better than an error.

Compiled and run by the test suite:

<!-- example: manual/examples/ex_08_fmt_custom.c -->
```c
/*
 * Formatting a type the library has never heard of.
 *
 * `PROVEN_ARG` is built on `_Generic`, which can only dispatch on types it was told
 * about - and it cannot be told about yours. So before `PROVEN_ARG_OF` existed, a
 * `rect_t` simply could not be printed. You either pre-formatted it into a scratch
 * string and passed that (an allocation and a copy per value, in the logging path,
 * which is the one path that must not allocate), or you printed the fields one at a
 * time and gave up on ever aligning the column.
 *
 * A renderer receives a *sink*, not a buffer. That is what makes it compose: it can
 * call the formatter again, and its output is just bytes going somewhere. And because
 * the formatter measures the renderer's output before emitting it - by running it once
 * against a counting sink - width, fill and alignment work on a user type exactly as
 * they do on an int.
 */

typedef struct { int w, h; } rect_t;

static proven_err_t render_rect(proven_fmt_sink_t out, const void *obj) {
    const rect_t *r = (const rect_t *)obj;

    /* Compose: the formatter, into a stack buffer, no allocator anywhere. */
    proven_byte_t tmp[64];
    proven_u8str_t s = proven_u8str_borrow(tmp, sizeof tmp);
    proven_fmt_result_t f = proven_u8str_append_fmt(&s, "{}x{}",
                                                    PROVEN_ARG(r->w), PROVEN_ARG(r->h));
    if (!PROVEN_FMT_IS_OK(f)) return f.err;

    return proven_fmt_put(out, proven_u8str_as_view(&s));
}

int main(void) {
    rect_t a = { .w = 1920, .h = 1080 };
    rect_t b = { .w = 640,  .h = 480  };

    proven_byte_t buf[128];
    proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);

    /* Just like any other argument. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&line, "mode={}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a user type should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line), PROVEN_LIT("mode=1920x1080")),
                    "the renderer's bytes should be what came out");

    /* And it aligns, which is the whole reason the formatter measures it first: a
     * column of user-defined values lines up like a column of anything else. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "[{:>10}]\n[{:>10}]",
                                PROVEN_ARG_OF(&a, render_rect),
                                PROVEN_ARG_OF(&b, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "two user types should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line),
                                         PROVEN_LIT("[ 1920x1080]\n[   640x480]")),
                    "both rows should be right-aligned to the same width");

    /* A spec the library cannot interpret for your type is refused, not guessed at.
     * `{:x}` on a rectangle has no meaning, and answering it with something plausible
     * while reporting success is how a formatter starts lying to you. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "{:x}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_INVALID_FORMAT,
                    "a type letter on a user type should be an error");

    return EXAMPLE_OK();
}
```

## 6. Console print helpers

### What `proven_println` is, and what it costs

`proven_println("{}", PROVEN_ARG(x))` is the shortest way to get text out of a program, and it is
the right tool for a diagnostic, a one-off tool, or a program whose output is a few lines.

It is the wrong tool for a loop, and the reason is worth stating because it is invisible at the
call site: **each call is its own write syscall.** Ten thousand `proven_println` calls are ten
thousand syscalls, which is roughly two orders of magnitude more expensive than the formatting
itself. `printf` hides this behind a buffer that libc flushes for you; this library does not
buffer behind your back, because a buffer you did not ask for is a buffer that surprises you at
exit, on a crash, or when two writers interleave.

**One allocation caveat.** The line is formatted into a 512-byte stack buffer, so a typical line
costs zero allocations. A line that does *not* fit falls back to the global heap for that call
rather than being refused — refusing to print something because it is long would be worse. This is
the one place in the library where a call with no allocator parameter can still allocate, and it is
bounded to the over-long case. If you need the guarantee that nothing allocates, format into your
own buffer and write that.

When output volume matters, take a buffered writer from `proven_sysio_stdout_buffered` and format
into that — same argument rules, one syscall per flush instead of one per line.
[Chapter 5](manual-05-hosted-services.md) covers the stream layer; this section is short because
that is where the I/O API is documented.

Wrong — reaching for the scanning argument constructor when printing:

```text
proven_println("{}", PROVEN_SCAN_ARG(&x));   /* wrong: that builds a scan destination */
```

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

On a **complete view**, "the input ran out" and "the input is wrong" are the same fact -
there is no more input, so a number cut off at the end really is malformed. Over a
**stream** they are opposite facts, and the difference decides whether you wait or report
an error. The scanner sets `proven_scan_t::needs_more` when the parse ran off the end of
what it had, and the buffered scanner uses exactly that to refill and retry: a pipe that
delivers `-` and then, a moment later, `12` scans as `-12`. Before, it was a malformed
number. A wrong byte that is actually *present* - a letter where a digit belongs - is
still an error, and no amount of further input will change that; the scanner does not wait
for it.

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

### Why parsing with a format string is more dangerous than printing with one

Formatting with a bad format string produces wrong output. **Parsing with one corrupts memory**,
because the format decides what to write through the pointers you passed. `sscanf("%d", &c)` where
`c` is a `char` writes four bytes into a one-byte object, and nothing in the call says so.

That asymmetry shapes this section. The structural scan uses the same `{}` grammar as the
formatter, but the destination type comes from `PROVEN_SCAN_ARG(&x)` — the same `_Generic`
dispatch, so the width written is the width of the object, and a value too large for it is
`PROVEN_ERR_OVERFLOW` rather than three neighbouring bytes.

The one property to internalise before using it: **the structural scan is not transactional across
placeholders.** If the third `{}` fails, the first two destinations have already been written. That
is a deliberate trade — buffering every destination until the whole line parsed would need
allocation, and this scanner allocates nothing — but it means a failed `proven_scan_fmt` leaves
your variables in a partly-updated state. Either treat them as garbage on failure, or scan into
locals and copy out only on success. §11.1 shows both patterns.

Literals in the pattern are the other half of the grammar and the part people underuse: anything
that is not a placeholder must match the input exactly, so `"{}:{}"` on `"12-34"` fails at the
literal rather than quietly returning one field.

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
| `PROVEN_ERR_NEED_MORE` | **Buffered scanner only.** The token is cut in half by the read boundary: the rest of it has not arrived yet. You will not normally see this - `proven_sysio_scanner_scan` refills and retries for you - it is what the scanner says to itself. | Nothing. It is handled. |
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

### Why float is the one thing that gets compiled out

Everything in this chapter is portable computation except one part, and that part is unusually
expensive.

Formatting a `double` correctly — so that the shortest decimal that round-trips is what you get, on
every input including subnormals — needs big-integer arithmetic and lookup tables. It is the
largest single piece of code in the formatter, and most firmware never prints a `double` at all. So
the freestanding profile sets `PROVEN_FMT_NO_FLOAT` and drops it, and a build for a microcontroller
does not carry kilobytes of decimal-conversion tables it will never call.

This is a build-time decision rather than a run-time one on purpose: the code is *absent*, not
merely unreachable, so the linker cannot be talked into keeping it. §8a of the
[freestanding guide](manual-freestanding.md) covers the related knob — the big-integer capacity — for
builds that do want floats on a small target.

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

### Worked example: wider numbers, loose text, and floating point

The examples above scan rigid formats into `proven_i32`. Three things come up as
soon as the input is real, and this example covers them together.

**The destination type is the contract.** `proven_scan_arg_i64()`,
`proven_scan_arg_u32()` and `proven_scan_arg_u64()` name the type the value is
written into, and the scanner reports an overflow rather than wrapping. A byte
count that needs more than 32 bits, and a value that must never be negative, are
the two cases where the wrong destination type is a bug nobody notices until the
numbers get big. The generic `PROVEN_SCAN_ARG()` macro chooses these from the
pointer you hand it; the named forms are what it chooses, written out.

**Not every input is a rigid format.** `proven_scan_skip_whitespace()` advances
past spaces, tabs and newlines, and `proven_scan_skip_until_number()` runs the
cursor forward to the next digit — or to a sign immediately followed by one, so
a negative number keeps its sign. Neither can fail: "there was nothing to skip"
is not an error, and at the end of the input the cursor stops instead of running
off.

**Floating point has a locale problem, and this library does not have it.**
`strtod()` reads `"3,5"` as `3.5` under one locale and as `3` under another, so
the same program disagrees with itself across machines.
`proven_parse_double_ascii()` is locale-free by construction: a comma is never a
decimal point. It reports how many bytes the number used, so a caller can carry
on from there, and it reports unparsable text as an error rather than as a `0`
that cannot be told from a real one. `proven_parse_f64_ascii()` is the same
function under its earlier name, kept so existing call sites still read correctly.

On the way out, `proven_arg_f64()` is how a floating-point value enters the
formatter — both `float` and `double` go through it, so what a program prints
does not depend on the width of the variable it was kept in. When the exact
spelling matters, `proven_float_format_f32_policy()` and its `f64` sibling take
the policy and options explicitly. Shortest mode asks for the fewest digits that
read back as exactly this value, which is what a serialiser wants: an exact round
trip without printing seventeen digits for numbers that do not need them.

<!-- example: manual/examples/ex_08_numbers.c -->
```c
#include <string.h>

/*
 * Reading numbers out of text, and writing them back.
 *
 * The text here is the sort a program actually meets: a log line with mixed
 * widths and signs, and a measurements file where the interesting number is
 * buried in prose. That means three jobs the earlier chapter-8 examples do not
 * cover:
 *
 *   - Scanning into types WIDER than int, and into unsigned ones, where the
 *     destination type is the whole question - a byte count that does not fit in
 *     32 bits is the classic overflow nobody notices until the file is 5 GB.
 *   - Positioning the cursor by hand - skipping whitespace, or skipping ahead to
 *     wherever the next number starts - when the input is not a rigid format.
 *   - Converting a decimal string to a double and back, exactly, without going
 *     through the C library's locale-dependent conversions.
 *
 * The float part matters more than it sounds. strtod reads "3,5" as 3.5 in one
 * locale and as 3 in another, and the same program then disagrees with itself
 * across machines. The parser used here is locale-free by construction: a comma
 * is never a decimal point, whatever the environment says.
 */

int main(void) {
    /* --- 1. scanning into the type the value needs ------------------------ */

    /* A log line: a request id that needs 64 bits, a byte count that is never
     * negative and can exceed 4 GB, a small status code, and a negative offset. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("id=9007199254740993 bytes=5368709120 status=404 delta=-17"));

    proven_i64 id = 0;
    proven_u64 bytes = 0;
    proven_u32 status = 0;
    proven_i32 delta = 0;

    /* Each argument constructor names the destination type, so the scanner
     * writes the right width and reports an overflow instead of wrapping. The
     * generic PROVEN_SCAN_ARG() macro picks these for you from the pointer's
     * type; the named forms are what it picks, and what you write when you want
     * the choice visible. */
    proven_err_t err = proven_scan_fmt_cursor(&scan, "id={} bytes={} status={} delta={}",
                                       proven_scan_arg_i64(&id),
                                       proven_scan_arg_u64(&bytes),
                                       proven_scan_arg_u32(&status),
                                       proven_scan_arg_i32(&delta));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning the log line must succeed");
    EXAMPLE_REQUIRE(id == 9007199254740993LL, "a 64-bit id survives, which a 32-bit destination could not");
    EXAMPLE_REQUIRE(bytes == 5368709120ULL, "and so does a byte count larger than 4 GiB");
    EXAMPLE_REQUIRE(status == 404u, "the small unsigned value reads normally");
    EXAMPLE_REQUIRE(delta == -17, "and a signed destination accepts the minus sign");

    /* The destination type is a contract, not a hint. A negative number has no
     * unsigned representation, and the scanner refuses rather than wrapping it
     * to something enormous. */
    proven_scan_t neg = proven_scan_init(PROVEN_LIT("-17"));
    proven_u32 nowhere = 12345;
    err = proven_scan_fmt_cursor(&neg, "{}", proven_scan_arg_u32(&nowhere));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a negative value cannot be scanned into an unsigned destination");
    EXAMPLE_REQUIRE(nowhere == 12345, "and the destination is left as it was");

    /* --- 2. moving the cursor when the input is not rigid ----------------- */

    /* Free-form text: the numbers matter, the words between them do not. */
    proven_scan_t notes = proven_scan_init(
        PROVEN_LIT("   sample A measured 42 units; sample B measured -8 units"));

    /* skip_whitespace advances past spaces, tabs and newlines. It is what you
     * call between fields of a format you are driving by hand - it never fails,
     * because "there was no whitespace" is not an error. */
    proven_scan_skip_whitespace(&notes);
    EXAMPLE_REQUIRE(notes.cursor == 3, "the three leading spaces are consumed");

    /* skip_until_number runs the cursor forward to the first digit, or to a sign
     * immediately followed by one. It is the call that turns "find the number in
     * this line" from a hand-written loop into one statement. */
    proven_scan_skip_until_number(&notes);
    proven_i32 first = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&first));
    EXAMPLE_REQUIRE(proven_is_ok(err) && first == 42, "the first number in the line is 42");

    proven_scan_skip_until_number(&notes);
    proven_i32 second = 0;
    err = proven_scan_fmt_cursor(&notes, "{}", proven_scan_arg_i32(&second));
    EXAMPLE_REQUIRE(proven_is_ok(err) && second == -8,
                    "and the next one keeps its sign, because the sign is part of the number");

    /* At the end there is nothing left to find, and the cursor stops rather than
     * running off: the scan is over, not broken. */
    proven_scan_skip_until_number(&notes);
    EXAMPLE_REQUIRE(notes.cursor == notes.view.size, "with no number left, the cursor lands at the end");

    /* --- 3. decimal text to double, without a locale --------------------- */

    proven_parse_double_result_t d = proven_parse_double_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(proven_is_ok(d.err), "parsing a decimal number must succeed");
    EXAMPLE_REQUIRE(d.val > 3.14158 && d.val < 3.14160, "and produce the value the digits spell");

    /* consumed says where the number ended, so the caller can carry on from
     * there - which is how you parse a list without copying it into pieces. */
    EXAMPLE_REQUIRE(d.consumed == 7, "consumed reports exactly the bytes the number used");

    /* A comma is not a decimal point here and never will be, whatever locale the
     * program is running under. The number ends at the comma. */
    proven_parse_double_result_t comma = proven_parse_double_ascii(PROVEN_LIT("3,5"));
    EXAMPLE_REQUIRE(proven_is_ok(comma.err) && comma.consumed == 1 && comma.val == 3.0,
                    "a comma ends the number: the parser is locale-free by construction");

    /* proven_parse_f64_ascii is the same function under its earlier name, kept
     * for code written before the current one existed. New code should use
     * proven_parse_double_ascii; both are here so an old call site still reads
     * correctly. */
    proven_parse_f64_result_t same = proven_parse_f64_ascii(PROVEN_LIT("3.14159 rest"));
    EXAMPLE_REQUIRE(same.val == d.val && same.consumed == d.consumed,
                    "the compatibility name is the same parser");

    /* Text that is not a number at all is an error, not a silent zero - which is
     * what atof() would have handed back with nothing to distinguish it from a
     * genuine "0". */
    proven_parse_double_result_t junk = proven_parse_double_ascii(PROVEN_LIT("not a number"));
    EXAMPLE_REQUIRE(!proven_is_ok(junk.err), "unparsable text is reported, not turned into 0");
    EXAMPLE_REQUIRE(junk.consumed == 0, "and nothing was consumed");

    /* --- 4. writing a float back ------------------------------------------ */

    /* proven_arg_f64 is how a floating-point value enters the formatter. Both
     * float and double go through it, so the digits a program prints do not
     * depend on the width of the variable it happened to be kept in. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t line = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");

    proven_fmt_result_t out = proven_u8str_append_fmt(&line.value, "measured {} units",
                                                      proven_arg_f64(d.val));
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "formatting the parsed value must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&line.value),
                                                  PROVEN_LIT("measured 3.14159")),
                    "and spell the number it was given");

    /* The policy form is the explicit one, for when the caller needs a
     * particular spelling rather than the default. SHORTEST asks for the fewest
     * digits that read back as exactly this value - which is the property a
     * serialiser wants, because it makes the round trip exact without printing
     * seventeen digits for numbers that do not need them. */
    char shortest[64];
    proven_size_t wrote = 0;
    float measured = 0.1f;
    proven_err_t ferr = proven_float_format_f32_policy(shortest, sizeof shortest, measured,
                                                       PROVEN_FLOAT_FORMAT_POLICY_RYU,
                                                       proven_float_format_options_shortest(),
                                                       &wrote);
    EXAMPLE_REQUIRE(proven_is_ok(ferr), "formatting a float in shortest mode must succeed");
    EXAMPLE_REQUIRE(wrote > 0 && shortest[wrote] == '\0', "the result is written and terminated");
    EXAMPLE_REQUIRE(strcmp(shortest, "0.1") == 0,
                    "0.1f prints as 0.1: the shortest text that reads back as the same float");

    /* And it round-trips: the text reads back as the value it came from. That is
     * the guarantee shortest mode exists to provide. */
    proven_parse_double_result_t roundtrip = proven_parse_double_ascii(
        (proven_u8str_view_t){ .ptr = (const proven_byte_t *)shortest, .size = wrote });
    EXAMPLE_REQUIRE(proven_is_ok(roundtrip.err), "the shortest form parses back");
    EXAMPLE_REQUIRE((float)roundtrip.val == measured, "as exactly the float it was printed from");

    printf("scanned id=%lld bytes=%llu; formatted %s and %s\n",
           (long long)id, (unsigned long long)bytes, proven_u8str_as_cstr(&line.value), shortest);

    proven_u8str_destroy(alloc, &line.value);
    return EXAMPLE_OK();
}
```

Wrong — scanning a large value into a 32-bit destination:

```text
proven_i32 bytes = 0;
proven_err_t e = proven_scan_fmt_cursor(&scan, "bytes={}", proven_scan_arg_i32(&bytes));
/* input says 5368709120 */                                        /* wrong type */
```

The value does not fit. Choosing the destination type by what is convenient,
rather than by the range the field can hold, is the same mistake as declaring a
file offset `int`.

Wrong — using `atof` or `strtod` for data that crosses machines:

```text
double v = strtod(text, NULL);   /* wrong: the decimal point depends on the locale */
```

And `atof` cannot report failure at all: unparsable text becomes `0`, which is
indistinguishable from a genuine zero in the data.

Wrong — assuming `skip_until_number` reports "not found":

```text
proven_scan_skip_until_number(&scan);
proven_i32 n = 0;
proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", proven_scan_arg_i32(&n));  /* may fail */
```

It moves the cursor and returns nothing. When there is no number left the cursor
lands at the end of the input, and it is the scan afterwards that tells you so —
check that error rather than assuming a number was found.

### Worked example: naming the argument type yourself

`PROVEN_ARG(x)` and `PROVEN_SCAN_ARG(&x)` choose an argument constructor from the
type of what you hand them, and most code never needs to know which one they
chose. Two situations take the choice back:

1. **The macro has no type to dispatch on that means what you mean.** A
   `proven_u8str_view_t`, a broken-down date, a raw address printed for a
   diagnostic — `proven_arg_str_view()`, `proven_arg_datetime()` and
   `proven_arg_ptr()` exist because those are deliberate choices, not defaults to
   fall into.
2. **The argument list is built at run time.** A logging helper whose parameters
   are already `proven_arg_t` values cannot re-wrap them — a macro that turns a
   value into an argument has nothing to do with something that is already one.
   `proven_arg_identity()` and `proven_scan_arg_identity()` are what let the same
   macro-driven code path accept an argument built earlier.

The example writes both sides out by name: the formatting constructors for a
character, a boolean, unsigned 32- and 64-bit values, the three ways of passing
text (`proven_arg_str_view()`, `proven_arg_cstr()`, `proven_arg_ucstr()`), a date
and a pointer; and the scanning constructors for every plain C integer width
(`short`, `int`, `long`, `long long` and their unsigned forms), a `double`, and a
string view captured without copying.

The three text constructors differ in how much they trust the caller, and the
order is worth remembering:

| Constructor | What it is given | What can go wrong |
|---|---|---|
| `proven_arg_str_view` | a pointer **and** a length | nothing is scanned for a terminator, so there is none to be missing — prefer this |
| `proven_arg_cstr` | a NUL-terminated C string | the terminator must be there and the memory must be alive, or the formatter reads past the end |
| `proven_arg_ucstr` | the same, as `unsigned char *` | exists so a byte buffer does not need a cast that silences a real warning |

<!-- example: manual/examples/ex_08_arguments.c -->
```c
/*
 * Every value handed to the formatter arrives as a proven_arg_t, and every value
 * the scanner writes into arrives as a proven_scan_arg_t. Most of the time you
 * never see either: PROVEN_ARG(x) and PROVEN_SCAN_ARG(&x) pick the right
 * constructor from the type of what you passed, and that is the whole point of
 * them.
 *
 * Two situations take the choice back off the macro, and both are ordinary:
 *
 *   1. The macro cannot see a type it recognises. A `proven_u8str_view_t`, a
 *      broken-down date, a raw address printed for a diagnostic - these need the
 *      constructor named, because there is no plain C type for the macro to
 *      dispatch on that means what you mean.
 *
 *   2. You are building the argument list at RUNTIME. A logging helper that
 *      takes "whatever the caller already assembled" receives proven_arg_t
 *      values, not ints and strings - and a macro that turns a value into an
 *      argument cannot be handed something that is already an argument. That is
 *      what the identity constructors are for: they let the same macro-driven
 *      code path accept an argument that was built earlier.
 *
 * So this example writes both sides out by name. The width and signedness of a
 * destination is not decoration - it is what decides whether 70000 arrives as
 * 70000 or as 4464.
 */

/* A logging helper that takes arguments the caller has already built. Because
 * its parameters are proven_arg_t, PROVEN_ARG would be the wrong tool inside it
 * - the value is already an argument, and proven_arg_identity is what says so. */
static proven_fmt_result_t log_pair(proven_u8str_t *out, const char *fmt,
                                    proven_arg_t a, proven_arg_t b) {
    return proven_u8str_append_fmt(out, fmt, proven_arg_identity(a), proven_arg_identity(b));
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t line = proven_u8str_create(alloc, 256);
    EXAMPLE_REQUIRE(proven_is_ok(line.err), "creating the output string must succeed");
    if (!proven_is_ok(line.err)) {
        return 1;
    }
    proven_u8str_t out = line.value;

    /* --- naming the formatting argument yourself -------------------------- */

    /* A single character and a flag. Written by name here so it is visible that
     * a bool prints as true/false rather than as 1/0. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&out, "flag={} mark={}",
                                                    proven_arg_bool(true),
                                                    proven_arg_char('!'));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a bool and a char must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("flag=true mark=!")),
                    "a bool renders as a word, not as a digit");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Unsigned widths. The constructor is the declaration of what the value is:
     * an unsigned 32-bit count and an unsigned 64-bit byte total are different
     * facts, and writing them out says which one you have. */
    r = proven_u8str_append_fmt(&out, "files={} bytes={}",
                                proven_arg_u32(1200u),
                                proven_arg_u64(5368709120ULL));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting unsigned values must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("files=1200 bytes=5368709120")),
                    "a 64-bit total is printed in full, not truncated to 32 bits");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Three ways to give the formatter text, in order of how much they trust
     * the caller:
     *
     *   proven_arg_str_view  - a pointer AND a length. Nothing is scanned for a
     *                          terminator, so there is no terminator to be
     *                          missing. Prefer this.
     *   proven_arg_cstr      - a NUL-terminated C string. The formatter walks it
     *                          looking for the terminator, so it must be there,
     *                          and the memory must still be alive.
     *   proven_arg_ucstr     - the same, for `unsigned char *`, which is what a
     *                          byte buffer is usually typed as. It exists so the
     *                          caller does not have to write a cast that
     *                          silences a real warning.
     */
    const char *name = "report.txt";
    const unsigned char *tag = (const unsigned char *)"draft";
    proven_u8str_view_t note = PROVEN_LIT("first pass");

    r = proven_u8str_append_fmt(&out, "{} [{}] {}",
                                proven_arg_cstr(name),
                                proven_arg_ucstr(tag),
                                proven_arg_str_view(note));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting the three text forms must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("report.txt [draft] first pass")),
                    "all three produce the same kind of output from different kinds of pointer");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* A date, and an address. Neither has a plain C type that means what it
     * means: a date is a struct, and an address printed for a diagnostic is a
     * deliberate act rather than something to fall into. */
    proven_datetime_t when = proven_time_breakdown(0);   /* the epoch: a fixed, checkable value */
    int local = 0;

    r = proven_u8str_append_fmt(&out, "at {} object {}",
                                proven_arg_datetime(when),
                                proven_arg_ptr(&local));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting a date and a pointer must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(proven_u8str_as_view(&out), PROVEN_LIT("at 1970-01-01")),
                    "the epoch breaks down to the first of January 1970");
    EXAMPLE_REQUIRE(proven_u8str_view_find(proven_u8str_as_view(&out), 0, PROVEN_LIT("0x")) != PROVEN_SIZE_MAX,
                    "and an address is rendered in hexadecimal");

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "clearing the line must succeed");

    /* Arguments built here, passed on, and formatted there. */
    r = log_pair(&out, "status={} retries={}", proven_arg_cstr("ok"), proven_arg_u32(3u));
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "formatting pre-built arguments must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("status=ok retries=3")),
                    "an argument built by the caller formats exactly as one built in place");

    /* --- naming the scanning destination yourself ------------------------- */

    /* On the way in, the constructor names the destination, and the destination
     * decides what "too big" means. These are the plain C types - short, int,
     * long, long long and their unsigned forms - for the very common case where
     * the variable you already have is one of them rather than a fixed-width
     * type. */
    proven_scan_t scan = proven_scan_init(
        PROVEN_LIT("h=-32000 uh=65000 i=-2000000 ui=4000000000 l=-9000000 ul=9000000 "
                   "ll=-9007199254740993 ull=18446744073709551615 f=2.5 word=alpha"));

    short h = 0;
    unsigned short uh = 0;
    int i = 0;
    unsigned int ui = 0;
    long l = 0;
    unsigned long ul = 0;
    long long ll = 0;
    unsigned long long ull = 0;
    double f = 0.0;
    proven_u8str_view_t word = {0};

    proven_err_t err = proven_scan_fmt_cursor(
        &scan, "h={} uh={} i={} ui={} l={} ul={} ll={} ull={} f={} word={}",
        proven_scan_arg_short(&h),
        proven_scan_arg_ushort(&uh),
        proven_scan_arg_int(&i),
        proven_scan_arg_uint(&ui),
        proven_scan_arg_long(&l),
        proven_scan_arg_ulong(&ul),
        proven_scan_arg_llong(&ll),
        proven_scan_arg_ullong(&ull),
        proven_scan_arg_f64(&f),
        proven_scan_arg_str_view(&word));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning every plain integer width must succeed");
    EXAMPLE_REQUIRE(h == -32000 && uh == 65000u, "the short forms hold their values");
    EXAMPLE_REQUIRE(i == -2000000 && ui == 4000000000u, "and so do the int forms");
    EXAMPLE_REQUIRE(l == -9000000L && ul == 9000000UL, "and the long forms");
    EXAMPLE_REQUIRE(ll == -9007199254740993LL, "a value needing 64 bits arrives whole");
    EXAMPLE_REQUIRE(ull == 18446744073709551615ULL, "including the largest unsigned 64-bit value");
    EXAMPLE_REQUIRE(f > 2.4999 && f < 2.5001, "the floating-point destination reads a decimal");

    /* A scanned string view points INTO the text being scanned. Nothing was
     * copied and nothing was allocated, so it is valid exactly as long as that
     * text is - copy it into a proven_u8str_t if it must outlive the scan. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(word, PROVEN_LIT("alpha")), "the word is captured as a view");

    /* The destination's width is a promise the scanner keeps. 70000 does not fit
     * in a short, so it is refused rather than wrapped to 4464 - which is what a
     * cast would have produced, silently, in a file nobody looks at again. */
    proven_scan_t narrow = proven_scan_init(PROVEN_LIT("70000"));
    short too_small = 7;
    err = proven_scan_fmt_cursor(&narrow, "{}", proven_scan_arg_short(&too_small));
    EXAMPLE_REQUIRE(err != PROVEN_OK, "a value that does not fit the destination is refused");
    EXAMPLE_REQUIRE(too_small == 7, "and the destination keeps the value it had");

    /* The scanning identity constructor, for the same reason as the formatting
     * one: a helper that receives ready-made scan arguments cannot re-wrap them. */
    proven_scan_t again = proven_scan_init(PROVEN_LIT("41"));
    proven_i32 answer = 0;
    proven_scan_arg_t prebuilt = proven_scan_arg_i32(&answer);
    err = proven_scan_fmt_cursor(&again, "{}", proven_scan_arg_identity(prebuilt));
    EXAMPLE_REQUIRE(proven_is_ok(err) && answer == 41, "a pre-built scan argument works unchanged");

    printf("arguments: %s\n", proven_u8str_as_cstr(&out));

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
```

Wrong — wrapping an argument that is already an argument:

```text
static proven_fmt_result_t log_one(proven_u8str_t *out, proven_arg_t a) {
    return proven_u8str_append_fmt(out, "{}", PROVEN_ARG(a));   /* wrong */
}
```

Use `proven_arg_identity(a)`. `PROVEN_ARG` is for values; `a` is already an
argument.

Wrong — keeping a scanned string view after the text is gone:

```text
proven_u8str_view_t word = {0};
{
    proven_u8str_t line = read_a_line(alloc);
    proven_scan_t s = proven_scan_init(proven_u8str_as_view(&line));
    proven_err_t e = proven_scan_fmt_cursor(&s, "word={}", proven_scan_arg_str_view(&word));
    proven_u8str_destroy(alloc, &line);     /* wrong: `word` pointed into `line` */
}
use(word);
```

A scanned view points into the text being scanned. Copy it into a
`proven_u8str_t` if it has to outlive that text.
