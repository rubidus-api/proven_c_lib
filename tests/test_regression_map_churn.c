#include "proven.h"
#include "proven_test.h"

/*
 * A map with churn used to grow forever.
 *
 * remove() cannot free a bucket: an open-addressed table needs the tombstone, or every
 * key that probed past it becomes unfindable. So `used` (occupied + tombstoned) only ever
 * goes up, and the load-factor test that triggers a rehash reads `used`. The rehash then
 * doubled the capacity - unconditionally, even when the LIVE count had not moved.
 *
 * That is every cache, every session table, every work queue: insert, remove, insert,
 * remove, with a bounded live set. Measured before the fix: 100 live entries and two
 * million operations produced a capacity of 1,048,576 and 33 MB held. It is not a leak.
 * Every byte is reachable, and destroy frees all of it - which is exactly why no leak
 * checker ever said a word. The program simply grows until it dies.
 *
 * The fix: grow only when the live set needs the room. Otherwise rehash at the same
 * capacity, which costs the same single walk and reclaims every tombstone.
 */

int main(void) {
    PROVEN_TEST_SUITE("a map with churn does not grow without bound",
        "Insert and remove with a bounded live set must keep the table's capacity bounded too.",
        "Inspect map_rehash in src/proven/map.c: it must compare the LIVE count against the capacity, not double on every trigger. `used` counts tombstones and can never fall on its own.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("100 live entries, 200,000 operations, bounded capacity",
        "The live set never exceeds 100. The table must not end up sized for the operation count.",
        "If cap explodes, map_rehash is doubling on tombstone pressure instead of reclaiming.");
    // ---------------------------------------------------------------
    {
        proven_result_map_t mr = PROVEN_MAP_INIT_INT(heap, int, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup: a map", "");
        proven_map_t m = mr.value;

        const int live = 100;
        const int ops = 200000;

        for (int i = 0; i < ops; ++i) {
            proven_err_t e = PROVEN_MAP_SET_INT(&m, i, int, i);
            PROVEN_TEST_ASSERT(proven_is_ok(e), "every insert must succeed", "");

            if (i >= live) {
                e = proven_map_remove(&m, (proven_map_key_t){ .id = (proven_size_t)(i - live) });
                PROVEN_TEST_ASSERT(proven_is_ok(e), "every remove must succeed", "");
            }
        }

        PROVEN_TEST_ASSERT(m.len == (proven_size_t)live,
            "exactly 100 entries must be live at the end", "");

        /* Sized for the live set, not for the op count. The old code reached 1,048,576
         * for two million ops; the bound below would have failed at roughly 4,000. */
        PROVEN_TEST_ASSERT(m.cap <= 1024u,
            "the capacity must stay bounded by the live set, not by the number of operations",
            "It used to double on every rehash regardless of how many entries were actually live, so a steady-state workload grew the table forever.");

        /* And it must still WORK: every live key present, every dead key gone. */
        int found = 0;
        for (int k = ops - live; k < ops; ++k) {
            const int *p = PROVEN_MAP_GET_INT(&m, int, k);
            PROVEN_TEST_ASSERT(p != NULL && *p == k, "every live key must still be found", "");
            found++;
        }
        PROVEN_TEST_ASSERT(found == live, "all 100 live keys must be found", "");

        for (int k = 0; k < ops - live; k += 4096) {
            PROVEN_TEST_ASSERT(PROVEN_MAP_GET_INT(&m, int, k) == NULL,
                "a removed key must not come back",
                "An in-place rehash must not resurrect tombstoned entries.");
        }

        proven_map_destroy(&m);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a map that really does grow still grows",
        "Reclaiming tombstones must not starve a map whose live set is genuinely expanding.",
        "If this fails, map_rehash refuses to double when it should, and every insert past the load factor turns into a rehash.");
    // ---------------------------------------------------------------
    {
        proven_result_map_t mr = PROVEN_MAP_INIT_INT(heap, int, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup: a map", "");
        proven_map_t m = mr.value;

        for (int i = 0; i < 10000; ++i) {
            proven_err_t e = PROVEN_MAP_SET_INT(&m, i, int, i * 2);
            PROVEN_TEST_ASSERT(proven_is_ok(e), "every insert must succeed", "");
        }

        PROVEN_TEST_ASSERT(m.len == 10000u, "all 10,000 entries must be live", "");
        PROVEN_TEST_ASSERT(m.cap >= 16384u,
            "and the table must have grown to hold them",
            "A 10,000-entry map cannot live in a table that never doubled.");

        for (int i = 0; i < 10000; i += 97) {
            const int *p = PROVEN_MAP_GET_INT(&m, int, i);
            PROVEN_TEST_ASSERT(p != NULL && *p == i * 2, "and every entry must be findable", "");
        }

        proven_map_destroy(&m);
    }

    PROVEN_TEST_PASS("the map grows for live entries and reclaims tombstones.");
    return 0;
}
