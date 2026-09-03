#include "example.h"

/*
 * 고리 버퍼는 내용을 옮기지도, 늘어나지도 않는 고정 크기 큐다. 용량을 한 번 정해 주면
 * push 는 꼬리에 넣고 pop 은 머리에서 빼며, 가득 차면 push 는 *거부한다*.
 *
 * 이것이 없으면 여러분이 쓰게 될 C 는 배열 하나에 인덱스 둘, 그리고 그것을 감아 주는
 * 나머지 연산이고, 버그는 언제나 같다 - "지금 가득 찬 것인가 빈 것인가?" 개수를 따로
 * 세거나 칸 하나를 버리지 않는 한 두 상태 모두 head == tail 이다. 이 고리는 개수를
 * 세므로 그 물음이 생기지 않는다.
 *
 * 만드는 쪽과 쓰는 쪽의 속도가 다르고 만드는 쪽이 얼마나 앞서 나갈지에 단단한 한계를
 * 두고 싶을 때 쓴다 - 이벤트 큐, 로그 고리, 오디오 버퍼. 가득 찼을 때의 거부가 바로
 * 그 기능이다 - 역압(backpressure)이다. 늘어나는 큐는 몰려드는 입력에 기억을 먹어
 * 치우는 것으로 답하다가 더 나쁜 일을 맞는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 이벤트 4개짜리 용량. 5가 되는 일은 없다. */
    proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 4-slot ring should succeed");
    proven_ring_t ring = r.value;

    /* --- 가득 찰 때까지 push ------------------------------------------- */
    for (int i = 1; i <= 4; ++i) {
        proven_err_t err = PROVEN_RING_PUSH(&ring, int, i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the first four pushes fit");
    }

    /* 다섯 번째 push 는 거부된다. 가장 오래된 항목을 덮어쓰지도, 늘어나지도 않는다.
     * 가득 찬 고리는 가득 찬 고리이고, 어떻게 할지는 부르는 쪽이 정한다 - 기다리든,
     * 새 이벤트를 버리든, 역압을 알리든. */
    proven_err_t full = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(full == PROVEN_ERR_OUT_OF_BOUNDS,
                    "pushing into a full ring must be refused, not silently absorbed");

    /* --- 넣은 차례대로 pop --------------------------------------------- */
    int out = 0;
    proven_err_t err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(proven_is_ok(err) && out == 1, "pop returns the oldest entry first");

    /* 이제 자리가 다시 생겼고, 고리는 아무것도 옮기지 않은 채 저장소를 감아 돈다. */
    err = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(proven_is_ok(err), "after a pop there is room for one more");

    int expected[] = { 2, 3, 4, 5 };
    for (int i = 0; i < 4; ++i) {
        err = PROVEN_RING_POP(&ring, int, &out);
        EXAMPLE_REQUIRE(proven_is_ok(err) && out == expected[i],
                        "entries come out in the order they went in");
    }

    /* --- 빈 상태도 가득 찬 상태처럼 군다: 거부할 뿐 자료를 지어내지 않는다 --- */
    err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(!proven_is_ok(err), "popping an empty ring must fail rather than return junk");

    PROVEN_RING_DESTROY(&ring);
    return EXAMPLE_OK();
}
