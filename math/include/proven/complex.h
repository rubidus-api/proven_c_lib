#ifndef PROVEN_COMPLEX_H
#define PROVEN_COMPLEX_H

#include "proven/types.h"

/**
 * @file complex.h
 * @brief C's <complex.h> as a pair of values: arithmetic and the elementary functions of a
 *        complex number, binary64 and binary32, with the same bits on every target
 *        (RFC-0058 D-20; proven_math).
 *
 * A complex number is a struct of two parts, not C's `_Complex` type, so it is the same on
 * every compiler (MSVC has no `_Complex`) and its parts are plain fields. Each function is a
 * fixed formula of correctly rounded operations - float_ops.h's arithmetic and float_math.h's
 * functions - so every target and every build gives the same bits; the results are not
 * correctly rounded as a whole, and each function's error, measured against GNU MPC part by
 * part, is in the manual (chapter 8). The binary32 forms compute in binary64 and round each
 * part once.
 *
 * Branch cuts, zeros, infinities and NaNs are C23 Annex G's (G.6); where Annex G leaves a sign
 * unspecified, the manual says which one comes back. A NaN part of a result is the format's
 * default NaN (0x7FF8000000000000, binary32 0x7FC00000), whatever NaNs went in - except in
 * proven_complex_conj and _proj, which only move signs and parts. No errno, no flags, no
 * state, no allocation, no libm; freestanding.
 */

/** @brief A binary64 complex number: `re + i im`. */
typedef struct {
    double re;
    double im;
} proven_complex_t;

/** @brief A binary32 complex number. */
typedef struct {
    float re;
    float im;
} proven_complex32_t;

/** @brief `re + i im`. */
[[nodiscard]] static inline proven_complex_t proven_complex(double re, double im) {
    return (proven_complex_t){ re, im };
}
/** @brief `re + i im`, binary32. */
[[nodiscard]] static inline proven_complex32_t proven_complex32(float re, float im) {
    return (proven_complex32_t){ re, im };
}

/*
 * To and from C's `_Complex` types, where the compiler has them: not under
 * `__STDC_NO_COMPLEX__`, not in C++, not with Microsoft's own compiler (clang-cl has them). They
 * only move the two parts, bit for bit - a NaN or a signed zero as it is - through C's
 * guarantee that a complex number is laid out as an array of its real and imaginary parts, so
 * they need neither <complex.h> nor libm. The arithmetic stays this header's: `a * b` on
 * `double _Complex` is the compiler's formula, which this library does not fix.
 */
#if !defined(__STDC_NO_COMPLEX__) && !defined(__cplusplus) && \
    !(defined(_MSC_VER) && !defined(__clang__))
#define PROVEN_HAVE_C_COMPLEX 1

/** @brief A `double _Complex` as a proven_complex_t, the parts as they are. */
[[nodiscard]] static inline proven_complex_t proven_complex_from_c(double _Complex z) {
    union { double _Complex c; double p[2]; } u;
    u.c = z;
    return (proven_complex_t){ u.p[0], u.p[1] };
}
/** @brief A proven_complex_t as a `double _Complex`, the parts as they are. */
[[nodiscard]] static inline double _Complex proven_complex_to_c(proven_complex_t z) {
    union { double _Complex c; double p[2]; } u;
    u.p[0] = z.re;
    u.p[1] = z.im;
    return u.c;
}
/** @brief A `float _Complex` as a proven_complex32_t, the parts as they are. */
[[nodiscard]] static inline proven_complex32_t proven_complex32_from_c(float _Complex z) {
    union { float _Complex c; float p[2]; } u;
    u.c = z;
    return (proven_complex32_t){ u.p[0], u.p[1] };
}
/** @brief A proven_complex32_t as a `float _Complex`, the parts as they are. */
[[nodiscard]] static inline float _Complex proven_complex32_to_c(proven_complex32_t z) {
    union { float _Complex c; float p[2]; } u;
    u.p[0] = z.re;
    u.p[1] = z.im;
    return u.c;
}
#endif

/** @brief `a + b`, part by part. */
[[nodiscard]] proven_complex_t proven_complex_add(proven_complex_t a, proven_complex_t b);
/** @brief `a - b`, part by part. */
[[nodiscard]] proven_complex_t proven_complex_sub(proven_complex_t a, proven_complex_t b);
/** @brief `a * b`: each part a sum of two products with one fused correction (within 2 units of
 *         the last place of that part), and Annex G's infinities. */
[[nodiscard]] proven_complex_t proven_complex_mul(proven_complex_t a, proven_complex_t b);
/** @brief `a / b`, both scaled by powers of 2 first so that nothing overflows or underflows on
 *         the way, and Annex G's infinities and zeros. */
[[nodiscard]] proven_complex_t proven_complex_div(proven_complex_t a, proven_complex_t b);
/** @brief The magnitude `|z|`: hypot, correctly rounded. */
[[nodiscard]] double proven_complex_abs(proven_complex_t z);
/** @brief The argument, in [-pi, pi]: atan2(im, re), correctly rounded. */
[[nodiscard]] double proven_complex_arg(proven_complex_t z);
/** @brief The conjugate: the imaginary part's sign flipped (a NaN kept as it is). */
[[nodiscard]] proven_complex_t proven_complex_conj(proven_complex_t z);
/** @brief The projection onto the Riemann sphere: `z`, or `inf + i0` (the zero with the sign of
 *         the imaginary part) when a part is infinite. */
[[nodiscard]] proven_complex_t proven_complex_proj(proven_complex_t z);

/** @brief e to the power `z`. */
[[nodiscard]] proven_complex_t proven_complex_exp(proven_complex_t z);
/** @brief The natural logarithm, its imaginary part in [-pi, pi]; the cut along the negative
 *         real axis. */
[[nodiscard]] proven_complex_t proven_complex_log(proven_complex_t z);
/** @brief `z` to the power `w`: exp(w log z), so its error grows with |w log z|; `z^0 = 1` for
 *         every `z`. */
[[nodiscard]] proven_complex_t proven_complex_pow(proven_complex_t z, proven_complex_t w);
/** @brief The square root with a real part of +0 or more; the cut along the negative real axis. */
[[nodiscard]] proven_complex_t proven_complex_sqrt(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_sin(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_cos(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_tan(proven_complex_t z);
/** @brief The inverse sine; cuts outside [-1, 1] on the real axis. */
[[nodiscard]] proven_complex_t proven_complex_asin(proven_complex_t z);
/** @brief The inverse cosine, its real part in [0, pi]; cuts outside [-1, 1] on the real axis. */
[[nodiscard]] proven_complex_t proven_complex_acos(proven_complex_t z);
/** @brief The inverse tangent; cuts outside [-i, i] on the imaginary axis. */
[[nodiscard]] proven_complex_t proven_complex_atan(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_sinh(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_cosh(proven_complex_t z);
[[nodiscard]] proven_complex_t proven_complex_tanh(proven_complex_t z);
/** @brief The inverse hyperbolic sine; cuts outside [-i, i] on the imaginary axis. */
[[nodiscard]] proven_complex_t proven_complex_asinh(proven_complex_t z);
/** @brief The inverse hyperbolic cosine, its real part +0 or more; the cut below 1 on the real
 *         axis. */
[[nodiscard]] proven_complex_t proven_complex_acosh(proven_complex_t z);
/** @brief The inverse hyperbolic tangent; cuts outside [-1, 1] on the real axis. */
[[nodiscard]] proven_complex_t proven_complex_atanh(proven_complex_t z);

/** @brief The same for binary32: computed in binary64, each part rounded once. */
[[nodiscard]] proven_complex32_t proven_complex32_add(proven_complex32_t a, proven_complex32_t b);
[[nodiscard]] proven_complex32_t proven_complex32_sub(proven_complex32_t a, proven_complex32_t b);
[[nodiscard]] proven_complex32_t proven_complex32_mul(proven_complex32_t a, proven_complex32_t b);
[[nodiscard]] proven_complex32_t proven_complex32_div(proven_complex32_t a, proven_complex32_t b);
[[nodiscard]] float proven_complex32_abs(proven_complex32_t z);
[[nodiscard]] float proven_complex32_arg(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_conj(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_proj(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_exp(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_log(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_pow(proven_complex32_t z, proven_complex32_t w);
[[nodiscard]] proven_complex32_t proven_complex32_sqrt(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_sin(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_cos(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_tan(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_asin(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_acos(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_atan(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_sinh(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_cosh(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_tanh(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_asinh(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_acosh(proven_complex32_t z);
[[nodiscard]] proven_complex32_t proven_complex32_atanh(proven_complex32_t z);

#endif /* PROVEN_COMPLEX_H */
