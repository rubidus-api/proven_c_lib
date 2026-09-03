#include "example.h"

/*
 * 컨테이너 장들은 구조를 하나씩 따로 보인다. 실제 프로그램은 여럿을 한꺼번에 쓰고, 그때
 * 떠오르는 물음들은 어느 한 컨테이너에 대한 것이 아니다.
 *
 *   - 채우는 동안 컨테이너가 재할당하지 않게 하려면?  reserve.
 *   - 남이 건넨 컨테이너가 쓸 만한지 확인하려면?  is_valid.
 *   - 자료가 정렬돼 있지 *않을* 때 찾으려면?  선형 탐색 - 그리고 여기서 이진 탐색이 왜
 *     틀린 답을 주는지 아는 것.
 *   - 맵에 이미 있는 값을 두 번 찾지 않고 고치려면?  get_mut.
 *   - 공격에 견디는 해시가 필요한가, 빠른 쪽인가?  키를 누가 고르느냐에 달렸고,
 *     map_hash 가 그 차이를 눈으로 보게 해 준다.
 *   - 토막토막 도착하는 자료의 검사합을 구하려면?  crc32_update.
 *
 * 이 프로그램은 작은 이벤트 수집기다. 이벤트가 도착하고, 클라이언트별로 세어지고, 최근
 * 몇 개는 진단 덤프용으로 남고, 지나가는 동안 묶음의 검사합이 구해진다.
 */

typedef struct {
    proven_u32 client;
    proven_u32 code;      /* 이벤트 코드. 0 은 "연결이 닫혔다" 는 뜻이다 */
} event_t;

/* 클라이언트 번호로 찾기. 이것은 이벤트를 *찾기* 위한 비교이지 정렬하기 위한 것이
 * 아니다. 아래 배열은 *도착한 차례*로 있고 그대로 둔다. */
static int by_client(const void *a, const void *b) {
    const event_t *x = (const event_t *)a;
    const event_t *y = (const event_t *)b;
    return (x->client > y->client) - (x->client < y->client);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 1. reserve: 용량을 한 번에 정한다 -------------------------------- */

    proven_result_array_t ar = PROVEN_ARRAY_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(ar.err), "creating the event log must succeed");
    if (!proven_is_ok(ar.err)) {
        return 1;
    }
    proven_array_t events = ar.value;

    /* 시작하기 전에 묶음 크기를 알고 있으니 자리를 한 번에 청한다. 이것이 없으면 배열은
     * 채워지는 동안 두 배씩 늘며 그때마다 내용을 복사하고, 아레나 할당자 뒤에서라면 그
     * 복사마다 옛 블록이 아레나가 reset 될 때까지 남는다. */
    proven_err_t err = proven_array_reserve(&events, 16);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving room for the whole batch must succeed");
    EXAMPLE_REQUIRE(events.cap >= 16, "the capacity is now at least what was asked for");

    /* is_valid 는 손잡이 자체가 구조적으로 온전한지 묻는다 - 서로 맞아떨어지는 포인터와
     * 길이와 용량. 컨테이너가 다른 코드에서 넘어오는 자리나, 0 으로 초기화된 손잡이가
     * 빠져나갔을 수 있는 자리에서 단언할 것. push 마다 되풀이할 것은 아니다. */
    EXAMPLE_REQUIRE(proven_array_is_valid(&events), "a created array must be structurally valid");

    proven_array_t never_created = {0};
    EXAMPLE_REQUIRE(!proven_array_is_valid(&never_created),
                    "a zero-initialised handle is not a usable array");

    static const event_t batch[] = {
        { .client = 7, .code = 200 }, { .client = 3, .code = 200 },
        { .client = 7, .code = 404 }, { .client = 9, .code = 200 },
        { .client = 3, .code = 500 }, { .client = 7, .code = 0   },
    };
    proven_size_t batch_len = sizeof batch / sizeof batch[0];

    proven_size_t cap_before = events.cap;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        err = PROVEN_ARRAY_PUSH(&events, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing an event must succeed");
    }
    EXAMPLE_REQUIRE(events.cap == cap_before, "the reserve was enough: nothing reallocated while filling");

    /* --- 2. 정렬되지 않은 자료에서 찾기 ----------------------------------- */

    /* 로그는 도착한 차례로 있고, 그 차례를 지키고 싶다. 그것이 기록하려는 대상이기
     * 때문이다. 여기서 이진 탐색은 더 빠르고 *틀리다*. 정렬된 구간에서만 써야 하는데,
     * 정렬되지 않은 자료에서는 "없다" 를 돌려주는 것이 아니라 헛소리를 돌려준다. 옳은
     * 도구는 선형 탐색이고, 이 크기의 묶음에서 O(n) 은 아무것도 아니다. */
    event_t key = { .client = 9, .code = 0 };
    const event_t *found = (const event_t *)proven_array_linear_search(&events, &key, by_client);
    EXAMPLE_REQUIRE(found != NULL, "client 9 appears in the batch");
    EXAMPLE_REQUIRE(found->code == 200, "and linear search returns the FIRST match in order");

    event_t absent = { .client = 42, .code = 0 };
    EXAMPLE_REQUIRE(proven_array_linear_search(&events, &absent, by_client) == NULL,
                    "a client that never appeared is reported as not found");

    /* --- 3. 가장 최근 이벤트를 담는 고리 ---------------------------------- */

    proven_result_ring_t rr = PROVEN_RING_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(rr.err), "creating the recent-events ring must succeed");
    proven_ring_t recent = rr.value;
    EXAMPLE_REQUIRE(proven_ring_is_valid(&recent), "a created ring must be structurally valid");

    proven_ring_t unset = {0};
    EXAMPLE_REQUIRE(!proven_ring_is_valid(&unset), "a zero-initialised ring handle is not usable");

    /* 이 고리는 가득 차면 덮어쓰는 대신 거부한다. 그래서 "최근 넷을 남긴다" 는 것은
     * 넣기 전에 가장 오래된 것을 우리가 직접 버린다는 뜻이다. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (recent.len == recent.cap) {
            event_t dropped;
            err = PROVEN_RING_POP(&recent, event_t, &dropped);
            EXAMPLE_REQUIRE(proven_is_ok(err), "a full ring must yield its oldest element");
        }
        err = PROVEN_RING_PUSH(&recent, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing after making room must succeed");
    }
    EXAMPLE_REQUIRE(recent.len == 4, "the ring holds the four most recent events");

    /* --- 4. 클라이언트별로 세기, 고칠 때마다 찾기는 한 번 ----------------- */

    /* create_with_capacity 는 proven_map_create 인데, 용량 인자가 왜 거기 있는지 말해
     * 주는 이름을 달았다. 지금 크기를 잡아 두면 나중의 재해싱을 피한다. 재해싱은 모든
     * 버킷을 복사하고, 전에 get_mut 이 돌려준 모든 포인터를 무효로 만든다. */
    proven_result_map_t mr = proven_map_create_with_capacity(alloc, 8, PROVEN_KEY_TYPE_INT,
                                                            sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(mr.err), "creating the counter map must succeed");
    proven_map_t counts = mr.value;
    EXAMPLE_REQUIRE(proven_map_is_valid(&counts), "a created map must be structurally valid");

    err = proven_map_reserve(&counts, 32);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving map capacity must succeed");

    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_map_key_t k = { .id = batch[i].client };

        /* get_mut 은 맵 저장소 *안*을 가리키는 포인터를 돌려준다. 그래서 계수기는 그것이
         * 사는 자리에서 올라간다 - 찾기 한 번, 되쓰는 복사 없음. 그 포인터는 다음 삽입
         * 전까지만 쓸 수 있고, 위에서 용량을 미리 잡아 둔 또 하나의 이유가 그것이다. */
        proven_u32 *seen = (proven_u32 *)proven_map_get_mut(&counts, k);
        if (seen) {
            *seen += 1;
        } else {
            proven_u32 one = 1;
            err = proven_map_set(&counts, k, &one);
            EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a new client must succeed");
        }
    }

    const proven_u32 *seven = PROVEN_MAP_GET_INT(&counts, proven_u32, 7);
    EXAMPLE_REQUIRE(seven != NULL && *seven == 3, "client 7 sent three events");

    /* 닫힘 이벤트(코드 0)는 그 클라이언트가 갔다는 뜻이다. 계수기를 버린다. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (batch[i].code == 0) {
            err = proven_map_remove(&counts, (proven_map_key_t){ .id = batch[i].client });
            EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
        }
    }
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&counts, proven_u32, 7) == NULL, "the closed client is gone");

    /* --- 5. 어느 해시인가, 그리고 그 차이를 보는 법 ----------------------- */

    /* 같은 키를 담은 문자열 키 맵 둘. 기본 쪽은 키가 있는 SipHash 로 해싱하므로, 키를
     * 고르는 공격자도 그것들을 한 버킷으로 몰아넣을 수 없다. 믿는 쪽은 빠른 FNV-1a 를
     * 쓰고, 모든 키를 여러분 코드가 고를 때*만* 옳은 선택이다. */
    proven_result_map_t untrusted = PROVEN_MAP_INIT_U8_BORROWED(alloc, proven_u32, 8);
    EXAMPLE_REQUIRE(proven_is_ok(untrusted.err), "creating the default string-key map must succeed");
    proven_map_t from_network = untrusted.value;

    proven_result_map_t tr = proven_map_create_trusted(alloc, 8, PROVEN_KEY_TYPE_U8_BORROWED,
                                                       sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(tr.err), "creating the trusted-key map must succeed");
    proven_map_t internal = tr.value;

    EXAMPLE_REQUIRE(from_network.trusted_keys == false, "the default map defends against chosen keys");
    EXAMPLE_REQUIRE(internal.trusted_keys == true, "the trusted map opts out of that defence");

    /* map_hash 는 맵이 실제로 키를 놓을 때 쓰는 값을 드러낸다. 그래서 이 선택이 믿고
     * 받아들이는 것이 아니라 눈으로 볼 수 있는 것이 된다. */
    proven_map_key_t name = { .str = PROVEN_LIT("user-agent") };
    proven_u64 keyed = proven_map_hash(&from_network, name);
    proven_u64 fast  = proven_map_hash(&internal, name);
    EXAMPLE_REQUIRE(keyed != fast, "the same key hashes differently under the two functions");
    EXAMPLE_REQUIRE(proven_map_hash(&internal, name) == fast, "and each function is deterministic");

    /* 맵이 소유하지 않은 기억에 사는 키는 *빌린* 것이다. 그 바이트가 맵보다 오래 살아야
     * 한다. 여기서는 문자열 리터럴이니 그렇다. 곧 다시 쓸 버퍼에서 키가 온다면 대신
     * 소유하는 키 맵(PROVEN_MAP_INIT_U8_OWNED / proven_map_set_u8_owned)을 쓸 것.
     * 그쪽은 복사한다. */
    proven_u32 hits = 1;
    err = proven_map_set(&from_network, name, &hits);
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a borrowed string key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_BORROWED(&from_network, proven_u32, PROVEN_LIT("user-agent")) != NULL,
                    "and it can be looked up by an equal view, not the same pointer");

    /* --- 6. 흘러가는 자료를 토막으로 검사합 ------------------------------- */

    /* 묶음은 지나가는 동안 검사합이 구해진다. 파일이나 소켓을 읽는 프로그램이 해야 하는
     * 일이 그것이다 - 전체를 한꺼번에 쥐지 않는다. 흐르는 값을 0 에서 시작해 토막을 하나씩
     * 먹이면, 마지막 값은 이어 붙인 것을 한 번에 부른 결과와 같다. */
    proven_u32 running = 0;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_mem_view_t chunk = { .ptr = (const proven_byte_t *)&batch[i], .size = sizeof batch[i] };
        running = proven_crc32_update(running, chunk);
    }
    proven_mem_view_t whole = { .ptr = (const proven_byte_t *)batch, .size = sizeof batch };
    EXAMPLE_REQUIRE(running == proven_crc32(whole),
                    "chunked and whole-buffer CRC-32 must agree, whatever the chunking");

    printf("events=%zu recent=%zu crc32=%08x\n",
           (size_t)events.len, (size_t)recent.len, (unsigned)running);

    proven_map_destroy(&internal);
    proven_map_destroy(&from_network);
    proven_map_destroy(&counts);
    PROVEN_RING_DESTROY(&recent);
    PROVEN_ARRAY_DESTROY(&events);
    return EXAMPLE_OK();
}
