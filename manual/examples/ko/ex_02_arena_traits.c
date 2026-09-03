#include "example.h"

/*
 * 자기 기억을 소유하는 프로그램이 언젠가 반드시 하게 되는 세 가지, 그리고 그것을 하는
 * 호출들.
 *
 *   - 시작할 때 할당하기. 이때 기억이 바닥나는 것은 프로그램이 이어서 갈 수 있는
 *     상황이 아니다. `_or_panic` 계열이 그것을 위한 것이고, 패닉 처리기를 다는 것이
 *     "이어서 갈 수 없다" 가 이 프로그램에서 무슨 뜻인지 정하는 방법이다 - 덫에
 *     걸리는 대신 로그 한 줄과 종료 같은 것으로.
 *
 *   - 마지막에 할당한 블록을, 복사 없이 늘리기. 아레나는 그것을 제자리에서 할 수
 *     있다. 마지막에 할당된 블록이 곧 쓰인 구역의 끝에 앉아 있는 그 블록이기 때문이다.
 *
 *   - 할당자를 계측하기 - 할당 횟수를 세거나, 시험에서 열 번째를 일부러 실패시키거나 -
 *     재는 대상 코드는 건드리지 않고서. 여기서 할당자는 함수 포인터 셋과 문맥 포인터
 *     하나이므로, 감싸는 일은 전달 함수 셋을 쓰는 일이다. 아레나 자신의 셋이 공개되어
 *     있는 이유가 바로 이것이다 - 여러분의 껍데기가 그것으로 전달한다.
 */

/* --- 패닉 처리기는 무엇을 위한 것인가 ------------------------------------ */

static int g_panics = 0;
static char g_last_panic[128];

/* 패닉 처리기는 메시지를 받아 프로그램의 운명을 정한다. 기본 처리기는 즉시 덫에
 * 걸리는데, 그것이 운영에서는 옳고 시험에서는 쓸모없다 - 그래서 이 처리기는 메시지를
 * 적어 두고 돌아온다. 돌아오는 것은 패닉 경로를 *일부러 시험할 때만* 허용되고, 그때
 * 패닉한 호출이 돌려준 기억 블록은 써서는 안 된다. */
static void record_panic(const char *msg) {
    ++g_panics;
    snprintf(g_last_panic, sizeof g_last_panic, "%s", msg);
}

/* --- 자기를 지나가는 것을 세는 할당자 ------------------------------------- */

typedef struct {
    proven_arena_t *arena;
    proven_size_t   live_bytes;
    proven_size_t   alloc_calls;
    proven_size_t   free_calls;
} counting_ctx_t;

/* 셋 각각이 proven_allocator_t 의 필드 하나에 대응한다. 각자 제 장부를 적은 뒤 아레나의
 * 공개된 특성 함수로 전달하므로, 재는 대상이 되는 동작은 아레나를 다시 구현한 것이
 * 아니라 정확히 아레나의 동작이다. */
static proven_result_mem_mut_t counting_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    proven_result_mem_mut_t r = proven_arena_alloc_trait(c->arena, size, align);
    if (proven_is_ok(r.err)) {
        c->live_bytes += size;
        ++c->alloc_calls;
    }
    return r;
}

static proven_result_mem_mut_t counting_realloc(void *ctx, void *old_ptr, proven_size_t old_size,
                                               proven_size_t new_size, proven_size_t align) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    proven_result_mem_mut_t r = proven_arena_realloc_trait(c->arena, old_ptr, old_size, new_size, align);
    if (proven_is_ok(r.err)) {
        c->live_bytes = c->live_bytes - old_size + new_size;
    }
    return r;
}

static void counting_free(void *ctx, void *ptr) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    ++c->free_calls;
    proven_arena_free_trait(c->arena, ptr);   /* 아레나의 free 는 아무 일도 하지 않는다. 세는 것이 요점이다 */
}

int main(void) {
    alignas(max_align_t) static proven_byte_t storage[1024];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ .ptr = storage, .size = sizeof storage });

    /* --- 1. 실패하면 안 되는 시작 시점 할당 -------------------------------- */

    proven_set_panic_handler(record_panic);

    /* 풀어 볼 result 가 없다. 이들은 블록을 곧바로 돌려준다. 부르는 쪽이 손쓸 수 있는
     * 오류가 없기 때문이다. 차이는 그것이 전부다. */
    proven_mem_mut_t table = proven_arena_alloc_or_panic(&arena, 256);
    EXAMPLE_REQUIRE(table.ptr != NULL, "a 256-byte start-up allocation must succeed");
    EXAMPLE_REQUIRE(g_panics == 0, "a successful allocation must not panic");

    /* 기본 경계보다 큰 정렬이 필요한 타입을 위한 정렬 지정 꼴 - 여기서는 64바이트
     * 캐시 줄이고, 그것이 흔한 이유다. */
    proven_mem_mut_t cache_line = proven_arena_alloc_aligned_or_panic(&arena, 64, 64);
    EXAMPLE_REQUIRE(((proven_uintptr_t)cache_line.ptr % 64) == 0,
                    "the block must start on the boundary that was asked for");
    EXAMPLE_REQUIRE(g_panics == 0, "an over-aligned allocation that fits must not panic either");

    /* 이제 일부러 실패하는 경우다. 아레나가 결코 담을 수 없는 크기를 청한다. 기록하는
     * 처리기를 달아 두었으니 그것을 지켜볼 수 있다. 기본 처리기였다면 프로그램은 여기서
     * 멈췄을 것이고, 그것이 기본 처리기의 목적이다. */
    proven_mem_mut_t impossible = proven_arena_alloc_or_panic(&arena, sizeof storage * 2);
    (void)impossible;   /* 처리기가 돌아온 뒤, 이 블록은 아무 뜻도 없다 */
    EXAMPLE_REQUIRE(g_panics == 1, "exhausting the arena through _or_panic must panic");
    EXAMPLE_REQUIRE(g_last_panic[0] != '\0', "the handler receives a message naming the call");
    printf("panic handler saw: %s\n", g_last_panic);

    /* NULL 을 건네면 덫에 거는 기본 처리기가 되돌아온다. 시험용 처리기를 그대로 두면
     * 진짜 실패가 조용한 오염으로 바뀐다. */
    proven_set_panic_handler(NULL);

    /* --- 2. 가장 최근 블록을 제자리에서 늘리기 ----------------------------- */

    /* 머리말을 읽고 나서 본문이 짐작보다 길다는 것을 알게 된 파서는, 방금 받은 버퍼를
     * 복사하는 것이 아니라 늘리고 싶어 한다. */
    proven_result_mem_mut_t buf = proven_arena_alloc_aligned(&arena, 32, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(buf.err), "the initial 32-byte buffer must fit");

    proven_size_t before = arena.offset;
    proven_result_mem_mut_t grown = proven_arena_realloc_aligned(&arena, buf.value.ptr, 32, 96, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(grown.err), "growing the most recent block must succeed");
    EXAMPLE_REQUIRE(grown.value.ptr == buf.value.ptr,
                    "the most recent block grows in place: same address, no copy");
    EXAMPLE_REQUIRE(arena.offset == before + 64, "only the extra 64 bytes were taken");

    /* 제자리 경로는 *마지막에* 할당된 블록에만 열려 있다. 다른 블록을 하나 더 받고 나면
     * 앞의 것은 있던 자리에서 더 늘릴 수 없다 - 아레나는 대신 그것을 끝으로 복사하고,
     * 옛 바이트는 다음 reset 까지 죽은 자리가 된다. 어느 쪽이든 옳다. 다만 공짜가 아닐
     * 뿐이다. */
    proven_result_mem_mut_t other = proven_arena_alloc(&arena, 16);
    EXAMPLE_REQUIRE(proven_is_ok(other.err), "a second block must fit");
    proven_result_mem_mut_t moved = proven_arena_realloc_aligned(&arena, grown.value.ptr, 96, 128, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(moved.err), "growing an older block must still succeed");
    EXAMPLE_REQUIRE(moved.value.ptr != grown.value.ptr, "but it is relocated, not extended");

    /* --- 3. 세는 껍데기 뒤의 아레나 --------------------------------------- */

    proven_arena_reset(&arena);

    counting_ctx_t counted = { .arena = &arena };
    proven_allocator_t alloc = {
        .ctx        = &counted,
        .alloc_fn   = counting_alloc,
        .realloc_fn = counting_realloc,
        .free_fn    = counting_free,
    };
    EXAMPLE_REQUIRE(proven_alloc_is_valid(alloc), "all three function pointers must be present");

    /* 이제 할당자를 받는 라이브러리의 어느 부분이든 그 껍데기를 지나 돈다. 그것을 알지도
     * 못한 채, 바뀐 것도 없이. */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "creating a string through the wrapper must succeed");

    proven_err_t err = proven_u8str_append_grow(alloc, &s.value, PROVEN_LIT("a line long enough to need more room"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending past the initial capacity must succeed");

    proven_u8str_destroy(alloc, &s.value);

    EXAMPLE_REQUIRE(counted.alloc_calls >= 1, "the wrapper saw the string being created");
    EXAMPLE_REQUIRE(counted.free_calls >= 1, "and saw it being destroyed");
    printf("counting allocator: %zu alloc call(s), %zu free call(s), %zu bytes handed out\n",
           (size_t)counted.alloc_calls, (size_t)counted.free_calls, (size_t)counted.live_bytes);

    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
