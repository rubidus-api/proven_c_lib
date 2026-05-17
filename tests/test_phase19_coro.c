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

int main(void) {
    PROVEN_TEST_INFO("Running Phase 19: Stackless Coroutine Evaluator...");

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
