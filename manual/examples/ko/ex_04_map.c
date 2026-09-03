#include "example.h"

/*
 * proven_map_t 는 납작한 개방 주소법 해시 맵이다. 값 타입은 만들 때 정해지고 버킷
 * 배열 안에 그대로 담긴다 - 값을 위한 항목별 할당이 없고, get 은 그 배열 속을 곧장
 * 가리키는 포인터를 돌려준다.
 *
 * 재미있는 결정은 *키* 쪽이다.
 *
 *   PROVEN_KEY_TYPE_INT          - 키가 proven_size_t 다. 소유할 것이 없다.
 *   PROVEN_KEY_TYPE_U8_BORROWED  - 버킷이 여러분의 포인터와 길이를 담는다. 맵은
 *                                  바이트를 복사하지 않으므로, 항목이 살아 있는
 *                                  동안 *여러분*이 그 바이트를 살아 있고 움직이지
 *                                  않게 지켜야 한다. 문자열 리터럴에 알맞다.
 *   PROVEN_KEY_TYPE_U8_OWNED     - 맵이 키 바이트를 제 저장소로 복사하고 나중에
 *                                  해제한다. 실행 중에 만든 키에 알맞고, 대개의
 *                                  키가 그렇다.
 *
 * 이 예제의 뒷부분이 OWNED 가 존재하는 이유다.
 */

typedef struct {
    int  level;
    long score;
} player_t;

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 정수 키 ------------------------------------------------------------ */
    proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an int-keyed map must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_map_t by_id = r.value;

    proven_err_t err = PROVEN_MAP_SET_INT(&by_id, 404, player_t, ((player_t){ .level = 3, .score = 990 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting into the map must succeed");
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 1, .score = 10 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a second key must succeed");

    /* 이미 있는 키에 set 하면 값을 바꾼다. 항목을 더하지 않는다. */
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 2, .score = 40 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "re-setting an existing key must succeed");
    EXAMPLE_REQUIRE(by_id.len == 2, "re-setting a key replaces its value rather than adding an entry");

    /* get 은 버킷 배열 속을 가리키는 포인터를, 없으면 NULL 을 돌려준다. 다시 해싱하는
     * 삽입이 일어나면 무효가 된다 - 찾고, 쓰고, 버릴 것. */
    const player_t *p = PROVEN_MAP_GET_INT(&by_id, player_t, 7);
    EXAMPLE_REQUIRE(p && p->level == 2 && p->score == 40, "get must see the replaced value");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 999) == NULL, "a missing key yields NULL");

    err = PROVEN_MAP_REMOVE_INT(&by_id, 7);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 7) == NULL, "a removed key is gone");
    EXAMPLE_REQUIRE(by_id.len == 1, "removal decrements the live entry count");

    PROVEN_MAP_DESTROY(&by_id);

    /* --- 소유하는 문자열 키 -------------------------------------------------- */
    /* 같은 맵인데, 실행 중에 지어낸 이름을 키로 쓴다 - 빌린 키였다면 매달린 포인터가
     * 되기를 기다리는 자리다. */
    proven_result_map_t rm = PROVEN_MAP_INIT_U8_OWNED(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(rm.err), "creating an owned-string-keyed map must succeed");
    if (!proven_is_ok(rm.err)) {
        return 1;
    }
    proven_map_t by_name = rm.value;

    /* 키마다 다시 쓸 작정인 임시 버퍼. BORROWED 맵에서 그 작정은 치명적이다. 모든
     * 항목이 바로 이 같은 바이트를 가리키게 된다. */
    proven_byte_t scratch[32];
    proven_u8str_t name = proven_u8str_borrow(scratch, sizeof scratch);

    err = proven_u8str_append(&name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "building the first key must succeed");

    /* set_u8_owned 는 키 바이트를 맵 저장소로 *복사한다*. 돌아온 뒤로 맵의 키는
     * `scratch` 와 아무 상관이 없다. */
    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 9, .score = 5000 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting with an owned key must succeed");

    /* 그래서 버퍼는 곧바로 다음 키에 다시 쓸 수 있고... */
    err = proven_u8str_reset(&name);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the key buffer is ours again the moment set returns");
    err = proven_u8str_append(&name, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "overwriting the buffer with the next key must succeed");

    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 4, .score = 700 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting the second owned key must succeed");

    /* ...첫 항목은 그 덮어쓰기에 조금도 다치지 않는다. 이것이 요점 전부다. 맵은
     * 지금 "grace" 라고 적힌 버퍼의 뷰가 아니라 "ada" 의 제 사본을 쥐고 있다. BORROWED
     * 맵이었다면 둘 다 "grace" 로 키가 잡힌 항목 둘을 보고했을 것이다 - 아니면 더 나쁘게,
     * 해제된 기억이 키인 항목 하나를. */
    const player_t *ada = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(ada && ada->score == 5000, "the copied key survives the caller reusing its buffer");

    const player_t *grace = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(grace && grace->score == 700, "the second key is a separate entry");
    EXAMPLE_REQUIRE(by_name.len == 2, "two distinct keys means two entries");

    /* remove 는 맵이 만든 키 사본을 해제한다 - 여러분이 직접 해제하는 일은 없다. */
    err = PROVEN_MAP_REMOVE_U8_OWNED(&by_name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing an owned key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada")) == NULL,
                    "the removed entry is gone");

    printf("map: %zu name(s) left, grace at level %d\n",
           (size_t)by_name.len, grace ? grace->level : -1);

    /* destroy 는 버킷 배열과 그 안에 남은 키 사본을 모두 해제한다(여기서는 "grace").
     * `scratch` 는 우리 것이고 맵보다 오래 살며, 빌린 `name` 손잡이는 해제할 것이 없다. */
    PROVEN_MAP_DESTROY(&by_name);
    proven_u8str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
