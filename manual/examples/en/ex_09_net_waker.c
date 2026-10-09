#include "example.h"

/*
 * A loop blocked in proven_net_poll sees only sockets. To tell it something from another
 * thread - "there is work for you", "stop" - you need a socket that becomes readable when
 * you say so. That is a waker.
 *
 * proven_net_pair is what a waker is made of, and is useful by itself: two connected ends
 * with no address, for handing bytes between two parts of a program.
 */

static void finish_work(void *arg) {
    proven_net_waker_t *waker = arg;
    proven_time_sleep(20);              /* stands for some work */
    proven_net_waker_wake(waker);       /* any thread may call this */
}

int main(void) {
    /* A pair: what goes into one end comes out of the other. */
    proven_net_conn_t a, b;
    proven_err_t err = proven_net_pair(&a, &b);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a pair opens");
    EXAMPLE_REQUIRE(proven_net_write_all(&a, proven_mem_view_from_u8(PROVEN_LIT("across")), proven_net_deadline_in(1000)).err == PROVEN_OK, "write to one end");
    proven_byte_t buf[16];
    proven_result_size_t got = proven_net_read(&b, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 6, "read from the other");
    (void)proven_net_close(&a);
    (void)proven_net_close(&b);

    /* A waker in a poll list. Nothing is readable, so the poll would wait its full second... */
    proven_net_waker_t waker;
    EXAMPLE_REQUIRE(proven_net_waker_open(&waker) == PROVEN_OK, "a waker opens");
    proven_job_sys_t *jobs = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(proven_heap_allocator(), 1, 4, &jobs) == PROVEN_OK, "a worker thread");
    EXAMPLE_REQUIRE(proven_job_submit_ex(jobs, finish_work, &waker) == PROVEN_OK, "work is handed to it");

    proven_net_poll_item_t item = { .handle = proven_net_waker_handle(&waker), .want = PROVEN_NET_READABLE };
    proven_size_t ready = 0;
    proven_time_t start = proven_time_monotonic_now();
    err = proven_net_poll(&item, 1, proven_net_deadline_in(1000), &ready);
    proven_i64 waited_ms = (proven_time_monotonic_now() - start) / 1000000;
    /* ...but the worker woke it. */
    EXAMPLE_REQUIRE(err == PROVEN_OK && ready == 1 && waited_ms < 900, "the poll returns when the worker says so, not at its deadline");

    /* Drain it, or it stays readable and the next poll returns at once for nothing. */
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "drained: quiet again");

    /* Several wakes before a drain are one wake. */
    proven_net_waker_wake(&waker);
    proven_net_waker_wake(&waker);
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "two wakes, one drain: quiet");

    /* Close the waker only when no thread can still wake it: stop the workers first. */
    proven_job_system_close(jobs);
    proven_job_system_destroy(jobs);
    proven_net_waker_close(&waker);
    return EXAMPLE_OK();
}
