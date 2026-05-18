# Chapter 1: Foundation API

This chapter covers `types.h`, `error.h`, `memory.h`, `align.h`, `version.h`, and `panic.h`.

## Table of contents

1. [Fundamental types](#1-fundamental-types)
2. [Error model](#2-error-model)
3. [Checked arithmetic](#3-checked-arithmetic)
4. [Memory views](#4-memory-views)
5. [Alignment helpers](#5-alignment-helpers)
6. [Version macros](#6-version-macros)
7. [Panic hook](#7-panic-hook)
8. [Examples and misuse cases](#8-examples-and-misuse-cases)

## 1. Fundamental types

### Fixed-width aliases

| Name | Meaning | Notes |
|---|---|---|
| `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64` | Signed fixed-width integers | Aliases of standard integer types. |
| `proven_u8`, `proven_u32`, `proven_u64` | Unsigned fixed-width integers | `proven_u8` is numeric; use `proven_byte_t` for raw object bytes. |
| `proven_u16` | 16-bit code unit type | Uses `char16_t` when `<uchar.h>` is available, otherwise `uint_least16_t`. Current U16 string APIs use this type for code units. |
| `proven_byte_t` | Byte-level object representation | Alias of `unsigned char`; use this for raw memory inspection and copying. |
| `proven_size_t` | Size and index type | Alias of `size_t`. |
| `proven_ptrdiff_t` | Pointer difference and signed offset type | Alias of `ptrdiff_t`. |
| `proven_intptr_t`, `proven_uintptr_t` | Pointer-sized integer types | Used only for explicit pointer-integer work such as arena range checks. |

### `proven_result_size_t`

```c
typedef struct {
    proven_err_t  err;
    proven_size_t value;
} proven_result_size_t;
```

Purpose: return either a size or an error. The size is valid only when `err == PROVEN_OK`.

Common users: partial append operations, file read/write, file size queries.

## 2. Error model

### `proven_err_t`

```c
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

| Error | Typical meaning |
|---|---|
| `PROVEN_OK` | Success. |
| `PROVEN_ERR_NOMEM` | Allocator could not provide memory. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | Index, size, capacity, or range is invalid. |
| `PROVEN_ERR_INVALID_ENCODING` | Encoded text failed validation. |
| `PROVEN_ERR_INVALID_ARG` | Null pointer, invalid allocator, impossible mode, or wrong argument count. |
| `PROVEN_ERR_IO` | OS or device I/O failed. |
| `PROVEN_ERR_NOT_FOUND` | File, key, substring, or resource does not exist. |
| `PROVEN_ERR_INVALID_STATE` | Object state does not allow the requested operation. |
| `PROVEN_ERR_OVERFLOW` | Integer conversion or size arithmetic overflowed. |
| `PROVEN_ERR_UNSUPPORTED` | Feature is unavailable on this platform or build profile. |
| `PROVEN_ERR_AGAIN` | Retry later. |
| `PROVEN_ERR_EOF` | End of input. |
| `PROVEN_ERR_BUSY` | Queue, lock, or resource is busy. |
| `PROVEN_ERR_PERMISSION` | Access was denied. |
| `PROVEN_ERR_INVALID_FORMAT` | Format or scan template is invalid. |

### Error helpers

```c
static inline int proven_is_ok(proven_err_t err);
#define PROVEN_IS_OK(err) proven_is_ok(err)
```

Purpose: make success checks explicit and readable.

Return: non-zero when `err == PROVEN_OK`, zero otherwise.

Correct:

```c
proven_err_t err = do_work();
if (!proven_is_ok(err)) {
    return err;
}
```

Wrong:

```c
if (err) {
    /* works today because PROVEN_OK is 0, but it hides the API convention */
}
```

## 3. Checked arithmetic

### `PROVEN_SIZE_MAX`

Maximum value of `proven_size_t`.

### `PROVEN_CKD_ADD`, `PROVEN_CKD_SUB`, `PROVEN_CKD_MUL`

```c
#define PROVEN_CKD_ADD(res, a, b) ...
#define PROVEN_CKD_SUB(res, a, b) ...
#define PROVEN_CKD_MUL(res, a, b) ...
```

Purpose: checked integer arithmetic for size and offset calculations.

Parameters:

- `res`: pointer to the output object.
- `a`, `b`: operands.

Return: true on overflow, false on success.

Correct:

```c
proven_size_t total = 0;
if (PROVEN_CKD_MUL(&total, count, sizeof(Item))) {
    return PROVEN_ERR_OVERFLOW;
}
```

Wrong:

```c
proven_size_t total = count * sizeof(Item); /* wrong: may wrap */
```

## 4. Memory views

### `proven_mem_t`

```c
typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_t;
```

Purpose: describes an owned memory block. Ownership is not enforced by the type; callers still need to free with the correct allocator.

### `proven_mem_view_t`

```c
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_view_t;
```

Purpose: borrowed read-only byte range. It does not own memory and is not NUL-terminated by contract.

### `proven_mem_mut_t`

```c
typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_mut_t;
```

Purpose: borrowed mutable byte range.

### `proven_result_mem_mut_t`

```c
typedef struct {
    proven_err_t err;
    proven_mem_mut_t value;
} proven_result_mem_mut_t;
```

Purpose: allocator and slice-producing result type.

### `proven_result_mem_view_t`

```c
typedef struct {
    proven_err_t err;
    proven_mem_view_t value;
} proven_result_mem_view_t;
```

Purpose: checked slicing result type.

### Memory functions and inline helpers

| API | Purpose | Parameters | Return |
|---|---|---|---|
| `proven_mem_view_from_owned(mem)` | Create a read-only view from an owned block. | `mem`: memory block. | `proven_mem_view_t`. |
| `proven_mem_mut_from_owned(mem)` | Create a mutable view from an owned block. | `mem`: memory block. | `proven_mem_mut_t`. |
| `proven_mem_view_slice_unchecked(view, offset, size)` | Make a subview without validation. | `view`, `offset`, `size`. | `proven_mem_view_t`. |
| `proven_mem_view_slice_checked(view, offset, size)` | Make a checked subview. | `view`, `offset`, `size`. | `proven_result_mem_view_t`. |
| `proven_mem_mut_slice_unchecked(mut, offset, size)` | Make a mutable subslice without validation. | `mut`, `offset`, `size`. | `proven_mem_mut_t`. |
| `proven_mem_mut_slice_checked(mut, offset, size)` | Make a checked mutable subslice. | `mut`, `offset`, `size`. | `proven_result_mem_mut_t`. |
| `proven_range_contains_ptr(base, cap, ptr, size, out_offset)` | Check whether a pointer range is inside a base allocation using integer address checks. | `base`, `cap`, `ptr`, `size`, optional `out_offset`. | `_Bool`. |
| `proven_memcmp(s1, s2, size)` | Compare raw memory regions. | Two pointers and byte size. | Zero if equal, negative or positive by byte order. |

Checked slice behavior:

- If `view.size > 0 && view.ptr == NULL`, returns `PROVEN_ERR_INVALID_ARG`.
- If the range is outside the view, returns `PROVEN_ERR_OUT_OF_BOUNDS`.
- If requested `size == 0`, returns an empty view with null pointer and `PROVEN_OK`.

## 5. Alignment helpers

### Macros

| Macro | Meaning |
|---|---|
| `PROVEN_DEFAULT_ALIGNMENT` | Default alignment, currently 8. |
| `PROVEN_MAX_ALIGN` | `alignof(max_align_t)`. |

### Functions

| API | Purpose | Return |
|---|---|---|
| `proven_is_pow2(x)` | Test whether `x` is a non-zero power of two. | `bool`. |
| `proven_mem_align_up(addr, align)` | Align a size/address upward. | Aligned value, or 0 if `align` is invalid or overflow occurs. |
| `proven_uintptr_align_up(addr, align)` | Align a `proven_uintptr_t` address upward. | Aligned value, or 0 on invalid alignment or overflow. |

Correct:

```c
proven_size_t aligned = proven_mem_align_up(size, alignof(Item));
if (aligned == 0) {
    return PROVEN_ERR_OVERFLOW;
}
```

Wrong:

```c
proven_size_t aligned = (size + align - 1) & ~(align - 1); /* wrong: may overflow */
```

## 6. Version macros

```c
#define PROVEN_VERSION_STRING "proven_c_lib-v26.05.19"
#define PROVEN_VERSION_NUM    260519
#define PROVEN_VERSION_SUFFIX ""
```

Purpose: compile-time version identification.

Use `PROVEN_VERSION_STRING` in diagnostics and build reports. Use `PROVEN_VERSION_NUM` only for numeric comparisons.

## 7. Panic hook

```c
void proven_panic_handler(const char *msg);
```

Purpose: handle terminal failure paths used by panic-style APIs such as `proven_arena_alloc_or_panic()`.

Default behavior: the library provides a weak default implementation that traps.

User override:

```c
void proven_panic_handler(const char *msg) {
    log_critical(msg);
    for (;;) {
        /* reset, halt, or wait for debugger */
    }
}
```

Contract: production panic handlers should not return. If a panic handler returns, `_or_panic` result validity is not guaranteed.

## 8. Examples and misuse cases

### Safe result handling

```c
proven_result_mem_view_t part = proven_mem_view_slice_checked(view, 4, 8);
if (!proven_is_ok(part.err)) {
    return part.err;
}
consume(part.value);
```

### Unchecked slicing only after proof

Correct when the caller already proved the range:

```c
if (offset <= view.size && size <= view.size - offset) {
    proven_mem_view_t part = proven_mem_view_slice_unchecked(view, offset, size);
    consume(part);
}
```

Wrong:

```c
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were not validated */
```

### Empty views are allowed

A zero-size view may have a null pointer. Do not reject it blindly.

Wrong:

```c
if (!view.ptr) return PROVEN_ERR_INVALID_ARG; /* wrong for empty views */
```

Better:

```c
if (view.size > 0 && !view.ptr) return PROVEN_ERR_INVALID_ARG;
```
