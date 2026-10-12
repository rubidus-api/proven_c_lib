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

/*
 * Internal: "after this round". Not public API.
 *
 * Something on the loop's thread that wants to be called back soon, from the loop and not
 * from where it stands - a connection whose caller has just given it something to send - has
 * had two ways: a post, which wakes the loop with a system call, and asking to be told the
 * socket is writable, which is two changes of the socket's watch and a turn of the loop. This
 * is the third, and costs none of that: `fn` runs when the round being delivered has been
 * delivered, or, asked for between rounds, at the start of the next before the loop sleeps.
 *
 * Loop thread only. Asking again while one is waiting changes nothing (it runs once, with
 * the first `fn` and `ctx`). They run in the order asked. One asked for from inside another
 * runs in the same round, twice over at most; after that at the next round, which does not
 * sleep first. Cancel before the memory goes.
 */
typedef struct proven_loop_after {
    struct proven_loop_after *next, *prev;
    proven_loop_fn fn;
    void *ctx;
    bool waiting;
} proven_loop_after_t;
void proven_loop_after_(proven_loop_t *loop, proven_loop_after_t *after, proven_loop_fn fn, void *ctx);
void proven_loop_after_cancel_(proven_loop_t *loop, proven_loop_after_t *after);

#endif /* PROVEN_INTERNAL_LOOP_H */
