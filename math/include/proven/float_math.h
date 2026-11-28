#ifndef PROVEN_FLOAT_MATH_H
#define PROVEN_FLOAT_MATH_H

#include "proven/types.h"

/**
 * @file float_math.h
 * @brief math.h's transcendental functions, correctly rounded, for binary64 and binary32
 *        (RFC-0056, RFC-0058; proven_math): exp, exp2, exp10, expm1, log, log2, log10, log1p,
 *        pow, cbrt, hypot, sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, asinh,
 *        acosh, atanh, erf, erfc, tgamma, lgamma, sinpi, cospi, tanpi, rsqrt, pown, rootn,
 *        compoundn.
 *
 * Each result is the exact value rounded once to nearest, ties to even - the one right answer
 * - so every target, every build of this library and every other correctly rounded library
 * gives the same bits. Special values as C23 Annex F says (exp(-inf) = +0, log(-0) = -inf,
 * pow(x, +-0) = 1 and pow(+1, y) = 1 even for a quiet NaN - a signalling one gives itself
 * quieted, as IEEE 754 says - and so hypot(+-inf, quiet NaN) = +inf; atan2's signed zeros and
 * infinities, asin(+-1) = +-pi/2 rounded, atanh(+-1) = +-inf, ...). The exact results are
 * exact: exp2 and exp10 of an integer (10^23 rounds to even), log2 of a power of 2, log10 of a
 * power of 10, cbrt of a cube, hypot of a Pythagorean triple.
 * A NaN given comes back quieted with its payload (of two, the first in argument order); a NaN
 * made (log of a negative number, sin of an infinity, pow of a negative number to a
 * non-integer, asin of 2, acosh of 0) is 0x7FF8000000000000 (binary32 0x7FC00000). No errno, no flags, no state, no
 * allocation, no libm; freestanding. The FPU path (CORE-MATH's code) on x86-64 with FMA and AVX2,
 * AArch64 and WebAssembly; integer arithmetic only elsewhere, with the same bits.
 */

[[nodiscard]] double proven_float_exp(double x);
[[nodiscard]] double proven_float_log(double x);
/** @brief `x` to the power `y`. */
[[nodiscard]] double proven_float_pow(double x, double y);
/** @brief Of `x` in radians, reduced exactly for every finite `x`. */
[[nodiscard]] double proven_float_sin(double x);
[[nodiscard]] double proven_float_cos(double x);
[[nodiscard]] double proven_float_tan(double x);
/** @brief The angle of the point (`x`, `y`), in [-pi, pi]. */
[[nodiscard]] double proven_float_atan2(double y, double x);

/** @brief 2 to the power `x`. */
[[nodiscard]] double proven_float_exp2(double x);
/** @brief 10 to the power `x`. */
[[nodiscard]] double proven_float_exp10(double x);
/** @brief exp(x) - 1, without the cancellation near 0. */
[[nodiscard]] double proven_float_expm1(double x);
/** @brief The base-2 logarithm. */
[[nodiscard]] double proven_float_log2(double x);
/** @brief The base-10 logarithm. */
[[nodiscard]] double proven_float_log10(double x);
/** @brief log(1 + x), without the cancellation near 0. */
[[nodiscard]] double proven_float_log1p(double x);
/** @brief The cube root (of a negative `x` too). */
[[nodiscard]] double proven_float_cbrt(double x);
/** @brief sqrt(x^2 + y^2), without overflow or underflow in between. */
[[nodiscard]] double proven_float_hypot(double x, double y);
/** @brief The inverse sine, in [-pi/2, pi/2]. */
[[nodiscard]] double proven_float_asin(double x);
/** @brief The inverse cosine, in [0, pi]. */
[[nodiscard]] double proven_float_acos(double x);
/** @brief The inverse tangent, in [-pi/2, pi/2]. */
[[nodiscard]] double proven_float_atan(double x);
[[nodiscard]] double proven_float_sinh(double x);
[[nodiscard]] double proven_float_cosh(double x);
[[nodiscard]] double proven_float_tanh(double x);
[[nodiscard]] double proven_float_asinh(double x);
[[nodiscard]] double proven_float_acosh(double x);
[[nodiscard]] double proven_float_atanh(double x);
/** @brief The error function, (2 / sqrt(pi)) times the integral of exp(-t^2) from 0 to `x`. */
[[nodiscard]] double proven_float_erf(double x);
/** @brief 1 - erf(x), without the cancellation for large `x` (down to the subnormals near 27.2). */
[[nodiscard]] double proven_float_erfc(double x);
/** @brief The gamma function; a pole (+-inf) at +-0, a NaN at the negative integers and -inf. */
[[nodiscard]] double proven_float_tgamma(double x);
/** @brief log|Gamma(x)|. The sign of Gamma(x) is not returned and no global is set (C's
 *         `signgam`): it is tgamma's sign where tgamma is in range. +inf at the poles. */
[[nodiscard]] double proven_float_lgamma(double x);
/** @brief sin(pi x), exact for every finite `x` (no reduction error); +-0 at the integers. */
[[nodiscard]] double proven_float_sinpi(double x);
/** @brief cos(pi x); +0 at the half-integers. */
[[nodiscard]] double proven_float_cospi(double x);
/** @brief tan(pi x); +-0 at the integers (the sign of x, flipped at odd ones), +-inf at the
 *         half-integers (+inf where n + 1/2 has an even n). */
[[nodiscard]] double proven_float_tanpi(double x);
/** @brief 1 / sqrt(x): +-inf at +-0, a NaN below 0, +0 at +inf. */
[[nodiscard]] double proven_float_rsqrt(double x);
/** @brief `x` to the integer power `n`; `pown(x, 0) = 1` for every `x`, a quiet NaN too (as pow). */
[[nodiscard]] double proven_float_pown(double x, proven_i64 n);
/** @brief The `n`-th root of `x`, of a negative `x` for an odd `n`; `rootn(x, 0)` is a NaN. Exact
 *         where `x` is an `n`-th power. No FPU path: the integer path everywhere. */
[[nodiscard]] double proven_float_rootn(double x, proven_i64 n);
/** @brief (1 + x)^n, without the rounding of 1 + x: compound interest; a NaN below -1,
 *         `compoundn(-1, n)` is +0 for n > 0 and +inf for n < 0, `compoundn(x, 0) = 1` for x >= -1
 *         and a NaN (IEEE 754 9.2). No FPU
 *         path: the integer path everywhere. */
[[nodiscard]] double proven_float_compoundn(double x, proven_i64 n);

/** @brief The same for binary32, rounded once in binary32. */
[[nodiscard]] float proven_float32_exp(float x);
[[nodiscard]] float proven_float32_log(float x);
[[nodiscard]] float proven_float32_pow(float x, float y);
[[nodiscard]] float proven_float32_exp2(float x);
[[nodiscard]] float proven_float32_exp10(float x);
[[nodiscard]] float proven_float32_expm1(float x);
[[nodiscard]] float proven_float32_log2(float x);
[[nodiscard]] float proven_float32_log10(float x);
[[nodiscard]] float proven_float32_log1p(float x);
[[nodiscard]] float proven_float32_cbrt(float x);
[[nodiscard]] float proven_float32_hypot(float x, float y);
[[nodiscard]] float proven_float32_asin(float x);
[[nodiscard]] float proven_float32_acos(float x);
[[nodiscard]] float proven_float32_atan(float x);
[[nodiscard]] float proven_float32_sinh(float x);
[[nodiscard]] float proven_float32_cosh(float x);
[[nodiscard]] float proven_float32_tanh(float x);
[[nodiscard]] float proven_float32_asinh(float x);
[[nodiscard]] float proven_float32_acosh(float x);
[[nodiscard]] float proven_float32_atanh(float x);
[[nodiscard]] float proven_float32_erf(float x);
[[nodiscard]] float proven_float32_erfc(float x);
[[nodiscard]] float proven_float32_tgamma(float x);
[[nodiscard]] float proven_float32_lgamma(float x);
[[nodiscard]] float proven_float32_sinpi(float x);
[[nodiscard]] float proven_float32_cospi(float x);
[[nodiscard]] float proven_float32_tanpi(float x);
[[nodiscard]] float proven_float32_rsqrt(float x);
[[nodiscard]] float proven_float32_pown(float x, proven_i64 n);
[[nodiscard]] float proven_float32_rootn(float x, proven_i64 n);
[[nodiscard]] float proven_float32_compoundn(float x, proven_i64 n);
[[nodiscard]] float proven_float32_sin(float x);
[[nodiscard]] float proven_float32_cos(float x);
[[nodiscard]] float proven_float32_tan(float x);
[[nodiscard]] float proven_float32_atan2(float y, float x);

#endif /* PROVEN_FLOAT_MATH_H */
