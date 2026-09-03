#include "example.h"

/*
 * 2과 - 자기 길이를 아는 텍스트.
 *
 * 여러분이 아는 C 에서 문자열은 포인터 하나이고, 길이는 "처음 0 바이트가 나오는
 * 자리"다. 함수마다 얼마나 되는지 알아내려고 바이트를 훑어야 하고, 0 이 없으면
 * 끝을 넘어 훑는다.
 *
 * 뷰(view)는 C 가 암묵으로 남겨 둔 그 짝이다. 포인터와 크기가 함께 다닌다.
 * 빌려 쓸 뿐이라 바이트를 소유하지 않고 해제하지도 않는다.
 */

int main(void) {
    /* PROVEN_LIT 은 리터럴에서 뷰를 만든다. 크기는 컴파일 때 정해지므로 여기서
     * 훑는 일은 없다 - strlen 과 다른 점이다. */
    proven_u8str_view_t hello = PROVEN_LIT("hello");

    EXAMPLE_REQUIRE(hello.size == 5, "the view already knows its own length");
    EXAMPLE_REQUIRE(hello.ptr != NULL, "and it points at the literal's bytes");

    /* 길이가 포인터와 함께 다니므로, 비교는 크기 검사에 memcmp 하나다.
     * 훑을 일도 없고 끝을 넘어갈 길도 없다. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(hello, PROVEN_LIT("hello")), "same text");
    EXAMPLE_REQUIRE(!proven_u8str_view_eq(hello, PROVEN_LIT("hell")), "shorter text differs");

    /* 뷰는 무언가의 *일부*를 복사 없이 가리킬 수 있다. 아래는 리터럴의 가운데다 -
     * 여전히 빌린 것이고, 할당은 없다. */
    proven_u8str_view_t ell = { .ptr = hello.ptr + 1, .size = 3 };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(ell, PROVEN_LIT("ell")), "a window onto the same bytes");

    proven_println("whole: {} (size {})", PROVEN_ARG(hello), PROVEN_ARG(hello.size));
    proven_println("part : {} (size {})", PROVEN_ARG(ell), PROVEN_ARG(ell.size));

    return EXAMPLE_OK();
}
