#include "proven.h"
#include "proven_test.h"

#include <string.h>

/* Verifies proven_mem_copy: a bounded byte copy used to replace raw memcpy in
 * downstream byte-range reads. */
int main(void) {
    PROVEN_TEST_SUITE("mem copy",
        "Verify the bounded proven_mem_copy primitive.",
        "Inspect proven_mem_copy in src/proven/memory.c and its capacity/null guards.");

    proven_byte_t dst[8];

    /* fits */
    proven_mem_view_t src = { (const proven_byte_t *)"hello", 5 };
    proven_err_t e = proven_mem_copy(dst, sizeof dst, src);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e), "copy that fits ok", "Within-capacity copy should succeed.");
    PROVEN_TEST_ASSERT(memcmp(dst, "hello", 5) == 0, "bytes copied", "Check copied content.");

    /* exact fit */
    proven_mem_view_t eight = { (const proven_byte_t *)"ABCDEFGH", 8 };
    e = proven_mem_copy(dst, sizeof dst, eight);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e) && memcmp(dst, "ABCDEFGH", 8) == 0, "exact-capacity copy ok", "size == cap must fit.");

    /* overflow rejected, destination untouched beyond what fits */
    proven_mem_view_t big = { (const proven_byte_t *)"123456789", 9 };
    e = proven_mem_copy(dst, sizeof dst, big);
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "overflow rejected", "size > cap must return OUT_OF_BOUNDS.");
    PROVEN_TEST_ASSERT(memcmp(dst, "ABCDEFGH", 8) == 0, "dst untouched on overflow", "Rejected copy must not write.");

    /* zero-size is a no-op (ok) */
    proven_mem_view_t empty = { (const proven_byte_t *)0, 0 };
    e = proven_mem_copy(dst, sizeof dst, empty);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e), "zero-size no-op ok", "Empty source should be a no-op.");

    /* null dst with non-zero source rejected */
    e = proven_mem_copy((void *)0, 8, src);
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "null dst rejected", "Null pointer with size must be INVALID_ARG.");

    PROVEN_TEST_PASS("mem copy tests passed");
    return 0;
}
