#include "example.h"

/*
 * 4과 - 돌려줄 값이 있는 호출은, 값과 오류가 함께 온다.
 *
 * 돌려줄 것이 없을 때는 proven_err_t 하나로 충분하다. 돌려줄 것이 *있으면*
 * 필드 둘짜리 작은 구조체를 받는다. `err` 와 `value` 다. 규칙은 한 문장이다.
 * `err` 를 보기 전에는 `value` 는 아무 뜻도 없다.
 *
 * malloc 이 NULL 인지 확인하던 것과 같은 규율인데, 확인해야 한다는 사실이
 * 기억해야 할 관습이 아니라 타입의 일부라는 점이 다르다.
 */

/* 여러분의 함수도 이것을 돌려줄 수 있다. 라이브러리에 마법은 없다. */
static proven_result_size_t safe_div(proven_size_t a, proven_size_t b) {
    proven_result_size_t res = {0};
    if (b == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;    /* value 는 0 그대로 - 아무 뜻도 없다 */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = a / b;
    return res;
}

int main(void) {
    proven_result_size_t ok = safe_div(10, 2);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "dividing by 2 is fine");
    EXAMPLE_REQUIRE(ok.value == 5, "and only now may we read the value");

    proven_result_size_t bad = safe_div(10, 0);
    EXAMPLE_REQUIRE(!proven_is_ok(bad.err), "dividing by zero must fail");
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "and say which rule was broken");
    /* bad.value 는 0 이지만 그것은 답이 아니다. 답이 없다는 뜻이다. */

    /* 라이브러리 자신의 호출도 같은 모양이다. proven_u8str_create 는 만든
     * 문자열과 그것을 지키는 오류를 함께 돌려준다. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    if (!proven_is_ok(s.err)) return 1;      /* 만들어진 것이 없으니 지울 것도 없다 */

    proven_println("10 / 2 = {}", PROVEN_ARG(ok.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
