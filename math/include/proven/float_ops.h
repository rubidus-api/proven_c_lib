#ifndef PROVEN_FLOAT_OPS_H
#define PROVEN_FLOAT_OPS_H

#include "proven/types.h"
#include "proven/error.h"

/**
 * @file float_ops.h
 * @brief binary64 and binary32 operations that give the same bits on every target (RFC-0054,
 *        RFC-0055).
 *
 * Each of these has exactly one right answer under IEEE 754 - it is exact, or correctly
 * rounded - and each gives that answer whatever the target, the compiler's floating-point
 * flags (`-ffast-math`, `-ffp-contract`), or x87's extended precision. RFC-0054's functions
 * work on the bits with integer arithmetic, except the square root, which takes the IEEE
 * instruction on x86 SSE2, AArch64 and WebAssembly; the arithmetic below takes it on more
 * targets (listed there). Elsewhere, and in a `-ffast-math` build, an integer implementation
 * gives the same bits. Comparisons and conversions are on the bits everywhere. No libm, no
 * errno, no state, no allocation; freestanding.
 *
 * NaN: a NaN given comes back with its payload and its quiet bit set - of two or three, the
 * first in argument order that is a NaN; a NaN made here (the square root of a negative number,
 * `fmod` of an infinity or by zero, inf - inf, 0 * inf, 0 / 0, an invalid fma) is
 * 0x7FF8000000000000 (binary32: 0x7FC00000), whatever the instruction would have made.
 * proven_float_fabs and proven_float_copysign touch only the sign bit, as IEEE 754 says, so a
 * signalling NaN stays signalling through them. Except on 32-bit x86, where the calling
 * convention returns a double or a float in the x87 register, which quiets a signalling NaN:
 * there none survives a function's return, these functions' included.
 *
 * Arithmetic (RFC-0055): add, subtract, multiply, divide and fma, correctly rounded to nearest,
 * ties to even, with subnormals in full. Each takes the target's instruction where it is IEEE
 * 754 by construction (x86 SSE2, AArch64, ARM VFP, RISC-V F/D, WebAssembly; fma only where the
 * target says it is fused), and an integer implementation that gives the same bits elsewhere -
 * no FPU and no soft-float runtime needed. A build with `-ffast-math` takes the integers
 * everywhere, since that flag also turns on flush-to-zero at startup. Built with
 * PROVEN_FLOAT_SOFT=1, every function takes the integers. There are no exception flags, and the instruction path assumes the default
 * floating-point environment: a program that changes the rounding mode, or turns on
 * flush-to-zero (as `-ffast-math`'s startup code does on x86), gets these bits only with
 * PROVEN_FLOAT_SOFT=1.
 */

/** @brief The bits of `x`, and the double with these bits. */
[[nodiscard]] proven_u64 proven_float_to_bits(double x);
[[nodiscard]] double proven_float_from_bits(proven_u64 bits);

/** @brief By the bits; `-ffast-math` cannot fold them away. */
[[nodiscard]] bool proven_float_is_nan(double x);
[[nodiscard]] bool proven_float_is_inf(double x);
[[nodiscard]] bool proven_float_is_finite(double x);

/** @brief The square root, correctly rounded. sqrt(-0) is -0, sqrt(+inf) is +inf; any other
 *         negative number gives the NaN 0x7FF8000000000000. */
[[nodiscard]] double proven_float_sqrt(double x);

/** @brief Integral values. The sign of a zero result is the sign of `x`: floor(-0.5) is -1,
 *         ceil(-0.5) is -0, trunc(-0.5) is -0. Infinities come back as they are. */
[[nodiscard]] double proven_float_floor(double x);
[[nodiscard]] double proven_float_ceil(double x);
[[nodiscard]] double proven_float_trunc(double x);
/** @brief To the nearest integer, halfway cases away from zero (round(2.5) is 3, round(-0.5) is -1). */
[[nodiscard]] double proven_float_round(double x);
/** @brief To the nearest integer, halfway cases to even (rint(2.5) is 2, rint(-0.5) is -0). */
[[nodiscard]] double proven_float_rint(double x);

/** @brief `x` without its sign; `x`'s magnitude with `y`'s sign. */
[[nodiscard]] double proven_float_fabs(double x);
[[nodiscard]] double proven_float_copysign(double x, double y);

/** @brief The smaller and the larger. A number wins over a NaN: NaN comes back only when both
 *         are (then `x`, quieted). -0 is below +0. */
[[nodiscard]] double proven_float_fmin(double x, double y);
[[nodiscard]] double proven_float_fmax(double x, double y);

/** @brief The remainder of `x / y` truncated - always exact - with `x`'s sign. NaN for an
 *         infinite `x` or a zero `y`; `x` itself for an infinite `y`. */
[[nodiscard]] double proven_float_fmod(double x, double y);

/** @brief `x` = m * 2^*exp with |m| in [0.5, 1); for a zero, an infinity or a NaN, `x` itself
 *         (a NaN quieted) and *exp 0. */
[[nodiscard]] double proven_float_frexp(double x, int *exp);
/** @brief `x` * 2^`exp`, rounded once to nearest even when it falls among the subnormals;
 *         infinity when it overflows. */
[[nodiscard]] double proven_float_ldexp(double x, int exp);

/** @brief `v` rounded to nearest even: exact up to 2^53 in magnitude. */
[[nodiscard]] double proven_float_from_i64(proven_i64 v);
/**
 * @brief `x` truncated toward zero.
 * @return PROVEN_ERR_OUT_OF_BOUNDS for a NaN and for values outside [-2^63, 2^63) (`*out` is
 *         then not written); PROVEN_ERR_INVALID_ARG for a null `out`.
 */
[[nodiscard]] proven_err_t proven_float_to_i64(double x, proven_i64 *out);

/** @brief `x` + `y`, `x` - `y`, `x` * `y`, `x` / `y`, rounded once. inf - inf, 0 * inf,
 *         0 / 0 and inf / inf are the NaN 0x7FF8000000000000; x - x is +0. */
[[nodiscard]] double proven_float_add(double x, double y);
[[nodiscard]] double proven_float_sub(double x, double y);
[[nodiscard]] double proven_float_mul(double x, double y);
[[nodiscard]] double proven_float_div(double x, double y);
/** @brief `x` * `y` + `z` with one rounding. 0 * inf is a NaN even when `z` is a number. */
[[nodiscard]] double proven_float_fma(double x, double y, double z);

/** @brief IEEE comparison: a NaN is unordered with everything, itself included; -0 equals +0.
 *         compare gives -1, 0 or 1, and 2 when unordered. */
[[nodiscard]] int proven_float_compare(double x, double y);
[[nodiscard]] bool proven_float_eq(double x, double y);
[[nodiscard]] bool proven_float_lt(double x, double y);
[[nodiscard]] bool proven_float_le(double x, double y);
[[nodiscard]] bool proven_float_unordered(double x, double y);

/** @brief To binary32 rounded to nearest even (infinity past its largest), and back exactly. A
 *         NaN keeps its sign and the top of its payload, quieted. */
[[nodiscard]] float proven_float_to_float32(double x);
[[nodiscard]] double proven_float_from_float32(float x);

/** @brief From integers, exact (proven_float_from_u64 rounds to nearest even past 2^53). */
[[nodiscard]] double proven_float_from_i32(proven_i32 v);
[[nodiscard]] double proven_float_from_u32(proven_u32 v);
[[nodiscard]] double proven_float_from_u64(proven_u64 v);
/** @brief `x` truncated toward zero. PROVEN_ERR_OUT_OF_BOUNDS for a NaN and for a value whose
 *         truncation is outside the type (`*out` not written); PROVEN_ERR_INVALID_ARG for a
 *         null `out`. -0.5 truncates to 0, so it is in range of the unsigned types. */
[[nodiscard]] proven_err_t proven_float_to_i32(double x, proven_i32 *out);
[[nodiscard]] proven_err_t proven_float_to_u32(double x, proven_u32 *out);
[[nodiscard]] proven_err_t proven_float_to_u64(double x, proven_u64 *out);

/*
 * binary32 (RFC-0055): every function above for `float`, with the same contract. The integral
 * values, fmod, frexp, ldexp, fmin and fmax are exact or rounded once, as for binary64; the
 * arithmetic and the square root round once in binary32 (never through binary64), so a
 * single-precision FPU (Cortex-M4F, M33, RISC-V F) and the integers give the same bits.
 */
[[nodiscard]] proven_u32 proven_float32_to_bits(float x);
[[nodiscard]] float proven_float32_from_bits(proven_u32 bits);
[[nodiscard]] bool proven_float32_is_nan(float x);
[[nodiscard]] bool proven_float32_is_inf(float x);
[[nodiscard]] bool proven_float32_is_finite(float x);
[[nodiscard]] float proven_float32_sqrt(float x);
[[nodiscard]] float proven_float32_floor(float x);
[[nodiscard]] float proven_float32_ceil(float x);
[[nodiscard]] float proven_float32_trunc(float x);
[[nodiscard]] float proven_float32_round(float x);
[[nodiscard]] float proven_float32_rint(float x);
[[nodiscard]] float proven_float32_fabs(float x);
[[nodiscard]] float proven_float32_copysign(float x, float y);
[[nodiscard]] float proven_float32_fmin(float x, float y);
[[nodiscard]] float proven_float32_fmax(float x, float y);
[[nodiscard]] float proven_float32_fmod(float x, float y);
[[nodiscard]] float proven_float32_frexp(float x, int *exp);
[[nodiscard]] float proven_float32_ldexp(float x, int exp);
[[nodiscard]] float proven_float32_add(float x, float y);
[[nodiscard]] float proven_float32_sub(float x, float y);
[[nodiscard]] float proven_float32_mul(float x, float y);
[[nodiscard]] float proven_float32_div(float x, float y);
[[nodiscard]] float proven_float32_fma(float x, float y, float z);
[[nodiscard]] int proven_float32_compare(float x, float y);
[[nodiscard]] bool proven_float32_eq(float x, float y);
[[nodiscard]] bool proven_float32_lt(float x, float y);
[[nodiscard]] bool proven_float32_le(float x, float y);
[[nodiscard]] bool proven_float32_unordered(float x, float y);
/** @brief From integers, rounded to nearest even past 2^24. */
[[nodiscard]] float proven_float32_from_i32(proven_i32 v);
[[nodiscard]] float proven_float32_from_u32(proven_u32 v);
[[nodiscard]] float proven_float32_from_i64(proven_i64 v);
[[nodiscard]] float proven_float32_from_u64(proven_u64 v);
[[nodiscard]] proven_err_t proven_float32_to_i32(float x, proven_i32 *out);
[[nodiscard]] proven_err_t proven_float32_to_u32(float x, proven_u32 *out);
[[nodiscard]] proven_err_t proven_float32_to_i64(float x, proven_i64 *out);
[[nodiscard]] proven_err_t proven_float32_to_u64(float x, proven_u64 *out);

#endif /* PROVEN_FLOAT_OPS_H */
