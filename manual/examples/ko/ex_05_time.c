#include "example.h"

/*
 * 시간에는 똑같이 생겼지만 같지 않은 두 갈래가 있고, 잘못 고르는 것이 시간 재기의
 * 고전적 버그다.
 *
 *   - *벽시계*는 "지금 몇 시인가" 에 답한다. 사용자가 보고 싶어 하는 것이고, 뛰는 것이
 *     허용된다 - NTP 가 고치고, 서머타임이 옮기고, 관리자가 맞춘다. 그것으로 기간을
 *     재면 음수 경과 시간이 나올 수 있고, 윤초가 있던 날에 실제로 유명하게 그랬다.
 *
 *   - *단조* 시계는 "그때로부터 얼마나" 에 답한다. 앞으로만, 고른 속도로 가고, 어떤
 *     달력과도 관계가 없다. 어떤 작업의 시간을 잴 때 쓰는 것이 이것이다.
 *
 * libc 는 이 구분을 흐린다. time() 은 초 단위 벽시계라 재는 데는 쓸모없다. clock() 은
 * 경과 시간이 아니라 CPU 시간을 재므로, 잠든 프로그램은 즉시 끝난 것처럼 보인다. 어느
 * 이름도 자기가 둘 중 어느 물음에 답하는지 말해 주지 않는다.
 *
 * proven_time_now() 는 유닉스 기점부터의 나노초다. 날짜로 형식화되기도 하고 기간으로
 * 빼지기도 하는 수 하나이며, 실제 작업의 시간을 재기에 충분히 고운 해상도를 갖는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 기간으로 쓰기 -------------------------------------------------- */
    proven_time_t start = proven_time_now();
    proven_time_sleep(15);                 /* 밀리초 */
    proven_time_t end = proven_time_now();

    proven_i64 elapsed_ns = end - start;
    EXAMPLE_REQUIRE(elapsed_ns > 0, "time must move forward across a sleep");
    /* sleep 은 청한 시간 *이상*을 보장하지, 이하를 보장하지 않는다. 언제 다시 돌지는
     * 스케줄러가 정한다. 여기서 상한을 단언하면 바쁜 기계에서 실패하는 시험이 되고,
     * 그래서 이 예제는 그러지 않는다. */
    EXAMPLE_REQUIRE(elapsed_ns >= 10 * 1000 * 1000,
                    "sleeping 15ms must take at least ~10ms of wall time");

    /* --- 날짜로 쓰기 ---------------------------------------------------- */
    proven_datetime_t dt = proven_time_breakdown(start);
    EXAMPLE_REQUIRE(dt.year >= 2020 && dt.year < 3000, "the epoch breakdown gives a plausible year");
    EXAMPLE_REQUIRE(dt.month >= 1 && dt.month <= 12, "month is 1-12, not 0-11 as in libc's tm");
    EXAMPLE_REQUIRE(dt.day >= 1 && dt.day <= 31, "day is 1-31");
    EXAMPLE_REQUIRE(dt.hour <= 23 && dt.min <= 59 && dt.sec <= 60, "sec allows 60 for leap seconds");
    EXAMPLE_REQUIRE(dt.weekday <= 6, "weekday is 0-6 with 0 = Sunday");

    /* proven_time_now_datetime() 은 위의 두 호출을 하나로 합친 것이다. 달력 꼴만
     * 필요할 때 쓴다. */
    proven_datetime_t now = proven_time_now_datetime();
    EXAMPLE_REQUIRE(now.year == dt.year, "both routes read the same clock");

    /* --- 시각을 글자로 ------------------------------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "a 64-byte string is enough for a timestamp");

    /* 달 이름과 요일 이름은 로케일이 준다. proven_time_locale_en 이 내장된 영어
     * 로케일이다. 다른 언어로 찍으려면 여러분의 것을 건넨다. */
    proven_err_t err = proven_time_u8_fmt(alloc, &s.value, dt, &proven_time_locale_en,
                                          "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2}");
    EXAMPLE_REQUIRE(proven_is_ok(err), "formatting a datetime should succeed");

    proven_u8str_view_t out = proven_u8str_as_view(&s.value);
    EXAMPLE_REQUIRE(out.size == 19, "year-month-day hour:min:sec is exactly 19 characters");
    EXAMPLE_REQUIRE(out.ptr[4] == '-' && out.ptr[7] == '-' && out.ptr[13] == ':',
                    "the separators land where the pattern put them");

    proven_println("formatted: {}", PROVEN_ARG(out));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
