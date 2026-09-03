#include "example.h"

/*
 * 6과 - 0장의 프로그램을 한 줄씩 읽는다.
 *
 * 새로 나오는 것은 없다. 매뉴얼이 첫 쪽에 싣는 그 인사말 프로그램 그대로이고,
 * 이 과의 요점은 이제 그 모든 부분에 이름을 댈 수 있다는 것이다. 건네받는
 * 할당자(5과), 반드시 확인해야 하는 result(4과), 거부가 돌려주는 오류(3과),
 * 자기 길이를 들고 다니는 뷰(2과), 그리고 인자를 검사하는 출력(1과).
 *
 * 이것이 이제 평범하게 읽힌다면 이 실습은 할 일을 다한 것이고, 레퍼런스 장들이
 * 여러분에게 열린 것이다.
 */

int main(void) {
    /* 5과 - 기억이 어디서 오는지는 부르는 쪽이 정한다 */
    proven_allocator_t alloc = proven_heap_allocator();

    /* 4과 - 문자열과 그것을 지키는 오류가 함께 온다 */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* 2과 - 자기 크기를 아는, 빌려 쓰는 텍스트 */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* 3과 - 각 append 는 들어가거나 거부한다. 자르는 것은 하나도 없다 */
    proven_err_t err = proven_u8str_append(&greeting.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, name);
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, PROVEN_LIT("!"));

    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &greeting.value);
        return 1;
    }

    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&greeting.value),
                                         PROVEN_LIT("hello, world!")),
                    "the three appends should have built the whole greeting");

    /* 1과 - 서식 문자열과 인자는 어긋날 수 없다 */
    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* 다시 5과 - 만든 할당자로 지운다 */
    proven_u8str_destroy(alloc, &greeting.value);
    return EXAMPLE_OK();
}
