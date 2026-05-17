#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>



/**
 * @file test_phase2.c
 * @brief Unit tests for Phase 2 (Memory Core: view/mut).
 */

int main() {
    PROVEN_TEST_INFO("Running Phase 2 Memory Core Tests...");

    /* 1. Setup raw memory for testing */
    PROVEN_TEST_INFO("Setting up raw memory buffer...");
    proven_u8 raw_data[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };

    /* 2. Create owned abstraction */
    proven_mem_t owned = { .ptr = (proven_byte_t*)raw_data, .size = 16 };

    /* 3. Test View (Read-Only) */
    PROVEN_TEST_INFO("Testing proven_mem_view_t...");
    proven_mem_view_t view = proven_mem_view_from_owned(owned);
    PROVEN_TEST_ASSERT(view.ptr == (const proven_byte_t*)raw_data, "Testing condition: view.ptr == (const proven_byte_t*)raw_data", "Review logic surrounding view.ptr == (const proven_byte_t*)raw_data");
    PROVEN_TEST_ASSERT(view.size == 16, "Testing condition: view.size == 16", "Review logic surrounding view.size == 16");
    PROVEN_TEST_ASSERT(view.ptr[5] == 5, "Testing condition: view.ptr[5] == 5", "Review logic surrounding view.ptr[5] == 5");

    /* 4. Test Mut (Read-Write) */
    PROVEN_TEST_INFO("Testing proven_mem_mut_t...");
    proven_mem_mut_t mut = proven_mem_mut_from_owned(owned);
    PROVEN_TEST_ASSERT(mut.ptr == (proven_byte_t*)raw_data, "Testing condition: mut.ptr == (proven_byte_t*)raw_data", "Review logic surrounding mut.ptr == (proven_byte_t*)raw_data");
    PROVEN_TEST_ASSERT(mut.size == 16, "Testing condition: mut.size == 16", "Review logic surrounding mut.size == 16");
    
    /* Modify through mut */
    mut.ptr[5] = 0xFF;
    PROVEN_TEST_ASSERT(raw_data[5] == 0xFF, "Testing condition: raw_data[5] == 0xFF", "Review logic surrounding raw_data[5] == 0xFF");
    PROVEN_TEST_ASSERT(view.ptr[5] == 0xFF, "Testing condition: view.ptr[5] == 0xFF", "Review logic surrounding view.ptr[5] == 0xFF"); /* view should see the change */

    /* 5. Test Slicing */
    PROVEN_TEST_INFO("Testing slicing utilities...");
    
    /* Slice from index 4, size 4 (should be [4, 0xFF, 6, 7]) */
    proven_mem_view_t sub_view = proven_mem_view_slice_unchecked(view, 4, 4);
    PROVEN_TEST_ASSERT(sub_view.size == 4, "Testing condition: sub_view.size == 4", "Review logic surrounding sub_view.size == 4");
    PROVEN_TEST_ASSERT(sub_view.ptr[0] == 4, "Testing condition: sub_view.ptr[0] == 4", "Review logic surrounding sub_view.ptr[0] == 4");
    PROVEN_TEST_ASSERT(sub_view.ptr[1] == 0xFF, "Testing condition: sub_view.ptr[1] == 0xFF", "Review logic surrounding sub_view.ptr[1] == 0xFF");
    PROVEN_TEST_ASSERT(sub_view.ptr[3] == 7, "Testing condition: sub_view.ptr[3] == 7", "Review logic surrounding sub_view.ptr[3] == 7");

    proven_mem_mut_t sub_mut = proven_mem_mut_slice_unchecked(mut, 8, 2);
    PROVEN_TEST_ASSERT(sub_mut.size == 2, "Testing condition: sub_mut.size == 2", "Review logic surrounding sub_mut.size == 2");
    PROVEN_TEST_ASSERT(sub_mut.ptr[0] == 8, "Testing condition: sub_mut.ptr[0] == 8", "Review logic surrounding sub_mut.ptr[0] == 8");
    sub_mut.ptr[0] = 0xAA;
    PROVEN_TEST_ASSERT(raw_data[8] == 0xAA, "Testing condition: raw_data[8] == 0xAA", "Review logic surrounding raw_data[8] == 0xAA");

    PROVEN_TEST_PASS("All Phase 2 Tests Passed Successfully!");
    return 0;
}
