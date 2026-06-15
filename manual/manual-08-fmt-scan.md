# Chapter 8: Formatting and Scanning (v26.05.19u)

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

```c
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
proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
&s,
"hello {}",
PROVEN_ARG("world")
);
if (!proven_is_ok(r.err)) {
return r.err;
}
```

A truncating result can still tell you how much more space you would have needed:

```c
proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
&s,
"name={} score={}",
PROVEN_ARG("ada"),
PROVEN_ARG(42)
);
/* r.written and r.required are useful here. */
```

### `proven_arg_type_t`

```c
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

```c
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

```c
proven_arg_t arg = {0};
arg.type = PROVEN_ARG_I64;
arg.value.u64 = 123; /* wrong: type and union field do not match */
```

Correct:

```c
proven_arg_t arg = proven_arg_i64(123);
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

```c
void helper(void) {}
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(helper)); /* wrong */
```

Correct:

```c
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG_FN(helper));
```

### `PROVEN_ARG_FN(f)`

This macro exists so callers can pass function pointers without casting them through `void *`.
It is a small safety wrapper around `proven_arg_fn`.

Example:

```c
proven_fmt_result_t r = proven_u8str_append_fmt_grow(
alloc,
&s,
"callback = {}",
PROVEN_ARG_FN(helper)
);
```

### `PROVEN_ARG_CSTR_N(v, max_len)`

This macro is the bounded-string helper.
Use it when the source may not be a fully trusted C string, but you still want C-string-like input handling.

It scans for NUL only up to `max_len` and then formats the bounded prefix as a view.

Good use case:

```c
const char *buf = get_network_buffer();
proven_fmt_result_t r = proven_u8str_append_fmt_grow(
alloc,
&s,
"payload={}",
PROVEN_ARG_CSTR_N(buf, 128)
);
```

Bad use case:

```c
const char *buf = get_network_buffer();
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(buf)); /* wrong if buf is not trusted */
```

### Float formatting note

If `PROVEN_FMT_NO_FLOAT` is defined, float support is removed from the generic selector and the float constructor is not available.
That is a compile-time configuration choice, not a runtime toggle.

Current float rendering keeps a fixed six-digit fractional form for finite values, then switches to scientific notation when the magnitude is too large or too small for the compact form. The carry logic is bounded so values near a rounding boundary stay stable instead of expanding into an unbounded normalization loop.

### Accuracy and limits

- Floating-point output uses six fractional digits with round-half-up behavior.
- The text form is intended for diagnostics and logs, not for round-trip serialization.
- Decimal-to-double scanning routes through the shared exact backend and rounds finite decimal inputs to IEEE-754 binary64 with round-to-nearest, ties-to-even behavior.
- Values below the half-way threshold to the smallest subnormal round to signed zero with the input sign preserved.
- The current parser path is layered as `Clinger fast path -> staged Eisel-Lemire layer -> exact bigint fallback`.
- Internal metrics exist so tests can verify which path accepted a representative decimal token.
- The current cached `5^q` table is checked in as generated source and can be
  regenerated from `scripts/generate_float_decimal_tables.py`.
- The current staged Eisel-Lemire layer handles a conservative subset only:
  positive exponents backed by the generated `5^q` table and exact
  negative-exponent cases where the same `5^q` cancels cleanly out of the
  significand.
- When `__uint128_t` is available, the staged layer also handles a bounded
  negative-exponent ratio subset by rounding `mantissa / 5^q` directly for
  normal-range candidates, including cases that need more than 64 bits of
  temporary left-shift during normalization.
- On the current implementation, staged successes finish through the shared
  cached-power product plan. Negative cases that remain uncertain now defer
  directly to the exact bigint fallback instead of going through a separate
  denominator-normalization staged family.
- The same staged cached-power path now also reaches some subnormal candidates
  such as `5e-324`; values below the half-threshold to the smallest subnormal
  still fall through to exact fallback.

### Public float parsing APIs

- `proven_scan_f64()` parses from a scanner cursor and restores the cursor on failure.
- `proven_parse_double_ascii()` parses one locale-free ASCII token from a view and reports consumed length.
- `proven_strtod()` is the wrapper layer that skips leading ASCII whitespace and reports `endptr`.

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
proven_u8str_append_fmt_grow(alloc, &s, "{:0>5}", PROVEN_ARG(42));    /* 00042 */
proven_u8str_append_fmt_grow(alloc, &s, "{:*^10}", PROVEN_ARG("ok")); /* ****ok**** */
proven_u8str_append_fmt_grow(alloc, &s, "{:.<10}", PROVEN_ARG("x"));  /* x......... */
```

### Hex mode

A trailing `x` turns on lowercase hexadecimal rendering for numeric arguments.
The formatter does not use uppercase hex and does not add a `0x` prefix for integer values.

Example:

```c
proven_u8str_append_fmt_grow(alloc, &s, "0x{:x}", PROVEN_ARG(48879));
```

For signed integers, the numeric value is rendered through the implementation's unsigned conversion path when hex mode is enabled.
That means negative numbers are shown in their unsigned representation rather than as a signed decimal value.

### Width limit and invalid specs

Width parsing is checked.
Very large widths are rejected instead of silently wrapping.
The current parser also rejects unknown format characters.

Wrong:

```c
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

```c
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
It is intentionally small and keeps the current fixed-precision formatter behavior as the default path.

The main entry points are:

- `proven_float_format_f64_policy(...)`
- `proven_float_format_f32_policy(...)`
- `proven_float_format_options_fixed_default()`
- `proven_float_format_options_shortest()`

Policy notes:

- `PROVEN_FLOAT_FORMAT_POLICY_DEFAULT` and `PROVEN_FLOAT_FORMAT_POLICY_SIMPLE` preserve the current fixed-precision output.
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
if (!proven_is_ok(err)) {
    return err;
}
```

### `PROVEN_FMT_IS_OK(res)`

A small helper macro for checking `proven_fmt_result_t`.
Use it when you want the intent to stay compact.

Example:

```c
proven_fmt_result_t r = proven_u8str_append_fmt_grow(
alloc,
&s,
"name={} score={:0>4}",
PROVEN_ARG("ada"),
PROVEN_ARG(42)
);
if (!PROVEN_FMT_IS_OK(r)) {
return r.err;
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
return 1;
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
