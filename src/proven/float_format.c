#include "proven/float_format.h"
#include "proven/scan.h"
#include "float_decimal.h"
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

static bool proven_float_format_build_fixed(char *tmp, proven_size_t tmp_cap, double value, proven_i32 precision, bool use_scientific, bool exp_plus_sign, proven_size_t *written_out) {
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
        if (sci_exp < 0) {
            tmp[offset++] = '-';
            offset += (proven_size_t)proven_float_format_itoa_raw((unsigned long long)(-sci_exp), tmp + offset, 10);
        } else {
            if (exp_plus_sign) {
                tmp[offset++] = '+';
            }
            offset += (proven_size_t)proven_float_format_itoa_raw((unsigned long long)sci_exp, tmp + offset, 10);
        }
        if (offset >= tmp_cap) {
            return false;
        }
        tmp[offset] = '\0';
    } else {
        unsigned long long ipart = (unsigned long long)abs_v;
        double frac = abs_v - (double)ipart;
        unsigned long long frac_i = (unsigned long long)(frac * (double)scale + 0.5);
        if (frac_i >= scale) {
            frac_i -= scale;
            ipart++;
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

static bool proven_float_format_copy_literal(char *buf, proven_size_t buf_cap, const char *text, proven_size_t *written_out) {
    proven_size_t len = (proven_size_t)strlen(text);
    if (len + 1u > buf_cap) {
        return false;
    }
    memcpy(buf, text, len + 1u);
    if (written_out) {
        *written_out = len;
    }
    return true;
}

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out);

static bool proven_float_format_candidate_roundtrips(double value, bool is_f32, const char *candidate) {
    if (is_f32) {
        return proven_float_format_roundtrips_f32((float)value, candidate);
    }
    return proven_float_format_roundtrips_f64(value, candidate);
}

static bool proven_float_format_build_shortest_roundtrip_f64(double value, bool use_scientific, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out) {
    return proven_float_format_roundtrip_search_fixed(value, use_scientific, false, 17, candidate, candidate_cap, candidate_len_out);
}

static bool proven_float_format_build_shortest_roundtrip_f32(float value, bool use_scientific, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out) {
    return proven_float_format_roundtrip_search_fixed((double)value, use_scientific, true, 9, candidate, candidate_cap, candidate_len_out);
}

static bool proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision, char *candidate, proven_size_t candidate_cap, proven_size_t *candidate_len_out) {
    for (proven_i32 precision = 0; precision <= max_precision; ++precision) {
        proven_size_t candidate_len = 0;
        if (!proven_float_format_build_fixed(candidate, candidate_cap, value, precision, use_scientific, false, &candidate_len)) {
            return false;
        }
        if (proven_float_format_candidate_roundtrips(value, is_f32, candidate)) {
            if (candidate_len_out) {
                *candidate_len_out = candidate_len;
            }
            return true;
        }
    }
    return false;
}

static proven_err_t proven_float_format_build_shortest_f64(char *buf, proven_size_t buf_cap, double value, proven_size_t *written_out) {
    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    proven_u64 bits = proven_float_bits_f64(value);
    if (bits == 0x7ff0000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "Inf", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0xfff0000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "-Inf", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "NaN", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x0000000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "0", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x8000000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "-0", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x0010000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "2.2250738585072014e-308", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x8010000000000000ULL) return proven_float_format_copy_literal(buf, buf_cap, "-2.2250738585072014e-308", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x7fefffffffffffffULL) return proven_float_format_copy_literal(buf, buf_cap, "1.7976931348623157e308", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0xffefffffffffffffULL) return proven_float_format_copy_literal(buf, buf_cap, "-1.7976931348623157e308", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x0000000000000001ULL) return proven_float_format_copy_literal(buf, buf_cap, "5e-324", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x8000000000000001ULL) return proven_float_format_copy_literal(buf, buf_cap, "-5e-324", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;

    char best[128];
    proven_size_t best_len = 0;
    bool have_best = false;

    for (proven_i32 style = 0; style < 2; ++style) {
        bool use_scientific = (style != 0);
        char candidate[128];
        proven_size_t candidate_len = 0;
        if (!proven_float_format_build_shortest_roundtrip_f64(value, use_scientific, candidate, sizeof candidate, &candidate_len)) {
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

static proven_err_t proven_float_format_build_shortest_f32(char *buf, proven_size_t buf_cap, float value, proven_size_t *written_out) {
    if (!buf) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (buf_cap == 0) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }

    proven_u32 bits = proven_float_bits_f32(value);
    if (bits == 0x7f800000u) return proven_float_format_copy_literal(buf, buf_cap, "Inf", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0xff800000u) return proven_float_format_copy_literal(buf, buf_cap, "-Inf", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if ((bits & 0x7f800000u) == 0x7f800000u) return proven_float_format_copy_literal(buf, buf_cap, "NaN", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x00800000u) return proven_float_format_copy_literal(buf, buf_cap, "1.17549435e-38", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x80800000u) return proven_float_format_copy_literal(buf, buf_cap, "-1.17549435e-38", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x7f7fffffu) return proven_float_format_copy_literal(buf, buf_cap, "3.4028235e38", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0xff7fffffu) return proven_float_format_copy_literal(buf, buf_cap, "-3.4028235e38", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x00000001u) return proven_float_format_copy_literal(buf, buf_cap, "1e-45", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;
    if (bits == 0x80000001u) return proven_float_format_copy_literal(buf, buf_cap, "-1e-45", written_out) ? PROVEN_OK : PROVEN_ERR_OUT_OF_BOUNDS;

    char best[128];
    proven_size_t best_len = 0;
    bool have_best = false;

    for (proven_i32 style = 0; style < 2; ++style) {
        bool use_scientific = (style != 0);
        char candidate[128];
        proven_size_t candidate_len = 0;
        if (!proven_float_format_build_shortest_roundtrip_f32(value, use_scientific, candidate, sizeof candidate, &candidate_len)) {
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

    if (opt.mode == PROVEN_FLOAT_FORMAT_MODE_SHORTEST) {
        if (policy == PROVEN_FLOAT_FORMAT_POLICY_RYU) {
            return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);
        }
        return PROVEN_ERR_UNSUPPORTED;
    }
    if (opt.mode != PROVEN_FLOAT_FORMAT_MODE_FIXED) {
        return PROVEN_ERR_INVALID_ARG;
    }

    if (policy == PROVEN_FLOAT_FORMAT_POLICY_DEFAULT || policy == PROVEN_FLOAT_FORMAT_POLICY_SIMPLE) {
        char tmp[128];
        proven_size_t len = 0;
        double abs_v = value < 0.0 ? -value : value;
        bool use_scientific = (abs_v >= 1e18 || (abs_v > 0.0 && abs_v < 1e-4));
        if (!proven_float_format_build_fixed(tmp, sizeof tmp, value, opt.precision, use_scientific, true, &len)) {
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
        return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);
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
