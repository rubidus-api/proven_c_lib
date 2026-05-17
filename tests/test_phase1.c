#include "proven.h"
#include "proven_test.h"

/**
 * @file test_phase1.c
 * @brief Unit tests for Phase 1 (Foundation).
 */

int main(void) {
    PROVEN_TEST_INFO("Running Phase 1 Foundation Tests...");

    /* 1. Validate fixed-width integer sizes (C23 standard expectations) */
    PROVEN_TEST_INFO("Checking integer sizes according to C23 standard specifications.");
    PROVEN_TEST_ASSERT(sizeof(proven_u8) == 1, "proven_u8 must be 1 byte", "Check proven/types.h configuration for u8");
    PROVEN_TEST_ASSERT(sizeof(proven_i8) == 1, "proven_i8 must be 1 byte", "Check proven/types.h configuration for i8");
    PROVEN_TEST_ASSERT(sizeof(proven_u16) == 2, "proven_u16 must be 2 bytes", "Check proven/types.h configuration for u16");
    PROVEN_TEST_ASSERT(sizeof(proven_i16) == 2, "proven_i16 must be 2 bytes", "Check proven/types.h configuration for i16");
    PROVEN_TEST_ASSERT(sizeof(proven_u32) == 4, "proven_u32 must be 4 bytes", "Check proven/types.h configuration for u32");
    PROVEN_TEST_ASSERT(sizeof(proven_i32) == 4, "proven_i32 must be 4 bytes", "Check proven/types.h configuration for i32");
    PROVEN_TEST_ASSERT(sizeof(proven_u64) == 8, "proven_u64 must be 8 bytes", "Check proven/types.h configuration for u64");
    PROVEN_TEST_ASSERT(sizeof(proven_i64) == 8, "proven_i64 must be 8 bytes", "Check proven/types.h configuration for i64");

    /* 2. Validate semantic types */
    PROVEN_TEST_INFO("Checking semantic offset types.");
    PROVEN_TEST_ASSERT(sizeof(proven_size_t) >= 4, "proven_size_t must be at least 4 bytes", "Check architecture bitness (must be >= 32-bit)");
    PROVEN_TEST_ASSERT(sizeof(proven_ptrdiff_t) == sizeof(proven_size_t), "ptrdiff_t size must match size_t size", "Check proven/types.h integer definitions");

    /* 3. Validate alignment utilities */
    PROVEN_TEST_INFO("Testing memory alignment logic...");
    PROVEN_TEST_ASSERT(proven_mem_align_up(0, 8) == 0, "align_up(0, 8) should be 0", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(1, 8) == 8, "align_up(1, 8) should round to 8", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(7, 8) == 8, "align_up(7, 8) should round to 8", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(8, 8) == 8, "align_up(8, 8) should remain 8", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(9, 8) == 16, "align_up(9, 8) should round to 16", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(15, 16) == 16, "align_up(15, 16) should round to 16", "Check proven_mem_align_up logic in proven/align.h");
    PROVEN_TEST_ASSERT(proven_mem_align_up(17, 16) == 32, "align_up(17, 16) should round to 32", "Check proven_mem_align_up logic in proven/align.h");

    /* 4. Validate memory core structure */
    PROVEN_TEST_INFO("Evaluating proven_mem_t core structure...");
    proven_mem_t mem = { .ptr = (proven_byte_t*)0x1234, .size = 100 };
    PROVEN_TEST_ASSERT(mem.ptr == (proven_byte_t*)0x1234, "Memory pointer assignment check", "Review proven_mem_t in memory.h");
    PROVEN_TEST_ASSERT(mem.size == 100, "Memory size assignment check", "Review proven_mem_t in memory.h");

    PROVEN_TEST_PASS("All Phase 1 Tests Passed Successfully!");
    return 0;
}
