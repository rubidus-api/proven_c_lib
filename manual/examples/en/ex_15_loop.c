#include "example.h"
#include <stdatomic.h>

/*
 * An event loop by itself: timers, and work that is done on another thread and comes back.
 *
 * A loop's thread never waits for anything but "what is next". Whatever takes time - here a
 * worker that sleeps, standing in for a file or a database - happens elsewhere, and its result
 * is posted to the loop, where it runs between other events.
 */

typedef struct {
    proven_loop_t *loop;
    proven_loop_timer_t tick, late;
    int ticks;
    int order[8];
    int count;
    atomic_int from_worker;
} app_t;

static void note(app_t *app, int what) { if (app->count < 8) app->order[app->count++] = what; }

/* A timer fires once. One that should repeat sets itself again. */
static void on_tick(void *ctx) {
    app_t *app = ctx;
    note(app, 1);
    if (++app->ticks < 3) proven_loop_timer_set(app->loop, &app->tick, 20, on_tick, app);
}

static void on_late(void *ctx) { note(ctx, 9); }

/* This runs on the loop's thread, although a worker thread asked for it. */
static void on_result(void *ctx) {
    app_t *app = ctx;
    note(app, 5);
    proven_loop_timer_cancel(app->loop, &app->late);      /* it would have fired in five seconds; now it never will */
    proven_loop_stop(app->loop);
}

/* This runs on a worker thread. It may take as long as it likes: the loop is not waiting. */
static void slow_work(void *ctx) {
    app_t *app = ctx;
    proven_time_sleep(400);
    atomic_store(&app->from_worker, 1);
    if (proven_loop_post(app->loop, on_result, app) != PROVEN_OK) proven_loop_stop(app->loop);   /* only a failed allocation refuses a post; then just end the run */
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- a loop -------------------------------------------------------------------
    /* A loop is a selector, a set of timers, and a queue other threads can post to. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    // ---- timers --------------------------------------------------------------------
    /* The timer structs are yours: they live in your own state, and setting one allocates
     * nothing. A timer waiting to fire costs nothing either - it is not looked at until its time. */
    proven_loop_timer_set(app.loop, &app.tick, 20, on_tick, &app);
    proven_loop_timer_set(app.loop, &app.late, 5000, on_late, &app);
    EXAMPLE_REQUIRE(proven_loop_timer_is_set(&app.tick) && proven_loop_timer_is_set(&app.late), "two timers are set");

    // ---- work elsewhere ------------------------------------------------------------
    /* The job system runs slow_work on a worker thread. When it is done it posts on_result back. */
    proven_job_sys_t *workers = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &workers) == PROVEN_OK && proven_job_submit(workers, slow_work, &app), "a worker has the slow job");

    // ---- run -----------------------------------------------------------------------
    /* proven_loop_run returns when something calls proven_loop_stop - here on_result does. */
    proven_time_t began = proven_time_monotonic_now();
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop ran and was stopped");
    proven_time_t took = proven_time_monotonic_now() - began;

    EXAMPLE_REQUIRE(app.ticks == 3 && app.count == 4 && app.order[0] == 1 && app.order[1] == 1 && app.order[2] == 1 && app.order[3] == 5,
                    "three ticks, then the worker's result, in that order");
    EXAMPLE_REQUIRE(atomic_load(&app.from_worker) == 1 && took < 2000000000, "the result came from the worker, and long before the five-second timer");
    EXAMPLE_REQUIRE(!proven_loop_timer_is_set(&app.late), "a cancelled timer is no longer set");

    proven_job_system_close(workers);
    proven_job_system_destroy(workers);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
