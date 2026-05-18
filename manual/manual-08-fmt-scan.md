# Chapter 8: Formatting and Scanning (v26.05.20)

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
- Decimal-to-double scanning is designed to stay exact within the implementation's limited power-of-ten range; outside that range, results are approximate.

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

### Result structs

```c
typedef struct { proven_err_t err; proven_i64 val; } proven_result_i64_t;
typedef struct { proven_err_t err; proven_u64 val; } proven_result_u64_t;
typedef struct { proven_err_t err; double val; } proven_result_f64_t;
typedef struct { proven_err_t err; proven_u8str_view_t val; } proven_result_u8str_view_t;
```

These results follow the standard project rule:

- inspect `err` first
- use `val` only when `err == PROVEN_OK`

### `proven_scan_t`

```c
typedef struct {
    proven_u8str_view_t view;
    proven_size_t cursor;
} proven_scan_t;
```

Meaning:

- `view` is the borrowed input
- `cursor` is the current byte position inside that input

### `proven_scan_init(view)`

Creates a scanner from a borrowed U8 view.
Invalid non-empty null views are normalized to empty views.

Example:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("123 456"));
```

## 8. Scanner primitive APIs

### `proven_scan_skip_whitespace(scan)`

Advance past spaces, tabs, and other standard ASCII whitespace.
This function updates the cursor in place.

### `proven_scan_i64(scan)`

Parse a signed 64-bit integer.

Rules:

- leading whitespace is skipped
- optional `+` or `-` is allowed
- no whitespace is allowed after the sign
- the cursor is rolled back on failure

Example:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("   -42"));
proven_result_i64_t r = proven_scan_i64(&scan);
```

Wrong:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("- 42")); /* wrong: space after sign */
```

### `proven_scan_u64(scan)`

Parse an unsigned 64-bit integer.

Rules:

- leading whitespace is skipped
- sign characters are not accepted
- the cursor is rolled back on failure

### `proven_scan_f64(scan)`

Parse a floating-point value.

Supported patterns include:

- decimal integers such as `12`
- fractions such as `12.5`
- scientific notation such as `1.25e3`
- signed values such as `-0.5`

The parser rejects values that are not finite or fall outside the supported exponent range. It restores the cursor on failure so callers can retry from the same position.

Accuracy note:

- Within the supported decimal range, scanning is designed to be exact enough for common finite inputs.
- Outside the exact range, the parsed result is approximate and may not round-trip exactly.
- For very large or very small decimal inputs, callers should treat the text form as a best-effort conversion rather than a full `strtod` replacement.

Example:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT(" 3.14159 2.5e2"));
proven_result_f64_t a = proven_scan_f64(&scan);
proven_result_f64_t b = proven_scan_f64(&scan);
```

### `proven_scan_str(scan)`

Parse the next whitespace-delimited token and return it as a borrowed view into the original input.
The returned view does not copy memory.

This is useful when you want to keep a token without allocating a new string.
It is not useful if the source buffer is about to disappear.

Wrong:

```c
proven_result_u8str_view_t token = proven_scan_str(&scan);
return token.val; /* wrong if scan input will die before the caller uses the view */
```

### `proven_scan_skip_until(scan, target)`

Search the remainder of the input for `target` and move the cursor to the first match.

Parameters:

- `scan`: scanner to update
- `target`: borrowed view that identifies the substring to find

Return values:

- `PROVEN_OK` when the target is found
- `PROVEN_ERR_NOT_FOUND` when the target does not occur
- `PROVEN_ERR_INVALID_ARG` for invalid inputs

If `target` is empty, the function returns `PROVEN_OK` immediately.

### `proven_scan_skip_until_number(scan)`

Advance the cursor until a number-looking position is found.
It stops at:

- a digit
- a `+` or `-` sign immediately followed by a digit

This helper is convenient for log scraping and loosely structured text.
It is not a general parser.

## 9. Scan argument model

### `proven_scan_arg_type_t`

```c
typedef enum {
    PROVEN_SCAN_ARG_TYPE_NONE,
    PROVEN_SCAN_ARG_TYPE_I32,
    PROVEN_SCAN_ARG_TYPE_U32,
    PROVEN_SCAN_ARG_TYPE_I64,
    PROVEN_SCAN_ARG_TYPE_U64,
    PROVEN_SCAN_ARG_TYPE_SHORT,
    PROVEN_SCAN_ARG_TYPE_USHORT,
    PROVEN_SCAN_ARG_TYPE_INT,
    PROVEN_SCAN_ARG_TYPE_UINT,
    PROVEN_SCAN_ARG_TYPE_LONG,
    PROVEN_SCAN_ARG_TYPE_ULONG,
    PROVEN_SCAN_ARG_TYPE_LLONG,
    PROVEN_SCAN_ARG_TYPE_ULLONG,
    PROVEN_SCAN_ARG_TYPE_F64,
    PROVEN_SCAN_ARG_TYPE_STR_VIEW,
} proven_scan_arg_type_t;
```

### `proven_scan_arg_t`

```c
typedef struct {
    proven_scan_arg_type_t type;
    union {
        proven_i32 *i32;
        proven_u32 *u32;
        proven_i64 *i64;
        proven_u64 *u64;
        short *s;
        unsigned short *us;
        int *i;
        unsigned int *ui;
        long *l;
        unsigned long *ul;
        long long *ll;
        unsigned long long *ull;
        double *f64;
        proven_u8str_view_t *str_view;
    } ptr;
} proven_scan_arg_t;
```

The scan side stores pointers, not values.
The pointed-to object is where parsed data will be written.

Do not pass a null destination pointer.

### Simple constructors

| API | Parameters | Returns | Intent |
|---|---|---|---|
| `proven_scan_arg_none(void)` | none | `proven_scan_arg_t` | Internal sentinel. |
| `proven_scan_arg_i32(proven_i32 *v)` | destination pointer | `proven_scan_arg_t` | Write a parsed signed 32-bit value. |
| `proven_scan_arg_u32(proven_u32 *v)` | destination pointer | `proven_scan_arg_t` | Write a parsed unsigned 32-bit value. |
| `proven_scan_arg_i64(proven_i64 *v)` | destination pointer | `proven_scan_arg_t` | Write a parsed signed 64-bit value. |
| `proven_scan_arg_u64(proven_u64 *v)` | destination pointer | `proven_scan_arg_t` | Write a parsed unsigned 64-bit value. |
| `proven_scan_arg_short(short *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `short`. |
| `proven_scan_arg_ushort(unsigned short *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `unsigned short`. |
| `proven_scan_arg_int(int *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `int`. |
| `proven_scan_arg_uint(unsigned int *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `unsigned int`. |
| `proven_scan_arg_long(long *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `long`. |
| `proven_scan_arg_ulong(unsigned long *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `unsigned long`. |
| `proven_scan_arg_llong(long long *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `long long`. |
| `proven_scan_arg_ullong(unsigned long long *v)` | destination pointer | `proven_scan_arg_t` | Write into a native `unsigned long long`. |
| `proven_scan_arg_f64(double *v)` | destination pointer | `proven_scan_arg_t` | Write a parsed floating-point value. |
| `proven_scan_arg_str_view(proven_u8str_view_t *v)` | destination pointer | `proven_scan_arg_t` | Write a borrowed U8 token view. |
| `proven_scan_arg_identity(proven_scan_arg_t v)` | existing scan arg | `proven_scan_arg_t` | Pass-through helper. |

### `PROVEN_SCAN_ARG(x)`

This macro selects a constructor by the type of the pointer you pass.
It is the usual public entry point.

Supported pointer types:

- `short *`, `unsigned short *`
- `int *`, `unsigned int *`
- `long *`, `unsigned long *`
- `long long *`, `unsigned long long *`
- `double *`
- `proven_u8str_view_t *`
- `proven_scan_arg_t` itself, for identity passthrough

Important distinctions:

- `PROVEN_SCAN_ARG` is for destination pointers.
- `PROVEN_ARG` is for formatting values.
- do not mix the two

Wrong:

```c
int id = 0;
proven_scan_fmt_cursor(&scan, "{}", PROVEN_ARG(&id)); /* wrong */
```

Correct:

```c
int id = 0;
proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&id));
```

### Explicit aliases for long destinations

```c
#define PROVEN_SCAN_ARG_LONG(ptr)  proven_scan_arg_long(ptr)
#define PROVEN_SCAN_ARG_ULONG(ptr) proven_scan_arg_ulong(ptr)
```

These are convenience macros for callers who want the call site to read clearly.

## 10. Structural scan grammar

The scan format language is much simpler than the formatter language.

### Supported scan syntax

- literal text must match exactly, except for whitespace
- whitespace in the format string matches any run of whitespace in the input
- placeholders are only bare `{}`
- every `{}` consumes exactly one scan argument
- there are no width, alignment, hex, or precision modifiers
- there are no explicit positional scan indexes

In other words, scan format strings are intentionally strict and intentionally small.

### Placeholder counting

The internal engine counts bare `{}` placeholders first and requires the argument count to match exactly.
If the counts differ, it returns `PROVEN_ERR_INVALID_ARG`.

### Whitespace behavior

Any whitespace character in the format string causes the scanner to skip input whitespace.
That means these two formats are equivalent in practice:

```c
"ID: {} SCORE: {}"
"ID:\n\t {}   SCORE: {}"
```

This is useful when input may have flexible spacing.
It also means you should not rely on exact whitespace preservation when using the structural scanner.

### Partial progress and rollback

The structural scan engine may write earlier destinations before a later mismatch occurs.
The primitive numeric scanners do roll back on their own failures, but the full structural engine is not transaction-safe across all destination variables.

If you need transaction-like behavior, save:

- the scan cursor
- and any destination values you care about

before calling the scanner.

## 11. Scan formatting APIs

### `proven_scan_fmt_internal(scan, fmt, args, args_count)`

Internal structural scan engine.
Use the public macros unless you are integrating the parser manually.

Parameters:

- `scan`: active scanner
- `fmt`: scan format string
- `args`: scan argument array, including the hidden sentinel at index 0
- `args_count`: total size of `args`, including the sentinel

Return value:

- `PROVEN_OK` on success
- `PROVEN_ERR_INVALID_ARG` when the placeholder count, arguments, or spec shape is invalid
- `PROVEN_ERR_NOT_FOUND` when a literal does not match
- `PROVEN_ERR_OVERFLOW` when a destination type cannot hold the parsed value
- `PROVEN_ERR_OUT_OF_BOUNDS` when floating-point parsing hits the parser's exponent limits

### `proven_scan_fmt_internal_view(view, fmt, args, count)`

Convenience wrapper that creates a temporary scanner from a borrowed view and then runs the internal engine.

### `proven_scan_fmt_cursor(scan_ptr, fmt, ...)`

Scan from an existing cursor.
The scanner is updated in place.

### `proven_scan_fmt(view, fmt, ...)`

Create a temporary scanner around a view and scan from it.
This is the simplest entry point when you do not need to keep the cursor.

### Example: parse a log record

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("ID: 402, SCORE: 99.5, USER: ada"));
int id = 0;
double score = 0.0;
proven_u8str_view_t user = {0};

proven_err_t e = proven_scan_fmt_cursor(
    &scan,
    "ID: {}, SCORE: {}, USER: {}",
    PROVEN_SCAN_ARG(&id),
    PROVEN_SCAN_ARG(&score),
    PROVEN_SCAN_ARG(&user)
);
if (!proven_is_ok(e)) {
    return e;
}
```

### Example: parse through a larger buffer with a cursor

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("noise: x\nVALUE: 1234\nmore"));
proven_err_t e = proven_scan_skip_until(&scan, PROVEN_LIT("VALUE:"));
if (!proven_is_ok(e)) {
    return e;
}

int value = 0;
e = proven_scan_fmt_cursor(&scan, "VALUE: {}", PROVEN_SCAN_ARG(&value));
```

## 11.1 Scan error code guide and recovery

The scan APIs use a small, predictable set of error codes. The practical meaning depends on which entry point you call, but the recovery strategy is usually the same: check the cursor, check the destination types, and check whether the format string is actually describing the input you have.

| Error code | Seen in current scan code | What it usually means | Typical recovery |
|---|---|---|---|
| `PROVEN_OK` | all scan entry points | The parse succeeded. | Use the output value and continue. |
| `PROVEN_ERR_INVALID_ARG` | primitive scanners, `skip_until`, and structural scan | The scanner object is invalid, a destination pointer is null, the format string is malformed, the placeholder count does not match the argument count, the input is empty where a token is required, or the numeric syntax is incomplete. | Check the scanner/view lifetime, make sure `PROVEN_SCAN_ARG` is used on the scan side, verify the destination pointers are non-null, and keep structural scan formats to bare `{}` placeholders. |
| `PROVEN_ERR_NOT_FOUND` | `proven_scan_skip_until`, structural literal matching | The target text is not present from the current cursor position, or a literal in the scan format does not match the input. | Confirm the cursor is at the right location, normalize or loosen surrounding whitespace, or search for the marker first with `skip_until`. |
| `PROVEN_ERR_OVERFLOW` | integer scanners and narrowing structural scans | The parsed value does not fit the destination type, or a floating-point parse produced a non-finite value. | Parse into a wider type first, widen the destination, or reject the input before it reaches the scanner. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | `proven_scan_f64`, `proven_sysio_scan_chunk_impl` | The floating-point parser hit its own exponent-range limit, or the one-chunk file scanner filled its fixed buffer before it saw a complete token. | Use a smaller exponent, pre-normalize the data, or switch to the buffered scanner when the input can exceed one chunk. |

Important notes:

- Structural scan currently treats malformed scan syntax as `PROVEN_ERR_INVALID_ARG`, not as a separate syntax-specific code.
- `PROVEN_ERR_OUT_OF_BOUNDS` is currently used by floating-point exponent limits and by the one-chunk sysio scanner when the buffer fills before a complete token is available.
- If a structural scan writes some earlier outputs before a later mismatch, restore the cursor and any destinations you care about yourself.
- The primitive numeric scanners roll back their own cursor on failure, but the full structural engine is not fully transactional across all destination variables.

Practical debugging tips:

1. If the failure is `INVALID_ARG`, first check the call site: `PROVEN_ARG` vs `PROVEN_SCAN_ARG`, null pointers, and placeholder count mismatch are the most common causes.
2. If the failure is `NOT_FOUND`, print the remaining input from the cursor and compare it to the literal text in the scan format.
3. If the failure is `OVERFLOW`, scan into a wider destination and narrow only after the parse succeeds.
4. If the failure is `OUT_OF_BOUNDS`, the data is usually using a larger scientific-notation exponent than this parser supports, or the one-chunk file scanner hit its fixed buffer limit before the token completed.

## 12. Examples and misuse cases

### Example: format a compact report

```c
proven_fmt_result_t r = proven_u8str_append_fmt_grow(
    alloc,
    &s,
    "name={} id={:0>5} hex={:x}",
    PROVEN_ARG("ada"),
    PROVEN_ARG(42),
    PROVEN_ARG(48879)
);
if (!PROVEN_FMT_IS_OK(r)) {
    return r.err;
}
```

### Example: format a datetime

```c
proven_datetime_t now = proven_time_now_datetime();
proven_fmt_result_t r = proven_u8str_append_fmt_grow(
    alloc,
    &s,
    "now={} UTC",
    PROVEN_ARG(now)
);
```

### Example: format a trusted C string and a bounded C string

```c
const char *trusted = "ready";
const char *maybe_truncated = read_from_somewhere();

proven_u8str_append_fmt_grow(alloc, &s, "{} / {}",
    PROVEN_ARG(trusted),
    PROVEN_ARG_CSTR_N(maybe_truncated, 64)
);
```

### Wrong: assume a view is NUL-terminated

```c
proven_u8str_view_t view = proven_scan_str(&scan).val;
printf("%s\n", (const char *)view.ptr); /* wrong: view may not end with NUL */
```

### Wrong: use scan syntax in the formatter

```c
proven_u8str_append_fmt_grow(alloc, &s, "{} {}", PROVEN_SCAN_ARG(&value)); /* wrong: scan arg on format side */
```

### Wrong: use format modifiers in the scanner

```c
int value = 0;
proven_scan_fmt_cursor(&scan, "{:x}", PROVEN_SCAN_ARG(&value)); /* wrong: scan only supports bare {} */
```

### Wrong: expect scan formatting to be transactional by itself

```c
size_t save = scan.cursor;
int a = 0;
int b = 0;
if (!proven_is_ok(proven_scan_fmt_cursor(&scan, "{} {}", PROVEN_SCAN_ARG(&a), PROVEN_SCAN_ARG(&b)))) {
    scan.cursor = save; /* this restores the cursor, but a and b may also need restoration */
}
```

### Wrong: assume truncated formatting is atomic

```c
proven_fmt_result_t r = proven_u8str_append_fmt_trunc(&s, "{} {}", PROVEN_ARG("a"), PROVEN_ARG("b"));
/* wrong if you later assume s was unchanged on failure */
```

### Wrong: assume `PROVEN_ARG` handles every type

```c
void helper(void) {}
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(helper)); /* wrong */
```

### Wrong: pass a null destination pointer to the scanner

```c
proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG((int *)0)); /* wrong */
```

## 13. Freestanding and build-mode notes

When `PROVEN_FMT_NO_FLOAT` is defined, floating-point formatting is removed from the formatter side.
This matters in reduced builds and freestanding configurations.

Practical consequences:

- `PROVEN_ARG(float_value)` will not compile if float support is disabled
- the float constructor is not available
- integer, string, view, pointer, and datetime formatting still work
- scanner float parsing is separate and may still be present depending on the build profile

If you are writing portable examples for freestanding builds, avoid floating-point formatting unless the build profile explicitly keeps it enabled.

## Quick reference

Formatting side, use these when in doubt:

- `PROVEN_ARG(x)` for normal values
- `PROVEN_ARG_FN(f)` for function pointers
- `PROVEN_ARG_CSTR_N(v, n)` for partially trusted C strings
- `proven_u8str_append_fmt_grow` when the output should fit
- `proven_u8str_append_fmt` when you want atomic fixed-capacity behavior
- `proven_u8str_append_fmt_trunc` when partial output is acceptable

Scanning side, use these when in doubt:

- `PROVEN_SCAN_ARG(&x)` for typed destinations
- `proven_scan_fmt_cursor` when you already have a scanner cursor
- `proven_scan_fmt` when you only have a borrowed view
- `proven_scan_str` when you want the next token as a borrowed view
- `proven_scan_skip_until_number` when you are scraping loose text
