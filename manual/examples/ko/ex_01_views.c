#include "example.h"
#include <string.h>

/*
 * 소유한 버퍼 하나를, 세 가지 방식으로 이야기하기.
 *
 * 이 프로그램은 고정 크기 레코드로 된 작은 표를 자기가 소유한 기억 덩이 하나에 담고,
 * 그런 프로그램이면 반드시 하게 되는 네 가지를 한다.
 *
 *   1. 고치면 안 되는 읽는 쪽에 표를 건네고(읽기 전용 뷰),
 *   2. 한 행만 고쳐도 되는 쪽에 그 행을 건네고(슬라이스),
 *   3. 뒤 행들을 앞으로 덮어 옮겨 한 행을 지우고(겹치는 복사 - 보통의 복사에는
 *      허용되지 않는 일이다),
 *   4. 남이 돌려준 포인터를 받아, 행 번호로 쓰기 전에 애초에 이 표를 가리키기는
 *      하는지 가린다.
 *
 * 네 걸음 모두 맨 `char *` 가 길이를 잃어버리고 프로그램이 나중에야 그것을 알게 되는
 * 자리다. 여기 쓰인 타입들은 길이를 포인터와 함께 들고 다니므로, 경계에 대한 물음이
 * 실수가 벌어질 바로 그 자리에서 답해진다.
 */

#define ROW_SIZE  8u
#define ROW_COUNT 4u

/* 읽는 쪽은 뷰를 받는다. 모든 바이트를 읽을 수 있고 하나도 쓸 수 없다. 타입에 붙은
 * const 는 권고가 아니다 - 그것을 통해 대입하면 컴파일되지 않는다. */
static proven_size_t count_nonzero(proven_mem_view_t table) {
    proven_size_t n = 0;
    for (proven_size_t i = 0; i < table.size; ++i) {
        if (table.ptr[i] != 0) {
            ++n;
        }
    }
    return n;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 표를 할당한다. 할당자는 읽고 쓸 수 있는 슬라이스를 돌려준다. 소유한 쪽이 쥐는
     * 것이 그것이다 - 포인터와 그에 딸린 크기. */
    proven_result_mem_mut_t got = alloc.alloc_fn(alloc.ctx, ROW_SIZE * ROW_COUNT, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(got.err), "allocating the record table must succeed");
    if (!proven_is_ok(got.err)) {
        return 1;
    }

    /* proven_mem_t 가 "내가 이것을 소유한다" 는 타입이다. 소유한 덩이를 변수 하나에
     * 두고 거기서 파생한 뷰와 슬라이스를 나눠 주는 것, 그것이 이 라이브러리가 요구하는
     * 소유 규율의 전부다. */
    proven_mem_t owned = { .ptr = got.value.ptr, .size = got.value.size };
    proven_mem_mut_t table = proven_mem_mut_from_owned(owned);

    /* i 번 행을 i + 1 값의 바이트로 채운다. 그래야 옮겨진 행을 알아볼 수 있다. */
    for (proven_u32 row = 0; row < ROW_COUNT; ++row) {
        proven_result_mem_mut_t slot = proven_mem_mut_slice_checked(table, row * ROW_SIZE, ROW_SIZE);
        EXAMPLE_REQUIRE(proven_is_ok(slot.err), "every row of the table must be in bounds");
        if (!proven_is_ok(slot.err)) {
            alloc.free_fn(alloc.ctx, owned.ptr);
            return 1;
        }
        memset(slot.value.ptr, (int)(row + 1), slot.value.size);
    }

    /* 1. 읽기 전용으로 쓰기. 읽는 쪽은 표를 고칠 수도, 끝을 넘어갈 수도 없다.
     *    두 사실을 한꺼번에 건네받았기 때문이다. */
    proven_mem_view_t read_only = proven_mem_view_from_owned(owned);
    EXAMPLE_REQUIRE(count_nonzero(read_only) == ROW_SIZE * ROW_COUNT,
                    "every byte of every row was written");

    /* 2. 없는 행을 달라고 하는 것은 디버깅할 사고가 아니라 다룰 수 있는 오류다.
     *    행이 넷인 표의 4번 행은 끝에서 한 행 지난 자리에서 시작한다. */
    proven_result_mem_mut_t past_end = proven_mem_mut_slice_checked(table, ROW_COUNT * ROW_SIZE, ROW_SIZE);
    EXAMPLE_REQUIRE(past_end.err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "slicing past the end must report OUT_OF_BOUNDS, not return a bad slice");

    /* 3. 2번과 3번 행을 한 행씩 앞으로 옮겨 1번 행을 지운다. 원본과 목적지가
     *    겹치므로, 보통의 복사가 다루면 안 되는 바로 그 경우다. proven_mem_copy 는
     *    겹치지 않는 구역을 계약으로 적고 있고, 겹쳐도 받는 것은 proven_mem_move 다. */
    proven_mem_view_t tail = {
        .ptr  = table.ptr + 2 * ROW_SIZE,
        .size = 2 * ROW_SIZE
    };
    proven_err_t moved = proven_mem_move(table.ptr + 1 * ROW_SIZE, table.size - 1 * ROW_SIZE, tail);
    EXAMPLE_REQUIRE(proven_is_ok(moved), "moving the tail of the table down must succeed");
    EXAMPLE_REQUIRE(table.ptr[1 * ROW_SIZE] == 3, "row 2 moved into row 1's place");
    EXAMPLE_REQUIRE(table.ptr[2 * ROW_SIZE] == 4, "row 3 moved into row 2's place");

    /* 옮기기도 다른 모든 것처럼 경계가 있다. 원본을 담기에 목적지가 작으면 거부되고,
     * 아무것도 쓰이지 않는다. */
    proven_err_t refused = proven_mem_move(table.ptr, ROW_SIZE, tail);
    EXAMPLE_REQUIRE(refused == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a move that would not fit must be refused before it writes");

    /* 4. 다른 데서 온 포인터. 서로 무관한 포인터를 < 로 비교하는 것은 C 에서 미정의
     *    동작이고 - 컴파일러는 그런 일이 없다고 가정해도 된다 - 그래서 "이 포인터가
     *    내 버퍼를 가리키는가" 는 비교를 옳게 해 주는 함수로 물어야 한다. */
    proven_size_t offset = 0;
    const proven_byte_t *from_elsewhere = table.ptr + 2 * ROW_SIZE;
    bool inside = proven_range_contains_ptr(table.ptr, table.size, from_elsewhere, ROW_SIZE, &offset);
    EXAMPLE_REQUIRE(inside, "a pointer to row 2 is inside the table");
    EXAMPLE_REQUIRE(offset / ROW_SIZE == 2, "and its offset recovers the row index");

    proven_u8 unrelated[ROW_SIZE] = { 0 };
    EXAMPLE_REQUIRE(!proven_range_contains_ptr(table.ptr, table.size, unrelated, ROW_SIZE, NULL),
                    "a pointer into a different object is not inside the table");

    /* 안에서 시작하지만 밖에서 끝나는 행도 밖이다. 검사는 시작 자리가 아니라 구간
     * 전체에 대한 것이다. */
    EXAMPLE_REQUIRE(!proven_range_contains_ptr(table.ptr, table.size,
                                               table.ptr + table.size - 1, ROW_SIZE, NULL),
                    "a range that starts inside but runs past the end is refused");

    /* 5. 구간 검사로 그 행이 경계 안임을 증명했다면, 검사 없는 슬라이스가 옳은
     *    호출이다. 하는 일이 없고, 증명은 주석이 아니라 바로 윗줄이다. 그 증명 없이는
     *    절대 쓰지 말 것. */
    proven_mem_mut_t row2 = proven_mem_mut_slice_unchecked(table, offset, ROW_SIZE);
    EXAMPLE_REQUIRE(row2.size == ROW_SIZE && row2.ptr[0] == 4,
                    "the unchecked slice names the row the checked test just proved");

    /* 6. 정렬 - 부르는 쪽이 그것을 만나는 유일한 자리다. 정렬이 다른 두 가지를 한
     *    덩이 안에 나란히 놓는 일. 둘째 것의 주소는 올림해야 하고, 올림은 2의 거듭제곱
     *    경계에서만 정의된다 - 그래서 경계를 먼저 검사한다. */
    proven_size_t want_align = alignof(proven_u32);
    EXAMPLE_REQUIRE(proven_is_pow2(want_align), "an alignment boundary must be a power of two");
    EXAMPLE_REQUIRE(!proven_is_pow2(24u), "24 is not a power of two, so it is not a valid boundary");

    proven_uintptr_t raw     = (proven_uintptr_t)(table.ptr + 1);   /* 일부러 홀수 */
    proven_uintptr_t aligned = proven_uintptr_align_up(raw, want_align);
    EXAMPLE_REQUIRE(aligned >= raw, "aligning up never moves backwards");
    EXAMPLE_REQUIRE(aligned % want_align == 0, "and the result is on the boundary asked for");
    EXAMPLE_REQUIRE(aligned - raw < want_align, "the padding is never more than one boundary");

    /* 2의 거듭제곱이 아닌 경계는 그럴듯한 틀린 주소 대신 0 을 돌려준다. 그래서 실수가
     * 아래로 흘러가지 않고 여기서 멈춘다. */
    EXAMPLE_REQUIRE(proven_uintptr_align_up(raw, 24u) == 0,
                    "aligning to a non-power-of-two returns 0, not a wrong address");

    alloc.free_fn(alloc.ctx, owned.ptr);
    return EXAMPLE_OK();
}
