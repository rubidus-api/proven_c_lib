#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_loop.h"
#include <stdatomic.h>
#include <string.h>

/*
 * The event loop: timers, sockets, and functions posted from other threads.
 *
 * Most cases drive the loop a round at a time with proven_loop_poll, so that the test decides
 * when time passes; the last ones let it run on its own and stop it from outside.
 */

static proven_loop_t *g_loop;
static int g_order[32];
static int g_order_n;

static proven_time_t g_fake_now;
static proven_time_t fake_clock(void *ctx) { (void)ctx; return g_fake_now; }

static proven_time_t g_fired_at[32];             /* when each note was taken, on the real clock */
static void note(void *ctx) { if (g_order_n < 32) { g_fired_at[g_order_n] = proven_time_monotonic_now(); g_order[g_order_n++] = (int)(proven_intptr_t)ctx; } }

static void spin(proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (proven_time_monotonic_now() < until) {
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, until) == PROVEN_OK, "a round", "");
    }
}

/* Timers that act on other timers from inside their callbacks. */
static proven_loop_timer_t g_ta, g_tb, g_tc, g_tick;
static int g_ticks;
static void cancel_b(void *ctx) { note(ctx); proven_loop_timer_cancel(g_loop, &g_tb); }
static void tick_again(void *ctx) {
    (void)ctx;
    if (++g_ticks < 3) proven_loop_timer_set(g_loop, &g_tick, 20, tick_again, NULL);
}

/* Posting from other threads. */
static atomic_int g_posted_ran;
static atomic_int g_wrong_thread;
static proven_u64 g_loop_thread_marker;
static _Thread_local proven_u64 t_marker;
static atomic_int g_out_of_order;
static int g_next_from[2];              /* touched on the loop's thread only */
static void posted(void *ctx) {
    /* ctx is the poster (0 or 1) and that poster's count: each must arrive in its own order. */
    uintptr_t tag = (uintptr_t)ctx;
    int who = (int)(tag >> 16), nth = (int)(tag & 0xffff);
    if (g_next_from[who] != nth) atomic_fetch_add(&g_out_of_order, 1);
    g_next_from[who] = nth + 1;
    if (t_marker != g_loop_thread_marker) atomic_fetch_add(&g_wrong_thread, 1);
    if (atomic_fetch_add(&g_posted_ran, 1) + 1 == 200) proven_loop_stop(g_loop);
}
static void poster(void *arg) {
    uintptr_t who = (uintptr_t)arg;
    for (uintptr_t i = 0; i < 100; ++i) {
        while (proven_loop_post(g_loop, posted, (void *)(who << 16 | i)) != PROVEN_OK) { }
    }
}

/* A thousand timers at once. */
enum { MANY = 1000 };
typedef struct { proven_loop_timer_t timer; proven_time_t due; } many_t;
static many_t g_many[MANY];
static int g_many_fired, g_many_early;
static void many_fire(void *ctx) {
    many_t *m = ctx;
    g_many_fired++;
    if (proven_time_monotonic_now() < m->due) g_many_early++;
}
static int g_forgotten_calls;
static void forgotten(void *ctx) { (void)ctx; g_forgotten_calls++; }
static void forgotten_acc(void *ctx, proven_err_t err, proven_net_conn_t *conn) { (void)ctx; (void)err; (void)conn; g_forgotten_calls++; }
/* "After this round": what each one does when it runs. */
static proven_loop_after_t g_after[5];
static int g_after_log[16], g_after_n;
static proven_loop_timer_t g_after_timer;
static void after_note(void *ctx) { if (g_after_n < 16) g_after_log[g_after_n++] = (int)(proven_size_t)ctx; }
static void after_asks_4(void *ctx) { after_note(ctx); proven_loop_after_(g_loop, &g_after[4], after_note, (void *)(proven_size_t)4); }
static void after_cancels_2(void *ctx) { after_note(ctx); proven_loop_after_cancel_(g_loop, &g_after[2]); }
static void after_again(void *ctx) { after_note(ctx); if (g_after_n < 6) proven_loop_after_(g_loop, &g_after[0], after_again, ctx); }
static void after_from_timer(void *ctx) { after_note(ctx); proven_loop_after_(g_loop, &g_after[1], after_note, (void *)(proven_size_t)1); }

static void stopper(void *arg) { (void)arg; proven_time_sleep(60); proven_loop_stop(g_loop); }

int main(void) {
    PROVEN_TEST_SUITE("the event loop",
        "Timers that fire in order and survive being cancelled from each other's callbacks, and work posted from other threads. (The operations on sockets: test_unit_loop_ops.)",
        "Inspect src/proven/loop.c: loop_timers_advance for timers, proven_loop_post and loop_run_tasks for posting.");

    proven_allocator_t heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(heap, &g_loop) == PROVEN_OK && g_loop != NULL, "a loop", "");
    proven_loop_t *none = (proven_loop_t *)1;
    PROVEN_TEST_ASSERT(proven_loop_create(heap, NULL) == PROVEN_ERR_INVALID_ARG && proven_loop_create((proven_allocator_t){ 0 }, &none) == PROVEN_ERR_INVALID_ARG && none == NULL,
        "no out pointer, and no allocator, are PROVEN_ERR_INVALID_ARG", "");
    proven_mem_mut_t scratch = proven_loop_scratch(g_loop);
    PROVEN_TEST_ASSERT(scratch.ptr != NULL && scratch.size == 65536, "a 64 KiB scratch buffer", "");
    memset(scratch.ptr, 0xa5, scratch.size);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("timers",
        "They fire once, in the order of their times; a timer may cancel another or set itself again from its callback.",
        "Check loop_timers_advance: due timers are moved to their own list before any is fired.");
    {
        proven_loop_timer_t t1 = { 0 }, t2 = { 0 }, t3 = { 0 }, never = { 0 };
        proven_loop_timer_set(g_loop, &t3, 120, note, (void *)3);
        proven_loop_timer_set(g_loop, &t1, 30, note, (void *)1);
        proven_loop_timer_set(g_loop, &t2, 70, note, (void *)2);
        proven_loop_timer_set(g_loop, &never, 50, note, (void *)99);
        PROVEN_TEST_ASSERT(proven_loop_timer_is_set(&t1) && proven_loop_timer_is_set(&never), "set", "");
        proven_loop_timer_cancel(g_loop, &never);
        proven_loop_timer_cancel(g_loop, &never);
        PROVEN_TEST_ASSERT(!proven_loop_timer_is_set(&never), "cancelled, and cancelling again is harmless", "");
        proven_time_t t0 = proven_time_monotonic_now();
        spin(20);
        /* Twenty milliseconds on, the 30 ms timer has not fired - unless this thread was kept
         * from running for longer than that, which a loaded machine may do. What must hold
         * either way is judged on the clock, below: no timer before its own time. */
        PROVEN_TEST_ASSERT(g_order_n == 0 || proven_time_monotonic_now() - t0 >= 30000000, "nothing fires before its time", "");
        spin(180);
        PROVEN_TEST_ASSERT(g_order_n == 3 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3, "three timers fire in the order of their times; the cancelled one never does", "");
        /* The timers were set a hair before t0, so their times are a hair before t0 plus the
         * delay: a millisecond of allowance covers that. */
        static const proven_time_t due[3] = { 30000000, 70000000, 120000000 };
        bool on_time = true;
        for (int i = 0; i < 3; ++i) on_time = on_time && g_fired_at[i] - t0 >= due[i] - 1000000;
        PROVEN_TEST_ASSERT(on_time, "and each at or after its own time, by the clock", "");
        PROVEN_TEST_ASSERT(!proven_loop_timer_is_set(&t1) && proven_time_monotonic_now() - t0 < 1500000000, "a fired timer is no longer set", "");
        spin(150);
        PROVEN_TEST_ASSERT(g_order_n == 3, "and fires once", "");

        /* A and B are due in the same tick; A's callback cancels B. Whichever the wheel reaches
         * first, B must not fire after being cancelled - and C, due with them, must. */
        g_order_n = 0;
        proven_loop_timer_set(g_loop, &g_tb, 40, note, (void *)20);
        proven_loop_timer_set(g_loop, &g_ta, 40, cancel_b, (void *)10);
        proven_loop_timer_set(g_loop, &g_tc, 40, note, (void *)30);
        spin(120);
        bool a_ran = false, c_ran = false, b_after_a = false;
        for (int i = 0; i < g_order_n; ++i) {
            if (g_order[i] == 10) a_ran = true;
            if (g_order[i] == 30) c_ran = true;
            if (g_order[i] == 20 && a_ran) b_after_a = true;
        }
        PROVEN_TEST_ASSERT(a_ran && c_ran && !b_after_a && !proven_loop_timer_is_set(&g_tb), "a timer cancelled by another that was due with it does not fire afterwards", "");

        proven_loop_timer_set(g_loop, &g_tick, 20, tick_again, NULL);
        spin(200);
        PROVEN_TEST_ASSERT(g_ticks == 3, "a timer that sets itself again from its callback repeats", "");

        /* Setting a set timer moves it. */
        g_order_n = 0;
        proven_loop_timer_set(g_loop, &t1, 30, note, (void *)1);
        proven_loop_timer_set(g_loop, &t1, 150, note, (void *)2);
        proven_time_t moved_at = proven_time_monotonic_now();
        spin(90);
        PROVEN_TEST_ASSERT(g_order_n == 0 || g_fired_at[0] - moved_at >= 149000000, "a timer set again waits for its new time", "");
        spin(120);
        PROVEN_TEST_ASSERT(g_order_n == 1 && g_order[0] == 2, "and fires once, with what it was last given", "");

        /* Precision (RFC-0019 T-1): a timer fires at the time asked for, not at the next 16 ms
         * boundary - on POSIX within a few milliseconds (12 here, loose for a loaded host; the
         * wheel alone fired a 3 ms timer 16 to 32 ms late). Windows waits at its system timer's
         * interval, so there only "not early" is checked. */
        {
            int early = 0, late = 0;
            for (int k = 0; k < 5; ++k) {
                g_order_n = 0;
                const proven_time_t set_at = proven_time_monotonic_now();
                proven_loop_timer_set(g_loop, &t1, 3, note, (void *)1);
                for (int i = 0; i < 200 && g_order_n == 0; ++i) (void)proven_loop_poll(g_loop, proven_net_deadline_in(50));
                const proven_time_t took = g_fired_at[0] - set_at;
                if (took < 3000000) ++early;
                if (took >= 12000000) ++late;
            }
            PROVEN_TEST_ASSERT(early == 0, "a 3 ms timer never fires before 3 ms", "");
#if !defined(_WIN32)
            PROVEN_TEST_ASSERT(late <= 1, "and fires within 12 ms (at least four times in five): not rounded up to the wheel's 16 ms", "");
#else
            (void)late;
#endif
            /* Two timers in one 16 ms slot, both due by the time the loop looks: earliest first. */
            proven_loop_timer_t a5 = { 0 }, a3 = { 0 };
            g_order_n = 0;
            /* Set earliest first: a slot's list then holds them latest first. */
            proven_loop_timer_set(g_loop, &a3, 3, note, (void *)3);
            proven_loop_timer_set(g_loop, &a5, 5, note, (void *)5);
            proven_time_sleep(20);
            for (int i = 0; i < 20 && g_order_n < 2; ++i) (void)proven_loop_poll(g_loop, proven_net_deadline_in(20));
            PROVEN_TEST_ASSERT(g_order_n == 2 && g_order[0] == 3 && g_order[1] == 5, "two timers of one slot that came due together fire earliest first", "");
            /* A timer one turn of the wheel away (16,385 ms: 1,024 slots of 16 ms and a bit)
             * lands in the slot the wheel stands in, as a later turn's. Polling must sleep until
             * the deadline asked for, not return at once: a handful of returns in 200 ms, not
             * thousands (the first cut of T-1 named the current slot's past boundary). */
            static const proven_u32 away[2] = { 16385, 32769 };      /* one turn, and two (no timer of the scanned turn at all) */
            for (int k = 0; k < 2; ++k) {
                proven_loop_timer_t far = { 0 };
                g_order_n = 0;
                proven_loop_timer_set(g_loop, &far, away[k], note, (void *)99);
                const proven_time_t until = proven_time_monotonic_now() + 200000000;
                int returns = 0;
                while (proven_time_monotonic_now() < until && returns < 100000) { (void)proven_loop_poll(g_loop, proven_net_deadline_in(50)); ++returns; }
                proven_loop_timer_cancel(g_loop, &far);
                PROVEN_TEST_ASSERT(returns <= 10 && g_order_n == 0, k == 0 ? "a timer a turn away does not wake the loop: no more than a handful of returns in 200 ms" : "nor one two turns away", "");
            }
        }

        /* Timers that come due while the loop was not looking - here it is simply not polled
         * for 200 ms - fire together, and still earliest first. */
        {
            proven_loop_timer_t late[5] = { 0 };
            static const proven_u32 at[5] = { 120, 20, 90, 50, 150 };
            g_order_n = 0;
            for (int i = 0; i < 5; ++i) proven_loop_timer_set(g_loop, &late[i], at[i], note, (void *)(proven_uintptr_t)at[i]);
            proven_time_sleep(200);
            spin(40);
            PROVEN_TEST_ASSERT(g_order_n == 5 && g_order[0] == 20 && g_order[1] == 50 && g_order[2] == 90 && g_order[3] == 120 && g_order[4] == 150,
                "timers that came due together fire in the order of their times", "");
        }

        /* Many at once, at times spread over 300 ms: each fires once, and none early. */
        for (int i = 0; i < MANY; ++i) {
            proven_u32 ms = (proven_u32)(i * 37 % 300 + 1);
            g_many[i].due = proven_time_monotonic_now() + (proven_time_t)ms * 1000000;
            proven_loop_timer_set(g_loop, &g_many[i].timer, ms, many_fire, &g_many[i]);
        }
        for (int i = 0; i < 100 && g_many_fired < MANY; ++i) spin(50);
        int still_set = 0;
        for (int i = 0; i < MANY; ++i) still_set += proven_loop_timer_is_set(&g_many[i].timer) ? 1 : 0;
        PROVEN_TEST_ASSERT(g_many_fired == MANY && g_many_early == 0 && still_set == 0, "a thousand timers set at once each fire once, none of them early", "");
        spin(60);
        PROVEN_TEST_ASSERT(g_many_fired == MANY, "and none fires a second time", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("posting and stopping",
        "Functions posted from any thread run on the loop's thread, in order; a stop from outside ends a waiting loop.",
        "Check proven_loop_post, loop_run_tasks and proven_loop_stop.");
    {
        g_order_n = 0;
        for (int i = 1; i <= 5; ++i) PROVEN_TEST_ASSERT(proven_loop_post(g_loop, note, (void *)(proven_intptr_t)i) == PROVEN_OK, "post", "");
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_order_n == 5 && g_order[0] == 1 && g_order[4] == 5,
            "five functions posted from this thread run in the order posted, in the next round", "");
        PROVEN_TEST_ASSERT(proven_loop_post(g_loop, NULL, NULL) == PROVEN_ERR_INVALID_ARG && proven_loop_post(NULL, note, NULL) == PROVEN_ERR_INVALID_ARG, "no function, no loop", "");

        proven_job_sys_t *jobs = NULL;
        proven_job_group_t group;
        proven_job_group_init(&group);
        PROVEN_TEST_ASSERT(proven_job_system_init(heap, 2, 8, &jobs) == PROVEN_OK, "two other threads", "");
        t_marker = 0x1234567;
        g_loop_thread_marker = t_marker;
        PROVEN_TEST_ASSERT(proven_job_group_submit(jobs, &group, poster, (void *)0) == PROVEN_OK && proven_job_group_submit(jobs, &group, poster, (void *)1) == PROVEN_OK, "each posts a hundred", "");
        proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(proven_loop_run(g_loop) == PROVEN_OK, "the loop runs until the two-hundredth stops it", "");
        proven_job_group_wait(jobs, &group);
        PROVEN_TEST_ASSERT(atomic_load(&g_posted_ran) == 200 && atomic_load(&g_wrong_thread) == 0 && proven_time_monotonic_now() - t0 < 5000000000LL,
            "all two hundred ran, every one on the loop's thread", "");
        PROVEN_TEST_ASSERT(atomic_load(&g_out_of_order) == 0 && g_next_from[0] == 100 && g_next_from[1] == 100, "and each thread's hundred in the order that thread posted them", "");

        proven_job_group_init(&group);
        PROVEN_TEST_ASSERT(proven_job_group_submit(jobs, &group, stopper, NULL) == PROVEN_OK, "a thread that will stop the loop", "");
        t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(proven_loop_run(g_loop) == PROVEN_OK, "a loop with nothing to do waits, and returns when stopped from outside", "");
        proven_time_t waited = proven_time_monotonic_now() - t0;
        PROVEN_TEST_ASSERT(waited > 30000000 && waited < 2000000000, "it was asleep until then, not spinning and not stuck", "");
        proven_job_group_wait(jobs, &group);
        proven_job_system_close(jobs);
        proven_job_system_destroy(jobs);
    }

    /* The scratch buffer was not touched by the loop itself. */
    bool intact = true;
    for (proven_size_t i = 0; i < scratch.size; i += 4099) intact = intact && scratch.ptr[i] == 0xa5;
    PROVEN_TEST_ASSERT(intact, "the scratch buffer is the callbacks' to use: the loop wrote nothing to it", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("timers further away than one turn of the wheel",
        "The wheel has 1,024 slots of 16 ms: 16.4 s. A timer beyond that waits in its slot and is passed over until its own turn. The loop is given a clock the test moves.",
        "Check loop_timers_advance: a timer is taken only when its tick has come, not when its slot is visited; and after a long jump every slot is visited once.");
    // ---------------------------------------------------------------
    {
        proven_loop_t *far = NULL;
        PROVEN_TEST_ASSERT(proven_loop_create(heap, &far) == PROVEN_OK, "a loop", "");
        g_fake_now = 1000000000000LL;
        proven_loop_test_clock_(far, fake_clock, NULL);
        static const proven_u32 at_ms[4] = { 10000, 20000, 40000, 100000 };
        proven_loop_timer_t t[4] = { 0 };
        g_order_n = 0;
        for (int i = 0; i < 4; ++i) proven_loop_timer_set(far, &t[i], at_ms[i], note, (void *)(proven_intptr_t)(i + 1));
        /* Step the clock in pieces of 250 ms, as a loop that is busy would see it, and record
         * the time on that clock at which each timer fired. */
        proven_time_t start = g_fake_now, fired_at[4] = { 0 };
        int seen = 0;
        bool early = false;
        for (int step = 0; step < 440 && seen < 4; ++step) {
            g_fake_now += 250000000LL;
            PROVEN_TEST_ASSERT(proven_loop_poll(far, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a round", "");
            while (seen < g_order_n) {
                int which = g_order[seen] - 1;
                fired_at[which] = g_fake_now - start;
                if (fired_at[which] < (proven_time_t)at_ms[which] * 1000000) early = true;
                seen++;
            }
        }
        PROVEN_TEST_ASSERT(seen == 4 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3 && g_order[3] == 4, "timers at 10, 20, 40 and 100 seconds each fire once, in order", "");
        PROVEN_TEST_ASSERT(!early, "none before its time, although each of the later three is passed over in its slot at least once first", "");
        bool prompt = true;
        for (int i = 0; i < 4; ++i) prompt = prompt && fired_at[i] <= (proven_time_t)at_ms[i] * 1000000 + 300000000LL;
        PROVEN_TEST_ASSERT(prompt, "and each within one step of its time", "");

        /* One jump past everything: each fires exactly once, earliest first. */
        g_order_n = 0;
        for (int i = 0; i < 4; ++i) proven_loop_timer_set(far, &t[i], at_ms[3 - i], note, (void *)(proven_intptr_t)(4 - i));
        g_fake_now += 200000000000LL;
        PROVEN_TEST_ASSERT(proven_loop_poll(far, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_order_n == 4, "after one jump of 200 seconds all four fire in the same round", "");
        PROVEN_TEST_ASSERT(proven_loop_poll(far, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_order_n == 4, "and none of them twice", "");

        /* A timer set far ahead and cancelled leaves nothing behind for a later turn. */
        g_order_n = 0;
        proven_loop_timer_set(far, &t[0], 30000, note, (void *)9);
        g_fake_now += 5000000000LL;
        PROVEN_TEST_ASSERT(proven_loop_poll(far, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a round", "");
        proven_loop_timer_cancel(far, &t[0]);
        g_fake_now += 60000000000LL;
        PROVEN_TEST_ASSERT(proven_loop_poll(far, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_order_n == 0 && !proven_loop_timer_is_set(&t[0]), "a far timer that was cancelled never fires", "");
        proven_loop_destroy(far);
    }

    /* Destroying a loop with a timer set and an accept in flight calls neither. */
    {
        proven_loop_t *other = NULL;
        proven_loop_timer_t t = { 0 };
        static proven_loop_accept_t acc;
        proven_net_listener_t listener;
        PROVEN_TEST_ASSERT(proven_loop_create(heap, &other) == PROVEN_OK &&
                           proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &listener, NULL) == PROVEN_OK, "a second loop and a listener", "");
        proven_loop_timer_set(other, &t, 10, forgotten, NULL);
        PROVEN_TEST_ASSERT(proven_loop_accept(other, &acc, &listener, forgotten_acc, NULL) == PROVEN_OK, "with a timer set and an accept in flight", "");
        proven_time_sleep(40);
        proven_loop_destroy(other);
        PROVEN_TEST_ASSERT(g_forgotten_calls == 0, "destroying it calls nothing, though the timer was due", "");
        (void)proven_net_listener_close(&listener);
    }

    PROVEN_TEST_SECTION("after this round",
        "An internal hook for a connection that wants a round from the loop: no post, no change of a socket's watch.",
        "Check proven_loop_after_, proven_loop_after_cancel_ and loop_run_after in src/proven/loop.c, and where proven_loop_poll calls it.");
    {
        /* Asked for between rounds: they run at the next poll, in the order asked, once each -
         * and a poll that would have slept for ever does not sleep on them. */
        g_after_n = 0;
        proven_loop_after_(g_loop, &g_after[0], after_note, (void *)(proven_size_t)10);
        proven_loop_after_(g_loop, &g_after[1], after_note, (void *)(proven_size_t)11);
        proven_loop_after_(g_loop, &g_after[0], after_note, (void *)(proven_size_t)99);      /* waiting already: nothing changes */
        PROVEN_TEST_ASSERT(g_after_n == 0, "asking runs nothing", "");
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n == 2 && g_after_log[0] == 10 && g_after_log[1] == 11, "the next poll runs both, in the order asked, the one asked for twice once", "");
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n == 2, "and not again", "");
        /* One that asks for another: the other runs in the same poll. */
        g_after_n = 0;
        proven_loop_after_(g_loop, &g_after[0], after_asks_4, (void *)(proven_size_t)20);
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n == 2 && g_after_log[0] == 20 && g_after_log[1] == 4, "one asked for from inside another runs in the same poll", "");
        /* One that cancels a later one; and a cancelled one may be asked for again. */
        g_after_n = 0;
        proven_loop_after_(g_loop, &g_after[0], after_cancels_2, (void *)(proven_size_t)30);
        proven_loop_after_(g_loop, &g_after[2], after_note, (void *)(proven_size_t)32);
        proven_loop_after_(g_loop, &g_after[3], after_note, (void *)(proven_size_t)33);
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n == 2 && g_after_log[0] == 30 && g_after_log[1] == 33, "one cancelled by another before its turn does not run; the one behind it does", "");
        proven_loop_after_cancel_(g_loop, &g_after[2]);                                      /* not waiting: nothing */
        proven_loop_after_(g_loop, &g_after[2], after_note, (void *)(proven_size_t)34);
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n == 3 && g_after_log[2] == 34, "and it can be asked for again", "");
        /* One that asks for itself every time: a poll runs it a bounded number of times and
         * returns, and the next poll goes on without sleeping. */
        g_after_n = 0;
        proven_loop_after_(g_loop, &g_after[0], after_again, (void *)(proven_size_t)40);
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK && g_after_n >= 1 && g_after_n <= 3, "one that asks for itself each time does not keep a poll for ever", "");
        proven_time_t t0 = proven_time_monotonic_now();
        for (int i = 0; i < 10 && g_after_n < 6; ++i) (void)proven_loop_poll(g_loop, proven_net_deadline_in(2000));
        PROVEN_TEST_ASSERT(g_after_n == 6 && proven_time_monotonic_now() - t0 < 1000000000, "and a poll that could sleep two seconds does not sleep while one is waiting", "");
        /* Asked for from a timer: it runs in the poll that fired the timer. */
        g_after_n = 0;
        proven_loop_timer_set(g_loop, &g_after_timer, 1, after_from_timer, (void *)(proven_size_t)50);
        for (int i = 0; i < 200 && g_after_n == 0; ++i) { (void)proven_loop_poll(g_loop, proven_net_deadline_in(5)); if (g_after_n == 1) break; }
        PROVEN_TEST_ASSERT(g_after_n == 2 && g_after_log[0] == 50 && g_after_log[1] == 1, "one asked for by a timer runs in the poll that fired the timer", "");
        /* Asked for between rounds, it runs before the poll looks at anything else: a timer
         * that is already due comes after it. */
        g_after_n = 0;
        proven_loop_timer_set(g_loop, &g_after_timer, 1, after_note, (void *)(proven_size_t)71);
        proven_time_sleep(20);
        proven_loop_after_(g_loop, &g_after[0], after_note, (void *)(proven_size_t)70);
        for (int i = 0; i < 50 && g_after_n < 2; ++i) (void)proven_loop_poll(g_loop, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(g_after_n == 2 && g_after_log[0] == 70 && g_after_log[1] == 71, "one asked for between rounds runs first in the next poll, before a timer that was due", "");
        /* One left waiting when the loop is destroyed is simply not run. */
        g_after_n = 0;
        proven_loop_after_(g_loop, &g_after[0], after_note, (void *)(proven_size_t)60);
    }

    proven_loop_destroy(g_loop);
    proven_loop_destroy(NULL);
    PROVEN_TEST_ASSERT(g_after_n == 0, "one left waiting when its loop is destroyed is not run", "");
    PROVEN_TEST_PASS("timers and posted work behave, also when callbacks act on each other.");
    return 0;
}
