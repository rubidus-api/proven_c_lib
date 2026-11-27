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
    PROVEN_FLOAT_FN_POW_
};

[[nodiscard]] proven_u64 proven_float_math_int_(int fn, bool b32, proven_u64 a, proven_u64 b);
/* The integer path's value at Ziv level k against a more precise one, in 1/1024 units of level
 * k's last place (-1: not a general input); what the rounding test's bound of 4 units rests on. */
[[nodiscard]] proven_i64 proven_float_math_level_gap_(int fn, bool b32, proven_u64 a, proven_u64 b, int k);
/* Whether the public functions take the FPU path (CORE-MATH) in this build on this processor. */
[[nodiscard]] bool proven_float_math_is_fpu_(void);

#endif
