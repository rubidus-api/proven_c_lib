#include "example.h"
#include <string.h>

/*
 * 매일 하는 텍스트 일을 뷰 위에서: 한 줄을 필드로 나누고, 다듬고, 알려진 접두사나
 * 접미사를 떼고, 마지막 점을 찾고, 얻은 것을 정렬한다. 정렬에 쓰는 배열 말고는 아무것도
 * 할당하지 않는다. 모든 결과는 처음 받은 텍스트 안을 가리킨다.
 *
 * 두 규칙이 내내 지켜진다. 빈 결과는 {NULL, 0} 이므로, 뷰는 포인터가 아니라 크기로
 * 검사한다. 그리고 분할 루프는 split_next 의 반환값으로 끝낸다 - 구분자 n 개는 빈 필드를
 * 포함해 언제나 n + 1 개의 필드가 된다.
 */

static bool is(proven_u8str_view_t v, const char *want) {
    return v.size == strlen(want) && (v.size == 0 || memcmp(v.ptr, want, v.size) == 0);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 나누고, 필드마다 다듬기 -------------------------------------------- */

    /* 끝에 구분자가 있고 빈 필드가 있는 레코드: 구분자 다섯 개, 그래서 필드 여섯 개 -
     * 마지막은 빈 필드다. 사람이 처음 쓰는 루프("구분자가 발견되는 동안")는 다섯 개를
     * 내고 꼬리를 잃는다. */
    proven_u8str_view_t record = PROVEN_LIT(" report.tar.gz , draft.txt,,notes.md ,\tREADME ,");
    proven_u8str_view_t fields[8];
    int n = 0;
    proven_u8str_view_split_t it = proven_u8str_view_split(record, PROVEN_LIT(","));
    proven_u8str_view_t f;
    while (n < 8 && proven_u8str_view_split_next(&it, &f)) {
        fields[n++] = proven_u8str_view_trim(f);   /* ' ', \t, \n, \v, \f, \r - 그 밖에는 없다 */
    }
    EXAMPLE_REQUIRE(n == 6, "five separators give six fields");
    EXAMPLE_REQUIRE(is(fields[0], "report.tar.gz") && is(fields[3], "notes.md") && is(fields[4], "README"),
                    "each field is trimmed on both ends");
    EXAMPLE_REQUIRE(fields[2].size == 0 && fields[5].size == 0, "the empty fields are kept, and tested by size");

    /* 한쪽 끝이 의미 있는 텍스트를 위한 한쪽 다듬기. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_start(PROVEN_LIT("  indented  ")), "indented  "), "trim_start");
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_end(PROVEN_LIT("  indented  ")), "  indented"), "trim_end");

    /* --- 접두사, 접미사, 그리고 마지막 점 ----------------------------------- */

    proven_u8str_view_t name = fields[0];
    /* 확장자는 마지막 점 뒤에 있다: find 가 아니라 find_last. */
    proven_size_t dot = proven_u8str_view_find_last(name, PROVEN_LIT("."));
    EXAMPLE_REQUIRE(dot == 10, "the last dot in report.tar.gz is at 10");
    EXAMPLE_REQUIRE(is(proven_u8str_view_slice(name, dot + 1, name.size), "gz"), "so the extension is gz");

    /* remove_suffix 는 접미사가 없으면 뷰를 그대로 둔다 - 오류가 아니므로, 알아야 할
     * 때는 starts_with/ends_with 로 먼저 물어라. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".tar.gz")), "report"), "the double suffix goes");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".zip")), "report.tar.gz"),
                    "an absent suffix changes nothing");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_prefix(PROVEN_LIT("# heading"), PROVEN_LIT("# ")), "heading"),
                    "a known prefix goes");
    EXAMPLE_REQUIRE(proven_u8str_view_contains(fields[1], PROVEN_LIT("draft")), "contains is find != NOT_FOUND");

    /* --- 비어 있지 않은 필드 정렬 ------------------------------------------ */

    proven_result_array_t ra = proven_array_create(alloc, 8, sizeof(proven_u8str_view_t), alignof(proven_u8str_view_t));
    EXAMPLE_REQUIRE(proven_is_ok(ra.err), "creating the array must succeed");
    if (!proven_is_ok(ra.err)) return EXAMPLE_OK();
    proven_array_t names = ra.value;
    for (int i = 0; i < n; ++i) {
        if (fields[i].size > 0) (void)proven_array_push(&names, &fields[i]);
    }
    /* cmp_ptr 는 정렬용 모양의 cmp 다: 뷰를 가리키는 포인터를 받는다. 바이트는 부호 없이
     * 비교되고, 접두사가 먼저 온다. ASCII 에서 'R' 이 'd' 보다 작으므로 "README" 가
     * 소문자 이름들보다 앞선다. */
    proven_array_sort(&names, proven_u8str_view_cmp_ptr);
    const proven_u8str_view_t *first = proven_array_get(&names, 0);
    const proven_u8str_view_t *last = proven_array_get(&names, 3);
    EXAMPLE_REQUIRE(first && is(*first, "README") && last && is(*last, "report.tar.gz"), "sorted bytewise");
    EXAMPLE_REQUIRE(proven_u8str_view_cmp(PROVEN_LIT("app"), PROVEN_LIT("apple")) < 0,
                    "cmp answers by sign - never compare it with -1");

    /* --- well-formed 는 "찾았다" 도 "비어 있지 않다" 도 아니다 ---------------- */

    /* 끝을 넘은 슬라이스는 {NULL, 0} 이고, 이는 well formed 다 - 그래서 이 술어로는
     * 루프를 끝낼 수도, "비었다" 와 "끝을 넘었다" 를 가를 수도 없다. 뷰를 읽어도
     * 안전하다는 것만 말한다. */
    proven_u8str_view_t past = proven_u8str_view_slice(name, 100, 5);
    EXAMPLE_REQUIRE(proven_u8str_view_is_well_formed(past) && past.size == 0, "past the end: empty and well formed");
    EXAMPLE_REQUIRE(!proven_u8str_view_is_well_formed((proven_u8str_view_t){ NULL, 3 }), "only {NULL, n > 0} is not");

    printf("%d fields, %zu names sorted\n", n, (size_t)names.len);
    proven_array_destroy(&names);
    return EXAMPLE_OK();
}
