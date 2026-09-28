#include "example.h"
#include <string.h>

/*
 * 잘못된 할당자를, 실수가 일어난 자리에서 잡기.
 *
 * 이 라이브러리의 어떤 것도 블록이 어느 할당자에서 왔는지 기억하지 않는다: 호출자가
 * 만들 때 하나, 파괴할 때 하나를 넘기고, 둘이 다르면 할당자의 장부가 망가져 프로그램은
 * 나중에 엉뚱한 곳에서 깨진다. alloc_check 래퍼는 어떤 할당자 앞에든 놓여, 자기가 내준
 * 블록을 기록하고, 그 밖의 것은 - panic 으로, 그 호출에서 - 거부한다.
 *
 * 테스트와 버그 추적용이다. 당신의 코드에서는 proven_alloc_checked() 를 쓰고
 * -DPROVEN_ALLOC_CHECK 로 켜고 끈다: 매크로가 없으면 받은 할당자를 그대로 돌려주어 비용이
 * 없다. (첫 proven #include 뒤의 #define 은 늦다 - 스위치는 헤더와 함께 읽힌다.) 이 예제는
 * 무조건 검사하는 proven_alloc_check_wrap() 을 불러, 빌드 플래그와 상관없이 검사를 보여 준다.
 */

/* 돌아오는 panic 핸들러 - 이 예제가 거부를 보여 주고 계속 가도록. 실제 프로그램은 실수에서
 * 멈추는 기본 핸들러를 그대로 둔다. */
static int g_refused;
static void note_panic(const char *msg) {
    ++g_refused;
    printf("refused: %s\n", msg);
}

int main(void) {
    proven_set_panic_handler(note_panic);

    /* 할당자 둘, 각자 블록 16개를 기록할 자리가 있는 검사기 뒤에. */
    static proven_byte_t arena_mem[2048];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ arena_mem, sizeof arena_mem });
    proven_alloc_check_t arena_chk, heap_chk;
    proven_alloc_check_entry_t arena_rec[16], heap_rec[16];
    proven_allocator_t scratch = proven_alloc_check_wrap(&arena_chk, proven_arena_as_allocator(&arena), arena_rec, 16);
    proven_allocator_t heap = proven_alloc_check_wrap(&heap_chk, proven_heap_allocator(), heap_rec, 16);

    /* --- 올바른 사용은 그대로 통과한다 --------------------------------------- */

    proven_result_u8str_t name = proven_u8str_create_from_view(heap, PROVEN_LIT("report"));
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "a string from the checked heap");
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_append_grow(heap, &name.value, PROVEN_LIT(".txt"))), "grown by the same allocator");
    EXAMPLE_REQUIRE(proven_alloc_check_owns(&heap_chk, name.value.internal.ptr), "the heap checker owns its block");

    /* --- 실수를 그 호출에서 잡는다 ------------------------------------------ */

    /* 임시 작업용 아레나에서 만든 임시 문자열을... */
    proven_result_u8str_t tmp = proven_u8str_create(scratch, 64);
    EXAMPLE_REQUIRE(proven_is_ok(tmp.err), "a temporary in the arena");
    proven_u8str_t t = tmp.value;
    /* ...힙으로 파괴한다. 검사가 없으면 힙이 아레나 메모리를 해제할 것이다. */
    proven_u8str_destroy(heap, &t);
    EXAMPLE_REQUIRE(g_refused == 1 && heap_chk.faults == 1, "the heap refuses a block it never gave out");
    EXAMPLE_REQUIRE(proven_alloc_check_live(&arena_chk) == 1, "and the arena block is untouched");
    t = tmp.value;
    proven_u8str_destroy(scratch, &t);   /* 올바른 쪽 */

    /* --- 테스트 끝의 누수 검사 --------------------------------------------- */

    proven_u8str_destroy(heap, &name.value);
    EXAMPLE_REQUIRE(proven_alloc_check_live(&heap_chk) == 0 && proven_alloc_check_live(&arena_chk) == 0,
                    "nothing is left live");
    printf("peak %zu live block(s) on the heap, %zu refusal(s)\n", (size_t)heap_chk.peak_live, (size_t)heap_chk.faults);

    proven_set_panic_handler(NULL);
    return EXAMPLE_OK();
}
