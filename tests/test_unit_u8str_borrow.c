#include "proven.h"
#include "proven_test.h"

#include <string.h>

/* Verifies proven_u8str_borrow / proven_u8str_reset: a fixed-capacity string
 * over caller-owned memory. Fixed-capacity ops work; growing ops succeed within
 * capacity but refuse to reallocate caller memory; destroy is a no-op. */
int main(void) {
    PROVEN_TEST_SUITE("u8str borrow",
        "Verify borrowed fixed-capacity strings over caller memory.",
        "Inspect proven_u8str_borrow/_reset and the borrowed-flag guards in the grow/destroy paths.");

    proven_allocator_t alloc = proven_heap_allocator();

    /* --- borrow a stack buffer (cap includes the NUL) --- */
    proven_byte_t store[16];
    proven_u8str_t s = proven_u8str_borrow(store, sizeof store);
    PROVEN_TEST_ASSERT(s.borrowed, "borrow sets the borrowed flag", "Check proven_u8str_borrow.");
    PROVEN_TEST_ASSERT(proven_u8str_is_valid(&s), "borrowed string is valid", "Check is_valid on a fresh borrow.");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&s).size == 0, "borrow starts empty", "Check len init.");

    /* --- fixed-capacity append works --- */
    proven_err_t e = proven_u8str_append(&s, PROVEN_LIT("hello"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e), "append within cap ok", "Fixed append should work on a borrow.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s), "hello") == 0, "content == hello", "Check append bytes.");

    /* --- append that exceeds cap fails atomically (15 content bytes max) --- */
    e = proven_u8str_append(&s, PROVEN_LIT("0123456789ab"));   /* 5 + 12 = 17 > 15 */
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "overflow append rejected", "Cap check must fire.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s), "hello") == 0, "unchanged after overflow", "Append must be failure-atomic.");

    /* --- append_byte works within cap, rejects when full --- */
    while (PROVEN_IS_OK(proven_u8str_append_byte(alloc, &s, 'x'))) { }
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&s).size == sizeof store - 1, "filled to cap-1", "append_byte should fill to capacity then stop.");
    e = proven_u8str_append_byte(alloc, &s, 'y');
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "append_byte at full rejected", "Borrowed grow must not reallocate.");

    /* --- a grow that would reallocate is rejected (caller memory untouched) --- */
    proven_byte_t small[4];
    proven_u8str_t t = proven_u8str_borrow(small, sizeof small);
    e = proven_u8str_append_grow(alloc, &t, PROVEN_LIT("toolong"));   /* needs > 4 */
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "append_grow rejects realloc of borrow", "Borrowed grow must return OUT_OF_BOUNDS, not realloc.");
    e = proven_u8str_reserve(alloc, &t, 64);
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "reserve rejects borrow growth", "Borrowed reserve must not reallocate.");
    e = proven_u8str_append_grow(alloc, &t, PROVEN_LIT("abc"));       /* 3 fits in cap 4 */
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e) && strcmp(proven_u8str_as_cstr(&t), "abc") == 0, "grow within cap works on borrow", "Within-capacity grow should succeed.");

    /* --- reset reuses the buffer --- */
    e = proven_u8str_reset(&s);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e) && proven_u8str_as_view(&s).size == 0, "reset empties", "reset must zero len and seal NUL.");
    e = proven_u8str_append(&s, PROVEN_LIT("again"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(e) && strcmp(proven_u8str_as_cstr(&s), "again") == 0, "reuse after reset", "Append after reset should start fresh.");

    /* --- type-safe fixed-capacity fmt into a borrow --- */
    proven_byte_t fbuf[32];
    proven_u8str_t f = proven_u8str_borrow(fbuf, sizeof fbuf);
    proven_fmt_result_t fr = proven_u8str_append_fmt(&f, "L{}/{} {}",
        PROVEN_ARG(3), PROVEN_ARG(9), PROVEN_ARG(proven_u8str_view_from_cstr("ok")));
    PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(fr), "append_fmt into borrow ok", "Fixed-capacity fmt should work on a borrow.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&f), "L3/9 ok") == 0, "fmt output correct", "Check fmt rendering into borrowed memory.");

    /* --- destroy on a borrow is a no-op (no free of stack memory) --- */
    proven_u8str_destroy(alloc, &s);
    PROVEN_TEST_ASSERT(s.internal.ptr == 0 && s.internal.cap == 0, "destroy clears the borrowed handle", "Borrowed destroy must not free; it clears the handle.");

    PROVEN_TEST_PASS("u8str borrow tests passed");
    return 0;
}
