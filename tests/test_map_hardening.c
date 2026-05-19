#include "proven.h"
#include "proven_test.h"

static void check_borrowed_key_hardening(void) {
    PROVEN_TEST_SECTION(
        "map borrowed-key hardening",
        "Verify borrowed U8 keys that point into the map's own internal storage are rejected when hardening or debug validation is enabled.",
        "Inspect the borrowed-key range guard in map insertion if an internal pointer is accepted or if external borrowed keys stop working."
    );

    proven_allocator_t heap = proven_heap_allocator();
    proven_result_map_t res = PROVEN_MAP_INIT_U8_BORROWED(heap, int, 4);
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Map should initialize for borrowed-key testing", "Check map allocation setup if initialization fails.");

    proven_map_t map = res.value;
    int value = 7;

    proven_err_t external_err = PROVEN_MAP_SET_U8_BORROWED(&map, PROVEN_LIT("external"), int, value);
    PROVEN_TEST_ASSERT(external_err == PROVEN_OK, "External borrowed keys should still insert successfully", "Inspect the borrowed-key validation if a normal external key is rejected.");

#if PROVEN_HARDENED || !defined(NDEBUG)
    proven_u8str_view_t internal_view = {
        .ptr = map.internal.ptr,
        .size = 1
    };
    proven_err_t internal_err = PROVEN_MAP_SET_U8_BORROWED(&map, internal_view, int, value);
    PROVEN_TEST_ASSERT(internal_err == PROVEN_ERR_INVALID_ARG, "Borrowed keys that point into internal map storage should be rejected under validation", "Inspect the internal-storage borrowed-key guard before relaxing validation.");
#endif

    PROVEN_MAP_DESTROY(&map);
    PROVEN_TEST_PASS("Map borrowed-key hardening checks passed.");
}

int main(void) {
    PROVEN_TEST_INFO("Running map hardening checks...");
    check_borrowed_key_hardening();
    return 0;
}
