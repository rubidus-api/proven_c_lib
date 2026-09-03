#include "example.h"

/*
 * 아레나는 기억을 소유하지 않는다. *여러분이* 가진 기억 위로 포인터를 밀고 갈 뿐이다.
 * 거래는 그것이 전부다. 할당은 덧셈 한 번이고, 낱개 해제는 아예 없으며, reset 한 번에
 * 전부 되돌려 받는다.
 *
 * 쓸 값어치가 나는 모양은 "밀고 나서 버리기"다. 한 단계 동안 마음껏 할당하고, 단계가
 * 끝나면 reset 하나로 전부 회수한다. 물건마다 장부를 적을 일이 없으니 틀릴 일도 없고,
 * 샐 것도 없다 - 아래의 뒷받침 저장소는 자동 수명을 가진 평범한 배열이다.
 */

int main(void) {
    /* 뒷받침 저장소는 부르는 쪽의 것이다. 넉넉히 정렬해 두어, 첫 바이트부터도 어떤
     * 정렬 요구든 아레나가 맞춰 줄 수 있게 한다. */
    alignas(max_align_t) static proven_byte_t storage[4096];

    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = storage,
        .size = sizeof storage,
    });

    /* --- 밀기 ------------------------------------------------------------- */
    proven_result_mem_mut_t a = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "64 bytes must fit in a 4 KiB arena");
    EXAMPLE_REQUIRE(a.value.ptr == storage, "the first allocation starts at the backing store");

    /* 타입이 PROVEN_DEFAULT_ALIGNMENT 보다 큰 정렬을 요구하면 그것을 명시한다.
     * 아레나는 거기까지 채워 넘어가므로, 건너뛴 바이트는 reset 전까지 그냥 사라진다. */
    proven_result_mem_mut_t b = proven_arena_alloc_aligned(&arena, 32, 64);
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "an over-aligned block must still fit");
    EXAMPLE_REQUIRE(((uintptr_t)b.value.ptr % 64) == 0, "the block must honour the requested alignment");

    /* --- 다른 API 에 건네는 할당자로서의 아레나 --------------------------- */
    /* proven 에서 proven_allocator_t 를 받는 것이면 무엇이든 아레나로 굴릴 수 있다.
     * 그래서 아래 문자열은 `storage` 안에 산다. */
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);
    EXAMPLE_REQUIRE(proven_alloc_is_valid(arena_alloc), "the arena must expose a usable allocator");

    proven_result_u8str_t s = proven_u8str_create(arena_alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "the arena should be able to back a 32-byte string");

    proven_err_t err = proven_u8str_append_grow(arena_alloc, &s.value, PROVEN_LIT("scratch line"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into an arena-backed string must succeed");

    /* 지우는 것은 여전히 옳고 소유 규칙이 요구하는 일이다 - 다만 아레나의 free 는
     * 아무 일도 하지 않으므로 되돌리는 것이 없다. 이것은 누수가 아니다. 바이트는
     * `storage` 의 것이고, 아래의 reset 이 그것을 돌려준다. */
    proven_u8str_destroy(arena_alloc, &s.value);

    proven_size_t used = arena.offset;
    EXAMPLE_REQUIRE(used > 64, "every allocation above came out of the same backing store");

    /* --- 버리기 ------------------------------------------------------------ */
    /* 한 문장이 64바이트 블록과 정렬된 블록과 문자열을 모두 되돌린다. reset 의 값은
     * 열 개를 할당했든 만 개를 할당했든 같다. */
    proven_arena_reset(&arena);
    EXAMPLE_REQUIRE(arena.offset == 0, "reset must reclaim every allocation at once");

    /* 저장소가 정말 다시 쓰인다는 증거다. 다음 할당이 처음 자리로 돌아온다. reset
     * 이전에 나눠 준 포인터는 이제 전부 매달린 포인터다 - reset 이 공짜인 값이다. */
    proven_result_mem_mut_t c = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(c.err), "allocation after reset must succeed");
    EXAMPLE_REQUIRE(c.value.ptr == storage, "after reset the arena bumps from the beginning again");

    /* --- 바닥나면 죽는 것이 아니라 오류다 ---------------------------------- */
    proven_result_mem_mut_t too_big = proven_arena_alloc(&arena, sizeof storage);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "an arena cannot grow: it reports NOMEM instead");

    printf("arena: %zu bytes used before reset, %zu in use now\n",
           (size_t)used, (size_t)arena.offset);

    /* 형식상의 뒷정리. 부르는 쪽이 뒷받침하는 아레나에서는 하는 일이 없지만, 나중에
     * 뒷받침 저장소가 힙이 되더라도 수명이 눈에 보이게 남는다. */
    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
