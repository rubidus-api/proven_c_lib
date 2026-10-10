#include "proven.h"
#include "proven_test.h"
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

static void note(void *ctx) { if (g_order_n < 32) g_order[g_order_n++] = (int)(proven_intptr_t)ctx; }

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

/* Sockets. */
typedef struct {
    proven_loop_io_t io;
    proven_net_conn_t sock;
    int readable, writable;
    proven_byte_t got[64];
    proven_size_t got_len;
    proven_loop_io_t *also_remove;
    bool eof;
} peer_t;

static void peer_ready(void *ctx, proven_u8 got) {
    peer_t *p = ctx;
    if (got & PROVEN_NET_WRITABLE) p->writable++;
    if (got & PROVEN_NET_READABLE) {
        p->readable++;
        proven_result_size_t r = proven_net_read(&p->sock, (proven_mem_mut_t){ .ptr = p->got + p->got_len, .size = sizeof p->got - p->got_len }, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_OK) p->got_len += r.value;
        if (r.err == PROVEN_ERR_EOF) { p->eof = true; proven_loop_io_remove(g_loop, &p->io); }
    }
    if (p->also_remove) { proven_loop_io_remove(g_loop, p->also_remove); p->also_remove = NULL; }
}

typedef struct { proven_loop_io_t io; proven_net_listener_t listener; proven_net_conn_t accepted[4]; int count; } acceptor_t;
static void acceptor_ready(void *ctx, proven_u8 got) {
    acceptor_t *a = ctx;
    (void)got;
    while (a->count < 4 && proven_net_accept(&a->listener, PROVEN_NET_DONT_WAIT, &a->accepted[a->count], NULL) == PROVEN_OK) a->count++;
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
static void forgotten_io(void *ctx, proven_u8 got) { (void)ctx; (void)got; g_forgotten_calls++; }
static void stopper(void *arg) { (void)arg; proven_time_sleep(60); proven_loop_stop(g_loop); }

int main(void) {
    PROVEN_TEST_SUITE("the event loop",
        "Timers that fire in order and survive being cancelled from each other's callbacks, sockets watched and unwatched mid-round, and work posted from other threads.",
        "Inspect src/proven/loop.c: loop_timers_advance for timers, proven_loop_io_remove for the batch, proven_loop_post and loop_run_tasks for posting.");

    proven_allocator_t heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_loop_create(heap, &g_loop) == PROVEN_OK && g_loop != NULL, "a loop", "");
    proven_loop_t *none = (proven_loop_t *)1;
    PROVEN_TEST_ASSERT(proven_loop_create(heap, NULL) == PROVEN_ERR_INVALID_ARG && proven_loop_create((proven_allocator_t){ 0 }, &none) == PROVEN_ERR_INVALID_ARG && none == NULL,
        "no out pointer, and no allocator, are PROVEN_ERR_INVALID_ARG", "");
    proven_mem_mut_t scratch = proven_loop_scratch(g_loop);
    PROVEN_TEST_ASSERT(scratch.ptr != NULL && scratch.size == 65536 && proven_loop_io_count(g_loop) == 0, "a 64 KiB scratch buffer, and nothing watched", "");
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
        PROVEN_TEST_ASSERT(g_order_n == 0, "nothing fires before its time", "");
        spin(180);
        PROVEN_TEST_ASSERT(g_order_n == 3 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3, "three timers fire in the order of their times; the cancelled one never does", "");
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
        spin(90);
        PROVEN_TEST_ASSERT(g_order_n == 0, "a timer set again waits for its new time", "");
        spin(120);
        PROVEN_TEST_ASSERT(g_order_n == 1 && g_order[0] == 2, "and fires once, with what it was last given", "");

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
    PROVEN_TEST_SECTION("sockets",
        "A watched socket's function is called when it is ready and not otherwise; removing a socket silences events already collected for it.",
        "Check proven_loop_io_add/_set/_remove and the batch handling in proven_loop_poll.");
    {
        acceptor_t acc;
        memset(&acc, 0, sizeof acc);
        proven_net_addr_t at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &acc.listener, &at) == PROVEN_OK, "a listener", "");
        PROVEN_TEST_ASSERT(proven_loop_io_add(g_loop, &acc.io, proven_net_listener_handle(&acc.listener), PROVEN_NET_READABLE, acceptor_ready, &acc) == PROVEN_OK &&
                           proven_loop_io_count(g_loop) == 1, "watched", "");
        PROVEN_TEST_ASSERT(proven_loop_io_add(g_loop, &acc.io, proven_net_listener_handle(&acc.listener), PROVEN_NET_READABLE, acceptor_ready, &acc) == PROVEN_ERR_INVALID_STATE,
            "registering the same record twice is PROVEN_ERR_INVALID_STATE", "");
        proven_net_conn_t out1, out2;
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(3000), &out1) == PROVEN_OK && proven_net_connect(at, proven_net_deadline_in(3000), &out2) == PROVEN_OK, "two connections", "");
        spin(60);
        PROVEN_TEST_ASSERT(acc.count == 2, "the listener's function accepted both", "");

        peer_t a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.sock = acc.accepted[0]; b.sock = acc.accepted[1];
        PROVEN_TEST_ASSERT(proven_loop_io_add(g_loop, &a.io, proven_net_conn_handle(&a.sock), PROVEN_NET_READABLE, peer_ready, &a) == PROVEN_OK &&
                           proven_loop_io_add(g_loop, &b.io, proven_net_conn_handle(&b.sock), 0, peer_ready, &b) == PROVEN_OK && proven_loop_io_count(g_loop) == 3, "both watched, the second for nothing yet", "");
        spin(40);
        PROVEN_TEST_ASSERT(a.readable == 0 && b.readable == 0 && a.writable == 0, "nothing is ready, so nothing is called", "");
        PROVEN_TEST_ASSERT(proven_net_write_all(&out1, (proven_mem_view_t){ (const proven_byte_t *)"first", 5 }, proven_net_deadline_in(2000)).err == PROVEN_OK &&
                           proven_net_write_all(&out2, (proven_mem_view_t){ (const proven_byte_t *)"second", 6 }, proven_net_deadline_in(2000)).err == PROVEN_OK, "something is sent to each", "");
        spin(60);
        PROVEN_TEST_ASSERT(a.readable >= 1 && a.got_len == 5 && memcmp(a.got, "first", 5) == 0 && b.readable == 0, "the one watched for reading reads; the one watched for nothing is left alone", "");
        PROVEN_TEST_ASSERT(proven_loop_io_set(g_loop, &b.io, PROVEN_NET_READABLE | PROVEN_NET_WRITABLE) == PROVEN_OK, "now watch the second for both", "");
        spin(40);
        PROVEN_TEST_ASSERT(b.got_len == 6 && b.writable >= 1, "it reads what was waiting and is told it may write", "");
        PROVEN_TEST_ASSERT(proven_loop_io_set(g_loop, &b.io, PROVEN_NET_READABLE) == PROVEN_OK, "back to reading only", "");
        int writable_before = b.writable;
        spin(40);
        PROVEN_TEST_ASSERT(b.writable == writable_before, "no longer told it may write", "");

        /* Both become readable before the next round; whichever is delivered first removes the
         * other. The other must then not be called in that round, although its event was
         * already collected. */
        a.also_remove = &b.io; b.also_remove = &a.io;
        int ra = a.readable, rb = b.readable;
        PROVEN_TEST_ASSERT(proven_net_write_all(&out1, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 }, proven_net_deadline_in(2000)).err == PROVEN_OK &&
                           proven_net_write_all(&out2, (proven_mem_view_t){ (const proven_byte_t *)"y", 1 }, proven_net_deadline_in(2000)).err == PROVEN_OK, "both are sent to", "");
        proven_time_sleep(50);
        spin(60);
        PROVEN_TEST_ASSERT((a.readable - ra) + (b.readable - rb) == 1 && proven_loop_io_count(g_loop) == 2, "exactly one was delivered: the removed one's collected event was dropped", "");
        proven_loop_io_remove(g_loop, &a.io); proven_loop_io_remove(g_loop, &b.io);
        proven_loop_io_remove(g_loop, &a.io);
        PROVEN_TEST_ASSERT(proven_loop_io_count(g_loop) == 1, "removing what is not registered is harmless", "");

        /* The peer closing is readable, and reads as the end. */
        memset(&a, 0, sizeof a);
        a.sock = acc.accepted[0];
        PROVEN_TEST_ASSERT(proven_loop_io_add(g_loop, &a.io, proven_net_conn_handle(&a.sock), PROVEN_NET_READABLE, peer_ready, &a) == PROVEN_OK, "watched again", "");
        (void)proven_net_close(&out1);
        spin(80);
        PROVEN_TEST_ASSERT(a.eof && !a.io.registered, "a closed peer is delivered as readable, and the function unwatched itself on the end of stream", "");

        proven_loop_io_remove(g_loop, &acc.io);
        (void)proven_net_close(&out2); (void)proven_net_close(&acc.accepted[0]); (void)proven_net_close(&acc.accepted[1]);
        (void)proven_net_listener_close(&acc.listener);
        proven_loop_io_t bad = { 0 };
        PROVEN_TEST_ASSERT(proven_loop_io_add(g_loop, &bad, (proven_net_handle_t){ 0 }, PROVEN_NET_READABLE, peer_ready, &a) == PROVEN_ERR_INVALID_ARG &&
                           proven_loop_io_set(g_loop, &bad, PROVEN_NET_READABLE) == PROVEN_ERR_INVALID_ARG, "a handle that is not one, and a record that is not registered, are PROVEN_ERR_INVALID_ARG", "");
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

    /* Destroying a loop with a timer set and a socket watched calls neither. */
    {
        proven_loop_t *other = NULL;
        proven_loop_timer_t t = { 0 };
        proven_loop_io_t io = { 0 };
        proven_net_listener_t listener;
        PROVEN_TEST_ASSERT(proven_loop_create(heap, &other) == PROVEN_OK &&
                           proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &listener, NULL) == PROVEN_OK, "a second loop and a listener", "");
        proven_loop_timer_set(other, &t, 10, forgotten, NULL);
        PROVEN_TEST_ASSERT(proven_loop_io_add(other, &io, proven_net_listener_handle(&listener), PROVEN_NET_READABLE, forgotten_io, NULL) == PROVEN_OK &&
                           proven_loop_io_count(other) == 1, "with a timer set and a socket watched", "");
        proven_time_sleep(40);
        proven_loop_destroy(other);
        PROVEN_TEST_ASSERT(g_forgotten_calls == 0, "destroying it calls nothing, though the timer was due", "");
        (void)proven_net_listener_close(&listener);
    }

    proven_loop_destroy(g_loop);
    proven_loop_destroy(NULL);
    PROVEN_TEST_PASS("timers, sockets and posted work behave, also when callbacks act on each other.");
    return 0;
}
