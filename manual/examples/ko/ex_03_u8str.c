#include "example.h"

/*
 * 여기 문자열 손잡이가 둘 있고, 둘을 가르는 것은 크기가 아니라 소유다.
 *
 *   proven_u8str_t      - 고칠 수 있는 바이트 문자열. 할당을 소유하거나(create)
 *                         여러분의 것을 빌린다(borrow).
 *   proven_u8str_view_t - 남의 바이트를 가리키는 포인터와 길이. 아무것도 소유하지
 *                         않고, NUL 로 끝나지 않으며, 그 바이트가 살아 있는 동안에만
 *                         쓸 수 있다.
 *
 * 읽기만 하는 함수에 건네는 것이 뷰이고, 손에 쥐고 있는 것이 u8str 다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- *소유하는* 문자열: 할당자의 기억, 여러분이 지울 것 ---------------- */
    /* 용량 인자는 내용 바이트 수다. NUL 은 그 밖에 따로 있어서, as_cstr 은 언제나
     * O(1) 이고 언제나 안전하다. */
    proven_result_u8str_t r = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 16-byte string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t path = r.value;

    /* append 는 용량이 고정이다. 들어가거나 실패하고, 실패했다면 문자열에 손도 대지
     * 않았다. 재할당하지 않으므로 할당자도 필요 없다. */
    proven_err_t err = proven_u8str_append(&path, PROVEN_LIT("/etc/hosts"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "10 bytes fit in a 16-byte string");

    /* append_grow 는 늘어나는 쌍둥이다. 문자열을 만들 때 쓴 할당자를 주면 필요할 때
     * 재할당한다. 여전히 실패 원자적이다 - 할당이 실패하면 문자열은 그대로다. */
    err = proven_u8str_append_grow(alloc, &path, PROVEN_LIT(".backup.original"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "append_grow must reallocate rather than fail");

    /* 가운데를 고치기. insert 는 꼬리를 오른쪽으로, remove 는 왼쪽으로 민다. */
    err = proven_u8str_insert_grow(alloc, &path, 0, PROVEN_LIT("/srv"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a prefix must succeed");

    err = proven_u8str_remove(&path, proven_u8str_as_view(&path).size - 9, 9);  /* ".original" 을 떼어 낸다 */
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the trailing suffix must succeed");

    /* 찾는 것이 없으면 replace_first 는 PROVEN_OK 를 돌려준다 - "할 일이 없다" 는
     * 오류가 아니다. 그 차이가 중요하면 먼저 찾아볼 것. */
    err = proven_u8str_replace_first(&path, 0, PROVEN_LIT("hosts"), PROVEN_LIT("fstab"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "replacing an existing substring must succeed");

    /* --- 읽기: 복사하지 말고 뷰를 빌릴 것 ---------------------------------- */
    /* as_view 는 공짜다. 그 뷰는 다음 편집 전까지만 쓸 수 있다. 늘리는 호출은 재할당할
     * 수 있고, 그러면 뷰(그리고 cstr)는 매달린 포인터가 된다. */
    proven_u8str_view_t v = proven_u8str_as_view(&path);

    EXAMPLE_REQUIRE(proven_u8str_view_eq(v, PROVEN_LIT("/srv/etc/fstab.backup")),
                    "the edits above should have produced /srv/etc/fstab.backup");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(v, PROVEN_LIT("/srv")),
                    "the inserted prefix is at the front");

    proven_size_t dot = proven_u8str_view_find(v, 0, PROVEN_LIT(".backup"));
    EXAMPLE_REQUIRE(dot != PROVEN_INDEX_NOT_FOUND, "the suffix must be found");

    /* 슬라이스는 *같은* 바이트를 가리키는 뷰다 - 할당도 복사도 없다. */
    proven_u8str_view_t stem = proven_u8str_view_slice(v, 0, dot);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(stem, PROVEN_LIT("/srv/etc/fstab")),
                    "slicing at the suffix leaves the stem");

    /* as_cstr 은 C API 로 나가는 비상구이고, 소유한 문자열이 길이 뒤에 NUL 을 지키기
     * 때문에만 옳다. 뷰로는 이렇게 하지 *말 것*. `stem.ptr` 은 NUL 로 끝나지 않는다 -
     * 그냥 `path` 안을 가리킬 뿐이다. */
    printf("owned:  %s\n", proven_u8str_as_cstr(&path));

    /* --- *빌린* 문자열: 여러분의 기억, 할당은 전혀 없다 -------------------- */
    /* 타입도 연산도 같다 - 다만 바이트가 이 스택 버퍼다. `cap` 은 NUL 을 포함하므로
     * 내용은 31 바이트까지 담긴다. */
    proven_byte_t line[32];
    proven_u8str_t status = proven_u8str_borrow(line, sizeof line);

    err = proven_u8str_append(&status, PROVEN_LIT("mounted "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into a borrowed buffer needs no allocator");
    err = proven_u8str_append(&status, stem);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a view can be appended just like a literal");

    /* 빌린 문자열에도 늘리는 호출은 있지만, 자기 것이 아닌 기억을 재할당하지는
     * 않는다. 자료가 넘치면 OUT_OF_BOUNDS 이고 `line` 은 손대지 않은 채로 남는다.
     * 빌린 문자열이 등 뒤에서 조용히 힙으로 달아나는 일은 없다. */
    err = proven_u8str_append_grow(alloc, &status,
                                   PROVEN_LIT(" ...and a great deal more text than fits"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a borrowed string reports overflow instead of reallocating caller memory");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&status), PROVEN_LIT("mounted /srv/etc/fstab")),
                    "the failed append must have left the string unchanged");

    printf("borrowed: %s\n", proven_u8str_as_cstr(&status));

    /* reset 은 비어 있는 상태로 자르고 버퍼는 그대로 둔다. 그래서 다음 프레임이 같은
     * 32 바이트를 할당 없이 다시 쓴다. */
    err = proven_u8str_reset(&status);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reset must succeed on a borrowed string");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&status).size == 0, "reset empties the string");

    /* --- destroy: 소유 규칙을 그대로 적어 보면 ------------------------------ */
    /* 빌린 문자열에 대한 destroy 는 아무 일도 하지 않는다 - `line` 은 라이브러리가
     * 해제할 것이 아니다. 그래도 부르는 것이 옳고 값도 들지 않으며, 덕분에 뒷정리 코드는
     * 자기가 쥔 문자열이 어느 쪽인지 몰라도 된다. */
    proven_u8str_destroy(alloc, &status);

    /* 소유한 문자열에 대한 destroy 는 할당을 해제한다. 그리고 반드시 그 문자열을 만들
     * 때 쓴 할당자를 주어야 한다. */
    proven_u8str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
