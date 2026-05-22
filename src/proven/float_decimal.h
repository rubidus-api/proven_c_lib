#ifndef PROVEN_FLOAT_DECIMAL_H
#define PROVEN_FLOAT_DECIMAL_H

#include "proven/types.h"

/*
 * Internal float helpers shared by the scanner and formatter.
 * This is a private implementation detail, not a public API surface.
 */

typedef struct proven_u128_parts_t {
    proven_u64 lo;
    proven_u64 hi;
} proven_u128_parts_t;

proven_u32 proven_float_bits_f32(float value);
proven_u64 proven_float_bits_f64(double value);
proven_u128_parts_t proven_float_mul_u64_u64_to_u128(proven_u64 lhs, proven_u64 rhs);

double proven_float_scale_pow10(double value, proven_i64 exp10);
double proven_float_convert_decimal(proven_u64 mantissa, proven_i64 exp10);
bool proven_float_normalize_scientific(double *abs_v, int *sci_exp);
bool proven_float_shortest_literal_f64(double value, char *buf, proven_size_t buf_cap, proven_size_t *written_out);
bool proven_float_shortest_literal_f32(float value, char *buf, proven_size_t buf_cap, proven_size_t *written_out);

#endif /* PROVEN_FLOAT_DECIMAL_H */
