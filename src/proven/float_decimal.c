#include "float_decimal.h"

static const double proven_float_pow10_exact[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

double proven_float_scale_pow10(double value, proven_i64 exp10) {
    if (value == 0.0 || exp10 == 0) {
        return value;
    }

    if (exp10 > 0) {
        while (exp10 > 22) {
            value *= proven_float_pow10_exact[22];
            exp10 -= 22;
        }
        value *= proven_float_pow10_exact[(proven_size_t)exp10];
    } else {
        while (exp10 < -22) {
            value /= proven_float_pow10_exact[22];
            exp10 += 22;
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

    if (guard == 0) {
        return false;
    }

    return true;
}
