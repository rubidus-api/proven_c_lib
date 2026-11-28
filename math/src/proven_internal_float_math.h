#ifndef PROVEN_INTERNAL_FLOAT_MATH_H
#define PROVEN_INTERNAL_FLOAT_MATH_H

/* For tests (RFC-0056): each function of float_math.h by the integer path, on the bits of
 * either format (binary32's in the low 32), whichever path this build's public functions take;
 * atan2 takes (y, x) as (a, b), pow (x, y). */

#include "proven/types.h"

enum {
    PROVEN_FLOAT_FN_EXP_,
    PROVEN_FLOAT_FN_LOG_,
    PROVEN_FLOAT_FN_SIN_,
    PROVEN_FLOAT_FN_COS_,
    PROVEN_FLOAT_FN_TAN_,
    PROVEN_FLOAT_FN_ATAN2_,
    PROVEN_FLOAT_FN_POW_,
    /* RFC-0058 */
    PROVEN_FLOAT_FN_EXP2_,
    PROVEN_FLOAT_FN_EXP10_,
    PROVEN_FLOAT_FN_EXPM1_,
    PROVEN_FLOAT_FN_LOG2_,
    PROVEN_FLOAT_FN_LOG10_,
    PROVEN_FLOAT_FN_LOG1P_,
    PROVEN_FLOAT_FN_CBRT_,
    PROVEN_FLOAT_FN_HYPOT_,
    PROVEN_FLOAT_FN_ASIN_,
    PROVEN_FLOAT_FN_ACOS_,
    PROVEN_FLOAT_FN_ATAN_,
    PROVEN_FLOAT_FN_SINH_,
    PROVEN_FLOAT_FN_COSH_,
    PROVEN_FLOAT_FN_TANH_,
    PROVEN_FLOAT_FN_ASINH_,
    PROVEN_FLOAT_FN_ACOSH_,
    PROVEN_FLOAT_FN_ATANH_,
    PROVEN_FLOAT_FN_ERF_,
    PROVEN_FLOAT_FN_ERFC_,
    PROVEN_FLOAT_FN_TGAMMA_,
    PROVEN_FLOAT_FN_LGAMMA_,
    PROVEN_FLOAT_FN_SINPI_,
    PROVEN_FLOAT_FN_COSPI_,
    PROVEN_FLOAT_FN_TANPI_,
    PROVEN_FLOAT_FN_RSQRT_,
    PROVEN_FLOAT_FN_POWN_,
    PROVEN_FLOAT_FN_ROOTN_,
    PROVEN_FLOAT_FN_COMPOUNDN_,
    PROVEN_FLOAT_FN_COUNT_
};

/* Every function of float_math.h: X(ID, name, arguments), in the enum's order, for the checks.
 * Arguments: 1 (x), 2 (x, y), or 3 (x and an integer n, proven_i64; passed as the second operand's
 * 64 bits). */
#define PROVEN_FLOAT_MATH_FNS(X) \
    X(EXP, exp, 1) X(LOG, log, 1) X(SIN, sin, 1) X(COS, cos, 1) X(TAN, tan, 1) X(ATAN2, atan2, 2) X(POW, pow, 2) \
    X(EXP2, exp2, 1) X(EXP10, exp10, 1) X(EXPM1, expm1, 1) X(LOG2, log2, 1) X(LOG10, log10, 1) X(LOG1P, log1p, 1) \
    X(CBRT, cbrt, 1) X(HYPOT, hypot, 2) X(ASIN, asin, 1) X(ACOS, acos, 1) X(ATAN, atan, 1) X(SINH, sinh, 1) \
    X(COSH, cosh, 1) X(TANH, tanh, 1) X(ASINH, asinh, 1) X(ACOSH, acosh, 1) X(ATANH, atanh, 1) \
    X(ERF, erf, 1) X(ERFC, erfc, 1) X(TGAMMA, tgamma, 1) X(LGAMMA, lgamma, 1) \
    X(SINPI, sinpi, 1) X(COSPI, cospi, 1) X(TANPI, tanpi, 1) X(RSQRT, rsqrt, 1) \
    X(POWN, pown, 3) X(ROOTN, rootn, 3) X(COMPOUNDN, compoundn, 3)


[[nodiscard]] proven_u64 proven_float_math_int_(int fn, bool b32, proven_u64 a, proven_u64 b);
/* The integer path's value at Ziv level k against a more precise one, in 1/1024 units of level
 * k's last place (-1: not a general input); what the rounding test's bound of 4 units rests on. */
[[nodiscard]] proven_i64 proven_float_math_level_gap_(int fn, bool b32, proven_u64 a, proven_u64 b, int k);
/* Whether the public functions take the FPU path (CORE-MATH) in this build on this processor. */
[[nodiscard]] bool proven_float_math_is_fpu_(void);

#endif
