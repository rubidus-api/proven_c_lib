#include "proven/scan.h"

static bool is_whitespace(proven_u8 c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f';
}

static bool is_digit(proven_u8 c) {
    return c >= '0' && c <= '9';
}

void proven_scan_skip_whitespace(proven_scan_t *scan) {
    while (scan->cursor < scan->view.size && is_whitespace(scan->view.ptr[scan->cursor])) {
        scan->cursor++;
    }
}

proven_result_u64_t proven_scan_u64(proven_scan_t *scan) {
    proven_scan_skip_whitespace(scan);
    
    if (scan->cursor >= scan->view.size || !is_digit(scan->view.ptr[scan->cursor])) {
        return (proven_result_u64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    proven_u64 val = 0;
    while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
        // Simple overflow check
        proven_u64 next = val * 10 + (scan->view.ptr[scan->cursor] - '0');
        if (next < val) return (proven_result_u64_t){ .err = PROVEN_ERR_INVALID_ARG }; // Overflow
        val = next;
        scan->cursor++;
    }

    return (proven_result_u64_t){ .val = val, .err = PROVEN_OK };
}

proven_result_i64_t proven_scan_i64(proven_scan_t *scan) {
    proven_scan_skip_whitespace(scan);
    
    if (scan->cursor >= scan->view.size) {
        return (proven_result_i64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    bool negative = false;
    if (scan->view.ptr[scan->cursor] == '-') {
        negative = true;
        scan->cursor++;
    } else if (scan->view.ptr[scan->cursor] == '+') {
        scan->cursor++;
    }

    proven_result_u64_t res = proven_scan_u64(scan);
    if (res.err != PROVEN_OK) return (proven_result_i64_t){ .err = res.err };

    proven_i64 final_val = (proven_i64)res.val;
    if (negative) final_val = -final_val;

    return (proven_result_i64_t){ .val = final_val, .err = PROVEN_OK };
}

proven_result_f64_t proven_scan_f64(proven_scan_t *scan) {
    proven_scan_skip_whitespace(scan);

    if (scan->cursor >= scan->view.size) {
        return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
    }

    bool negative = false;
    if (scan->view.ptr[scan->cursor] == '-') {
        negative = true;
        scan->cursor++;
    } else if (scan->view.ptr[scan->cursor] == '+') {
        scan->cursor++;
    }

    double val = 0.0;
    bool found_digit = false;

    // Integer part
    while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
        val = val * 10.0 + (scan->view.ptr[scan->cursor] - '0');
        scan->cursor++;
        found_digit = true;
    }

    // Fractional part
    if (scan->cursor < scan->view.size && scan->view.ptr[scan->cursor] == '.') {
        scan->cursor++;
        double weight = 0.1;
        while (scan->cursor < scan->view.size && is_digit(scan->view.ptr[scan->cursor])) {
            val += (scan->view.ptr[scan->cursor] - '0') * weight;
            weight /= 10.0;
            scan->cursor++;
            found_digit = true;
        }
    }

    // Exponent part (e.g., 1.23e-10)
    if (scan->cursor < scan->view.size && (scan->view.ptr[scan->cursor] == 'e' || scan->view.ptr[scan->cursor] == 'E')) {
        scan->cursor++;
        proven_result_i64_t exp_res = proven_scan_i64(scan);
        if (exp_res.err == PROVEN_OK) {
            double factor = 1.0;
            proven_i64 e = exp_res.val;
            if (e > 0) {
                while (e--) factor *= 10.0;
            } else if (e < 0) {
                while (e++) factor /= 10.0;
            }
            val *= factor;
        }
    }

    if (!found_digit) return (proven_result_f64_t){ .err = PROVEN_ERR_INVALID_ARG };
    if (negative) val = -val;

    return (proven_result_f64_t){ .val = val, .err = PROVEN_OK };
}

proven_result_u8str_view_t proven_scan_str(proven_scan_t *scan) {
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
    if (target.size == 0) return PROVEN_OK;
    
    for (proven_size_t i = scan->cursor; i + target.size <= scan->view.size; i++) {
        bool match = true;
        for (proven_size_t j = 0; j < target.size; j++) {
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
    while (scan->cursor < scan->view.size) {
        proven_u8 c = scan->view.ptr[scan->cursor];
        
        if (is_digit(c)) {
            break;
        }
        
        // Check for minus/plus sign directly followed by a digit
        if ((c == '-' || c == '+') && scan->cursor + 1 < scan->view.size) {
            if (is_digit(scan->view.ptr[scan->cursor + 1])) {
                break;
            }
        }
        
        scan->cursor++;
    }
}

proven_err_t proven_scan_fmt_impl(proven_scan_t *scan, const char *fmt, const proven_scan_arg_t *args, size_t args_count) {
    size_t arg_idx = 1; // start from 1 since index 0 is proven_scan_arg_none()
    const char *p = fmt;

    while (*p != '\0') {
        if (*p == '{') {
            if (*(p + 1) == '}') {
                p += 2;
                
                if (arg_idx >= args_count) return PROVEN_ERR_INVALID_ARG;
                
                const proven_scan_arg_t *arg = &args[arg_idx++];
                
                switch (arg->type) {
                    case PROVEN_SCAN_ARG_I32: {
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.i32) *arg->ptr.i32 = (proven_i32)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_U32: {
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.u32) *arg->ptr.u32 = (proven_u32)res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_I64: {
                        proven_result_i64_t res = proven_scan_i64(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.i64) *arg->ptr.i64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_U64: {
                        proven_result_u64_t res = proven_scan_u64(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.u64) *arg->ptr.u64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_F64: {
                        proven_result_f64_t res = proven_scan_f64(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.f64) *arg->ptr.f64 = res.val;
                        break;
                    }
                    case PROVEN_SCAN_ARG_STR_VIEW: {
                        proven_result_u8str_view_t res = proven_scan_str(scan);
                        if (res.err != PROVEN_OK) return res.err;
                        if (arg->ptr.str_view) *arg->ptr.str_view = res.val;
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
            if (is_whitespace(*p)) {
                proven_scan_skip_whitespace(scan);
                while (*p != '\0' && is_whitespace(*p)) p++;
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
