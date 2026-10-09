#include "proven.h"
#include "proven_test.h"

/*
 * The monotonic clock: the one a duration, a timeout or a deadline is measured with.
 *
 * What can be asserted without a second clock to compare against: readings never decrease, a
 * sleep is seen as at least roughly that long, and the clock is not the wall clock in disguise -
 * a reading is not a plausible date. An upper bound on a sleep is not asserted beyond "not
 * minutes": the scheduler decides, and a loaded machine must not fail this test.
 */

int main(void) {
    PROVEN_TEST_SUITE("the monotonic clock",
        "proven_time_monotonic_now never goes backwards and measures a sleep.",
        "Inspect proven_sys_time_monotonic_ns in platform/proven_sys_time.c: CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter scaled by its frequency on Windows.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("readings never decrease",
        "A hundred thousand consecutive readings, each at least the one before.",
        "A decrease means the source is a settable clock, or the Windows scaling lost the high part.");
    // ---------------------------------------------------------------
    {
        proven_time_t prev = proven_time_monotonic_now();
        bool ok = true;
        bool moved = false;
        for (int i = 0; i < 100000; ++i) {
            proven_time_t now = proven_time_monotonic_now();
            if (now < prev) ok = false;
            if (now > prev) moved = true;
            prev = now;
        }
        PROVEN_TEST_ASSERT(ok, "no reading is smaller than the one before it", "");
        PROVEN_TEST_ASSERT(moved, "the clock advances while it is being read", "A clock that returns one value for ever passes the first check and times nothing.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a sleep is measured",
        "Sleeping 30 ms is seen as at least 25 ms and less than a minute.",
        "Too short: the unit is wrong (microseconds, or ticks not scaled). Absurdly long: the scaling overflowed.");
    // ---------------------------------------------------------------
    {
        proven_time_t t0 = proven_time_monotonic_now();
        proven_time_sleep(30);
        proven_i64 elapsed = proven_time_monotonic_now() - t0;
        PROVEN_TEST_ASSERT(elapsed >= 25 * 1000000LL, "at least 25 ms of nanoseconds passed", "");
        PROVEN_TEST_ASSERT(elapsed < 60 * 1000000000LL, "and less than a minute", "");
    }

    PROVEN_TEST_PASS("the monotonic clock is non-decreasing and counts nanoseconds.");
    return 0;
}
