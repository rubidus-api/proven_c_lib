#ifndef PROVEN_CM_CONFIG_H
#define PROVEN_CM_CONFIG_H

/* Whether float_math.h's FPU path (the CORE-MATH code of math/src/cm) is compiled: x86-64 and
 * AArch64 and wasm, little-endian, with a 128-bit integer type, evaluating at each type's own precision,
 * not under fast-math, not with PROVEN_FLOAT_SOFT or PROVEN_FLOAT_MATH_SMALL. On x86-64 the
 * code needs FMA and AVX2: chosen at run time in a hosted build, and only when the build itself
 * targets them in a freestanding one. See cm_shim.h. */

#include "proven/math_config.h"

#if !PROVEN_FLOAT_SOFT && !PROVEN_FLOAT_MATH_SMALL && !defined(__FAST_MATH__) && defined(__GNUC__) \
    && defined(__SIZEOF_INT128__) && defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 0      \
    && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__                        \
    && (defined(__aarch64__) || defined(__wasm__) || (defined(__x86_64__) && (!defined(PROVEN_FREESTANDING) || (defined(__FMA__) && defined(__AVX2__)))))
#define PROVEN_CM 1
#else
#define PROVEN_CM 0
#endif

#endif /* PROVEN_CM_CONFIG_H */
