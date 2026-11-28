#include "example.h"

/*
 * float_env.h: the floating-point environment as a value. C keeps the rounding direction and
 * the exception flags in the processor, shared by everything the thread calls; here they are a
 * proven_float_env_t you hold and pass, and nothing else is touched.
 *
 * The classic use is interval arithmetic: the same sum rounded down and rounded up brackets
 * the true value, whatever the rounding errors were. The second half shows the flags - which
 * stay set until you clear them - and the binary32 forms.
 */

int main(void) {
    /* 1/1 + 1/2 + ... + 1/1000, once rounding every operation down, once up. */
    proven_float_env_t down = { PROVEN_ROUND_DOWN, 0 }, up = { PROVEN_ROUND_UP, 0 };
    double lo = 0, hi = 0;
    for (int k = 1; k <= 1000; k++) {
        lo = proven_float_env_add(&down, lo, proven_float_env_div(&down, 1.0, proven_float_env_from_i64(&down, k)));
        hi = proven_float_env_add(&up, hi, proven_float_env_div(&up, 1.0, proven_float_env_from_i64(&up, k)));
    }
    /* H(1000) = 7.4854708605503449... - certainly inside, and the interval is a few ulps wide. */
    EXAMPLE_REQUIRE(lo < hi && lo <= 7.4854708605503449 && 7.4854708605503449 <= hi, "the true sum is between the two");
    EXAMPLE_REQUIRE(hi - lo < 1e-12, "and the bracket is tight");
    EXAMPLE_REQUIRE(down.flags == PROVEN_FLOAT_INEXACT && up.flags == PROVEN_FLOAT_INEXACT, "only inexact was raised");

    /* The other operations, and the flags that report trouble. */
    proven_float_env_t env = { PROVEN_ROUND_NEAREST, 0 };
    const double big = proven_float_env_mul(&env, 1e300, 1e10);          /* overflow: infinity */
    EXAMPLE_REQUIRE(env.flags & PROVEN_FLOAT_OVERFLOW, "overflow raised");
    env.flags = 0;                                                       /* sticky: clear them yourself */
    const double root = proven_float_env_sqrt(&env, -1.0);               /* invalid: a NaN */
    const double inf = proven_float_env_div(&env, 1.0, 0.0);             /* divide by zero */
    EXAMPLE_REQUIRE(env.flags == (PROVEN_FLOAT_INVALID | PROVEN_FLOAT_DIVBYZERO), "both reported, nothing else");
    env.flags = 0;
    const double tiny = proven_float_env_mul(&env, 1e-300, 1e-300);      /* underflow to 0 */
    EXAMPLE_REQUIRE(tiny == 0 && env.flags == (PROVEN_FLOAT_UNDERFLOW | PROVEN_FLOAT_INEXACT), "underflow and inexact");
    proven_float_env_t z = { PROVEN_ROUND_TOWARD_ZERO, 0 };
    EXAMPLE_REQUIRE(proven_float_env_sub(&z, 1.0, 1.0) == 0 && proven_float_env_fma(&z, 2.0, 3.0, 1.0) == 7.0, "exact results raise nothing");
    EXAMPLE_REQUIRE(z.flags == 0, "no flag");

    /* To integers in the direction - C's rint and llrint under fesetround - and to binary32. */
    proven_i64 n = 0;
    EXAMPLE_REQUIRE(proven_float_env_rint(&up, 2.1) == 3.0 && proven_float_env_rint(&down, -2.1) == -3.0, "rint up and down");
    EXAMPLE_REQUIRE(proven_float_env_to_i64(&z, -2.9, &n) == PROVEN_OK && n == -2, "toward zero");
    EXAMPLE_REQUIRE(proven_float_env_to_i64(&env, 1e30, &n) == PROVEN_ERR_OUT_OF_BOUNDS && (env.flags & PROVEN_FLOAT_INVALID), "too large: an error, and invalid");
    EXAMPLE_REQUIRE(proven_float_env_to_float32(&up, 0.1) > 0.1 && proven_float_env_to_float32(&down, 0.1) < 0.1, "0.1 narrowed up and down");

    /* binary32: the same functions with proven_float32_ in front. */
    proven_float_env_t f = { PROVEN_ROUND_UP, 0 };
    const float s = proven_float32_env_add(&f, 1.0f, 1e-10f);            /* rounded up past 1 */
    EXAMPLE_REQUIRE(s > 1.0f && proven_float32_env_sub(&f, s, 1.0f) > 0.0f, "1 + tiny, rounded up");
    EXAMPLE_REQUIRE(proven_float32_env_mul(&f, 3.0f, 3.0f) == 9.0f && proven_float32_env_div(&f, 1.0f, 3.0f) > 0.3333333f, "mul exact, div up");
    EXAMPLE_REQUIRE(proven_float32_env_fma(&f, 2.0f, 2.0f, 1.0f) == 5.0f && proven_float32_env_sqrt(&f, 2.0f) > 1.4142135f, "fma, sqrt");
    EXAMPLE_REQUIRE(proven_float32_env_rint(&f, 0.5f) == 1.0f && proven_float32_env_from_i64(&f, 16777217) == 16777218.0f, "rint, from_i64");
    EXAMPLE_REQUIRE(proven_float32_env_to_i64(&f, 2.5f, &n) == PROVEN_OK && n == 3, "to_i64 up");

    printf("H(1000) in [%.17g, %.17g]\n", lo, hi);
    (void)big; (void)root; (void)inf;
    return EXAMPLE_OK();
}
