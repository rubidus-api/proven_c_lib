#include "example.h"

/*
 * 5과 - 기억을 누가 주는가는 전역이 아니라 인자다.
 *
 * malloc 은 여러분 대신 내려진 전역 결정이다. 힙 하나, 전략 하나, 그리고 호출
 * 자리에는 보이지 않는다. 여기서는 기억이 필요한 것이면 무엇이든
 * proven_allocator_t 를 받아 그것만 쓴다. 따라 나오는 둘이 요점이다.
 *
 *   - "이건 누가 할당했지?" 에 호출을 보고 언제나 답할 수 있다.
 *   - 같은 코드에 다른 할당자를 건네도 코드는 그대로다.
 */

/* 이 함수는 기억이 어디서 오는지 알지도, 신경 쓰지도 않는다. */
static proven_result_u8str_t make_greeting(proven_allocator_t alloc,
                                           proven_u8str_view_t name) {
    proven_result_u8str_t out = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(out.err)) return out;

    proven_err_t err = proven_u8str_append(&out.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&out.value, name);
    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &out.value);   /* 만든 것을 되돌린다 */
        out.err = err;
        out.value = (proven_u8str_t){0};
    }
    return out;
}

int main(void) {
    /* (a) 보통의 힙 - 밑에서 malloc 과 free 가 돈다 */
    proven_allocator_t heap = proven_heap_allocator();

    proven_result_u8str_t a = make_greeting(heap, PROVEN_LIT("world"));
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "the heap should be able to give 64 bytes");
    proven_println("from the heap : {}", PROVEN_ARG(proven_u8str_as_view(&a.value)));
    /* 만든 것과 *같은* 할당자로 지운다. 그 짝이 이 라이브러리 소유 규칙의
     * 전부다. */
    proven_u8str_destroy(heap, &a.value);

    /* (b) 아레나 - 기억 덩이 하나를 차례로 나눠 주고 한꺼번에 되돌린다.
     *     위의 make_greeting 은 조금도 바뀌지 않았다는 데 주목할 것. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[512];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t from_arena = proven_arena_as_allocator(&arena);

    proven_result_u8str_t b = make_greeting(from_arena, PROVEN_LIT("arena"));
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "the arena has room for this too");
    proven_println("from an arena : {}", PROVEN_ARG(proven_u8str_as_view(&b.value)));

    /* 여기엔 지우는 반복문이 없다. 아레나는 reset 으로 전부 되돌린다. 그 이야기는
     * 2장의 몫이고, 지금의 요점은 *부르는 쪽이 골랐다*는 것뿐이다. */
    proven_arena_reset(&arena);

    return EXAMPLE_OK();
}
