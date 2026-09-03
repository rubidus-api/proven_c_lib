#include "example.h"

/*
 * 앞의 예제는 자리가 모자라면 문자열을 늘렸다. 이번 것은 늘리는 것이 허용되지 않는
 * 경우 - 크기가 고정된 레코드, 길이 상한이 단단한 로그 줄, 재할당하면 안 되는 아레나
 * 속 버퍼 - 그리고 자료가 들어가지 않을 때 호출이 줄 수 있는 두 가지 정직한 답에 대한
 * 것이다.
 *
 *   "안 됩니다, 그리고 아무것도 바꾸지 않았습니다" - 원자적 호출들: append, insert,
 *                                      replace_at. 먼저 용량을 검사하므로, 거부는
 *                                      문자열을 그대로 남긴다.
 *   "일부만 했고, 얼마나 했는지 알려 드립니다" - 최선 노력 호출: append_partial.
 *                                      담을 수 있는 만큼 담고 몇 바이트를 썼는지
 *                                      알려 준다.
 *
 * 둘 다 쓸모가 있다. 잘못 고르면 레코드를 조용히 자르거나 조용히 버린다. `_grow` 짝은
 * 세 번째 답 - "네, 자리를 더 찾았습니다" - 이고, 견주어 보라고 끝에 실었다.
 */

#define FIELD_CAP 32u

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the field buffer must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t field = r.value;

    /* is_valid 는 손잡이 자신의 구조를 검사한다 - 포인터와, 서로 어긋나지 않는 용량과
     * 길이. 문자열이 다른 데서 넘어올 때 여러분 코드의 경계에서 한 번 단언해 둘 값어치가
     * 있다. 편집할 때마다 할 검사는 아니다. 모든 편집이 그 성질을 지키기 때문이다. */
    EXAMPLE_REQUIRE(proven_u8str_is_valid(&field), "a freshly created string must be structurally valid");

    /* --- 미리 자리를 잡아 두기 -------------------------------------------- */

    /* reserve 는 용량을 지금 올려 두어 나중의 성장이 재할당하지 않게 한다. 힙에서는
     * 복사를 아끼고, 아레나에서는 더 나쁜 것을 아낀다. 거기서는 재할당마다 옛 블록이
     * 다음 reset 까지 새기 때문이다. 필요할 만큼을, 한 번에 청할 것. */
    proven_err_t err = proven_u8str_reserve(alloc, &field, 64);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving 64 bytes must succeed");
    EXAMPLE_REQUIRE(field.internal.cap >= 64, "the capacity must actually be at least what was asked for");

    /* --- 원자적 호출: 들어가거나, 아무것도 바꾸지 않거나 -------------------- */

    err = proven_u8str_append(&field, PROVEN_LIT("2026-01-01 level=info "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the prefix fits in the reserved capacity");

    /* append_byte 는 한 바이트를 더한다. 구분자, 종결자, 이스케이프 문자가 그런
     * 것들이다. 늘리는 호출이라 할당자를 받는다 - 한 바이트야말로 생각지 못한 경계에서
     * 용량 검사가 걸리는 바로 그 경우다. */
    err = proven_u8str_append_byte(alloc, &field, (proven_u8)'[');
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending a single separator byte must succeed");

    err = proven_u8str_append(&field, PROVEN_LIT("disk full"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the message fits");

    err = proven_u8str_append_byte(alloc, &field, (proven_u8)']');
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the bracket must succeed");

    /* 이제 용량이 담을 수 있는 것보다 많이 청해 본다. 원자적 append 는 거부하고 -
     * 이것이 믿고 기댈 값어치가 있는 성질이다 - 문자열은 호출 전에 담고 있던 것을
     * 그대로 담고 있다. */
    proven_size_t before = proven_u8str_as_view(&field).size;
    err = proven_u8str_append(&field, PROVEN_LIT(" and a very long trailing explanation that certainly does not fit"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized atomic append must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before, "and must leave the string untouched");

    /* --- 최선 노력 호출: 들어가는 만큼, 그리고 그 개수 --------------------- */

    /* 보고서의 폭 고정 열이 이것을 쓰는 경우다. 들어가는 만큼 쓰고, 얼마나 썼는지 알아
     * 두어 부르는 쪽이 그 값을 온전한 척하는 대신 잘렸다고 표시할 수 있게 한다. */
    proven_result_size_t part = proven_u8str_append_partial(&field, PROVEN_LIT(" ...more text than there is room for"));
    EXAMPLE_REQUIRE(part.err == PROVEN_ERR_OUT_OF_BOUNDS, "a partial append that truncates still reports the truncation");
    EXAMPLE_REQUIRE(part.value > 0, "but it wrote what it could");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before + part.value,
                    "and the string grew by exactly the number of bytes it reports");
    printf("partial append wrote %zu byte(s) before the buffer was full\n", (size_t)part.value);

    /* --- 늘리지 않고 가운데를 고치기 --------------------------------------- */

    proven_result_u8str_t r2 = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the second buffer must succeed");
    proven_u8str_t path = r2.value;
    err = proven_u8str_append(&path, PROVEN_LIT("var/log/service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the path fits");

    /* insert 는 꼬리를 오른쪽으로 민다. 용량 고정이다 - 들어가거나 거부한다. */
    err = proven_u8str_insert(&path, 0, PROVEN_LIT("/"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a leading slash must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/service.log")),
                    "the insert lands at index 0");

    /* replace_at 은 어떤 자리의 old_len 바이트를 길이에 상관없는 자료로 바꾼다.
     * 결과가 여전히 들어가기만 하면 된다. "service"(7)를 "daemon"(6)으로 바꾸면 문자열이
     * 짧아지므로 용량 때문에 실패할 수 없다. */
    proven_size_t at = proven_u8str_view_find(proven_u8str_as_view(&path), 0, PROVEN_LIT("service"));
    EXAMPLE_REQUIRE(at != PROVEN_SIZE_MAX, "the substring must be found before it can be replaced");
    err = proven_u8str_replace_at(&path, at, 7, PROVEN_LIT("daemon"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "a shortening replacement must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/daemon.log")),
                    "and produce the expected path");

    /* 같은 편집을 거꾸로 하면 32바이트 버퍼를 넘치고, 용량 고정 호출은 경로를 자르는
     * 대신 거부한다 - 자르는 쪽이 바로 엉뚱한 파일에 조용히 쓰게 되는 그 실패다. */
    before = proven_u8str_as_view(&path).size;
    err = proven_u8str_replace_at(&path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "a replacement that does not fit must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&path).size == before, "and must leave the path unchanged");

    /* replace_at_grow 는 재할당을 허락받은 같은 편집이다. 버퍼가 힙에 있고 길이가 정말
     * 한정되지 않았을 때 쓸 것. 한계가 형식의 일부라면 용량 고정 호출을 고를 것. */
    err = proven_u8str_replace_at_grow(alloc, &path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the growing variant makes room instead of refusing");
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".log")),
                    "the extension is still at the end after the edit");

    /* ends_with 는 확장자 검사가 실제로 던지는 물음에 답한다. 손으로 셈한 인덱스로 하면
     * off-by-one 이 사는 자리가 되고, 포인터에 strcmp 로 하면 뷰에는 없는 NUL 이
     * 필요해진다. */
    EXAMPLE_REQUIRE(!proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".txt")),
                    "and it is not a .txt file");

    /* 빈 접미사는 무엇의 접미사이기도 하다. 접미사 목록을 도는 반복문이 특별한 경우를
     * 두지 않아도 되게 해 주는 답이다. */
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT("")),
                    "every string ends with the empty suffix");

    printf("log line: %s\n", proven_u8str_as_cstr(&field));
    printf("path:     %s\n", proven_u8str_as_cstr(&path));

    proven_u8str_destroy(alloc, &path);
    proven_u8str_destroy(alloc, &field);
    return EXAMPLE_OK();
}
