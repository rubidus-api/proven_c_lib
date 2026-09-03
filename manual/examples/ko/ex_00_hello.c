#include "example.h"

/*
 * 첫 프로그램. 일부러 작게 만들었고, 여기 있는 줄 하나하나가 이 매뉴얼의 모든
 * 쪽에서 만나게 될 다섯 계약 가운데 하나다.
 *
 * 여러분이 이미 아는 C 와 견주어 보라.
 *
 *     char buf[64];
 *     strcpy(buf, name);          <- name 은 얼마나 긴가? strcpy 는 묻지 않는다.
 *     strcat(buf, ", welcome!");  <- 이제 남은 자리는? strcat 도 묻지 않는다.
 *     printf("%s\n", buf);
 *
 * 저 프로그램은 `name` 이 여러분의 짐작보다 길어지는 날까지만 옳고, 그날부터는
 * 보안 권고문이 된다. 아래 판은 그럴 수 없다. 모든 쓰기가 목적지의 크기를 알고,
 * 실패할 수 있는 모든 연산이 조용히 무시할 수 없는 오류를 돌려준다.
 */

int main(void) {
    /* (1) 할당자를 여러분이 건넨다. 라이브러리가 등 뒤에서 전역 malloc 에 손을
     *     뻗는 일이 없으므로, 무엇을 누가 할당했는지 언제나 알 수 있다. */
    proven_allocator_t alloc = proven_heap_allocator();

    /* (2) 실패할 수 있는 것은 오류를 값과 *함께* 돌려준다. 확인해야 한다고 기억할
     *     errno 같은 것이 없고, `greeting.err` 를 보기 전에는 `greeting.value` 는
     *     아무 뜻도 없다. */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* (3) 뷰는 자기 길이를 아는, 빌려 쓰는 텍스트다. PROVEN_LIT 은 리터럴에서
     *     컴파일 때 그것을 만든다 - 여기서 strlen 처럼 훑는 일은 없다. */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* (4) append 는 자르는 대신 거부한다. "hello, " 와 이름이 위에서 요청한 64
     *     바이트에 들어가지 못하면 PROVEN_ERR_OUT_OF_BOUNDS 를 돌려주고 아무것도
     *     쓰지 않는다 - 낱말의 반쪽을 조용히 담아 두고 넘어가는 일이 없다. */
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

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* (5) `alloc` 으로 만들었으니 *같은* `alloc` 으로 지운다. 소유하는 것은 정확히
     *     한 번 지우고, 빌린 것은 - 위의 `name` 같은 - 아예 지우지 않는다. */
    proven_u8str_destroy(alloc, &greeting.value);

    return EXAMPLE_OK();
}
