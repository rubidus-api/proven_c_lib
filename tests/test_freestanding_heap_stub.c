#include "proven/heap.h"
#include "proven_test.h"

int main(void) {
    PROVEN_TEST_INFO("--- Running test_freestanding_heap_stub ---");

#ifdef PROVEN_FREESTANDING
    proven_allocator_t alloc = proven_heap_allocator();
    PROVEN_TEST_ASSERT(!proven_alloc_is_valid(alloc), 
        "Freestanding heap allocator stub must be invalid", 
        "When PROVEN_FREESTANDING is defined, proven_heap_allocator() should return a NULL/invalid allocator because there is no default OS heap.");
#else
    PROVEN_TEST_INFO("SKIP: PROVEN_FREESTANDING is not defined.");
#endif

    PROVEN_TEST_PASS("--- Finished test_freestanding_heap_stub ---");
    return 0;
}
