#include "example.h"

/*
 * 7과 - 한꺼번에 풀려나는 기억.
 *
 * 5과는 make_greeting 에 아레나를 건넨 뒤 reset 했지만, 그 reset 이 무엇인지는
 * 말하지 않았다. 이번 과는 그 호출 하나에 대한 것이다.
 *
 * 아레나는 여러분이 가진 기억 덩어리 위에서 포인터를 앞으로 밀어 가며 나눠 준다.
 * 객체 하나하나를 푸는 free 는 없다. 아레나에 destroy 를 불러도 돌아오는 것은
 * 없다. 모든 것이 proven_arena_reset 한 번에 한꺼번에 돌아온다. 제약처럼 들리지만
 * 그것이 아레나를 쓰는 이유다. 일감마다 임시 글을 만드는 루프는 문자열을 몇 개를
 * 만들었든 문장 하나로 전부 버릴 수 있다.
 */

int main(void) {
    /* 이 배열의 주인은 아레나가 아니라 우리다. 아레나는 조각을 나눠 줄 뿐이다. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[256];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t scratch = proven_arena_as_allocator(&arena);

    static const proven_u8str_view_t names[] = {
        PROVEN_LIT("ada"), PROVEN_LIT("grace"), PROVEN_LIT("barbara"),
    };

    for (proven_size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        /* 한 바퀴에 문자열 둘, 둘 다 아레나에서 나온다. */
        proven_result_u8str_t line = proven_u8str_create(scratch, 32);
        proven_result_u8str_t note = proven_u8str_create(scratch, 32);
        EXAMPLE_REQUIRE(proven_is_ok(line.err) && proven_is_ok(note.err),
                        "each round fits easily in 256 bytes");
        if (!proven_is_ok(line.err) || !proven_is_ok(note.err)) return 1;

        proven_err_t err = proven_u8str_append(&line.value, PROVEN_LIT("hello, "));
        if (proven_is_ok(err)) err = proven_u8str_append(&line.value, names[i]);
        if (proven_is_ok(err)) err = proven_u8str_append(&note.value, PROVEN_LIT("(scratch)"));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the appends fit their capacity");

        /* 바퀴마다 첫 할당은 `backing` 의 맨 앞에 떨어진다. 앞 바퀴 끝의
         * reset 이 전부 돌려주었기 때문이다. */
        EXAMPLE_REQUIRE((const void *)proven_u8str_as_view(&line.value).ptr == (const void *)backing,
                        "each round starts again at the beginning of the block");

        proven_println("round {}: {} {} ({} bytes in use)",
                       PROVEN_ARG(i), PROVEN_ARG(proven_u8str_as_view(&line.value)),
                       PROVEN_ARG(proven_u8str_as_view(&note.value)),
                       PROVEN_ARG(arena.offset));

        /* 문장 하나로 두 문자열이 사라진다. destroy 루프도, 잊을 것도 없다.
         * 여기서부터 `line` 과 `note` 는 회수된 기억을 가리킨다. */
        proven_arena_reset(&arena);
        EXAMPLE_REQUIRE(arena.offset == 0, "reset reclaims everything the round allocated");
    }

    /* 아레나는 자라지 않는다. 덩어리보다 많이 달라고 하면 3과에서 본 것과 같은
     * 오류 값이 온다. 죽지도 않고, 몰래 malloc 으로 넘어가지도 않는다. */
    proven_result_u8str_t too_big = proven_u8str_create(scratch, 1024);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "a 256-byte arena refuses 1 KiB");

    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
