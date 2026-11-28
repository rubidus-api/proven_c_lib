#ifndef PROVEN_MATH_UMBRELLA_H
#define PROVEN_MATH_UMBRELLA_H

/**
 * @file proven_math.h
 * @brief Entry point for the floating-point library, proven_math: binary64 and binary32
 *        operations that give the same bits on every target, by the target's IEEE instruction
 *        or by integers on a core without an FPU (RFC-0054, RFC-0055), and exp, log, pow, sin,
 *        cos, tan, atan2 and the rest of math.h's transcendental functions correctly rounded
 *        (RFC-0056, RFC-0058), the floating-point environment as a value (float_env.h) and
 *        complex numbers (complex.h). It stands on the base
 *        library's public headers alone and is built from the same tree, under one version.
 *
 * Its headers live in math/include: add that directory to the include path, compile math/src
 * with the base library, and proven.h includes this file too. All of it is pure computation and
 * part of the freestanding profile. Decimal parsing and formatting of floats stay in the base
 * (float_parse.h, float_format.h), which fmt.h uses.
 */

#include "proven.h"
#include "proven/math_config.h"
#include "proven/float_ops.h"
#include "proven/float_math.h"
#include "proven/float_env.h"
#include "proven/complex.h"

#endif /* PROVEN_MATH_UMBRELLA_H */
