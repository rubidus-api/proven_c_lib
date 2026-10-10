#ifndef PROVEN_INTERNAL_LOOP_H
#define PROVEN_INTERNAL_LOOP_H

#include "proven/loop.h"
#include "proven/time.h"

/*
 * Internal: the loop's clock, for tests. Not public API.
 *
 * Timers are measured on the monotonic clock. A test of a timer that is a minute away cannot
 * wait a minute, so it gives the loop a clock of its own and moves it. Every reading the loop
 * makes - where the wheel stands, when a timer is due - goes through this one function.
 *
 * Call it on a loop with no timer set: the wheel is put where the new clock says "now" is.
 * With such a clock, proven_loop_poll must be given PROVEN_NET_DONT_WAIT: how long to sleep
 * is still decided against the real clock.
 */
typedef proven_time_t (*proven_loop_clock_fn)(void *ctx);
void proven_loop_test_clock_(proven_loop_t *loop, proven_loop_clock_fn clock, void *ctx);

#endif /* PROVEN_INTERNAL_LOOP_H */
