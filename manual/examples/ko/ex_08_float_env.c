#include "example.h"

/*
 * float_env.h: 값으로 다루는 부동소수점 환경. C 는 반올림 방향과 예외 플래그를 프로세서에 두고,
 * 스레드가 부르는 모든 것이 그것을 함께 쓴다. 여기서는 당신이 들고 넘기는 proven_float_env_t 이고,
 * 그 밖에는 아무것도 건드리지 않는다.
 *
 * 전형적인 쓰임은 구간 산술이다. 같은 합을 아래로 반올림한 것과 위로 반올림한 것이, 반올림 오차가
 * 어떠했든 참값을 감싼다. 뒤쪽 절반은 플래그 - 지우기 전까지 남아 있다 - 와 binary32 형태를 보인다.
 */

int main(void) {
    /* 1/1 + 1/2 + ... + 1/1000 을, 한 번은 모든 연산을 아래로, 한 번은 위로 반올림하며. */
    proven_float_env_t down = { PROVEN_ROUND_DOWN, 0 }, up = { PROVEN_ROUND_UP, 0 };
    double lo = 0, hi = 0;
    for (int k = 1; k <= 1000; k++) {
        lo = proven_float_env_add(&down, lo, proven_float_env_div(&down, 1.0, proven_float_env_from_i64(&down, k)));
        hi = proven_float_env_add(&up, hi, proven_float_env_div(&up, 1.0, proven_float_env_from_i64(&up, k)));
    }
    /* H(1000) = 7.4854708605503449... - 분명히 안에 있고, 구간의 폭은 몇 ulp 다. */
    EXAMPLE_REQUIRE(lo < hi && lo <= 7.4854708605503449 && 7.4854708605503449 <= hi, "the true sum is between the two");
    EXAMPLE_REQUIRE(hi - lo < 1e-12, "and the bracket is tight");
    EXAMPLE_REQUIRE(down.flags == PROVEN_FLOAT_INEXACT && up.flags == PROVEN_FLOAT_INEXACT, "only inexact was raised");

    /* 다른 연산들, 그리고 문제를 알리는 플래그. */
    proven_float_env_t env = { PROVEN_ROUND_NEAREST, 0 };
    const double big = proven_float_env_mul(&env, 1e300, 1e10);          /* 넘침: 무한대 */
    EXAMPLE_REQUIRE(env.flags & PROVEN_FLOAT_OVERFLOW, "overflow raised");
    env.flags = 0;                                                       /* 남아 있으므로 직접 지운다 */
    const double root = proven_float_env_sqrt(&env, -1.0);               /* invalid: NaN */
    const double inf = proven_float_env_div(&env, 1.0, 0.0);             /* 0 으로 나눔 */
    EXAMPLE_REQUIRE(env.flags == (PROVEN_FLOAT_INVALID | PROVEN_FLOAT_DIVBYZERO), "both reported, nothing else");
    env.flags = 0;
    const double tiny = proven_float_env_mul(&env, 1e-300, 1e-300);      /* 0 으로 아래넘침 */
    EXAMPLE_REQUIRE(tiny == 0 && env.flags == (PROVEN_FLOAT_UNDERFLOW | PROVEN_FLOAT_INEXACT), "underflow and inexact");
    proven_float_env_t z = { PROVEN_ROUND_TOWARD_ZERO, 0 };
    EXAMPLE_REQUIRE(proven_float_env_sub(&z, 1.0, 1.0) == 0 && proven_float_env_fma(&z, 2.0, 3.0, 1.0) == 7.0, "exact results raise nothing");
    EXAMPLE_REQUIRE(z.flags == 0, "no flag");

    /* 방향에 맞춘 정수로 - fesetround 아래 C 의 rint 와 llrint - 그리고 binary32 로. */
    proven_i64 n = 0;
    EXAMPLE_REQUIRE(proven_float_env_rint(&up, 2.1) == 3.0 && proven_float_env_rint(&down, -2.1) == -3.0, "rint up and down");
    EXAMPLE_REQUIRE(proven_float_env_to_i64(&z, -2.9, &n) == PROVEN_OK && n == -2, "toward zero");
    EXAMPLE_REQUIRE(proven_float_env_to_i64(&env, 1e30, &n) == PROVEN_ERR_OUT_OF_BOUNDS && (env.flags & PROVEN_FLOAT_INVALID), "too large: an error, and invalid");
    EXAMPLE_REQUIRE(proven_float_env_to_float32(&up, 0.1) > 0.1 && proven_float_env_to_float32(&down, 0.1) < 0.1, "0.1 narrowed up and down");

    /* binary32: 앞에 proven_float32_ 가 붙은 같은 함수들. */
    proven_float_env_t f = { PROVEN_ROUND_UP, 0 };
    const float s = proven_float32_env_add(&f, 1.0f, 1e-10f);            /* 1 을 넘어 위로 반올림 */
    EXAMPLE_REQUIRE(s > 1.0f && proven_float32_env_sub(&f, s, 1.0f) > 0.0f, "1 + tiny, rounded up");
    EXAMPLE_REQUIRE(proven_float32_env_mul(&f, 3.0f, 3.0f) == 9.0f && proven_float32_env_div(&f, 1.0f, 3.0f) > 0.3333333f, "mul exact, div up");
    EXAMPLE_REQUIRE(proven_float32_env_fma(&f, 2.0f, 2.0f, 1.0f) == 5.0f && proven_float32_env_sqrt(&f, 2.0f) > 1.4142135f, "fma, sqrt");
    EXAMPLE_REQUIRE(proven_float32_env_rint(&f, 0.5f) == 1.0f && proven_float32_env_from_i64(&f, 16777217) == 16777218.0f, "rint, from_i64");
    EXAMPLE_REQUIRE(proven_float32_env_to_i64(&f, 2.5f, &n) == PROVEN_OK && n == 3, "to_i64 up");

    printf("H(1000) in [%.17g, %.17g]\n", lo, hi);
    (void)big; (void)root; (void)inf;
    return EXAMPLE_OK();
}
