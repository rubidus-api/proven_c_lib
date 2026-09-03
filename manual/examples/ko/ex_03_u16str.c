#include "example.h"

/*
 * UTF-16 이 이 라이브러리에 있는 이유는 하나다. 어떤 운영체제 호출은 그것만 받는다.
 * 윈도의 "와이드" API 가 늘 그 경우다 - CreateFileW 에 건네는 파일 이름은 바이트가
 * 아니라 NUL 로 끝나는 16비트 코드 단위의 줄이다.
 *
 * 그래서 이 타입이 하는 일은 좁다. 코드 단위를 모으고, 개수를 맞게 지키고, 시스템
 * 호출이 원하는 포인터를 내주는 것. 프로그램의 나머지는 UTF-8 로 두어야 한다.
 *
 * 하나만 헷갈리지 말 것 - 단위다. 여기서 용량 32 는 코드 단위 32개, 곧 64 바이트이고,
 * 기본 다국어 평면 밖의 문자는 - 이모지, 드문 한자 대부분 - 두 개를 쓴다. 코드 단위의
 * 개수는 문자의 개수가 아니고, 한 번도 그랬던 적이 없다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 인자는 바이트 한계가 아니라 코드 단위 한계다. */
    proven_result_u16str_t r = proven_u16str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 32-code-unit string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u16str_t name = r.value;
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 0, "a new string is empty");

    /* PROVEN_U16_LIT 은 u"..." 리터럴에서 뷰를 만들고 단위 개수를 리터럴 자체에서
     * 계산한다. 그래서 개수가 글과 어긋날 수 없다. */
    proven_err_t err = proven_u16str_append(&name, PROVEN_U16_LIT("C:\\logs\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory prefix fits in 32 code units");

    err = proven_u16str_append(&name, PROVEN_U16_LIT("service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the file name fits too");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 8 + 11, "the length is a count of code units");

    /* 바이트 문자열 쌍둥이처럼 원자적이다. 자료가 넘치면 거부되고 문자열은 그대로
     * 남는다. 그래서 경로가 반쯤 적히는 일이 없다. */
    proven_size_t before = proven_u16str_len(&name);
    err = proven_u16str_append(&name, PROVEN_U16_LIT(".a-suffix-long-enough-to-overflow-the-capacity"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized append must be refused");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == before, "and must not truncate the path");

    /* --- 시스템 호출이 원하는 포인터 -------------------------------------- */

    /* as_ptr 은 내부의 코드 단위를 NUL 로 끝난 채로, 복사 없이 돌려준다. 호출 직전의
     * 마지막 걸음이고, 그 포인터는 다음 append 전까지만 쓸 수 있다. 늘리는 append 는
     * 저장소를 옮길 수 있다. */
    const proven_u16 *wide = proven_u16str_as_ptr(&name);
    EXAMPLE_REQUIRE(wide != NULL, "an assembled string must yield a pointer");
    EXAMPLE_REQUIRE(wide[0] == (proven_u16)'C', "the first code unit is the drive letter");
    EXAMPLE_REQUIRE(wide[proven_u16str_len(&name)] == 0, "the sequence is NUL-terminated for the system call");
    /* 윈도에서는 이것이 이 타입의 존재 이유 전부다.
     *     HANDLE h = CreateFileW((LPCWSTR)wide, ...);
     * 여기서는 부르지 않는다. 이 예제는 다른 모든 곳에서도 돌아야 하기 때문이다. */

    /* --- 자르는 것이 옳은 답일 때 ----------------------------------------- */

    /* 어떤 시스템 구조체에는 폭이 고정된 필드가 있다 - 이를테면 16 단위짜리 이름표 -
     * 거기서는 들어가지 않는 이름을 거부하는 것이 아니라 자르는 것이 뜻이다. 부분
     * append 가 그것을 위한 것이다. 담을 수 있는 만큼 담고 몇 단위를 썼는지 알려 주어,
     * 부르는 쪽이 그 값을 잘렸다고 표시할 수 있게 한다. */
    proven_result_u16str_t r2 = proven_u16str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the fixed-width label must succeed");
    proven_u16str_t label = r2.value;

    proven_result_size_t wrote = proven_u16str_append_partial(&label, PROVEN_U16_LIT("a-label-that-is-longer-than-the-field"));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_ERR_OUT_OF_BOUNDS, "the truncation is reported, not hidden");
    EXAMPLE_REQUIRE(wrote.value == 16, "it filled the field exactly");
    EXAMPLE_REQUIRE(proven_u16str_len(&label) == wrote.value, "and the length matches what it says it wrote");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&label)[wrote.value] == 0,
                    "a truncated string is still NUL-terminated, so it is still safe to pass on");

    printf("assembled %zu code unit(s); label truncated to %zu\n",
           (size_t)proven_u16str_len(&name), (size_t)wrote.value);

    proven_u16str_destroy(alloc, &label);
    proven_u16str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
