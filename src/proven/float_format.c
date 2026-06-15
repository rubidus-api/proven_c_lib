#include "proven/float_format.h"
#include "proven/float_config.h"
#include "proven/scan.h"
#include "float_decimal.h"
#include <limits.h>
#include <string.h>

/* Upper bound on decimal digits a full-capacity exact value can produce. */
#define PROVEN_FLOAT_FMT_DIGITS_MAX ((PROVEN_FLOAT_BIGINT_LIMBS * 20u) + 4u)
/* Highest fixed precision the exact path accepts (capacity still bounds it). */
#define PROVEN_FLOAT_FMT_PRECISION_MAX 1100

static int proven_float_format_itoa_raw(unsigned long long val, char *s, int base_in) {
    static const char digits[] = "0123456789abcdef";
    char tmp[64];
    int base = base_in;
    int n = 0;

    if (base != 10 && base != 16) {
        base = 10;
    }

    if (val == 0) {
        if (s) {
            s[0] = '0';
        }
        return 1;
    }

    while (val > 0 && n < (int)sizeof(tmp)) {
        unsigned long long rem = val % (unsigned long long)base;
        tmp[n++] = digits[rem];
        val /= (unsigned long long)base;
    }

    for (int i = 0; i < n; ++i) {
        s[i] = tmp[n - 1 - i];
    }
    return n;
}

static bool proven_float_normalize_scientific_ld(long double *abs_v, int *sci_exp) {
    const int guard_limit = 400;
    int guard = guard_limit;

    while (*abs_v >= 1e256L && guard > 0) {
        *abs_v /= 1e256L;
        *sci_exp += 256;
        guard--;
    }
    while (*abs_v >= 1e128L && guard > 0) {
        *abs_v /= 1e128L;
        *sci_exp += 128;
        guard--;
    }
    while (*abs_v >= 1e64L && guard > 0) {
        *abs_v /= 1e64L;
        *sci_exp += 64;
        guard--;
    }
    while (*abs_v >= 1e32L && guard > 0) {
        *abs_v /= 1e32L;
        *sci_exp += 32;
        guard--;
    }
    while (*abs_v >= 1e16L && guard > 0) {
        *abs_v /= 1e16L;
        *sci_exp += 16;
        guard--;
    }
    while (*abs_v >= 1e8L && guard > 0) {
        *abs_v /= 1e8L;
        *sci_exp += 8;
        guard--;
    }
    while (*abs_v >= 1e4L && guard > 0) {
        *abs_v /= 1e4L;
        *sci_exp += 4;
        guard--;
    }
    while (*abs_v >= 1e2L && guard > 0) {
        *abs_v /= 1e2L;
        *sci_exp += 2;
        guard--;
    }
    while (*abs_v >= 10.0L && guard > 0) {
        *abs_v /= 10.0L;
        (*sci_exp)++;
        guard--;
    }
    while (*abs_v > 0.0L && *abs_v < 1.0L && guard > 0) {
        *abs_v *= 1e256L;
        *sci_exp -= 256;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e128L;
        *sci_exp -= 128;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e64L;
        *sci_exp -= 64;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e32L;
        *sci_exp -= 32;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e16L;
        *sci_exp -= 16;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e8L;
        *sci_exp -= 8;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e4L;
        *sci_exp -= 4;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 1e2L;
        *sci_exp -= 2;
        if (*abs_v >= 1.0L) break;
        *abs_v *= 10.0L;
        *sci_exp -= 1;
    }
    while (*abs_v >= 10.0L && guard > 0) {
        *abs_v /= 10.0L;
        (*sci_exp)++;
        guard--;
    }

    return guard != 0;
}

static bool proven_float_format_build_scientific_ld(char *tmp, proven_size_t tmp_cap, double value, proven_i32 precision, bool exp_plus_sign, proven_size_t *written_out) {
    if (!tmp || tmp_cap == 0 || precision < 0 || precision > 18) {
        return false;
    }

    proven_u64 bits = proven_float_bits_f64(value);
    bool sign = (bits >> 63) != 0;
    int exp = (int)((bits >> 52) & 0x7FF);
    proven_u64 mantissa = bits & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF) {
        const char *special = mantissa != 0 ? "NaN" : (sign ? "-Inf" : "Inf");
        proven_size_t len = (proven_size_t)strlen(special);
        if (len + 1 > tmp_cap) {
            return false;
        }
        memcpy(tmp, special, len + 1u);
        if (written_out) {
            *written_out = len;
        }
        return true;
    }

    proven_size_t offset = 0;
    if (sign) {
        if (offset + 1u >= tmp_cap) {
            return false;
        }
        tmp[offset++] = '-';
    }

    const proven_u64 precision_scale_max = 1000000000000000000ULL;
    proven_u64 scale = 1;
    for (proven_i32 i = 0; i < precision; ++i) {
        if (scale > precision_scale_max / 10ULL) {
            return false;
        }
        scale *= 10ULL;
    }

    int sci_exp = 0;
    long double abs_v_ld = sign ? -(long double)value : (long double)value;
    if (!proven_float_normalize_scientific_ld(&abs_v_ld, &sci_exp)) {
        const char *special = sign ? "-Inf" : "Inf";
        proven_size_t len = (proven_size_t)strlen(special);
        if (len + 1u > tmp_cap) {
            return false;
        }
        memcpy(tmp, special, len + 1u);
        if (written_out) {
            *written_out = len;
        }
        return true;
    }

    unsigned long long digit = (unsigned long long)abs_v_ld;
    long double frac = abs_v_ld - (long double)digit;
    unsigned long long frac_i = (unsigned long long)(frac * (long double)scale + 0.5L);
    if (frac_i >= scale) {
        frac_i -= scale;
        digit++;
        if (digit >= 10ULL) {
            digit = 1ULL;
            sci_exp++;
        }
    }

    if (offset + 1u >= tmp_cap) {
        return false;
    }
    tmp[offset++] = (char)('0' + (int)digit);
    if (precision > 0) {
        if (offset + 1u >= tmp_cap) {
            return false;
        }
        tmp[offset++] = '.';
        for (proven_i32 i = precision - 1; i >= 0; --i) {
            if (offset + 1u >= tmp_cap) {
                return false;
            }
            tmp[offset + (proven_size_t)i] = (char)('0' + (int)(frac_i % 10ULL));
            frac_i /= 10ULL;
        }
        offset += (proven_size_t)precision;
    }
    if (offset + 2u >= tmp_cap) {
        return false;
    }
    tmp[offset++] = 'e';
    {
        char expbuf[32];
        proven_size_t exp_len = 0;
        unsigned long long exp_abs = sci_exp < 0 ? (unsigned long long)(-(long long)sci_exp) : (unsigned long long)sci_exp;
        exp_len = (proven_size_t)proven_float_format_itoa_raw(exp_abs, expbuf, 10);
        if (exp_len < 2u) {
            expbuf[1] = expbuf[0];
            expbuf[0] = '0';
            exp_len = 2u;
        }
        if (sci_exp < 0) {
            tmp[offset++] = '-';
        } else if (exp_plus_sign) {
            tmp[offset++] = '+';
        }
        if (offset + exp_len >= tmp_cap) {
            return false;
        }
        memcpy(tmp + offset, expbuf, exp_len);
        offset += exp_len;
    }
    if (offset >= tmp_cap) {
        return false;
    }
    tmp[offset] = '\0';
    if (written_out) {
        *written_out = offset;
    }
    return true;
}

static bool proven_float_format_roundtrips_f64(double original, const char *text) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    return parsed.err == PROVEN_OK && proven_float_bits_f64(parsed.val) == proven_float_bits_f64(original);
}

static bool proven_float_format_roundtrips_f32(float original, const char *text) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    return parsed.err == PROVEN_OK && proven_float_bits_f32((float)parsed.val) == proven_float_bits_f32(original);
}

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out);

/*
 * Formats the shortest significant digits as the shorter of fixed-point and
 * scientific notation (fixed wins ties), matching the documented shortest
 * spelling: scientific has no '+' on a positive exponent and at least two
 * exponent digits. The fixed-form length is computed analytically so an extreme
 * exponent never builds a huge fixed string before discarding it.
 */
static proven_err_t proven_float_format_shortest_from_digits(bool negative, const char *digits, int n,
                                                             proven_i64 dexp, char *buf, proven_size_t buf_cap,
                                                             proven_size_t *written_out) {
    char sci[40];
    proven_size_t sl = 0;
    proven_i64 last = dexp - (proven_i64)(n - 1);
    proven_size_t fixed_len;
    proven_size_t pos = 0;
    int i;

    /* scientific notation (always short) */
    sci[sl++] = digits[0];
    if (n > 1) {
        sci[sl++] = '.';
        for (i = 1; i < n; ++i) {
            sci[sl++] = digits[i];
        }
    }
    sci[sl++] = 'e';
    {
        proven_i64 ex = dexp;
        unsigned long long u;
        char r[20];
        int rl = 0;
        if (ex < 0) {
            sci[sl++] = '-';
            u = (unsigned long long)(-ex);
        } else {
            u = (unsigned long long)ex;
        }
        if (u == 0u) {
            r[rl++] = '0';
        } else {
            while (u != 0u) {
                r[rl++] = (char)('0' + (int)(u % 10u));
                u /= 10u;
            }
        }
        if (rl < 2) {
            sci[sl++] = '0';
        }
        while (rl > 0) {
            sci[sl++] = r[--rl];
        }
    }
    sci[sl] = '\0';

    /* fixed-form length, analytically */
    if (dexp >= 0) {
        fixed_len = (last >= 0) ? (proven_size_t)((proven_i64)n + last) : (proven_size_t)(n + 1);
    } else {
        fixed_len = (proven_size_t)((proven_i64)n + 1 - dexp); /* "0." + (-dexp-1) zeros + n digits */
    }

    if (negative) {
        if (1u >= buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        buf[pos++] = '-';
    }

    if (fixed_len <= sl) {
        /* build fixed in place (bounded: it is no longer than the scientific form) */
        if (pos + fixed_len + 1u > buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        if (dexp >= 0) {
            if (last >= 0) {
                for (i = 0; i < n; ++i) buf[pos++] = digits[i];
                for (proven_i64 z = 0; z < last; ++z) buf[pos++] = '0';
            } else {
                proven_size_t intd = (proven_size_t)dexp + 1u;
                for (i = 0; i < (int)intd; ++i) buf[pos++] = digits[i];
                buf[pos++] = '.';
                for (i = (int)intd; i < n; ++i) buf[pos++] = digits[i];
            }
        } else {
            buf[pos++] = '0';
            buf[pos++] = '.';
            for (proven_i64 z = 0; z < -dexp - 1; ++z) buf[pos++] = '0';
            for (i = 0; i < n; ++i) buf[pos++] = digits[i];
        }
    } else {
        if (pos + sl + 1u > buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        memcpy(buf + pos, sci, sl);
        pos += sl;
    }
    buf[pos] = '\0';
    if (written_out) {
        *written_out = pos;
    }
    return PROVEN_OK;
}

static proven_err_t proven_float_format_build_shortest_f64(char *buf, proven_size_t buf_cap, double value, proven_size_t *written_out) {
    proven_u64 bits = proven_float_bits_f64(value);
    bool negative = (bits >> 63) != 0;
    proven_u64 exp = (bits >> 52) & 0x7ffull;
    char digits[20];
    proven_i64 dexp = 0;
    int n;

    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0u) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (exp == 0x7ffull) {
        const char *s = (bits & 0x000fffffffffffffull) ? "NaN" : (negative ? "-Inf" : "Inf");
        proven_size_t len = (proven_size_t)strlen(s);
        if (len + 1u > buf_cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        memcpy(buf, s, len + 1u);
        if (written_out) *written_out = len;
        return PROVEN_OK;
    }
    n = proven_float_shortest_digits(value, digits, sizeof digits, &dexp);
    if (n < 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    return proven_float_format_shortest_from_digits(negative, digits, n, dexp, buf, buf_cap, written_out);
}

static proven_err_t proven_float_format_build_shortest_f32(char *buf, proven_size_t buf_cap, float value, proven_size_t *written_out) {
    proven_u32 bits = proven_float_bits_f32(value);
    bool negative = (bits >> 31) != 0;
    proven_u32 exp = (bits >> 23) & 0xffu;
    char digits[16];
    proven_i64 dexp = 0;
    int n;

    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0u) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (exp == 0xffu) {
        const char *s = (bits & 0x007fffffu) ? "NaN" : (negative ? "-Inf" : "Inf");
        proven_size_t len = (proven_size_t)strlen(s);
        if (len + 1u > buf_cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        memcpy(buf, s, len + 1u);
        if (written_out) *written_out = len;
        return PROVEN_OK;
    }
    n = proven_float_shortest_digits_f32(value, digits, sizeof digits, &dexp);
    if (n < 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    return proven_float_format_shortest_from_digits(negative, digits, n, dexp, buf, buf_cap, written_out);
}

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out);

static bool proven_float_format_candidate_roundtrips(double value, bool is_f32, const char *candidate) {
    if (is_f32) {
        return proven_float_format_roundtrips_f32((float)value, candidate);
    }
    return proven_float_format_roundtrips_f64(value, candidate);
}

static bool proven_float_format_adjust_fixed_neighbor(char *buf, proven_size_t buf_cap, proven_i32 precision, int delta) {
    if (!buf || buf_cap == 0 || precision <= 0 || (delta != -1 && delta != 1)) {
        return false;
    }

    char *dot = strchr(buf, '.');
    if (!dot) {
        return false;
    }

    char *first = buf;
    if (*first == '-' || *first == '+') {
        first++;
    }

    char *last = buf + strlen(buf);
    if (last == buf) {
        return false;
    }
    last--;

    if (delta > 0) {
        char *pos = last;
        while (pos >= first) {
            if (*pos == '.') {
                pos--;
                continue;
            }
            if (*pos < '9') {
                *pos = (char)(*pos + 1);
                for (char *q = pos + 1; q <= last; ++q) {
                    if (*q != '.') {
                        *q = '0';
                    }
                }
                return true;
            }
            *pos = '0';
            if (pos == first) {
                break;
            }
            pos--;
        }

        if (strlen(buf) + 1u >= buf_cap) {
            return false;
        }
        memmove(first + 1, first, strlen(first) + 1u);
        *first = '1';
        dot = strchr(buf, '.');
        if (!dot) {
            return false;
        }
        for (char *q = dot + 1; *q != '\0'; ++q) {
            if (*q != '.') {
                *q = '0';
            }
        }
        return true;
    }

    char *pos = last;
    while (pos >= first) {
        if (*pos == '.') {
            pos--;
            continue;
        }
        if (*pos > '0') {
            *pos = (char)(*pos - 1);
            for (char *q = pos + 1; q <= last; ++q) {
                if (*q != '.') {
                    *q = '9';
                }
            }
            dot = strchr(buf, '.');
            if (!dot) {
                return false;
            }
            while ((dot - first) > 1 && *first == '0') {
                memmove(first, first + 1, strlen(first) + 1u);
                dot = strchr(buf, '.');
                if (!dot) {
                    return false;
                }
            }
            return true;
        }
        *pos = '9';
        if (pos == first) {
            break;
        }
        pos--;
    }

    return false;
}

static proven_err_t proven_float_format_fixed_f_exact(char *buf, proven_size_t buf_cap, double value, proven_i32 precision, proven_size_t *written_out);
static proven_err_t proven_float_format_e_exact(char *buf, proven_size_t buf_cap, double value, proven_i32 precision, bool exp_force_sign, int exp_min_digits, proven_size_t *written_out);

/*
 * Correctly-rounded fixed or scientific candidate used by the shortest search.
 * The scientific form uses the shortest convention: no forced '+' on a positive
 * exponent and a minimum of two exponent digits, matching the documented output.
 */
static bool proven_float_format_candidate_exact(double value, proven_i32 precision, bool use_scientific, char *buf, proven_size_t cap, proven_size_t *len_out) {
    proven_err_t err = use_scientific
        ? proven_float_format_e_exact(buf, cap, value, precision, false, 2, len_out)
        : proven_float_format_fixed_f_exact(buf, cap, value, precision, len_out);
    return err == PROVEN_OK;
}

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out) {
    bool found = false;
    char best[128];
    proven_size_t best_len = 0;
    for (proven_i32 precision = max_precision; precision >= 0; --precision) {
        proven_size_t candidate_len = 0;
        if (!proven_float_format_candidate_exact(value, precision, use_scientific, candidate, candidate_cap, &candidate_len)) {
            return false;
        }
        char base[128];
        memcpy(base, candidate, candidate_len + 1u);
        bool roundtrips = proven_float_format_candidate_roundtrips(value, is_f32, base);
        if (!roundtrips && use_scientific && precision > 6) {
            char alt[128];
            proven_size_t alt_len = 0;
            if (proven_float_format_build_scientific_ld(alt, sizeof alt, value, precision, false, &alt_len) &&
                proven_float_format_candidate_roundtrips(value, is_f32, alt)) {
                memcpy(base, alt, alt_len + 1u);
                candidate_len = alt_len;
                roundtrips = true;
            }
        }
        if (roundtrips) {
            if (!found || candidate_len < best_len) {
                memcpy(best, base, candidate_len + 1u);
                best_len = candidate_len;
                found = true;
            }
        }
        if (!use_scientific && precision > 0) {
            char alt[128];
            memcpy(alt, base, candidate_len + 1u);
            if (proven_float_format_adjust_fixed_neighbor(alt, sizeof alt, precision, 1) &&
                proven_float_format_candidate_roundtrips(value, is_f32, alt)) {
                proven_size_t alt_copy_len = (proven_size_t)strlen(alt);
                if (!found || alt_copy_len < best_len) {
                    memcpy(best, alt, alt_copy_len + 1u);
                    best_len = alt_copy_len;
                    found = true;
                }
            }
            memcpy(alt, base, candidate_len + 1u);
            if (proven_float_format_adjust_fixed_neighbor(alt, sizeof alt, precision, -1) &&
                proven_float_format_candidate_roundtrips(value, is_f32, alt)) {
                proven_size_t alt_copy_len = (proven_size_t)strlen(alt);
                if (!found || alt_copy_len < best_len) {
                    memcpy(best, alt, alt_copy_len + 1u);
                    best_len = alt_copy_len;
                    found = true;
                }
            }
        }
    }
    if (found) {
        memcpy(candidate, best, best_len + 1u);
        if (candidate_len_out) {
            *candidate_len_out = best_len;
        }
    }
    return found;
}

/*
 * Exact `%f`-style fixed-precision formatting: `precision` digits after the
 * decimal point, correctly rounded (round-half-to-even), at arbitrary magnitude
 * and precision. Integer-only via the shared exact engine; no long double, no
 * precision/magnitude ceiling beyond the big-integer capacity.
 */
static proven_err_t proven_float_format_fixed_f_exact(char *buf, proven_size_t buf_cap, double value,
                                                      proven_i32 precision, proven_size_t *written_out) {
    proven_u64 bits = proven_float_bits_f64(value);
    bool sign = (bits >> 63) != 0;
    proven_u64 exp = (bits >> 52) & 0x7ffull;
    proven_u64 frac = bits & 0x000fffffffffffffull;
    proven_size_t pos = 0;
    char digits[PROVEN_FLOAT_FMT_DIGITS_MAX];
    int nd;
    proven_size_t L;
    proven_size_t P;
    proven_size_t i;

    if (!buf || buf_cap == 0u || precision < 0) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (exp == 0x7ffull) {
        const char *special = frac != 0u ? "NaN" : (sign ? "-Inf" : "Inf");
        proven_size_t len = (proven_size_t)strlen(special);
        if (len + 1u > buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        memcpy(buf, special, len + 1u);
        if (written_out) {
            *written_out = len;
        }
        return PROVEN_OK;
    }

    nd = proven_float_scaled_round_digits(value, (proven_i64)precision, digits, sizeof digits);
    if (nd < 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    L = (proven_size_t)nd;
    P = (proven_size_t)precision;

#define PROVEN_FMT_PUT(ch) do { if (pos + 1u >= buf_cap) { return PROVEN_ERR_OUT_OF_BOUNDS; } buf[pos++] = (ch); } while (0)
    if (sign) {
        PROVEN_FMT_PUT('-');
    }
    if (L > P) {
        proven_size_t ilen = L - P;
        for (i = 0; i < ilen; ++i) {
            PROVEN_FMT_PUT(digits[i]);
        }
        if (P > 0u) {
            PROVEN_FMT_PUT('.');
            for (i = ilen; i < L; ++i) {
                PROVEN_FMT_PUT(digits[i]);
            }
        }
    } else {
        PROVEN_FMT_PUT('0');
        if (P > 0u) {
            PROVEN_FMT_PUT('.');
            for (i = 0; i < P - L; ++i) {
                PROVEN_FMT_PUT('0');
            }
            for (i = 0; i < L; ++i) {
                PROVEN_FMT_PUT(digits[i]);
            }
        }
    }
#undef PROVEN_FMT_PUT
    if (pos >= buf_cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    buf[pos] = '\0';
    if (written_out) {
        *written_out = pos;
    }
    return PROVEN_OK;
}

/*
 * Exact `%e`-style scientific formatting: `precision` digits after the mantissa
 * point (precision + 1 significant digits), correctly rounded (round-half-to-even),
 * with a signed exponent of at least two digits. Integer-only via the shared exact
 * engine.
 */
static proven_err_t proven_float_format_e_exact(char *buf, proven_size_t buf_cap, double value,
                                                proven_i32 precision, bool exp_force_sign, int exp_min_digits,
                                                proven_size_t *written_out) {
    proven_u64 bits = proven_float_bits_f64(value);
    bool sign = (bits >> 63) != 0;
    proven_u64 exp = (bits >> 52) & 0x7ffull;
    proven_u64 frac = bits & 0x000fffffffffffffull;
    proven_size_t pos = 0;
    char digits[PROVEN_FLOAT_FMT_DIGITS_MAX];
    proven_i64 dexp = 0;
    int sig;
    int nd;
    int i;

    if (!buf || buf_cap == 0u || precision < 0) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (exp == 0x7ffull) {
        const char *special = frac != 0u ? "NaN" : (sign ? "-Inf" : "Inf");
        proven_size_t len = (proven_size_t)strlen(special);
        if (len + 1u > buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        memcpy(buf, special, len + 1u);
        if (written_out) {
            *written_out = len;
        }
        return PROVEN_OK;
    }

    sig = precision + 1;
    nd = proven_float_scaled_round_sig_digits(value, sig, digits, sizeof digits, &dexp);
    if (nd < 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

#define PROVEN_FMT_PUT(ch) do { if (pos + 1u >= buf_cap) { return PROVEN_ERR_OUT_OF_BOUNDS; } buf[pos++] = (ch); } while (0)
    if (sign) {
        PROVEN_FMT_PUT('-');
    }
    PROVEN_FMT_PUT(digits[0]);
    if (precision > 0) {
        PROVEN_FMT_PUT('.');
        for (i = 1; i < sig; ++i) {
            PROVEN_FMT_PUT(digits[i]);
        }
    }
    PROVEN_FMT_PUT('e');
    if (dexp < 0) {
        PROVEN_FMT_PUT('-');
    } else if (exp_force_sign) {
        PROVEN_FMT_PUT('+');
    }
    {
        char eb[24];
        unsigned long long uexp = dexp < 0 ? (unsigned long long)(-dexp) : (unsigned long long)dexp;
        int el = proven_float_format_itoa_raw(uexp, eb, 10);
        int pad = exp_min_digits - el;
        while (pad-- > 0) {
            PROVEN_FMT_PUT('0');
        }
        for (i = 0; i < el; ++i) {
            PROVEN_FMT_PUT(eb[i]);
        }
    }
#undef PROVEN_FMT_PUT
    if (pos >= buf_cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    buf[pos] = '\0';
    if (written_out) {
        *written_out = pos;
    }
    return PROVEN_OK;
}

static proven_err_t proven_float_format_dispatch_f64(char *buf, proven_size_t buf_cap, double value,
                                                     proven_float_format_policy_t policy,
                                                     proven_float_format_options_t opt,
                                                     proven_size_t *written_out) {
    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (policy != PROVEN_FLOAT_FORMAT_POLICY_DEFAULT &&
        policy != PROVEN_FLOAT_FORMAT_POLICY_SIMPLE &&
        policy != PROVEN_FLOAT_FORMAT_POLICY_RYU) {
        return PROVEN_ERR_INVALID_ARG;
    }

    if (opt.mode == PROVEN_FLOAT_FORMAT_MODE_SHORTEST) {
        if (policy == PROVEN_FLOAT_FORMAT_POLICY_RYU) {
            return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);
        }
        return PROVEN_ERR_UNSUPPORTED;
    }
    if (opt.mode != PROVEN_FLOAT_FORMAT_MODE_FIXED) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (opt.precision < 0 || opt.precision > PROVEN_FLOAT_FMT_PRECISION_MAX) {
        return PROVEN_ERR_INVALID_ARG;
    }

    if (policy == PROVEN_FLOAT_FORMAT_POLICY_DEFAULT || policy == PROVEN_FLOAT_FORMAT_POLICY_SIMPLE) {
        double abs_v = value < 0.0 ? -value : value;
        bool use_scientific = (abs_v >= 1e18 || (abs_v > 0.0 && abs_v < 1e-4));
        if (!use_scientific) {
            /* Exact, arbitrary-precision %f path. */
            return proven_float_format_fixed_f_exact(buf, buf_cap, value, opt.precision, written_out);
        }
        /* Exact, arbitrary-precision %e (scientific) path: signed two-digit exponent. */
        return proven_float_format_e_exact(buf, buf_cap, value, opt.precision, true, 2, written_out);
    }

    if (policy == PROVEN_FLOAT_FORMAT_POLICY_RYU) {
        if (opt.mode != PROVEN_FLOAT_FORMAT_MODE_SHORTEST) {
            return PROVEN_ERR_UNSUPPORTED;
        }
        return proven_float_format_roundtrip_search_fixed(value, true, false, 17, buf, buf_cap, written_out);
    }

    return PROVEN_ERR_INVALID_ARG;
}

proven_err_t proven_float_format_f64_policy(char *buf, proven_size_t buf_cap, double value,
                                            proven_float_format_policy_t policy,
                                            proven_float_format_options_t opt,
                                            proven_size_t *written_out) {
    if (opt.mode == PROVEN_FLOAT_FORMAT_MODE_FIXED && opt.precision == 0) {
        opt = proven_float_format_options_fixed_default();
    }
    return proven_float_format_dispatch_f64(buf, buf_cap, value, policy, opt, written_out);
}

proven_err_t proven_float_format_f32_policy(char *buf, proven_size_t buf_cap, float value,
                                            proven_float_format_policy_t policy,
                                            proven_float_format_options_t opt,
                                            proven_size_t *written_out) {
    if (policy == PROVEN_FLOAT_FORMAT_POLICY_RYU) {
        if (opt.mode != PROVEN_FLOAT_FORMAT_MODE_SHORTEST) {
            return PROVEN_ERR_UNSUPPORTED;
        }
        return proven_float_format_build_shortest_f32(buf, buf_cap, value, written_out);
    }
    return proven_float_format_f64_policy(buf, buf_cap, (double)value, policy, opt, written_out);
}
