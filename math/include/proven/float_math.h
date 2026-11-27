#ifndef PROVEN_FLOAT_MATH_H
#define PROVEN_FLOAT_MATH_H

#include "proven/types.h"

/**
 * @file float_math.h
 * @brief exp, log, pow, sin, cos, tan and atan2, correctly rounded, for binary64 and binary32
 *        (RFC-0056; proven_math).
 *
 * Each result is the exact value rounded once to nearest, ties to even - the one right answer
 * - so every target, every build of this library and every other correctly rounded library
 * gives the same bits. Special values as C23 Annex F says (exp(-inf) = +0, log(-0) = -inf,
 * pow(x, +-0) = 1 and pow(+1, y) = 1 even for a quiet NaN - a signalling one gives itself
 * quieted, as IEEE 754 says - atan2's signed zeros and infinities, ...).
 * A NaN given comes back quieted with its payload (of two, the first in argument order); a NaN
 * made (log of a negative number, sin of an infinity, pow of a negative number to a
 * non-integer) is 0x7FF8000000000000 (binary32 0x7FC00000). No errno, no flags, no state, no
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

/** @brief The same for binary32, rounded once in binary32. */
[[nodiscard]] float proven_float32_exp(float x);
[[nodiscard]] float proven_float32_log(float x);
[[nodiscard]] float proven_float32_pow(float x, float y);
[[nodiscard]] float proven_float32_sin(float x);
[[nodiscard]] float proven_float32_cos(float x);
[[nodiscard]] float proven_float32_tan(float x);
[[nodiscard]] float proven_float32_atan2(float y, float x);

#endif /* PROVEN_FLOAT_MATH_H */
