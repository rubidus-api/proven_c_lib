#ifndef PROVEN_FLOAT_DECIMAL_H
#define PROVEN_FLOAT_DECIMAL_H

#include "proven/types.h"

/*
 * Internal float helpers shared by the scanner and formatter.
 * This is a private implementation detail, not a public API surface.
 */

double proven_float_scale_pow10(double value, proven_i64 exp10);
double proven_float_convert_decimal(proven_u64 mantissa, proven_i64 exp10);
bool proven_float_normalize_scientific(double *abs_v, int *sci_exp);

#endif /* PROVEN_FLOAT_DECIMAL_H */
