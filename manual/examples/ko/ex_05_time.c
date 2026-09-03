#include "example.h"

/*
 * Time comes in two flavours that look identical and are not, and picking the
 * wrong one is the classic timing bug.
 *
 *   - A WALL CLOCK answers "what time is it?". It is what a user wants to see,
 *     and it is allowed to jump: NTP corrects it, daylight saving shifts it, an
 *     administrator sets it. Measuring a duration with it can produce a negative
 *     elapsed time, and did, famously, on leap-second days.
 *
 *   - A MONOTONIC clock answers "how long since?". It only moves forward, at a
 *     steady rate, and has no relationship to any calendar. It is what you time
 *     an operation with.
 *
 * libc blurs this. time() is wall clock in whole seconds - useless for
 * measurement. clock() measures CPU time, not elapsed time, so a program that
 * sleeps looks instantaneous. Neither name tells you which of the two questions
 * it is answering.
 *
 * proven_time_now() is nanoseconds since the Unix epoch: one number that both
 * formats as a date and subtracts as a duration, at a resolution fine enough to
 * time real work.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- as a duration ------------------------------------------------- */
    proven_time_t start = proven_time_now();
    proven_time_sleep(15);                 /* milliseconds */
    proven_time_t end = proven_time_now();

    proven_i64 elapsed_ns = end - start;
    EXAMPLE_REQUIRE(elapsed_ns > 0, "time must move forward across a sleep");
    /* Sleep guarantees AT LEAST the requested time, never at most: the scheduler
     * decides when you actually run again. Asserting an upper bound here would
     * be a test that fails on a busy machine, which is why this one does not. */
    EXAMPLE_REQUIRE(elapsed_ns >= 10 * 1000 * 1000,
                    "sleeping 15ms must take at least ~10ms of wall time");

    /* --- as a date ------------------------------------------------------ */
    proven_datetime_t dt = proven_time_breakdown(start);
    EXAMPLE_REQUIRE(dt.year >= 2020 && dt.year < 3000, "the epoch breakdown gives a plausible year");
    EXAMPLE_REQUIRE(dt.month >= 1 && dt.month <= 12, "month is 1-12, not 0-11 as in libc's tm");
    EXAMPLE_REQUIRE(dt.day >= 1 && dt.day <= 31, "day is 1-31");
    EXAMPLE_REQUIRE(dt.hour <= 23 && dt.min <= 59 && dt.sec <= 60, "sec allows 60 for leap seconds");
    EXAMPLE_REQUIRE(dt.weekday <= 6, "weekday is 0-6 with 0 = Sunday");

    /* proven_time_now_datetime() is the two calls above in one, for when you
     * only want the calendar form. */
    proven_datetime_t now = proven_time_now_datetime();
    EXAMPLE_REQUIRE(now.year == dt.year, "both routes read the same clock");

    /* --- formatting a timestamp ---------------------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "a 64-byte string is enough for a timestamp");

    /* The locale supplies month and weekday names; proven_time_locale_en is the
     * built-in English one. Pass your own to render other languages. */
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
