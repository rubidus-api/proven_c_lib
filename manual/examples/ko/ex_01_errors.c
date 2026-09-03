#include "example.h"

/*
 * proven 에서 오류는 값이다. 실패할 수 있는 호출은 오류나 result 를 돌려주고,
 * 컴파일러가 그것을 보게 만든다. 풀어 되돌릴 것도 없고, 찾아볼 전역도 없다.
 */

/* 돌려줄 값이 없는 실패 가능 연산은 proven_err_t 를 돌려준다. */
static proven_err_t write_greeting(proven_u8str_t *out) {
    return proven_u8str_append(out, PROVEN_LIT("hello"));
}

/* 돌려줄 값이 *있으면* 그것을 지키는 오류와 함께 싸여 온다. 값은 `err` 를 확인한
 * 뒤에야 뜻을 갖는다. */
static proven_result_size_t half(proven_size_t n) {
    proven_result_size_t res = {0};
    if (n % 2 != 0) {
        res.err = PROVEN_ERR_INVALID_ARG;   /* res.value 는 0 그대로 둔다: 아무 뜻도 없다 */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = n / 2;
    return res;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 맨 proven_err_t 를 확인하기 ------------------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "creating a 32-byte string should succeed");

    proven_err_t err = write_greeting(&s.value);
    if (!proven_is_ok(err)) {
        /* 아무것도 붙지 않았고 문자열은 여전히 온전하다. proven 의 늘리는 연산은
         * 실패 원자적이다. */
        proven_u8str_destroy(alloc, &s.value);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("hello")),
                    "the greeting should have been appended");

    /* --- result 구조체를 확인하기 ---------------------------------------- */
    proven_result_size_t ok = half(10);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "10 is even, so halving it must succeed");
    EXAMPLE_REQUIRE(ok.value == 5, "half of 10 is 5");

    proven_result_size_t bad = half(7);
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "7 is odd, so halving it must fail");
    /* bad.value 는 읽으면 안 된다. 여기서는 0 이지만 그것은 이 함수의 구현 사정일
     * 뿐, result 타입의 약속이 아니다. */

    /* --- 오류를 실수로 흘릴 수는 없다 ------------------------------------ */
    /* proven_u8str_append 에는 [[nodiscard]] 가 붙어 있어 이렇게 쓰면 컴파일 오류다.
     *
     *     proven_u8str_append(&s.value, PROVEN_LIT("!"));
     *
     * 실패를 정말로 무시하겠다면, 무시한다고 적어야 한다. */
    (void)proven_u8str_append(&s.value, PROVEN_LIT("!"));

    printf("greeting: %s\n", proven_u8str_as_cstr(&s.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
