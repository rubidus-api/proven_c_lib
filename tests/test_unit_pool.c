#include "proven.h"
#include "proven_test.h"


int main(void) {
    PROVEN_TEST_INFO("--- Phase 6: Pool Allocator Constraint & Recycling Tests ---");
    
    proven_allocator_t heap = proven_heap_allocator();
    proven_pool_t pool;
    
    PROVEN_TEST_INFO("Initializing pool for `proven_u64` (8 bytes) with bin capacity of 2.");
    proven_err_t err = proven_pool_init(&pool, heap, sizeof(proven_u64), alignof(proven_u64), 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Pool initialization should succeed", "Check proven_pool_init parameters and available heap memory");

    proven_allocator_t pool_alloc = proven_pool_as_allocator(&pool);

    PROVEN_TEST_INFO("Testing intentional size mismatch to verify strict constraints.");
    proven_result_mem_mut_t res = pool_alloc.alloc_fn(pool_alloc.ctx, 4, alignof(proven_u64));
    PROVEN_TEST_ASSERT(!PROVEN_IS_OK(res.err), "Allocation of 4 bytes should be strictly rejected by an 8-byte pool", "Verify that proven_pool_alloc_trait returns PROVEN_ERR_INVALID_ARG for mismatched sizes");

    PROVEN_TEST_INFO("Allocating 3 consecutive blocks from the pool.");
    proven_result_mem_mut_t r1 = pool_alloc.alloc_fn(pool_alloc.ctx, sizeof(proven_u64), alignof(proven_u64));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(r1.err), "Allocation 1 must succeed", "Check that base allocator is supplying memory when the bin is empty");
    proven_result_mem_mut_t r2 = pool_alloc.alloc_fn(pool_alloc.ctx, sizeof(proven_u64), alignof(proven_u64));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(r2.err), "Allocation 2 must succeed", "Check that base allocator is supplying memory when the bin is empty");
    proven_result_mem_mut_t r3 = pool_alloc.alloc_fn(pool_alloc.ctx, sizeof(proven_u64), alignof(proven_u64));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(r3.err), "Allocation 3 must succeed", "Check that base allocator is supplying memory when the bin is empty");

    PROVEN_TEST_ASSERT(pool.bin_len == 0, "Bin length must be 0 initially", "Verify initialization zeroes bin_len");

    PROVEN_TEST_INFO("Freeing items to test recycle bin mechanics (Stack behavior & Capacity Limits).");
    pool_alloc.free_fn(pool_alloc.ctx, r1.value.ptr);
    PROVEN_TEST_ASSERT(pool.bin_len == 1, "Bin length goes to 1 after free", "Ensure free_fn pushes onto the bin array");
    pool_alloc.free_fn(pool_alloc.ctx, r2.value.ptr);
    PROVEN_TEST_ASSERT(pool.bin_len == 2, "Bin length goes to 2 after second free", "Ensure free_fn pushes onto the bin array");
    
    PROVEN_TEST_INFO("Freeing the 3rd item; bin is fully capped at 2. Should pass to the underlying Heap.");
    pool_alloc.free_fn(pool_alloc.ctx, r3.value.ptr);
    PROVEN_TEST_ASSERT(pool.bin_len == 2, "Bin length stays at 2 (Cap)", "Ensure free_fn correctly delegates to base_alloc when bin is full"); // Cap is 2

    PROVEN_TEST_INFO("Re-allocating... expecting instant LIFO (Stack) pointer recycling.");
    proven_result_mem_mut_t r4 = pool_alloc.alloc_fn(pool_alloc.ctx, sizeof(proven_u64), alignof(proven_u64));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(r4.err), "Re-allocation must succeed natively", "Check bin popping logic in alloc_fn");
    PROVEN_TEST_ASSERT(r4.value.ptr == r2.value.ptr, "LIFO recycling must return r2's pointer first", "Ensure popping accesses bin[len-1], not bin[0]"); // Recycled Top
    PROVEN_TEST_ASSERT(pool.bin_len == 1, "Bin drains to 1 item", "Ensure dropping correctly decrements bin_len");

    proven_result_mem_mut_t r5 = pool_alloc.alloc_fn(pool_alloc.ctx, sizeof(proven_u64), alignof(proven_u64));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(r5.err), "Second re-allocation must succeed natively", "Check bin popping logic in alloc_fn");
    PROVEN_TEST_ASSERT(r5.value.ptr == r1.value.ptr, "LIFO recycling must return r1's pointer next", "Ensure popping accesses bin[len-1] cleanly"); // Recycled Next
    PROVEN_TEST_ASSERT(pool.bin_len == 0, "Bin fully drained to 0", "Ensure dropping correctly decrements bin_len cleanly");

    PROVEN_TEST_INFO("Testing teardown safety...");
    pool_alloc.free_fn(pool_alloc.ctx, r4.value.ptr);
    pool_alloc.free_fn(pool_alloc.ctx, r5.value.ptr);
    
    proven_pool_destroy(&pool);

    PROVEN_TEST_INFO("--- test_unit_pool Passed Completely! ---");
    return 0;
}
