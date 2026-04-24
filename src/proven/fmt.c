#include "proven/fmt.h"

// Internal helpers for string reversal and integer conversion
static const char digit_pairs[201] = 
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

typedef struct {
    char fill;
    char align; // '<', '>', '^'
    int  width;
    bool hex;
} proven_fmt_spec_t;

static int itoa_raw(unsigned long long val, char *s, int base) {
    if (val == 0) {
        s[0] = '0';
        s[1] = '\0';
        return 1;
    }

    char temp[64];
    int i = 0;
    
    if (base == 10) {
        // Fast base 10 using digit pairs
        while (val >= 100) {
            int rem = val % 100;
            val /= 100;
            temp[i++] = digit_pairs[rem * 2 + 1];
            temp[i++] = digit_pairs[rem * 2];
        }
        if (val < 10) {
            temp[i++] = '0' + (char)val;
        } else {
            temp[i++] = digit_pairs[val * 2 + 1];
            temp[i++] = digit_pairs[val * 2];
        }
    } else if (base == 16) {
        static const char hex_chars[] = "0123456789abcdef";
        while (val != 0) {
            temp[i++] = hex_chars[val & 0xF];
            val >>= 4;
        }
    } else {
        while (val != 0) {
            int r = val % base;
            temp[i++] = (r > 9) ? (r - 10) + 'a' : r + '0';
            val /= base;
        }
    }
    
    // Copy backwards
    for (int j = 0; j < i; j++) {
        s[j] = temp[i - 1 - j];
    }
    s[i] = '\0';
    return i;
}

static proven_err_t append_padding(proven_allocator_t alloc, proven_u8str_t *str, char fill, int count) {
    for (int i = 0; i < count; i++) {
        proven_err_t err = proven_u8str_append_byte(alloc, str, (proven_u8)fill);
        if (!PROVEN_IS_OK(err)) return err;
    }
    return PROVEN_OK;
}

static proven_err_t render_with_spec(proven_allocator_t alloc, proven_u8str_t *str, const char *val, int val_len, proven_fmt_spec_t spec) {
    int total_padding = spec.width - val_len;
    if (total_padding <= 0) {
        return proven_u8str_append_view(alloc, str, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
    }

    if (spec.align == '>') {
        proven_err_t err = append_padding(alloc, str, spec.fill, total_padding);
        if (!PROVEN_IS_OK(err)) return err;
        return proven_u8str_append_view(alloc, str, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
    } else if (spec.align == '<') {
        proven_err_t err = proven_u8str_append_view(alloc, str, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
        if (!PROVEN_IS_OK(err)) return err;
        return append_padding(alloc, str, spec.fill, total_padding);
    } else { // '^'
        int left = total_padding / 2;
        int right = total_padding - left;
        proven_err_t err = append_padding(alloc, str, spec.fill, left);
        if (!PROVEN_IS_OK(err)) return err;
        err = proven_u8str_append_view(alloc, str, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
        if (!PROVEN_IS_OK(err)) return err;
        return append_padding(alloc, str, spec.fill, right);
    }
}

static proven_err_t render_arg(proven_allocator_t alloc, proven_u8str_t *str, const proven_arg_t *arg, proven_fmt_spec_t spec) {
    char buf[128];
    int len = 0;

    switch (arg->type) {
        case PROVEN_ARG_I32: {
            if (spec.hex) {
                len = itoa_raw((unsigned int)arg->value.i32, buf, 16);
            } else {
                long long v = arg->value.i32;
                int offset = 0;
                if (v < 0) { buf[offset++] = '-'; v = -v; }
                len = itoa_raw((unsigned long long)v, buf + offset, 10) + offset;
            }
            return render_with_spec(alloc, str, buf, len, spec);
        }
        case PROVEN_ARG_U32:
            len = itoa_raw(arg->value.u32, buf, spec.hex ? 16 : 10);
            return render_with_spec(alloc, str, buf, len, spec);
        case PROVEN_ARG_I64: {
            if (spec.hex) {
                len = itoa_raw((unsigned long long)arg->value.i64, buf, 16);
            } else {
                long long v = arg->value.i64;
                int offset = 0;
                if (v < 0) { buf[offset++] = '-'; v = -v; }
                len = itoa_raw((unsigned long long)v, buf + offset, 10) + offset;
            }
            return render_with_spec(alloc, str, buf, len, spec);
        }
        case PROVEN_ARG_U64:
            len = itoa_raw(arg->value.u64, buf, spec.hex ? 16 : 10);
            return render_with_spec(alloc, str, buf, len, spec);
        case PROVEN_ARG_F64: {
            double v = arg->value.f64;
            union { double f; unsigned long long u; } decode;
            decode.f = v;
            unsigned long long bits = decode.u;
            bool sign = (bits >> 63) != 0;
            int exp = (int)((bits >> 52) & 0x7FF);
            unsigned long long mantissa = bits & 0xFFFFFFFFFFFFFull;
            
            // IEEE 754 Special Values handling
            if (exp == 0x7FF) {
                if (mantissa != 0) {
                    return render_with_spec(alloc, str, "NaN", 3, spec);
                } else {
                    if (sign) return render_with_spec(alloc, str, "-Inf", 4, spec);
                    else     return render_with_spec(alloc, str, "Inf", 3, spec);
                }
            }

            int offset = 0;
            if (sign) { buf[offset++] = '-'; v = -v; }

            int precision = 6; // default precision
            // Use scientific notation for very large or very small numbers
            bool use_scientific = (v >= 1e18 || (v > 0.0 && v < 1e-4));
            
            if (use_scientific) {
                int sci_exp = 0;
                if (v >= 10.0) {
                    while (v >= 10.0) { v /= 10.0; sci_exp++; }
                } else if (v > 0.0 && v < 1.0) {
                    while (v < 1.0) { v *= 10.0; sci_exp--; }
                }
                
                double round_factor = 0.5;
                for (int i = 0; i < precision; ++i) round_factor /= 10.0;
                v += round_factor;
                
                // Account for rounding pushing it over 10
                if (v >= 10.0) {
                    v /= 10.0;
                    sci_exp++;
                }

                unsigned long long d = (unsigned long long)v;
                double frac = v - (double)d;
                buf[offset++] = (char)('0' + d);
                buf[offset++] = '.';
                for (int i = 0; i < precision; i++) {
                    frac *= 10.0;
                    int f = (int)frac;
                    if (f > 9) f = 9;
                    buf[offset++] = (char)('0' + f);
                    frac -= f;
                }
                buf[offset++] = 'e';
                if (sci_exp >= 0) {
                    buf[offset++] = '+';
                    offset += itoa_raw((unsigned long long)sci_exp, buf + offset, 10);
                } else {
                    buf[offset++] = '-';
                    offset += itoa_raw((unsigned long long)(-sci_exp), buf + offset, 10);
                }
                buf[offset] = '\0';
                return render_with_spec(alloc, str, buf, offset, spec);
            }
            
            // Standard finite formatting
            double round_factor = 0.5;
            for (int i = 0; i < precision; ++i) round_factor /= 10.0;
            v += round_factor;

            unsigned long long ipart = (unsigned long long)v;
            double fpart = v - (double)ipart;
            
            int written = itoa_raw(ipart, buf + offset, 10);
            offset += written;
            
            buf[offset++] = '.';
            
            for (int i = 0; i < precision; i++) {
                fpart *= 10.0;
                int digit = (int)fpart;
                if (digit > 9) digit = 9;
                buf[offset++] = (char)(digit + '0');
                fpart -= (double)digit;
            }
            
            buf[offset] = '\0';
            return render_with_spec(alloc, str, buf, offset, spec);
        }
        case PROVEN_ARG_CSTR:
            return render_with_spec(alloc, str, arg->value.cstr, (int)proven_cstr_len(arg->value.cstr), spec);
        case PROVEN_ARG_STR_VIEW:
            return render_with_spec(alloc, str, (const char*)arg->value.str_view.ptr, (int)arg->value.str_view.size, spec);
        case PROVEN_ARG_DATETIME: {
            proven_datetime_t dt = arg->value.datetime;
            // Manual formatting to avoid snprintf (CRT)
            char *curr = buf;
            
            #define APPEND_FIXED(val, digits) { \
                char temp[20]; \
                int n = itoa_raw((unsigned long long)(val), temp, 10); \
                for (int _i = 0; _i < (digits) - n; _i++) *curr++ = '0'; \
                for (int _i = 0; _i < n; _i++) *curr++ = temp[_i]; \
            }
            
            APPEND_FIXED(dt.year, 4); *curr++ = '-';
            APPEND_FIXED(dt.month, 2); *curr++ = '-';
            APPEND_FIXED(dt.day, 2); *curr++ = ' ';
            APPEND_FIXED(dt.hour, 2); *curr++ = ':';
            APPEND_FIXED(dt.min, 2); *curr++ = ':';
            APPEND_FIXED(dt.sec, 2);
            *curr = '\0';
            len = (int)(curr - buf);
            #undef APPEND_FIXED
            
            return render_with_spec(alloc, str, buf, len, spec);
        }
        case PROVEN_ARG_PTR: {
            buf[0] = '0'; buf[1] = 'x';
            len = itoa_raw((unsigned long long)(proven_uintptr_t)arg->value.ptr, buf + 2, 16) + 2;
            return render_with_spec(alloc, str, buf, len, spec);
        }
        default: break;
    }
    return PROVEN_OK;
}

proven_err_t proven_u8str_format_impl(proven_allocator_t alloc, proven_u8str_t *str, const char *fmt, const proven_arg_t *args, size_t args_count) {
    const char *p = fmt;
    int next_arg_idx = 1;

    while (*p) {
        if (*p == '}') {
            p++;
            if (*p == '}') {
                proven_err_t err = proven_u8str_append_byte(alloc, str, (proven_u8)'}');
                if (!PROVEN_IS_OK(err)) return err;
                p++; continue;
            }
            proven_err_t err = proven_u8str_append_byte(alloc, str, (proven_u8)'}');
            if (!PROVEN_IS_OK(err)) return err;
            continue;
        }

        if (*p != '{') {
            proven_err_t err = proven_u8str_append_byte(alloc, str, (proven_u8)*p);
            if (!PROVEN_IS_OK(err)) return err;
            p++; continue;
        }

        p++; // skip {
        if (*p == '{') {
            proven_err_t err = proven_u8str_append_byte(alloc, str, (proven_u8)'{');
            if (!PROVEN_IS_OK(err)) return err;
            p++; continue;
        }

        int arg_idx = -1;
        if (*p >= '0' && *p <= '9') {
            arg_idx = 0;
            while (*p >= '0' && *p <= '9') { arg_idx = arg_idx * 10 + (*p - '0'); p++; }
            arg_idx++;
        } else {
            arg_idx = next_arg_idx++;
        }

        proven_fmt_spec_t spec = { .fill = ' ', .align = '>', .width = 0, .hex = false };
        if (*p == ':') {
            p++;
            // Check for [fill][align]
            if (*p && (p[1] == '<' || p[1] == '>' || p[1] == '^')) {
                spec.fill = *p;
                spec.align = p[1];
                p += 2;
            } else if (*p == '<' || *p == '>' || *p == '^') {
                spec.align = *p;
                p++;
            }

            // Parse width
            while (*p >= '0' && *p <= '9') {
                spec.width = spec.width * 10 + (*p - '0');
                p++;
            }

            if (*p == 'x') { spec.hex = true; p++; }
        }

        while (*p && *p != '}') p++;
        if (*p == '}') p++;

        if (arg_idx > 0 && (size_t)arg_idx < args_count) {
            proven_err_t err = render_arg(alloc, str, &args[arg_idx], spec);
            if (!PROVEN_IS_OK(err)) return err;
        }
    }
    return PROVEN_OK;
}
