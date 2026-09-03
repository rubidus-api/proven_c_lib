#include "example.h"

/*
 * 3과 - 실패할 수 있는 호출은 반환값으로 그렇다고 말한다.
 *
 * 여러분이 아는 C 는 실패를 세 가지 방식으로 알린다. 특별한 반환값(-1), 널
 * 포인터, 그리고 다음 호출이 덮어써 버리는 errno 라는 전역. 셋 다 안 보기 쉽고,
 * 안 봐도 아무도 뭐라 하지 않는다.
 *
 * 여기서는 실패할 수 있는 호출이 proven_err_t 를 돌려준다. 그냥 값이라서 담아
 * 둘 수도, 비교할 수도, 위로 넘길 수도 있다. 그리고 이 함수들에 붙은
 * [[nodiscard]] 덕분에 무시하는 것은 습관이 아니라 컴파일러 경고가 된다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 딱 8 바이트짜리 자리. (그 자리가 어디서 오는지는 2장의 주제다. 지금은
     * 우리가 "얼마나"를 말했다는 것만 보면 된다.) */
    proven_result_u8str_t s = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "8 bytes should be available");

    /* 이건 들어간다. */
    proven_err_t err = proven_u8str_append(&s.value, PROVEN_LIT("12345678"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "eight bytes into eight bytes fits exactly");

    /* 이건 안 들어간다 - 그리고 라이브러리는 *거부*한다. 들어갈 만큼만 붙이지
     * 않는다. 잘린 낱말은 짧아진 낱말이 아니라 다른 낱말이기 때문이다. */
    proven_err_t too_much = proven_u8str_append(&s.value, PROVEN_LIT("9"));
    EXAMPLE_REQUIRE(!proven_is_ok(too_much), "one byte more than capacity must fail");
    EXAMPLE_REQUIRE(too_much == PROVEN_ERR_OUT_OF_BOUNDS, "and it says why: out of bounds");

    /* 거부는 아무것도 바꾸지 않았다. 문자열은 그대로다. 이것이 "실패 원자성"의
     * 뜻이다 - 실패한 호출은 반쯤 된 상태를 남기지 않는다. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("12345678")),
                    "the refused append must not have written anything");

    proven_println("after the refusal, the string is still: {}",
                   PROVEN_ARG(proven_u8str_as_view(&s.value)));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
