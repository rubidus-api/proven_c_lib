#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



int main() {
    PROVEN_TEST_INFO("Running Phase 10 Ring Buffer Tests...");

    proven_allocator_t heap = proven_heap_allocator();

    // ============================================
    // 1. Creation and Edge Boundaries Checks
    // ============================================
    // Capping memory to precisely 3 integers capacity
    proven_result_ring_t res = PROVEN_RING_INIT(heap, int, 3);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err), "Testing condition: PROVEN_IS_OK(res.err)", "Review logic surrounding PROVEN_IS_OK(res.err)");
    proven_ring_t ring = res.value;

    PROVEN_TEST_ASSERT(ring.cap == 3, "Testing condition: ring.cap == 3", "Review logic surrounding ring.cap == 3");
    PROVEN_TEST_ASSERT(ring.len == 0, "Testing condition: ring.len == 0", "Review logic surrounding ring.len == 0");
    PROVEN_TEST_ASSERT(ring.head == 0 && ring.tail == 0, "Testing condition: ring.head == 0 && ring.tail == 0", "Review logic surrounding ring.head == 0 && ring.tail == 0");

    // ============================================
    // 2. FIFO Order Validation (Push & Pop)
    // ============================================
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 100)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 100))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 100))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 200)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 200))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 200))");
    PROVEN_TEST_ASSERT(ring.len == 2, "Testing condition: ring.len == 2", "Review logic surrounding ring.len == 2");
    
    int out;
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))");
    PROVEN_TEST_ASSERT(out == 100, "Testing condition: out == 100", "Review logic surrounding out == 100");  // Queue strictly pulls earliest value efficiently
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 300)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 300))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 300))"); 
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 400)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 400))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_PUSH(&ring, int, 400))"); // Tail is now overlapping bounds physically!

    // ============================================
    // 3. OOB Starvation & Full Rejection Defense
    // ============================================
    PROVEN_TEST_ASSERT(ring.len == 3, "Testing condition: ring.len == 3", "Review logic surrounding ring.len == 3"); // 200, 300, 400
    
    // Ring is mathematically fully capped. Prevent allocations safely!
    proven_err_t push_err = PROVEN_RING_PUSH(&ring, int, 500);
    PROVEN_TEST_ASSERT(push_err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: push_err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding push_err == PROVEN_ERR_OUT_OF_BOUNDS");
    
    // Now aggressively pop all 3 elements looping pointer across physical boundaries wrapping effectively
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))"); PROVEN_TEST_ASSERT(out == 200, "Testing condition: out == 200", "Review logic surrounding out == 200");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))"); PROVEN_TEST_ASSERT(out == 300, "Testing condition: out == 300", "Review logic surrounding out == 300");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out)), "Testing condition: PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))", "Review logic surrounding PROVEN_IS_OK(PROVEN_RING_POP(&ring, int, &out))"); PROVEN_TEST_ASSERT(out == 400, "Testing condition: out == 400", "Review logic surrounding out == 400");
    
    PROVEN_TEST_ASSERT(ring.len == 0, "Testing condition: ring.len == 0", "Review logic surrounding ring.len == 0");
    
    // Prevent extraction over empty lengths
    proven_err_t pop_err = PROVEN_RING_POP(&ring, int, &out);
    PROVEN_TEST_ASSERT(pop_err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: pop_err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding pop_err == PROVEN_ERR_OUT_OF_BOUNDS");

    PROVEN_RING_DESTROY(&ring);
    
    PROVEN_TEST_INFO("All Phase 10 Ring Buffer Tests Passed Successfully!");
    return 0;
}
