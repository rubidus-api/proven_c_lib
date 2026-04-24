#ifndef PROVEN_FMT_H
#define PROVEN_FMT_H

#include "u8str.h"
#include "time.h"

/**
 * @brief Supported argument types for the structural formatter.
 */
typedef enum {
    PROVEN_ARG_NONE,
    PROVEN_ARG_I32,
    PROVEN_ARG_U32,
    PROVEN_ARG_I64,
    PROVEN_ARG_U64,
    PROVEN_ARG_F64,
    PROVEN_ARG_CSTR,
    PROVEN_ARG_STR_VIEW,
    PROVEN_ARG_DATETIME,
    PROVEN_ARG_PTR,
} proven_arg_type_t;

/**
 * @brief Container for a single format argument.
 */
typedef struct {
    proven_arg_type_t type;
    union {
        proven_i32 i32;
        proven_u32 u32;
        proven_i64 i64;
        proven_u64 u64;
        double     f64;
        const char  *cstr;
        proven_u8str_view_t str_view;
        proven_datetime_t   datetime;
        const void         *ptr;
    } value;
} proven_arg_t;

/**
 * @brief Simple argument constructors.
 */
static inline proven_arg_t proven_arg_none(void) { return (proven_arg_t){PROVEN_ARG_NONE, {0}}; }
static inline proven_arg_t proven_arg_i32(int v) { return (proven_arg_t){PROVEN_ARG_I32, {.i32 = (proven_i32)v}}; }
static inline proven_arg_t proven_arg_u32(unsigned int v) { return (proven_arg_t){PROVEN_ARG_U32, {.u32 = (proven_u32)v}}; }
static inline proven_arg_t proven_arg_i64(long long v) { return (proven_arg_t){PROVEN_ARG_I64, {.i64 = (proven_i64)v}}; }
static inline proven_arg_t proven_arg_u64(unsigned long long v) { return (proven_arg_t){PROVEN_ARG_U64, {.u64 = (proven_u64)v}}; }
static inline proven_arg_t proven_arg_f64(double v) { return (proven_arg_t){PROVEN_ARG_F64, {.f64 = v}}; }
static inline proven_arg_t proven_arg_cstr(const char *v) { return (proven_arg_t){PROVEN_ARG_CSTR, {.cstr = v}}; }
static inline proven_arg_t proven_arg_str_view(proven_u8str_view_t v) { return (proven_arg_t){PROVEN_ARG_STR_VIEW, {.str_view = v}}; }
static inline proven_arg_t proven_arg_datetime(proven_datetime_t v) { return (proven_arg_t){PROVEN_ARG_DATETIME, {.datetime = v}}; }
static inline proven_arg_t proven_arg_ptr(const void *v) { return (proven_arg_t){PROVEN_ARG_PTR, {.ptr = v}}; }

/**
 * @brief Helper for unsigned char strings to avoid pointer-sign warnings.
 */
static inline proven_arg_t proven_arg_ucstr(const unsigned char *v) { return proven_arg_cstr((const char *)v); }

/**
 * @brief Identity function for arguments that are already proven_arg_t.
 */
static inline proven_arg_t proven_arg_identity(proven_arg_t v) { return v; }

/**
 * @brief Internal helper to capture function pointer bits safely.
 * Detects endianness at runtime and copies bytes to the least-significant-bit
 * side of a u64 to ensure the resulting address is logically correct.
 */
static inline proven_arg_t proven_arg_fn_impl(void (*fn)(void)) {
    proven_u64 addr = 0;
    
    // Runtime endianness detection
    const int endian_test = 1;
    bool is_le = (*(const unsigned char *)&endian_test == 1);
    
    const unsigned char *src = (const unsigned char *)&fn;
    unsigned char *dst = (unsigned char *)&addr;
    size_t fn_size = sizeof(fn);
    size_t u64_size = sizeof(addr);
    
    if (is_le) {
        // Little Endian: [LSB, ..., MSB]
        // Copy to the beginning of addr memory
        for (size_t i = 0; i < fn_size && i < u64_size; i++) {
            dst[i] = src[i];
        }
    } else {
        // Big Endian: [MSB, ..., LSB]
        // Copy to the end of addr memory to align LSBs
        size_t offset = (u64_size > fn_size) ? (u64_size - fn_size) : 0;
        for (size_t i = 0; i < fn_size && (i + offset) < u64_size; i++) {
            dst[i + offset] = src[i];
        }
    }
    
    // Now 'addr' holds the correct numerical value of the pointer.
    return proven_arg_ptr((const void *)(proven_uintptr_t)addr);
}

/**
 * @brief Safety macro for function pointers.
 * Casts the input to a generic function pointer first to avoid sizeof(function) issues.
 */
#define PROVEN_ARG_FN(f) proven_arg_fn_impl((void (*)(void))(f))

/**
 * @brief Type-safe argument selection using C11 _Generic.
 */
#define PROVEN_ARG(x) _Generic((x), \
    _Bool: proven_arg_i32, \
    char: proven_arg_i32, \
    signed char: proven_arg_i32, \
    unsigned char: proven_arg_u32, \
    short: proven_arg_i32, \
    unsigned short: proven_arg_u32, \
    int: proven_arg_i32, \
    unsigned int: proven_arg_u32, \
    long: proven_arg_i64, \
    unsigned long: proven_arg_u64, \
    long long: proven_arg_i64, \
    unsigned long long: proven_arg_u64, \
    double: proven_arg_f64, \
    float: proven_arg_f64, \
    const char*: proven_arg_cstr, \
    char*: proven_arg_cstr, \
    unsigned char*: proven_arg_ucstr, \
    const unsigned char*: proven_arg_ucstr, \
    void*: proven_arg_ptr, \
    const void*: proven_arg_ptr, \
    proven_u8str_view_t: proven_arg_str_view, \
    proven_datetime_t: proven_arg_datetime, \
    proven_arg_t: proven_arg_identity \
)(x)

/**
 * @brief Modern structural formatter using {} placeholders.
 */
[[nodiscard]]
proven_err_t proven_u8str_format_impl(proven_allocator_t alloc, proven_u8str_t *str, const char *fmt, const proven_arg_t *args, size_t args_count);

/**
 * @brief Variadic macro to format strings with type-safe arguments.
 * Usage: proven_u8str_append_fmt(alloc, &str, "Hello {}", PROVEN_ARG(name));
 */
#define proven_u8str_append_fmt(alloc, str, fmt, ...) \
    proven_u8str_format_impl(alloc, str, fmt, \
        (proven_arg_t[]){ proven_arg_none(), __VA_ARGS__ }, \
        (sizeof((proven_arg_t[]){ proven_arg_none(), __VA_ARGS__ }) / sizeof(proven_arg_t)))

#endif /* PROVEN_FMT_H */
