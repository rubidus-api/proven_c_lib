#include "proven.h"
#include "proven_test.h"

/*
 * RFC-0005 section 1.1: proven_u8str_view_find matches an empty needle at the position it is asked
 * to start from, so a split iterator that advances by `at + sep.size` never advances when the
 * separator is empty, and yields empty fields for ever. RFC-0004's sketch did exactly that. A
 * separator computed at runtime reaches it with a one-character mistake, and the failure is a
 * hang, not a wrong answer.
 *
 * This test reproduces the hang WITHOUT hanging: it counts fields up to a cap. The fix is step 2
 * of the iterator - the empty-separator case, before any search - which looks redundant and is
 * the only thing between the library and an infinite loop. Deleting it makes this fail.
 */

int main(void) {
    PROVEN_TEST_SUITE("an empty separator ends the split",
        "Splitting on an empty (or null) separator yields exactly one field - the whole input - for an empty and a non-empty source.",
        "Step 2 of proven_u8str_view_split_next (the empty-separator case) is missing or has moved after the search.");

    const proven_u8str_view_t seps[] = { PROVEN_LIT(""), { NULL, 0 } };
    const proven_u8str_view_t srcs[] = { PROVEN_LIT("abc"), PROVEN_LIT(""), PROVEN_LIT(",,") };
    for (size_t s = 0; s < 2; ++s) {
        for (size_t i = 0; i < 3; ++i) {
            proven_u8str_view_split_t it = proven_u8str_view_split(srcs[i], seps[s]);
            proven_u8str_view_t f = {0};
            int n = 0;
            while (n < 1000 && proven_u8str_view_split_next(&it, &f)) ++n;
            PROVEN_TEST_ASSERT(n == 1, "exactly one field, not an endless run of empty ones", "");
        }
    }

    PROVEN_TEST_PASS("an empty separator ends the split");
    return 0;
}
