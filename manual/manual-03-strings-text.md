# Chapter 3: Strings, Formatting, and Scanning

This chapter covers `u8str.h`, `u16str.h`, `fmt.h`, and `scan.h`.
For the full format grammar and scanner edge cases, see `manual-08-fmt-scan.md`.

## Table of contents

1. [U8 strings and views](#1-u8-strings-and-views)
2. [U16 strings and views](#2-u16-strings-and-views)
3. [Formatting](#3-formatting)
4. [Scanning](#4-scanning)
5. [Examples and misuse cases](#5-examples-and-misuse-cases)

## 1. U8 strings and views

A U8 string is an owned, NUL-terminated byte string. Length is counted in bytes, not Unicode scalar values. A U8 view is borrowed and is not guaranteed to be NUL-terminated.

### Structures

```c
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t size;
} proven_u8str_view_t;

typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_u8str_mut_t;

typedef struct {
    proven_buf_t internal;
} proven_u8str_t;

typedef struct {
    proven_err_t err;
    proven_u8str_t value;
} proven_result_u8str_t;

typedef struct {
    proven_err_t err;
    const char *value;
} proven_result_cstr_t;
```

Intent:

- `proven_u8str_view_t`: borrowed read-only byte string view.
- `proven_u8str_mut_t`: borrowed mutable byte string view.
- `proven_u8str_t`: owned string with NUL termination.
- `proven_result_u8str_t`: result wrapper for owned strings.
- `proven_result_cstr_t`: result wrapper for an allocated NUL-terminated C string.

### Macros

| Macro | Intent | Notes |
|---|---|---|
| `PROVEN_LIT(s)` | Make a `proven_u8str_view_t` from a string literal. | Use only with string literals. |
| `PROVEN_LIT_INIT(s)` | Initializer form for literal views. | Use in aggregate initialization. |
| `PROVEN_INDEX_NOT_FOUND` | Sentinel returned by find functions. | Equal to `(proven_size_t)-1`. |

### U8 functions

| API | Intent | Return |
|---|---|---|
| `proven_u8str_create(alloc, limit)` | Create an empty owned string with capacity for `limit` bytes plus NUL. | `proven_result_u8str_t`. |
| `proven_u8str_create_from_view(alloc, view)` | Copy a view into a new owned string. | `proven_result_u8str_t`. |
| `proven_u8str_is_valid(str)` | Validate public string invariants. | `bool`. |
| `proven_u8str_reserve(alloc, str, new_cap)` | Ensure at least `new_cap` bytes of internal capacity. | `proven_err_t`. |
| `proven_u8str_append(str, data)` | Atomic fixed-capacity append. | `PROVEN_OK` or `PROVEN_ERR_OUT_OF_BOUNDS`. |
| `proven_u8str_append_partial(str, data)` | Truncating append. | `proven_result_size_t`; `value` is bytes written. |
| `proven_u8str_append_grow(alloc, str, data)` | Atomic growable append. | `proven_err_t`. |
| `proven_u8str_append_byte(alloc, str, b)` | Append one byte, growing if needed. | `proven_err_t`. |
| `proven_u8str_replace_at(str, index, old_len, data)` | Replace an exact byte range with fixed-capacity semantics. | `proven_err_t`. |
| `proven_u8str_replace_at_grow(alloc, str, index, old_len, data)` | Like `replace_at`, but grows the buffer (doubling) when the edit does not fit instead of failing. | `proven_err_t`; string unchanged on alloc failure. |
| `proven_u8str_insert(str, index, data)` | Insert bytes at `index` with fixed-capacity semantics. | `proven_err_t`. |
| `proven_u8str_insert_grow(alloc, str, index, data)` | Like `insert`, but grows the buffer when needed. No manual `reserve` required. | `proven_err_t`; string unchanged on alloc failure. |
| `proven_u8str_remove(str, index, len)` | Remove a byte range. | `proven_err_t`. |
| `proven_u8str_replace_first(str, start_offset, target, replacement)` | Replace first matching target at or after start. | `PROVEN_OK` even when not found. |
| `proven_u8str_view_find(haystack, start_offset, needle)` | Find the first occurrence of a byte substring (correctly handles any byte values; not NUL-terminated). | index or `PROVEN_INDEX_NOT_FOUND`. |
| `proven_u8str_view_starts_with(str, prefix)` | Prefix test. | int truth value. |
| `proven_u8str_view_ends_with(str, suffix)` | Suffix test. | int truth value. |
| `proven_u8str_view_slice(str, index, len)` | Return a clamped subview. | `proven_u8str_view_t`. |
| `proven_u8str_as_cstr(str)` | Return internal NUL-terminated pointer. | `const char *`; invalidated by growth/destroy. |
| `proven_mem_view_from_u8(view)` | Convert U8 view to byte view. | `proven_mem_view_t`. |
| `proven_u8str_view_to_cstr(view, alloc)` | Allocate a NUL-terminated C string from any view. | `proven_result_cstr_t`; caller frees with allocator. |
| `proven_cstr_len(s)` | Count bytes until NUL. | `proven_size_t`. |
| `proven_u8str_view_from_cstr(s)` | Make a view from a trusted NUL-terminated string. | Empty view for null pointer. |
| `proven_u8str_view_eq(a, b)` | Byte equality test. | int truth value. |
| `proven_u8str_destroy(alloc, str)` | Free owned storage with matching allocator. | void. |
| `proven_u8str_as_view(str)` | Borrow current contents. | `proven_u8str_view_t`. |

### Basic U8 example

```c
proven_allocator_t alloc = proven_heap_allocator();
proven_result_u8str_t r = proven_u8str_create_from_view(alloc, PROVEN_LIT("log"));
if (!proven_is_ok(r.err)) return r.err;
proven_u8str_t s = r.value;

proven_err_t e = proven_u8str_append_grow(alloc, &s, PROVEN_LIT(": ready"));
if (!proven_is_ok(e)) {
    proven_u8str_destroy(alloc, &s);
    return e;
}

const char *cstr = proven_u8str_as_cstr(&s);
use_c_string(cstr);
proven_u8str_destroy(alloc, &s);
```

## 2. U16 strings and views

U16 APIs are excluded when `PROVEN_NO_U16STR` is defined. U16 sizes are counted in `proven_u16` code units. Internally, `proven_u16str_t` uses `proven_buf_t`, which tracks bytes.

### Structures

```c
typedef struct {
    const proven_u16 *ptr;
    proven_size_t size;
} proven_u16str_view_t;

typedef struct {
    proven_buf_t internal;
} proven_u16str_t;

typedef struct {
    proven_err_t err;
    proven_u16str_t value;
} proven_result_u16str_t;
```

### Macro

| Macro | Intent |
|---|---|
| `PROVEN_U16_LIT(s)` | Make a `proven_u16str_view_t` from a UTF-16 literal expression `u"..."`. |

### U16 functions

| API | Intent | Return |
|---|---|---|
| `proven_u16str_create(alloc, unit_limit)` | Create empty owned U16 string. | `proven_result_u16str_t`. |
| `proven_u16str_create_from_view(alloc, view)` | Copy U16 view into owned string. | `proven_result_u16str_t`. |
| `proven_u16str_destroy(alloc, str)` | Free owned U16 storage. | void. |
| `proven_u16str_append(str, data)` | Atomic fixed-capacity append. | `proven_err_t`. |
| `proven_u16str_append_partial(str, data)` | Truncating append. | `proven_result_size_t`. |
| `proven_u16str_append_grow(alloc, str, data)` | Atomic growable append. | `proven_err_t`. |
| `proven_u16str_as_ptr(str)` | Return internal `proven_u16` pointer. | `const proven_u16 *`. |
| `proven_u16str_len(str)` | Return length in code units. | `proven_size_t`. |

Example:

```c
#ifndef PROVEN_NO_U16STR
proven_result_u16str_t r =
    proven_u16str_create_from_view(alloc, PROVEN_U16_LIT("hello"));
if (!proven_is_ok(r.err)) return r.err;
proven_u16str_t s = r.value;

proven_u16str_append_grow(alloc, &s, PROVEN_U16_LIT(" world"));
proven_u16str_destroy(alloc, &s);
#endif
```

Note: `proven_u16` is a code unit, not necessarily one full Unicode character. UTF-16 surrogate pairs use two code units.

## 3. Formatting

The formatter writes into `proven_u8str_t` or PAL-backed streams. It uses a small structural format language with `{}` placeholders, explicit indexes like `{1}`, escaped braces `{{` and `}}`, and width/alignment specs such as `{:0>5}`, `{:*^10}`, and `{:.<10}`.

### Structures and enums

```c
typedef struct {
    proven_err_t err;
    proven_size_t written;
    proven_size_t required;
} proven_fmt_result_t;
```

Fields:

- `err`: result code.
- `written`: bytes actually written.
- `required`: bytes required for full output.

`proven_arg_type_t` variants:

- `PROVEN_ARG_NONE`
- `PROVEN_ARG_I32`
- `PROVEN_ARG_U32`
- `PROVEN_ARG_I64`
- `PROVEN_ARG_U64`
- `PROVEN_ARG_F64` unless `PROVEN_FMT_NO_FLOAT` is defined
- `PROVEN_ARG_CSTR`
- `PROVEN_ARG_STR_VIEW`
- `PROVEN_ARG_DATETIME`
- `PROVEN_ARG_PTR`
- `PROVEN_ARG_FN`

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

### Format argument constructors

| API | Intent |
|---|---|
| `proven_arg_none()` | Internal sentinel value. |
| `proven_arg_i32(v)`, `proven_arg_u32(v)` | Integer arguments. |
| `proven_arg_i64(v)`, `proven_arg_u64(v)` | Wide integer arguments. |
| `proven_arg_f64(v)` | Floating-point argument unless float formatting is disabled. |
| `proven_arg_cstr(v)` | Trusted live NUL-terminated C string. |
| `proven_arg_cstr_n(v, max_len)` | Bounded C-string argument; scans for NUL only up to `max_len`. |
| `proven_arg_str_view(v)` | Borrowed string view argument. |
| `proven_arg_datetime(v)` | Datetime argument. |
| `proven_arg_ptr(v)` | Object pointer argument. |
| `proven_arg_fn(v)` | Function pointer argument. |
| `proven_arg_ucstr(v)` | Unsigned-char C string helper. |
| `proven_arg_identity(v)` | Pass-through for existing `proven_arg_t`. |

### Format macros

| Macro | Intent |
|---|---|
| `PROVEN_ARG(x)` | `_Generic` selector for supported argument types. |
| `PROVEN_ARG_FN(f)` | Function pointer formatting helper. |
| `PROVEN_ARG_CSTR_N(v, max_len)` | Bounded C-string helper. |
| `proven_u8str_append_fmt(str, fmt, ...)` | Atomic fixed-capacity formatting. |
| `proven_u8str_append_fmt_trunc(str, fmt, ...)` | Best-effort truncating formatting. |
| `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)` | Atomic growable formatting. |
| `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)` | Growable formatting with scratch allocator for temporary patch allocations. |
| `PROVEN_FMT_IS_OK(res)` | Check `proven_fmt_result_t`. |

### Formatting engine

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

Normal user code should call the macros instead of this internal engine. The engine expects a leading `proven_arg_none()` sentinel at index 0.

Extra unused format arguments return `PROVEN_ERR_INVALID_ARG`.

Example:

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 8);
if (!proven_is_ok(r.err)) return r.err;
proven_u8str_t s = r.value;

proven_fmt_result_t fr = proven_u8str_append_fmt_grow(
    alloc,
    &s,
    "name={} score={:0>4}",
    PROVEN_ARG(PROVEN_LIT("ada")),
    PROVEN_ARG(42)
);
if (!PROVEN_FMT_IS_OK(fr)) {
    proven_u8str_destroy(alloc, &s);
    return fr.err;
}

proven_u8str_destroy(alloc, &s);
```

## 4. Scanning

The scanner parses from a borrowed `proven_u8str_view_t`. A cursor tracks progress.

### Result structs

```c
typedef struct { proven_err_t err; proven_i64 val; } proven_result_i64_t;
typedef struct { proven_err_t err; proven_u64 val; } proven_result_u64_t;
typedef struct { proven_err_t err; double val; } proven_result_f64_t;
typedef struct { proven_err_t err; proven_u8str_view_t val; } proven_result_u8str_view_t;
```

### `proven_scan_t`

```c
typedef struct {
    proven_u8str_view_t view;
    proven_size_t cursor;
} proven_scan_t;
```

Purpose: hold input and current parse position. `proven_scan_init()` normalizes invalid non-empty null views to empty views.

### Scanner functions

| API | Intent | Return |
|---|---|---|
| `proven_scan_init(view)` | Create scanner from view. | `proven_scan_t`. |
| `proven_scan_skip_whitespace(scan)` | Advance past whitespace. | void. |
| `proven_scan_i64(scan)` | Parse signed 64-bit integer. | `proven_result_i64_t`. |
| `proven_scan_u64(scan)` | Parse unsigned 64-bit integer. | `proven_result_u64_t`. |
| `proven_scan_f64(scan)` | Parse floating-point value. | `proven_result_f64_t`. |
| `proven_parse_double_ascii(view)` | Parse one locale-free ASCII float token without skipping whitespace. | `proven_parse_double_result_t`. |
| `proven_parse_f64_ascii(view)` | Compatibility alias for the same locale-free binary64 parser. | `proven_parse_f64_result_t`. |
| `proven_strtod(nptr, endptr)` | Parse one `strtod`-style token with whitespace skipping and `endptr` reporting. | `double`. |
| `proven_scan_str(scan)` | Parse a whitespace-delimited token as view into input. | `proven_result_u8str_view_t`. |
| `proven_scan_skip_until(scan, target)` | Move cursor to target if found. | `proven_err_t`. |
| `proven_scan_skip_until_number(scan)` | Move cursor to next number-looking position. | void. |

### Scan argument types

`proven_scan_arg_type_t` identifies the destination kind stored in a `proven_scan_arg_t`. `PROVEN_SCAN_ARG(&x)` supports pointers to:

- `short`, `unsigned short`
- `int`, `unsigned int`
- `long`, `unsigned long`
- `long long`, `unsigned long long`
- `double`
- `proven_u8str_view_t`

Native destination constructors:

- `proven_scan_arg_short`, `proven_scan_arg_ushort`
- `proven_scan_arg_int`, `proven_scan_arg_uint`
- `proven_scan_arg_long`, `proven_scan_arg_ulong`
- `proven_scan_arg_llong`, `proven_scan_arg_ullong`

Explicit fixed-width and utility helpers:

- `proven_scan_arg_i32`, `proven_scan_arg_u32`
- `proven_scan_arg_i64`, `proven_scan_arg_u64`
- `proven_scan_arg_f64`
- `proven_scan_arg_str_view`
- `proven_scan_arg_none`
- `proven_scan_arg_identity`

Long aliases:

```c
#define PROVEN_SCAN_ARG_LONG(ptr)  proven_scan_arg_long(ptr)
#define PROVEN_SCAN_ARG_ULONG(ptr) proven_scan_arg_ulong(ptr)
```

### Format scanning macros

| Macro | Intent |
|---|---|
| `PROVEN_SCAN_ARG(x)` | `_Generic` destination selector. |
| `proven_scan_fmt_cursor(scan_ptr, fmt, ...)` | Scan from an existing cursor. |
| `proven_scan_fmt(view, fmt, ...)` | Scan from a view with a temporary cursor. |

### Scan engine

```c
proven_err_t proven_scan_fmt_internal(
    proven_scan_t *scan,
    const char *fmt,
    const proven_scan_arg_t *args,
    proven_size_t args_count
);
```

| API | Intent | Return |
|---|---|---|
| `proven_scan_fmt_internal(scan, fmt, args, args_count)` | Structural scanner engine over an existing cursor. | `proven_err_t`. |
| `proven_scan_fmt_internal_view(view, fmt, args, count)` | Convenience wrapper that creates a temporary scanner over a view. | `proven_err_t`. |

Normal user code should call the macros. The engine expects a leading sentinel argument.

Important behavior: `proven_scan_fmt_internal()` may advance the cursor and write earlier destinations before returning an error if a later literal mismatch occurs. Save the cursor and destination values first if you need transaction-like parsing.

Example:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("ID: 402 SCORE: 99.5 ada"));
int id = 0;
double score = 0.0;
proven_u8str_view_t user = {0};

proven_err_t e = proven_scan_fmt_cursor(
    &scan,
    "ID: {} SCORE: {} {}",
    PROVEN_SCAN_ARG(&id),
    PROVEN_SCAN_ARG(&score),
    PROVEN_SCAN_ARG(&user)
);
if (!proven_is_ok(e)) return e;
```

### Float parsing notes

- `proven_scan_f64()` and `proven_parse_double_ascii()` route through the shared
  decimal-to-binary64 backend.
- Finite decimal inputs are rounded to IEEE-754 binary64 with
  round-to-nearest, ties-to-even behavior.
- The current conversion stack is `Clinger fast path -> staged
  Eisel-Lemire layer -> exact bigint fallback`, with internal counters used by
  tests to confirm which path took a representative input.
- The staged Eisel-Lemire layer currently accepts generated-`5^q` positive
  exponent cases and exact negative-exponent cases where the decimal
  significand cleanly cancels the required `5^q`.
- On compilers with `__uint128_t`, the same layer also accepts a conservative
  rounded negative-exponent ratio subset in normal-range cases, including
  wider left-shift normalization than the original narrow prototype allowed.
- The widened cached-power product path now also stages some subnormal cases,
  including `5e-324`, while values below the half-threshold to true-min still
  defer to the exact bigint fallback.
- The checked-in cached `5^q` constants are generated locally by
  `scripts/generate_float_decimal_tables.py`.
- `proven_parse_double_ascii()` does not skip leading whitespace.
- `proven_strtod()` skips leading ASCII whitespace, updates `endptr`, returns
  signed infinity on overflow, and preserves signed zero on underflow.

## 5. Examples and misuse cases

### Views are not C strings

Wrong:

```c
proven_u8str_view_t view = get_view();
printf("%s\n", (const char *)view.ptr); /* wrong: view may not be NUL-terminated */
```

Correct:

```c
proven_result_cstr_t c = proven_u8str_view_to_cstr(view, alloc);
if (!proven_is_ok(c.err)) return c.err;
call_c_api(c.value);
alloc.free_fn(alloc.ctx, (void *)c.value);
```

### `PROVEN_LIT` is for literals

Correct:

```c
proven_u8str_view_t a = PROVEN_LIT("abc");
```

Wrong:

```c
const char *runtime = getenv("NAME");
proven_u8str_view_t a = PROVEN_LIT(runtime); /* wrong: macro requires literal syntax */
```

Use:

```c
proven_u8str_view_t a = proven_u8str_view_from_cstr(runtime);
```

### Distinguish not found from replaced

`proven_u8str_replace_first()` returns `PROVEN_OK` when the target is not found. Search first if that matters.

```c
proven_size_t at = proven_u8str_view_find(proven_u8str_as_view(&s), 0, PROVEN_LIT("old"));
if (at != PROVEN_INDEX_NOT_FOUND) {
    proven_u8str_replace_first(&s, 0, PROVEN_LIT("old"), PROVEN_LIT("new"));
}
```

### Bounded format input

Wrong:

```c
char *untrusted = get_untrusted_pointer();
proven_println("{}", PROVEN_ARG(untrusted));
/* wrong: C-string formatting scans until NUL */
```

Correct:

```c
proven_println("{}", PROVEN_ARG_CSTR_N(untrusted, max_len));
```

### Self-referential formatting

Wrong:

```c
const char *inside = proven_u8str_as_cstr(&s);
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(inside));
/* wrong: self-aliasing C-string arguments are rejected */
```

Correct:

```c
proven_u8str_view_t before = proven_u8str_as_view(&s);
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(before));
```

### Transactional scanning

Wrong assumption:

```c
proven_err_t e = proven_scan_fmt_cursor(&scan, "{} suffix", PROVEN_SCAN_ARG(&x));
/* if suffix mismatches, x and scan.cursor may already have changed */
```

Correct pattern:

```c
proven_size_t old_cursor = scan.cursor;
int old_x = x;
proven_err_t e = proven_scan_fmt_cursor(&scan, "{} suffix", PROVEN_SCAN_ARG(&x));
if (!proven_is_ok(e)) {
    scan.cursor = old_cursor;
    x = old_x;
}
```
