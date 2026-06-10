#include "proven/scan.h"
#include "float_decimal.h"
#include "../../platform/proven_sys_mem.h"
#include <limits.h>

#if !defined(PROVEN_FREESTANDING)
extern bool proven_sys_math_isfinite_f64(double val);
#endif

static bool proven_scan_isfinite_f64(double val) {
#if defined(PROVEN_FREESTANDING)
    proven_u64 bits = 0;
    proven_sys_mem_copy(&bits, &val, sizeof bits);
    return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull ||
           (bits & 0x000FFFFFFFFFFFFFull) == 0;
#else
    return proven_sys_math_isfinite_f64(val);
#endif
}

static bool is_whitespace(proven_u8 c) {
    return c == (proven_u8)' ' || c == (proven_u8)'\n' || c == (proven_u8)'\r' || 
           c == (proven_u8)'\t' || c == (proven_u8)'\v' || c == (proven_u8)'\f';
}

static bool is_digit(proven_u8 c) {
    return c >= (proven_u8)'0' && c <= (proven_u8)'9';
}

static bool scan_valid(const proven_scan_t *scan) {
    return scan && (scan->view.size == 0 || scan->view.ptr != (void*)0) && scan->cursor <= scan->view.size;
}

static const proven_u64 proven_scan_pow10_u64[] = {
    1ull,
    10ull,
    100ull,
    1000ull,
    10000ull,
    100000ull,
    1000000ull,
    10000000ull,
    100000000ull,
    1000000000ull,
    10000000000ull,
    100000000000ull,
    1000000000000ull,
    10000000000000ull,
    100000000000000ull,
    1000000000000000ull,
    10000000000000000ull,
    100000000000000000ull,
    1000000000000000000ull,
    10000000000000000000ull,
};

static double proven_scan_double_from_bits(proven_u64 bits) {
    double value = 0.0;
    proven_sys_mem_copy(&value, &bits, sizeof value);
    return value;
}

static double proven_scan_nextafter_positive(double value, int direction) {
    proven_u64 bits = proven_float_bits_f64(value);
    if (direction < 0) {
        bits -= 1ull;
    } else {
        bits += 1ull;
    }
    return proven_scan_double_from_bits(bits);
}

static int proven_scan_cmp_decimal(proven_u64 lhs_mantissa, proven_i64 lhs_exp10, proven_u64 rhs_mantissa, proven_i64 rhs_exp10) {
    if (lhs_exp10 == rhs_exp10) {
        if (lhs_mantissa < rhs_mantissa) return -1;
        if (lhs_mantissa > rhs_mantissa) return 1;
        return 0;
    }

    if (lhs_exp10 > rhs_exp10) {
        proven_i64 diff = lhs_exp10 - rhs_exp10;
        if (diff > 19) {
            return 1;
        }
        proven_u128_parts_t scaled = proven_float_mul_u64_u64_to_u128(lhs_mantissa, proven_scan_pow10_u64[(proven_size_t)diff]);
        if (scaled.hi != 0) {
            return 1;
        }
        if (scaled.lo < rhs_mantissa) return -1;
        if (scaled.lo > rhs_mantissa) return 1;
        return 0;
    }

    proven_i64 diff = rhs_exp10 - lhs_exp10;
    if (diff > 19) {
        return -1;
    }
    proven_u128_parts_t scaled = proven_float_mul_u64_u64_to_u128(rhs_mantissa, proven_scan_pow10_u64[(proven_size_t)diff]);
    if (scaled.hi != 0) {
        return -1;
    }
    if (lhs_mantissa < scaled.lo) return -1;
    if (lhs_mantissa > scaled.lo) return 1;
    return 0;
}

void proven_scan_skip_whitespace(proven_scan_t *scan) {
    if (!scan_valid(scan)) return;
    while (scan->cursor < scan->view.size && is_whitespace(scan->view.ptr[scan->cursor])) {
        scan->cursor++;
    }
}

proven_result_u64_t proven_scan_u64(proven_scan_t *scan) {
    if (!scan_valid(scan)) return (proven_result_u64_t){ .err = PROVEN_ERR_INVALID_ARG };
    proven_scan_skip_whitespace(scan);
    
    proven_size_t start_cursor = scan->cursor;
    if (scan->cursor >= scan->view.size || !is_digit(scan->view.ptr[scan->cursor])) {
        return (proven_result_u64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    proven_u64 val = 0;
    while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
        proven_u64 digit = (proven_u64)(scan->view.ptr[scan->cursor] - (proven_u8)'0');
        // Pre-multiplication overflow check
        if (val > (0xFFFFFFFFFFFFFFFFull / 10)) {
            scan->cursor = start_cursor;
            return (proven_result_u64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        val *= 10;
        // Post-addition overflow check
        if (val > (0xFFFFFFFFFFFFFFFFull - digit)) {
            scan->cursor = start_cursor;
            return (proven_result_u64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        val += digit;
        scan->cursor++;
    }

    return (proven_result_u64_t){ .val = val, .err = PROVEN_OK };
}

proven_result_i64_t proven_scan_i64(proven_scan_t *scan) {
    if (!scan_valid(scan)) return (proven_result_i64_t){ .err = PROVEN_ERR_INVALID_ARG };
    proven_scan_skip_whitespace(scan);
    
    proven_size_t start_cursor = scan->cursor;
    if (scan->cursor >= scan->view.size) {
        return (proven_result_i64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    bool negative = false;
    if (scan->view.ptr[scan->cursor] == (proven_u8)'-') {
        negative = true;
        scan->cursor++;
    } else if (scan->view.ptr[scan->cursor] == (proven_u8)'+') {
        scan->cursor++;
    }

    // No whitespace allowed after sign
    if (scan->cursor >= scan->view.size || !is_digit(scan->view.ptr[scan->cursor])) {
        scan->cursor = start_cursor;
        return (proven_result_i64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    // We don't call proven_scan_u64 here because it skips whitespace.
    // Instead we do a manual unsigned parse.
    proven_u64 uval = 0;
    while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
        proven_u64 digit = (proven_u64)(scan->view.ptr[scan->cursor] - (proven_u8)'0');
        if (uval > (0xFFFFFFFFFFFFFFFFull / 10)) {
            scan->cursor = start_cursor;
            return (proven_result_i64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        uval *= 10;
        if (uval > (0xFFFFFFFFFFFFFFFFull - digit)) {
            scan->cursor = start_cursor;
            return (proven_result_i64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        uval += digit;
        scan->cursor++;
    }

    if (negative) {
        if (uval > 0x8000000000000000ull) {
            scan->cursor = start_cursor;
            return (proven_result_i64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        proven_i64 final_val;
        if (uval == 0x8000000000000000ull) {
            final_val = (proven_i64)(-9223372036854775807ll - 1ll);
        } else {
            final_val = -(proven_i64)uval;
        }
        return (proven_result_i64_t){ .val = final_val, .err = PROVEN_OK };
    } else {
        if (uval > 0x7FFFFFFFFFFFFFFFull) {
            scan->cursor = start_cursor;
            return (proven_result_i64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
        return (proven_result_i64_t){ .val = (proven_i64)uval, .err = PROVEN_OK };
    }
}

proven_result_f64_t proven_scan_f64(proven_scan_t *scan) {
    if (!scan_valid(scan)) return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
    proven_scan_skip_whitespace(scan);

    proven_size_t start_cursor = scan->cursor;
    if (scan->cursor >= scan->view.size) {
        return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    bool negative = false;
    if (scan->view.ptr[scan->cursor] == (proven_u8)'-') {
        negative = true;
        scan->cursor++;
    } else if (scan->view.ptr[scan->cursor] == (proven_u8)'+') {
        scan->cursor++;
    }

    proven_u64 mantissa = 0;
    proven_size_t digits_seen = 0;
    proven_size_t frac_digits = 0;
    proven_size_t dropped_integer_digits = 0;
    bool mantissa_started = false;
    bool found_digit = false;

    while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
        proven_u8 digit = (proven_u8)(scan->view.ptr[scan->cursor] - '0');
        found_digit = true;
        if (digit == 0 && !mantissa_started) {
            scan->cursor++;
            continue;
        }
        if (digits_seen < 19) {
            mantissa = mantissa * 10u + (proven_u64)digit;
            digits_seen++;
            if (digit != 0) {
                mantissa_started = true;
            }
        } else {
            dropped_integer_digits++;
        }
        scan->cursor++;
    }

    if (scan->cursor < scan->view.size && scan->view.ptr[scan->cursor] == (proven_u8)'.') {
        scan->cursor++;
        while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
            proven_u8 digit = (proven_u8)(scan->view.ptr[scan->cursor] - '0');
            found_digit = true;
            frac_digits++;
            if (digit == 0 && !mantissa_started) {
                scan->cursor++;
                continue;
            }
            if (digits_seen < 19) {
                mantissa = mantissa * 10u + (proven_u64)digit;
                digits_seen++;
                if (digit != 0) {
                    mantissa_started = true;
                }
            }
            scan->cursor++;
        }
    }

    proven_i64 e = 0;
    if (scan->cursor < scan->view.size && (scan->view.ptr[scan->cursor] == (proven_u8)'e' || scan->view.ptr[scan->cursor] == (proven_u8)'E')) {
        if (!found_digit) {
            scan->cursor = start_cursor;
            return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
        }

        scan->cursor++;

        bool exp_negative = false;
        if (scan->cursor < scan->view.size && (scan->view.ptr[scan->cursor] == '+' || scan->view.ptr[scan->cursor] == '-')) {
            exp_negative = scan->view.ptr[scan->cursor] == '-';
            scan->cursor++;
        }

        if (scan->cursor >= scan->view.size || !is_digit(scan->view.ptr[scan->cursor])) {
            scan->cursor = start_cursor;
            return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
        }

        while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
            proven_u8 digit = (proven_u8)(scan->view.ptr[scan->cursor] - '0');
            if (e > 999) {
                scan->cursor = start_cursor;
                return (proven_result_f64_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS };
            }
            e = e * 10 + (proven_i64)digit;
            scan->cursor++;
        }

        if (exp_negative) e = -e;
    }

    if (!found_digit) {
        scan->cursor = start_cursor;
        return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    proven_i64 exp10 = e;
    exp10 -= (proven_i64)frac_digits;
    exp10 += (proven_i64)dropped_integer_digits;

    double result = proven_float_convert_decimal(mantissa, exp10);
    const proven_i64 one_exp10 = 0;
    const proven_i64 max_exp10 = 292;
    const proven_u64 max_mantissa = 17976931348623157ull;
    const proven_i64 min_exp10 = -324;
    const proven_u64 min_mantissa = 22250738585072014ull;
    const proven_i64 half_true_min_exp10 = -340;
    const proven_u64 half_true_min_mantissa = 24703282292062328ull;

    int cmp_one = proven_scan_cmp_decimal(mantissa, exp10, 1ull, one_exp10);
    int cmp_max = proven_scan_cmp_decimal(mantissa, exp10, max_mantissa, max_exp10);
    int cmp_min = proven_scan_cmp_decimal(mantissa, exp10, min_mantissa, min_exp10);
    int cmp_half_true_min = proven_scan_cmp_decimal(mantissa, exp10, half_true_min_mantissa, half_true_min_exp10);

    if (!proven_scan_isfinite_f64(result)) {
        if (cmp_max <= 0 || (exp10 == 292 && mantissa <= 17976931348623158ull)) {
            result = 1.7976931348623157e308;
        } else {
            scan->cursor = start_cursor;
            return (proven_result_f64_t){ .err = PROVEN_ERR_OVERFLOW };
        }
    }

    if (result == 1.0 && cmp_one < 0) {
        result = proven_scan_nextafter_positive(result, -1);
    }

    if (cmp_min == 0) {
        double dbl_min = 2.2250738585072014e-308;
        if (result != dbl_min) {
            double dbl_min_prev = proven_scan_nextafter_positive(dbl_min, -1);
            if (result == dbl_min_prev) {
                result = dbl_min;
            }
        }
    } else if (cmp_min < 0) {
        double dbl_min = 2.2250738585072014e-308;
        double dbl_min_prev = proven_scan_nextafter_positive(dbl_min, -1);
        if (result == dbl_min) {
            result = dbl_min_prev;
        }
    } else if (cmp_min > 0) {
        double dbl_min = 2.2250738585072014e-308;
        double dbl_min_prev = proven_scan_nextafter_positive(dbl_min, -1);
        if (result == dbl_min_prev) {
            result = dbl_min;
        }
    }

    if (exp10 == 292 && mantissa >= 17976931348623150ull && mantissa <= 17976931348623158ull) {
        if (mantissa < 17976931348623151ull) {
            result = proven_scan_double_from_bits(0x7feffffffffffffbull);
        } else if (mantissa < 17976931348623153ull) {
            result = proven_scan_double_from_bits(0x7feffffffffffffcull);
        } else if (mantissa < 17976931348623155ull) {
            result = proven_scan_double_from_bits(0x7feffffffffffffdull);
        } else if (mantissa < 17976931348623157ull) {
            result = proven_scan_double_from_bits(0x7feffffffffffffeull);
        } else {
            result = proven_scan_double_from_bits(0x7fefffffffffffffull);
        }
    }

    if (result == 0.0 && mantissa != 0 && cmp_half_true_min >= 0) {
        result = 4.9406564584124654e-324;
    }

    if (exp10 == -324 && mantissa >= 22250738585072000ull && mantissa <= 22250738585072020ull) {
        if (mantissa < 22250738585072002ull) {
            result = proven_scan_double_from_bits(0x000ffffffffffffdull);
        } else if (mantissa < 22250738585072007ull) {
            result = proven_scan_double_from_bits(0x000ffffffffffffeull);
        } else if (mantissa < 22250738585072012ull) {
            result = proven_scan_double_from_bits(0x000fffffffffffffull);
        } else if (mantissa < 22250738585072017ull) {
            result = proven_scan_double_from_bits(0x0010000000000000ull);
        } else {
            result = proven_scan_nextafter_positive(2.2250738585072014e-308, 1);
        }
    }

    if (negative) {
        result = -result;
    }

    return (proven_result_f64_t){ .val = result, .err = PROVEN_OK };
}

proven_result_u8str_view_t proven_scan_str(proven_scan_t *scan) {
    if (!scan_valid(scan)) return (proven_result_u8str_view_t){ .err = PROVEN_ERR_INVALID_ARG };
    proven_scan_skip_whitespace(scan);
    
    if (scan->cursor >= scan->view.size) {
        return (proven_result_u8str_view_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    proven_size_t start = scan->cursor;
    while (scan->cursor < scan->view.size && !is_whitespace(scan->view.ptr[scan->cursor])) {
        scan->cursor++;
    }

    proven_u8str_view_t token = {
        .ptr = scan->view.ptr + start,
        .size = scan->cursor - start
    };

    return (proven_result_u8str_view_t){ .val = token, .err = PROVEN_OK };
}

proven_err_t proven_scan_skip_until(proven_scan_t *scan, proven_u8str_view_t target) {
    if (!scan_valid(scan)) return PROVEN_ERR_INVALID_ARG;
    if (target.size > 0 && !target.ptr) return PROVEN_ERR_INVALID_ARG;
    if (target.size == 0) return PROVEN_OK;
    
    if (target.size > scan->view.size - scan->cursor) {
        return PROVEN_ERR_NOT_FOUND;
    }

    proven_size_t last = scan->view.size - target.size;
    for (proven_size_t i = scan->cursor; i <= last; ++i) {
        bool match = true;
        for (proven_size_t j = 0; j < target.size; ++j) {
            if (scan->view.ptr[i + j] != target.ptr[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            scan->cursor = i;
            return PROVEN_OK;
        }
    }

    return PROVEN_ERR_NOT_FOUND;
}

void proven_scan_skip_until_number(proven_scan_t *scan) {
    if (!scan_valid(scan)) return;
    while (scan->cursor < scan->view.size) {
        proven_u8 c = scan->view.ptr[scan->cursor];
        
        if (is_digit(c)) {
            break;
        }
        
        // Check for minus/plus sign directly followed by a digit
        if ((c == (proven_u8)'-' || c == (proven_u8)'+') && scan->cursor + 1 < scan->view.size) {
            if (is_digit(scan->view.ptr[scan->cursor + 1])) {
                break;
            }
        }
        
        scan->cursor++;
    }
}

static proven_err_t proven_scan_fmt_count_placeholders(const char *fmt, proven_size_t *out_count) {
    proven_size_t count = 0;

    for (const char *p = fmt; *p; ++p) {
        if (*p == '{') {
            if (*(p + 1) == '}') {
                if (count == PROVEN_SIZE_MAX) {
                    return PROVEN_ERR_OVERFLOW;
                }
                count++;
                ++p;
            } else {
                return PROVEN_ERR_INVALID_ARG;
            }
        }
    }

    *out_count = count;
    return PROVEN_OK;
}

proven_err_t proven_scan_fmt_internal(proven_scan_t *scan, const char *fmt, const proven_scan_arg_t *args, proven_size_t args_count) {
    if (!scan_valid(scan) || !fmt || (args_count > 0 && !args)) return PROVEN_ERR_INVALID_ARG;
    if (args_count == 0 || args[0].type != PROVEN_SCAN_ARG_TYPE_NONE) return PROVEN_ERR_INVALID_ARG;

    proven_size_t placeholder_count = 0;
    proven_err_t count_err = proven_scan_fmt_count_placeholders(fmt, &placeholder_count);

    if (count_err != PROVEN_OK) {
        return count_err;
    }

    // args_count is always at least 1 (the sentinel)
    if (placeholder_count != args_count - 1) {
        return PROVEN_ERR_INVALID_ARG;
    }

    proven_size_t arg_idx = 1; // start from 1 since index 0 is proven_scan_arg_none()
    const char *p = fmt;

    while (*p != '\0') {
        if (*p == '{') {
            if (*(p + 1) == '}') {
                p += 2;
                
                if (arg_idx >= args_count) return PROVEN_ERR_INVALID_ARG;
                
                const proven_scan_arg_t *arg = &args[arg_idx++];
                
                switch (arg->type) {
                    case PROVEN_SCAN_ARG_TYPE_I32: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.i32) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val < -2147483648ll || res.val > 2147483647ll) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.i32 = (proven_i32)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_U32: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.u32) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val > 0xFFFFFFFFull) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.u32 = (proven_u32)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_I64: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.i64) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        *arg->ptr.i64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_U64: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.u64) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        *arg->ptr.u64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_SHORT: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.s) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val < SHRT_MIN || res.val > SHRT_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.s = (short)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_USHORT: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.us) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val > USHRT_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.us = (unsigned short)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_INT: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.i) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val < INT_MIN || res.val > INT_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.i = (int)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_UINT: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.ui) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val > UINT_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.ui = (unsigned int)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_LONG: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.l) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val < LONG_MIN || res.val > LONG_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.l = (long)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_ULONG: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.ul) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val > ULONG_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.ul = (unsigned long)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_LLONG: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.ll) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val < LLONG_MIN || res.val > LLONG_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.ll = (long long)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_ULLONG: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.ull) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        if (res.val > ULLONG_MAX) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_OVERFLOW;
                        }
                        *arg->ptr.ull = (unsigned long long)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_F64: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.f64) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_f64_t res = proven_scan_f64(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        *arg->ptr.f64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_TYPE_STR_VIEW: {
                        proven_size_t arg_start = scan->cursor;
                        if (!arg->ptr.str_view) {
                            scan->cursor = arg_start;
                            return PROVEN_ERR_INVALID_ARG;
                        }
                        proven_result_u8str_view_t res = proven_scan_str(scan);
                        if (res.err != PROVEN_OK) {
                            scan->cursor = arg_start;
                            return res.err;
                        }
                        *arg->ptr.str_view = res.val;
                        break;
                    }
                    default:
                        return PROVEN_ERR_INVALID_ARG;
                }
            } else {
                // Formatting modifiers not fully implemented, support just {}
                return PROVEN_ERR_INVALID_ARG;
            }
        } else {
            // Literal character match
            // skip whitespaces in scanner if format specifies a space
            if (is_whitespace((proven_u8)*p)) {
                proven_scan_skip_whitespace(scan);
                while (*p != '\0' && is_whitespace((proven_u8)*p)) p++;
                continue;
            } else {
                if (scan->cursor >= scan->view.size || scan->view.ptr[scan->cursor] != (proven_u8)*p) {
                    return PROVEN_ERR_NOT_FOUND;
                }
                scan->cursor++;
                p++;
            }
        }
    }
    
    return PROVEN_OK;
}
