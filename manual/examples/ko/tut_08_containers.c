#include "example.h"

/*
 * 8과 - 컨테이너는 자기 할당자를 기억한다.
 *
 * 문자열은 할당자로 만들고 같은 할당자로 없앤다. 두 번 다 여러분이 건넨다.
 * 자라는 배열은 push 가 저장 공간을 넘칠 때마다 나중에 다시 할당해야 하므로,
 * 만들 때 받은 할당자를 간직한다. 새로 나오는 것은 그 하나다. 할당자는 만들 때
 * 한 번 건네고, 그 뒤의 push, get, destroy 는 배열 말고는 아무것도 받지 않는다.
 *
 * PROVEN_ARRAY_* 매크로는 원소 타입을 인자로 받으므로 그것을 검사할 수 있다.
 * int 배열에 double 을 push 하면 컴파일되지 않는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 2 는 처음 용량이지 한도가 아니다. */
    proven_result_array_t made = PROVEN_ARRAY_INIT(alloc, int, 2);
    EXAMPLE_REQUIRE(proven_is_ok(made.err), "creating an empty array must succeed");
    if (!proven_is_ok(made.err)) return 1;
    proven_array_t squares = made.value;

    /* 둘 들어갈 자리에 열 번 push 한다. 들어가지 않는 push 마다 배열이 기억한
     * 할당자로 저장 공간을 늘린다. 여기에는 할당자가 없다. */
    for (int i = 1; i <= 10; ++i) {
        proven_err_t err = PROVEN_ARRAY_PUSH(&squares, int, i * i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the heap can grow a ten-int array");
        if (!proven_is_ok(err)) {
            PROVEN_ARRAY_DESTROY(&squares);
            return 1;
        }
    }
    EXAMPLE_REQUIRE(squares.len == 10, "ten pushes, ten elements");

    /* get 은 원소를 가리키는 포인터를 돌려주고, 인덱스가 끝을 넘으면 NULL 을
     * 돌려준다. 범위를 벗어난 인덱스는 엉뚱한 곳 읽기가 아니라 검사할 수 있는 답이다. */
    const int *third = PROVEN_ARRAY_GET(&squares, int, 2);
    EXAMPLE_REQUIRE(third && *third == 9, "element 2 is 3 * 3");
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&squares, int, 10) == NULL, "index 10 is past the end");

    int sum = 0;
    for (proven_size_t i = 0; i < squares.len; ++i) {
        sum += *PROVEN_ARRAY_GET(&squares, int, i);
    }
    EXAMPLE_REQUIRE(sum == 385, "1 + 4 + 9 + ... + 100");
    proven_println("{} squares, sum {}", PROVEN_ARG(squares.len), PROVEN_ARG(sum));

    /* destroy 는 배열만 받는다. 간직해 둔 할당자로 풀어 준다. */
    PROVEN_ARRAY_DESTROY(&squares);
    return EXAMPLE_OK();
}
