#include "example.h"

/*
 * 풀(pool)은 구역이 아니라 잦은 교체를 위한 최적화다. 타입 하나를 위한 것으로,
 * 같은 크기의 블록을 몇 번이고 할당하고 해제하는 자리 - 리스트 노드, 이벤트, 파티클 -
 * 에서 매번 malloc 값을 치르지 않게 한다.
 *
 * 풀은 해제된 블록을 담아 두는 작은 스택("bin")을 갖는다. 해제하면 바탕 할당자로
 * 돌려보내는 대신 bin 에 얹고, 할당하면 거기서 하나를 꺼낸다. 둘 다 O(1) 이고 힙을
 * 건드리지 않는다. 그 재활용이 존재 이유 전부이고, 아래 검사가 그것이 실제로
 * 일어남을 보인다.
 *
 * 소유: 풀은 해제된 블록을 캐시하지만, 자기가 *내준* 블록은 추적하지 않는다. 가져간
 * 블록은 destroy 전에 모두 돌려주어야 한다 - 풀은 자기가 모르는 것을 해제할 수 없다.
 */

typedef struct {
    int id;
    int score;
} node_t;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    EXAMPLE_REQUIRE(proven_alloc_is_valid(heap), "hosted builds have a heap allocator");

    /* 풀은 bin 으로 감당 못 하는 블록을 위한 바탕 할당자를 받고, 자기가 다루는 그 한
     * 타입의 정확한 크기와 정렬을 받는다. 마지막 인자는 재사용을 위해 세워 둘 해제된
     * 블록의 최대 개수다. */
    proven_pool_t pool = {0};
    proven_err_t err = proven_pool_init(&pool, heap, sizeof(node_t), alignof(node_t), 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "initializing a pool of node_t must succeed");
    if (!proven_is_ok(err)) {
        return 1;
    }

    proven_allocator_t nodes = proven_pool_as_allocator(&pool);

    /* --- 첫 블록: bin 이 비어 있으니 힙에서 온다 --------------------------- */
    proven_result_mem_mut_t first = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(first.err), "the pool must be able to serve its own item type");
    if (!proven_is_ok(first.err)) {
        proven_pool_destroy(&pool);
        return 1;
    }

    node_t *n = (node_t *)first.value.ptr;
    *n = (node_t){ .id = 1, .score = 100 };
    void *first_addr = n;

    /* --- 돌려주기: 힙이 아니라 bin 으로 들어간다 --------------------------- */
    nodes.free_fn(nodes.ctx, n);
    EXAMPLE_REQUIRE(pool.bin_len == 1, "a freed block is cached for reuse, not returned to the heap");
    /* 여기서부터 `n` 은 매달린 포인터다. 그 바이트는 다시 풀의 것이다. */

    /* --- 두 번째 블록: 방금 해제한 것이 곧바로 돌아온다 -------------------- */
    proven_result_mem_mut_t second = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(second.err), "allocating from a non-empty bin must succeed");
    EXAMPLE_REQUIRE(second.value.ptr == first_addr, "the recycled block is the one that was freed");
    EXAMPLE_REQUIRE(pool.bin_len == 0, "taking it back out empties the bin");

    /* 재활용된 기억은 0 으로 채워지지 *않는다* - 풀이 남겨 둔 그대로다.
     * 갓 받은 malloc 에 하듯 모든 필드를 초기화할 것. */
    node_t *m = (node_t *)second.value.ptr;
    *m = (node_t){ .id = 2, .score = 50 };
    EXAMPLE_REQUIRE(m->id == 2, "the recycled block is ours to overwrite");

    /* --- 풀 하나는 크기 하나와 정렬 하나만 감당한다 ------------------------ */
    /* 그 밖의 요청은 거부된다. 이것은 범용 할당자가 아니고, 크기가 다른 블록을 조용히
     * 내주지도 않는다. 코드는 PROVEN_ERR_UNSUPPORTED - "내 일이 아니다" - 이지 INVALID_ARG
     * 가 아니다. 후자였다면 "쓰레기를 건넸다"로 읽혀 여러분이 자기 코드에서 버그를 찾아
     * 헤매게 만들었을 것이다. */
    proven_result_mem_mut_t wrong = nodes.alloc_fn(nodes.ctx, sizeof(node_t) * 2, alignof(node_t));
    EXAMPLE_REQUIRE(wrong.err == PROVEN_ERR_UNSUPPORTED, "the pool only serves its configured item size");

    /* --- 지우기 전에 살아 있는 블록을 모두 돌려준다 ------------------------ */
    /* proven_pool_destroy 는 bin 에 있는 것과 bin 자체를 해제한다. `m` 은 아직 나가
     * 있으므로, 이 free 를 건너뛰면 그대로 누수가 된다. */
    nodes.free_fn(nodes.ctx, m);

    printf("pool: %zu block(s) cached for reuse at teardown\n", (size_t)pool.bin_len);

    proven_pool_destroy(&pool);
    return EXAMPLE_OK();
}
