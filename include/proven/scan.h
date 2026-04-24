#ifndef PROVEN_SCAN_H
#define PROVEN_SCAN_H

#include "proven/types.h"
#include "proven/u8str.h"
#include "proven/error.h"

typedef struct {
    proven_err_t err;
    proven_i64 val;
} proven_result_i64_t;

typedef struct {
    proven_err_t err;
    proven_u64 val;
} proven_result_u64_t;

typedef struct {
    proven_err_t err;
    double val;
} proven_result_f64_t;

typedef struct {
    proven_err_t err;
    proven_u8str_view_t val;
} proven_result_u8str_view_t;

/**
 * @struct proven_scan_t
 * @brief Stateful scanner for parsing data from a string view.
 */
typedef struct {
    proven_u8str_view_t view;
    proven_size_t cursor;
} proven_scan_t;

/**
 * @brief Initialize a scanner with a string view.
 */
static inline proven_scan_t proven_scan_init(proven_u8str_view_t view) {
    return (proven_scan_t){ .view = view, .cursor = 0 };
}

/**
 * @brief Skip leading whitespace.
 */
void proven_scan_skip_whitespace(proven_scan_t *scan);

/**
 * @brief Extract a 64-bit signed integer.
 */
[[nodiscard]] proven_result_i64_t proven_scan_i64(proven_scan_t *scan);

/**
 * @brief Extract a 64-bit unsigned integer.
 */
[[nodiscard]] proven_result_u64_t proven_scan_u64(proven_scan_t *scan);

/**
 * @brief Extract a 64-bit floating point number.
 */
[[nodiscard]] proven_result_f64_t proven_scan_f64(proven_scan_t *scan);

/**
 * @brief Extract a string (delimited by whitespace).
 * Returns a view into the original string.
 */
[[nodiscard]] proven_result_u8str_view_t proven_scan_str(proven_scan_t *scan);

/**
 * @brief Skip current input until the target substring is found.
 * If found, cursor is placed at the start of the target.
 * If not found, cursor remains at current position and returns error.
 */
[[nodiscard]] proven_err_t proven_scan_skip_until(proven_scan_t *scan, proven_u8str_view_t target);

/**
 * @brief Skip all characters until a valid number (including its sign) is encountered.
 * Stops at the first digit (0-9) or a sign (+, -) immediately followed by a digit.
 */
void proven_scan_skip_until_number(proven_scan_t *scan);

/**
 * @brief Supported argument types for the structural scanner.
 */
typedef enum {
    PROVEN_SCAN_ARG_NONE,
    PROVEN_SCAN_ARG_I32,
    PROVEN_SCAN_ARG_U32,
    PROVEN_SCAN_ARG_I64,
    PROVEN_SCAN_ARG_U64,
    PROVEN_SCAN_ARG_F64,
    PROVEN_SCAN_ARG_CSTR_BUF,
    PROVEN_SCAN_ARG_STR_VIEW,
} proven_scan_arg_type_t;

/**
 * @brief Container for a single format scan argument.
 */
typedef struct {
    proven_scan_arg_type_t type;
    union {
        proven_i32 *i32;
        proven_u32 *u32;
        proven_i64 *i64;
        proven_u64 *u64;
        double     *f64;
        char       *cstr_buf;
        proven_u8str_view_t *str_view;
    } ptr;
} proven_scan_arg_t;

/**
 * @brief Simple argument constructors.
 */
static inline proven_scan_arg_t proven_scan_arg_none(void) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_NONE, {0}}; }
static inline proven_scan_arg_t proven_scan_arg_i32(proven_i32 *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_I32, {.i32 = v}}; }
static inline proven_scan_arg_t proven_scan_arg_u32(proven_u32 *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_U32, {.u32 = v}}; }
static inline proven_scan_arg_t proven_scan_arg_i64(proven_i64 *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_I64, {.i64 = v}}; }
static inline proven_scan_arg_t proven_scan_arg_u64(proven_u64 *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_U64, {.u64 = v}}; }
static inline proven_scan_arg_t proven_scan_arg_f64(double *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_F64, {.f64 = v}}; }
static inline proven_scan_arg_t proven_scan_arg_cstr_buf(char *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_CSTR_BUF, {.cstr_buf = v}}; }
static inline proven_scan_arg_t proven_scan_arg_str_view(proven_u8str_view_t *v) { return (proven_scan_arg_t){PROVEN_SCAN_ARG_STR_VIEW, {.str_view = v}}; }

/**
 * @brief Identity function for arguments that are already proven_scan_arg_t.
 */
static inline proven_scan_arg_t proven_scan_arg_identity(proven_scan_arg_t v) { return v; }

/**
 * @brief Type-safe argument selection using C11 _Generic for pointers.
 */
#define PROVEN_SCAN_ARG(x) _Generic((x), \
    int*: proven_scan_arg_i32, \
    unsigned int*: proven_scan_arg_u32, \
    long*: proven_scan_arg_i64, \
    unsigned long*: proven_scan_arg_u64, \
    long long*: proven_scan_arg_i64, \
    unsigned long long*: proven_scan_arg_u64, \
    double*: proven_scan_arg_f64, \
    char*: proven_scan_arg_cstr_buf, \
    proven_u8str_view_t*: proven_scan_arg_str_view, \
    proven_scan_arg_t: proven_scan_arg_identity \
)(x)

/**
 * @brief Modern structural scanner.
 * Expects exactly as many {} in the format string as provided arguments.
 */
[[nodiscard]]
proven_err_t proven_scan_fmt_impl(proven_scan_t *scan, const char *fmt, const proven_scan_arg_t *args, size_t args_count);

static inline proven_err_t proven_scan_fmt_impl_view(proven_u8str_view_t view, const char *fmt, const proven_scan_arg_t *args, size_t count) {
    proven_scan_t scan = proven_scan_init(view);
    return proven_scan_fmt_impl(&scan, fmt, args, count);
}

/**
 * @brief Variadic macro to scan from a given proven_scan_t cursor element.
 * Usage: proven_scan_fmt_cursor(&scan, "Hello {}", PROVEN_SCAN_ARG(&num));
 */
#define proven_scan_fmt_cursor(scan_ptr, fmt, ...) \
    proven_scan_fmt_impl(scan_ptr, fmt, \
        (proven_scan_arg_t[]){ proven_scan_arg_none(), __VA_ARGS__ }, \
        (sizeof((proven_scan_arg_t[]){ proven_scan_arg_none(), __VA_ARGS__ }) / sizeof(proven_scan_arg_t)))

/**
 * @brief Variadic macro to scan strings into strictly-typed out-pointers from a view.
 * Usage: proven_scan_fmt(view, "Hello {}", PROVEN_SCAN_ARG(&num));
 */
#define proven_scan_fmt(view, fmt, ...) \
    proven_scan_fmt_impl_view(view, fmt, \
        (proven_scan_arg_t[]){ proven_scan_arg_none(), __VA_ARGS__ }, \
        (sizeof((proven_scan_arg_t[]){ proven_scan_arg_none(), __VA_ARGS__ }) / sizeof(proven_scan_arg_t)))

#endif // PROVEN_SCAN_H
