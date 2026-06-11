#include "proven/float_format.h"
#include "proven/scan.h"
#include "float_decimal.h"
#include <limits.h>
#include <string.h>

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

static bool proven_float_format_build_fixed(char *tmp, proven_size_t tmp_cap, double value, proven_i32 precision, bool use_scientific, bool exp_plus_sign, proven_size_t *written_out, bool *carried_out) {
    if (!tmp || tmp_cap == 0 || precision < 0 || precision > 18) {
        return false;
    }

    if (carried_out) {
        *carried_out = false;
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
    double abs_v = sign ? -value : value;
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

    if (use_scientific) {
        int sci_exp = 0;
        if (!proven_float_normalize_scientific(&abs_v, &sci_exp)) {
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

        unsigned long long digit = (unsigned long long)abs_v;
        double frac = abs_v - (double)digit;
        unsigned long long frac_i = (unsigned long long)(frac * (double)scale + 0.5);
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
    } else {
        unsigned long long ipart = (unsigned long long)abs_v;
        double frac = abs_v - (double)ipart;
        unsigned long long frac_i = (unsigned long long)(frac * (double)scale + 0.5);
        bool carried = false;
        if (frac_i >= scale) {
            frac_i -= scale;
            ipart++;
            carried = true;
        }
        if (carried_out) {
            *carried_out = carried;
        }

        offset += (proven_size_t)proven_float_format_itoa_raw(ipart, tmp + offset, 10);
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
        if (offset >= tmp_cap) {
            return false;
        }
        tmp[offset] = '\0';
    }

    if (written_out) {
        *written_out = offset;
    }
    return true;
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

static proven_err_t proven_float_format_build_shortest_common(char *buf, proven_size_t buf_cap, double value, bool is_f32, proven_i32 max_precision, proven_size_t *written_out) {
    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    if (is_f32) {
        if (proven_float_shortest_literal_f32((float)value, buf, buf_cap, written_out)) {
            return PROVEN_OK;
        }
    } else {
        if (proven_float_shortest_literal_f64(value, buf, buf_cap, written_out)) {
            return PROVEN_OK;
        }
    }

    char best[128];
    proven_size_t best_len = 0;
    bool have_best = false;

    for (proven_i32 style = 0; style < 2; ++style) {
        bool use_scientific = (style != 0);
        char candidate[128];
        proven_size_t candidate_len = 0;
        if (!proven_float_format_roundtrip_search_fixed(value, use_scientific, is_f32, max_precision, candidate, sizeof candidate, &candidate_len)) {
            continue;
        }
        if (!have_best || candidate_len < best_len) {
            memcpy(best, candidate, candidate_len + 1u);
            best_len = candidate_len;
            have_best = true;
        }
    }

    if (!have_best) {
        return PROVEN_ERR_UNSUPPORTED;
    }
    if (best_len + 1u > buf_cap) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    memcpy(buf, best, best_len + 1u);
    if (written_out) {
        *written_out = best_len;
    }
    return PROVEN_OK;
}

static proven_err_t proven_float_format_build_shortest_f64(char *buf, proven_size_t buf_cap, double value, proven_size_t *written_out) {
    return proven_float_format_build_shortest_common(buf, buf_cap, value, false, 17, written_out);
}

static proven_err_t proven_float_format_build_shortest_f32(char *buf, proven_size_t buf_cap, float value, proven_size_t *written_out) {
    return proven_float_format_build_shortest_common(buf, buf_cap, (double)value, true, 9, written_out);
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

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out) {
    bool found = false;
    char best[128];
    proven_size_t best_len = 0;
    for (proven_i32 precision = max_precision; precision >= 0; --precision) {
        proven_size_t candidate_len = 0;
        if (!proven_float_format_build_fixed(candidate, candidate_cap, value, precision, use_scientific, false, &candidate_len, NULL)) {
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
    if (opt.precision < 0 || opt.precision > 18) {
        return PROVEN_ERR_INVALID_ARG;
    }

    if (policy == PROVEN_FLOAT_FORMAT_POLICY_DEFAULT || policy == PROVEN_FLOAT_FORMAT_POLICY_SIMPLE) {
        char tmp[128];
        proven_size_t len = 0;
        double abs_v = value < 0.0 ? -value : value;
        bool use_scientific = (abs_v >= 1e18 || (abs_v > 0.0 && abs_v < 1e-4));
        if (!proven_float_format_build_fixed(tmp, sizeof tmp, value, opt.precision, use_scientific, true, &len, NULL)) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        if (len + 1u > buf_cap) {
            return PROVEN_ERR_OUT_OF_BOUNDS;
        }
        memcpy(buf, tmp, len + 1u);
        if (written_out) {
            *written_out = len;
        }
        return PROVEN_OK;
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
