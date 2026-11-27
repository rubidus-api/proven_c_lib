#ifndef PROVEN_INTERNAL_FLOAT_OPS_H
#define PROVEN_INTERNAL_FLOAT_OPS_H

/* For tests (RFC-0054 D-2): the integer square root every target without an IEEE instruction
 * uses, and whether this build's proven_float_sqrt takes the instruction. */

#include "proven/types.h"

[[nodiscard]] double proven_float_sqrt_soft_(double x);
[[nodiscard]] bool proven_float_sqrt_is_hardware_(void);

/* RFC-0055: the integer implementation of each rounding operation, on the bits of either
 * format (binary32's in the low 32), and whether this build's public function takes the
 * instruction instead. */
enum {
    PROVEN_FLOAT_OP_ADD_,
    PROVEN_FLOAT_OP_SUB_,
    PROVEN_FLOAT_OP_MUL_,
    PROVEN_FLOAT_OP_DIV_,
    PROVEN_FLOAT_OP_FMA_,
    PROVEN_FLOAT_OP_SQRT_
};
[[nodiscard]] proven_u64 proven_float_soft_(int op, bool b32, proven_u64 a, proven_u64 b, proven_u64 c);
[[nodiscard]] bool proven_float_op_is_hardware_(int op, bool b32);

/* m * 2^e (m nonzero; its bit 0 sticky) rounded once to nearest even into binary64, or binary32
 * when b32, with the sign given: the bits (binary32's in the low 32). For float_math.c. */
[[nodiscard]] proven_u64 proven_float_pack_(bool neg, proven_i64 e, proven_u64 m, bool b32);

#endif
