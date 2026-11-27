#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"
#include "proven/coro.h"


// A simple simulated network request logic block
struct DataFetcher {
    proven_coro_t coro;
    int loading_progress;
    int payload;
};

// Returns 0 while still processing, 1 if complete.
static int fetch_data(struct DataFetcher *f) {
    // Zero dynamic allocations inside! Extremely fast!
    PROVEN_CORO_BEGIN(&f->coro);

    PROVEN_TEST_INFO("  [fetch] Initiating connection...");
    PROVEN_CORO_YIELD(&f->coro);
    
    PROVEN_TEST_INFO("  [fetch] Downloading fragments...");
    while (f->loading_progress < 100) {
        f->loading_progress += 30; // Simulate chunk
        PROVEN_CORO_YIELD(&f->coro);
    }
    
    PROVEN_TEST_INFO("  [fetch] Decoding final bytes...");
    f->payload = 404;
    PROVEN_CORO_YIELD(&f->coro);
    
    PROVEN_TEST_INFO("  [fetch] Complete.");

    PROVEN_CORO_END(&f->coro);
}

/* RFC-0057: a parent that awaits a child. The child counts its steps; the input is a byte count
 * the test raises. Built with -Wextra -Werror by the strict modes: PROVEN_CORO_AWAIT's fall into
 * its resume label must not warn (-Wimplicit-fallthrough). */
struct Child { proven_coro_t coro; int avail, taken, steps; };
struct Parent { proven_coro_t coro; struct Child child; int result; };

static proven_i32 child_step(struct Child *c) {
    c->steps++;
    PROVEN_CORO_BEGIN(&c->coro);
    PROVEN_CORO_AWAIT(&c->coro, c->avail >= 1);
    c->taken = 1;
    PROVEN_CORO_AWAIT(&c->coro, c->avail >= 2);
    c->taken = 2;
    PROVEN_CORO_END(&c->coro);
}

static proven_i32 parent_step(struct Parent *p) {
    PROVEN_CORO_BEGIN(&p->coro);
    PROVEN_CORO_AWAIT(&p->coro, child_step(&p->child));
    p->result = p->child.taken;
    PROVEN_CORO_END(&p->coro);
}

int main(void) {
    PROVEN_TEST_INFO("Running Phase 19: Stackless Coroutine Evaluator...");
    {
        struct Parent p = { .result = -1 };
        PROVEN_CORO_INIT(&p.coro);
        PROVEN_CORO_INIT(&p.child.coro);
        const bool r0 = parent_step(&p) == 0 && p.child.taken == 0;
        p.child.avail = 1;
        const bool r1 = parent_step(&p) == 0 && p.child.taken == 1;
        const bool r2 = parent_step(&p) == 0 && p.child.taken == 1;      /* nothing new */
        p.child.avail = 2;
        const bool r3 = parent_step(&p) == 1 && p.result == 2;
        PROVEN_TEST_ASSERT(r0 && r1 && r2 && r3, "a parent awaiting a child goes on in the call that completes the child", "Check PROVEN_CORO_AWAIT: the condition is evaluated on every visit, the first included.");
        PROVEN_TEST_ASSERT(p.child.steps == 4, "the child is stepped exactly once per call of the parent", "Only the parent steps the child: count the calls of child_step.");
        PROVEN_TEST_ASSERT(parent_step(&p) == 1 && p.child.steps == 4 && PROVEN_CORO_IS_DONE(&p.child.coro), "done is sticky: a finished parent returns 1 and does not step the child", "PROVEN_CORO_END sets -1, which matches no case label.");
        struct Parent q = { .result = -1, .child = { .avail = 2 } };
        PROVEN_CORO_INIT(&q.coro);
        PROVEN_CORO_INIT(&q.child.coro);
        PROVEN_TEST_ASSERT(parent_step(&q) == 1 && q.result == 2 && q.child.steps == 1, "with all the input there, one call completes child and parent", "AWAIT must test its condition at once, not only after a first return.");
    }

    struct DataFetcher my_fetcher;
    my_fetcher.loading_progress = 0;
    my_fetcher.payload = 0;
    PROVEN_CORO_INIT(&my_fetcher.coro);

    int main_loop_ticks = 0;
    PROVEN_TEST_INFO("Starting coroutine main execution loop...");
    while (!PROVEN_CORO_IS_DONE(&my_fetcher.coro)) {
        PROVEN_TEST_INFO("Main Frame #{}", PROVEN_ARG(main_loop_ticks));
        
        // Execute a slice of the fetcher without halting the main thread
        fetch_data(&my_fetcher);
        main_loop_ticks++;
    }

    PROVEN_TEST_ASSERT(my_fetcher.payload == 404, "Testing condition: my_fetcher.payload == 404", "Review logic surrounding my_fetcher.payload == 404");
    PROVEN_TEST_ASSERT(main_loop_ticks >= 3, "Testing condition: main_loop_ticks >= 3", "Review logic surrounding main_loop_ticks >= 3");
    
    PROVEN_TEST_PASS("All Phase 19 Coroutine Tests Passed Successfully!");
    return 0;
}
