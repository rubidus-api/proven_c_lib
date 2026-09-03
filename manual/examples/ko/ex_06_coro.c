#include "example.h"

/*
 * 스택 없는 코루틴은 사실 switch 문이다. BEGIN 이 저장된 상태를 두고 switch 를 열고,
 * YIELD 마다 __LINE__ 을 재개 표지로 적고 *돌아간다*. 다음 호출은 함수를 위에서부터 다시
 * 들어와 그 표지로 곧장 뛴다.
 *
 * 뒤따르는 모든 것이 그 사실 하나에서 나온다.
 *
 *   - 지역 변수는 yield 를 넘겨 살아남지 *못한다*. 함수가 돌아갔고 그 스택 프레임은
 *     사라졌다. 남아 있어야 할 것은 코루틴 자신의 구조체에 산다 - 아래의 `value` 와
 *     `remaining` 이 하는 일이 그것이다.
 *   - 코루틴 매크로 둘이 같은 소스 줄에 있으면 안 된다(__LINE__ 이 부딪친다).
 *   - 자기가 부른 도우미 안에서 yield 할 수 없다. 멈춰 둘 스택이 없기 때문이다.
 *
 * 그 대가로, 멈춰 있는 코루틴은 정확히 자기 구조체만큼의 값이 든다 - 상태 4바이트에
 * 여러분이 곁에 둔 것 - 그리고 스레드도, 스택도, 문맥 전환도 없다.
 */

typedef struct {
    proven_coro_t coro;
    /* 생성기의 상태. 보통의 반복문이라면 `int i` 같은 지역 변수였을 것들이다. 여기서는
     * 필드여야 한다. 아니면 재개할 때마다 처음 값으로 되돌아가 반복문이 끝나지 않는다. */
    int value;
    int remaining;
} squares_t;

/* 코루틴은 proven_i32 를 돌려준다. 0 = 멈췄음(다시 불러 달라), 1 = 끝. */
static proven_i32 squares_next(squares_t *g) {
    PROVEN_CORO_BEGIN(&g->coro);

    g->remaining = 5;
    g->value = 1;

    while (g->remaining > 0) {
        g->value = g->value * g->value;
        PROVEN_CORO_YIELD(&g->coro);      /* 부르는 쪽이 여기서 g->value 를 읽는다 */
        g->value = g->value + 1;          /* 정확히 이 줄에서 재개한다 */
        g->remaining -= 1;
    }

    PROVEN_CORO_END(&g->coro);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 코루틴은 기억을 소유하지 않으므로 지울 것이 없다 - 다만 그것이 내놓는 값들은
     * 어딘가로 가야 하고, 그 문자열에는 소유자가 있다. */
    proven_result_u8str_t out = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "creating the output string should succeed");
    if (!proven_is_ok(out.err)) return 1;

    squares_t gen = {0};
    PROVEN_CORO_INIT(&gen.coro);   /* 조건 없이, 첫 호출 전에, 정확히 한 번 */

    int produced = 0;
    int last = 0;

    /* 끝까지 굴린다. squares_next 는 본문의 끝을 지나 달리는 호출에서 1 을 돌려준다 -
     * 그 호출은 값을 내놓지 않으므로, 반복문 본문은 0 을 돌려준 동안에만 돈다. */
    while (!squares_next(&gen)) {
        proven_fmt_result_t r = proven_u8str_append_fmt_grow(alloc, &out.value, "{} ",
                                                             PROVEN_ARG(gen.value));
        EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "appending a generated value should succeed");
        last = gen.value;
        ++produced;
    }

    /* 끝난 상태는 들러붙는다. 상태는 -1 이고 그대로 있다. 다시 불러도 본문을 다시 돌리지
     * 않고 1 만 돌려준다. */
    EXAMPLE_REQUIRE(PROVEN_CORO_IS_DONE(&gen.coro), "the generator should have finished");
    EXAMPLE_REQUIRE(squares_next(&gen) == 1, "a finished coroutine stays finished");

    /* 1, 그다음 (1+1)^2 = 4, 그다음 (4+1)^2 = 25, 그다음 676, 그다음 458329. */
    EXAMPLE_REQUIRE(produced == 5, "the generator yields once per iteration");
    EXAMPLE_REQUIRE(last == 458329, "the state carried across every yield");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out.value),
                                         PROVEN_LIT("1 4 25 676 458329 ")),
                    "the generated sequence should be exactly this");

    printf("squares: %s\n", proven_u8str_as_cstr(&out.value));

    proven_u8str_destroy(alloc, &out.value);
    return EXAMPLE_OK();
}
