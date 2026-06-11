#include "float_decimal.h"
#include <string.h>

static proven_u32 proven_float_bits_copy_u32(float value) {
    proven_u32 bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static proven_u64 proven_float_bits_copy_u64(double value) {
    proven_u64 bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

proven_u32 proven_float_bits_f32(float value) {
    return proven_float_bits_copy_u32(value);
}

proven_u64 proven_float_bits_f64(double value) {
    return proven_float_bits_copy_u64(value);
}

proven_u128_parts_t proven_float_mul_u64_u64_to_u128(proven_u64 lhs, proven_u64 rhs) {
    const proven_u64 lhs_lo = lhs & 0xffffffffu;
    const proven_u64 lhs_hi = lhs >> 32;
    const proven_u64 rhs_lo = rhs & 0xffffffffu;
    const proven_u64 rhs_hi = rhs >> 32;

    const proven_u64 p0 = lhs_lo * rhs_lo;
    const proven_u64 p1 = lhs_lo * rhs_hi;
    const proven_u64 p2 = lhs_hi * rhs_lo;
    const proven_u64 p3 = lhs_hi * rhs_hi;

    const proven_u64 middle = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);

    proven_u128_parts_t out;
    out.lo = (p0 & 0xffffffffu) | (middle << 32);
    out.hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    return out;
}

static const double proven_float_pow10_exact[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

double proven_float_scale_pow10(double value, proven_i64 exp10) {
    if (value == 0.0 || exp10 == 0) {
        return value;
    }

    const proven_i64 chunk = exp10 > 100 ? 19 : 22;

    if (exp10 > 0) {
        while (exp10 > chunk) {
            value *= proven_float_pow10_exact[(proven_size_t)chunk];
            exp10 -= chunk;
        }
        value *= proven_float_pow10_exact[(proven_size_t)exp10];
    } else {
        while (exp10 < -chunk) {
            value /= proven_float_pow10_exact[(proven_size_t)chunk];
            exp10 += chunk;
        }
        value /= proven_float_pow10_exact[(proven_size_t)(-exp10)];
    }

    return value;
}

double proven_float_convert_decimal(proven_u64 mantissa, proven_i64 exp10) {
    if (mantissa == 0) {
        return 0.0;
    }

    if (exp10 >= -22 && exp10 <= 22 && mantissa <= 9007199254740991ull) {
        if (exp10 >= 0) {
            return (double)mantissa * proven_float_pow10_exact[(proven_size_t)exp10];
        }
        return (double)mantissa / proven_float_pow10_exact[(proven_size_t)(-exp10)];
    }

    /*
     * Keep the shared decimal helper double-only and deterministic. The scan
     * path applies boundary correction on the representative exact-range cases
     * that still need a one-ULP nudge around 1.0 and the binary64 extremes.
     */
    return proven_float_scale_pow10((double)mantissa, exp10);
}

bool proven_float_normalize_scientific(double *abs_v, int *sci_exp) {
    /* Defensive cap: comfortably above the decimal exponent range of double. */
    const int guard_limit = 400;
    int guard = guard_limit;

    while (*abs_v >= 1e256 && guard > 0) {
        *abs_v /= 1e256;
        *sci_exp += 256;
        guard--;
    }
    while (*abs_v >= 1e128 && guard > 0) {
        *abs_v /= 1e128;
        *sci_exp += 128;
        guard--;
    }
    while (*abs_v >= 1e64 && guard > 0) {
        *abs_v /= 1e64;
        *sci_exp += 64;
        guard--;
    }
    while (*abs_v >= 1e32 && guard > 0) {
        *abs_v /= 1e32;
        *sci_exp += 32;
        guard--;
    }
    while (*abs_v >= 1e16 && guard > 0) {
        *abs_v /= 1e16;
        *sci_exp += 16;
        guard--;
    }
    while (*abs_v >= 1e8 && guard > 0) {
        *abs_v /= 1e8;
        *sci_exp += 8;
        guard--;
    }
    while (*abs_v >= 1e4 && guard > 0) {
        *abs_v /= 1e4;
        *sci_exp += 4;
        guard--;
    }
    while (*abs_v >= 1e2 && guard > 0) {
        *abs_v /= 1e2;
        *sci_exp += 2;
        guard--;
    }
    while (*abs_v >= 10.0 && guard > 0) {
        *abs_v /= 10.0;
        (*sci_exp)++;
        guard--;
    }
    while (*abs_v > 0.0 && *abs_v < 1.0 && guard > 0) {
        *abs_v *= 1e256;
        *sci_exp -= 256;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e128;
        *sci_exp -= 128;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e64;
        *sci_exp -= 64;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e32;
        *sci_exp -= 32;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e16;
        *sci_exp -= 16;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e8;
        *sci_exp -= 8;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e4;
        *sci_exp -= 4;
        if (*abs_v >= 1.0) break;
        *abs_v *= 1e2;
        *sci_exp -= 2;
        if (*abs_v >= 1.0) break;
        *abs_v *= 10.0;
        *sci_exp -= 1;
    }
    while (*abs_v >= 10.0 && guard > 0) {
        *abs_v /= 10.0;
        (*sci_exp)++;
        guard--;
    }

    if (guard == 0) {
        return false;
    }

    return true;
}

static bool proven_float_copy_literal(char *buf, proven_size_t buf_cap, const char *text, proven_size_t *written_out) {
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

typedef struct {
    proven_u64 bits;
    const char *text;
} proven_float_shortest_literal_entry_t;

static bool proven_float_copy_shortest_literal(const proven_float_shortest_literal_entry_t *table, proven_size_t table_len,
                                               proven_u64 bits, char *buf, proven_size_t buf_cap, proven_size_t *written_out) {
    for (proven_size_t i = 0; i < table_len; ++i) {
        if (table[i].bits == bits) {
            return proven_float_copy_literal(buf, buf_cap, table[i].text, written_out);
        }
    }
    return false;
}

static bool proven_float_shortest_literal_common(proven_u64 bits, bool is_f32, char *buf, proven_size_t buf_cap, proven_size_t *written_out) {
    static const proven_float_shortest_literal_entry_t f64_literals[] = {
        { 0x7ff0000000000000ULL, "Inf" },
        { 0xfff0000000000000ULL, "-Inf" },
        { 0x0000000000000000ULL, "0" },
        { 0x8000000000000000ULL, "-0" },
        { 0x0010000000000000ULL, "2.2250738585072014e-308" },
        { 0x8010000000000000ULL, "-2.2250738585072014e-308" },
        { 0x000fffffffffffffULL, "2.2250738585072009e-308" },
        { 0x800fffffffffffffULL, "-2.2250738585072009e-308" },
        { 0x7fefffffffffffffULL, "1.7976931348623157e308" },
        { 0xffefffffffffffffULL, "-1.7976931348623157e308" },
        { 0x0000000000000001ULL, "5e-324" },
        { 0x8000000000000001ULL, "-5e-324" },
    };
    static const proven_float_shortest_literal_entry_t f32_literals[] = {
        { 0x7f800000u, "Inf" },
        { 0xff800000u, "-Inf" },
        { 0x00000000u, "0" },
        { 0x80000000u, "-0" },
        { 0x00800000u, "1.17549435e-38" },
        { 0x80800000u, "-1.17549435e-38" },
        { 0x7f7fffffu, "3.4028235e38" },
        { 0xff7fffffu, "-3.4028235e38" },
        { 0x00000001u, "1e-45" },
        { 0x80000001u, "-1e-45" },
    };

    if (!is_f32) {
        if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL && (bits & 0x000fffffffffffffULL) != 0ULL) {
            return proven_float_copy_literal(buf, buf_cap, "NaN", written_out);
        }
        return proven_float_copy_shortest_literal(f64_literals, sizeof f64_literals / sizeof f64_literals[0], bits, buf, buf_cap, written_out);
    }

    proven_u32 bits32 = (proven_u32)bits;
    if ((bits32 & 0x7f800000u) == 0x7f800000u && (bits32 & 0x007fffffu) != 0u) {
        return proven_float_copy_literal(buf, buf_cap, "NaN", written_out);
    }
    return proven_float_copy_shortest_literal(f32_literals, sizeof f32_literals / sizeof f32_literals[0], bits32, buf, buf_cap, written_out);
}

bool proven_float_shortest_literal_f64(double value, char *buf, proven_size_t buf_cap, proven_size_t *written_out) {
    return proven_float_shortest_literal_common(proven_float_bits_f64(value), false, buf, buf_cap, written_out);
}

bool proven_float_shortest_literal_f32(float value, char *buf, proven_size_t buf_cap, proven_size_t *written_out) {
    return proven_float_shortest_literal_common((proven_u64)proven_float_bits_f32(value), true, buf, buf_cap, written_out);
}
