#ifndef PROVEN_FLOAT_ENV_H
#define PROVEN_FLOAT_ENV_H

#include "proven/types.h"

/**
 * @file float_env.h
 * @brief The floating-point environment as a value - C's <fenv.h> without global state
 *        (RFC-0058; proven_math).
 *
 * C's rounding direction and exception flags live in the processor, shared by every function the
 * thread calls: fesetround changes them for code that did not ask, and a flag raised anywhere
 * stays raised. Here they are a value the caller holds and passes: each function below rounds in
 * `env->round` and ORs the exceptions it raises into `env->flags`, and touches nothing else.
 * The processor's own mode is neither read nor changed, and every other function of proven_math
 * keeps rounding to nearest (RFC-0055 D-7).
 *
 * Arithmetic as IEEE 754 defines it in each direction, by the integer core of float_ops.h - so
 * the same bits and the same flags on every target, with or without an FPU. NaNs follow
 * float_ops.h's rule (the first NaN operand quieted, or 0x7FF8000000000000 made). Exceptions:
 *   - PROVEN_FLOAT_INEXACT: the result was rounded.
 *   - PROVEN_FLOAT_UNDERFLOW: the result is tiny and inexact; tiny is judged after rounding (as
 *     x86 and RISC-V do; AArch64 judges before).
 *   - PROVEN_FLOAT_OVERFLOW: past the largest finite; the result is infinity or the largest finite,
 *     as the direction says.
 *   - PROVEN_FLOAT_DIVBYZERO: a finite nonzero divided by zero.
 *   - PROVEN_FLOAT_INVALID: inf - inf, 0 * inf, 0 / 0, inf / inf, the root of a negative, a
 *     signalling NaN operand, a conversion out of range.
 * A NULL `env` is nearest-even with no flags kept.
 */

typedef enum {
    PROVEN_ROUND_NEAREST = 0,    /**< to nearest, ties to even (the default) */
    PROVEN_ROUND_TOWARD_ZERO = 1,
    PROVEN_ROUND_UP = 2,         /**< toward +infinity */
    PROVEN_ROUND_DOWN = 3        /**< toward -infinity */
} proven_round_t;

#define PROVEN_FLOAT_INEXACT 1u
#define PROVEN_FLOAT_UNDERFLOW 2u
#define PROVEN_FLOAT_OVERFLOW 4u
#define PROVEN_FLOAT_DIVBYZERO 8u
#define PROVEN_FLOAT_INVALID 16u

/** @brief A rounding direction and the exceptions raised so far (sticky: cleared only by you). */
typedef struct {
    proven_round_t round;
    unsigned flags;
} proven_float_env_t;

[[nodiscard]] double proven_float_env_add(proven_float_env_t *env, double x, double y);
[[nodiscard]] double proven_float_env_sub(proven_float_env_t *env, double x, double y);
[[nodiscard]] double proven_float_env_mul(proven_float_env_t *env, double x, double y);
[[nodiscard]] double proven_float_env_div(proven_float_env_t *env, double x, double y);
[[nodiscard]] double proven_float_env_fma(proven_float_env_t *env, double x, double y, double z);
[[nodiscard]] double proven_float_env_sqrt(proven_float_env_t *env, double x);
/** @brief To an integral value in the direction (C's rint); inexact when it changes the value. */
[[nodiscard]] double proven_float_env_rint(proven_float_env_t *env, double x);
/** @brief To an integer in the direction (C's llrint): PROVEN_ERR_OUT_OF_BOUNDS and invalid for
 *         a NaN or a value outside [-2^63, 2^63). */
[[nodiscard]] proven_err_t proven_float_env_to_i64(proven_float_env_t *env, double x, proven_i64 *out);
/** @brief To binary32 in the direction, and an integer to binary64 in it. */
[[nodiscard]] float proven_float_env_to_float32(proven_float_env_t *env, double x);
[[nodiscard]] double proven_float_env_from_i64(proven_float_env_t *env, proven_i64 v);

/** @brief The same for binary32. */
[[nodiscard]] float proven_float32_env_add(proven_float_env_t *env, float x, float y);
[[nodiscard]] float proven_float32_env_sub(proven_float_env_t *env, float x, float y);
[[nodiscard]] float proven_float32_env_mul(proven_float_env_t *env, float x, float y);
[[nodiscard]] float proven_float32_env_div(proven_float_env_t *env, float x, float y);
[[nodiscard]] float proven_float32_env_fma(proven_float_env_t *env, float x, float y, float z);
[[nodiscard]] float proven_float32_env_sqrt(proven_float_env_t *env, float x);
[[nodiscard]] float proven_float32_env_rint(proven_float_env_t *env, float x);
[[nodiscard]] proven_err_t proven_float32_env_to_i64(proven_float_env_t *env, float x, proven_i64 *out);
[[nodiscard]] float proven_float32_env_from_i64(proven_float_env_t *env, proven_i64 v);

#endif /* PROVEN_FLOAT_ENV_H */
