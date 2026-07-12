#include "proven.h"
#include "proven_test.h"

typedef struct {
    proven_allocator_t heap;
    void *owned_ptrs[32];
    bool owned_freed[32];
    proven_size_t owned_count;
    int owned_allocs;
    int owned_frees;
} owned_tracker_t;

static owned_tracker_t g_tracker;

static void tracker_record_owned_ptr(void *ptr) {
    if (!ptr) {
        return;
    }
    if (g_tracker.owned_count < (proven_size_t)(sizeof(g_tracker.owned_ptrs) / sizeof(g_tracker.owned_ptrs[0]))) {
        g_tracker.owned_ptrs[g_tracker.owned_count] = ptr;
        g_tracker.owned_freed[g_tracker.owned_count] = false;
        g_tracker.owned_count++;
    }
}

static bool tracker_mark_owned_free(void *ptr) {
    for (proven_size_t i = 0; i < g_tracker.owned_count; ++i) {
        if (g_tracker.owned_ptrs[i] == ptr && !g_tracker.owned_freed[i]) {
            g_tracker.owned_freed[i] = true;
            return true;
        }
    }
    return false;
}

static proven_result_mem_mut_t tracker_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    owned_tracker_t *tracker = (owned_tracker_t *)ctx;
    proven_result_mem_mut_t res = tracker->heap.alloc_fn(tracker->heap.ctx, size, align);
    if (PROVEN_IS_OK(res.err) && align == 1 && size > 0) {
        tracker_record_owned_ptr(res.value.ptr);
        tracker->owned_allocs++;
    }
    return res;
}

static void tracker_free(void *ctx, void *ptr) {
    owned_tracker_t *tracker = (owned_tracker_t *)ctx;
    if (tracker_mark_owned_free(ptr)) {
        tracker->owned_frees++;
    }
    tracker->heap.free_fn(tracker->heap.ctx, ptr);
}

static proven_result_mem_mut_t tracker_realloc(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    owned_tracker_t *tracker = (owned_tracker_t *)ctx;
    return tracker->heap.realloc_fn(tracker->heap.ctx, old_ptr, old_size, new_size, align);
}

static proven_allocator_t tracker_allocator(void) {
    return (proven_allocator_t){
        .ctx = &g_tracker,
        .alloc_fn = tracker_alloc,
        .free_fn = tracker_free,
        .realloc_fn = tracker_realloc,
    };
}

static void tracker_reset(void) {
    g_tracker.heap = proven_heap_allocator();
    g_tracker.owned_count = 0;
    g_tracker.owned_allocs = 0;
    g_tracker.owned_frees = 0;
    for (proven_size_t i = 0; i < (proven_size_t)(sizeof(g_tracker.owned_freed) / sizeof(g_tracker.owned_freed[0])); ++i) {
        g_tracker.owned_ptrs[i] = NULL;
        g_tracker.owned_freed[i] = false;
    }
}

static void test_owned_key_copy_and_release(void) {
    PROVEN_TEST_SECTION(
        "map owned-key copy and release",
        "Verify owned U8 keys are duplicated into map storage, survive source-buffer mutation, and free their bytes on remove and destroy.",
        "Inspect the owned-key allocation path and the remove/destroy cleanup path if the copied key disappears too early or leaks on teardown."
    );

    tracker_reset();
    proven_allocator_t alloc = tracker_allocator();
    proven_result_map_t res = proven_map_create(alloc, 4, PROVEN_KEY_TYPE_U8_OWNED, sizeof(int), alignof(int));
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Owned-key map should initialize", "Check map creation and allocator validation if initialization fails.");

    proven_map_t map = res.value;
    char key_buf[] = "alpha";
    proven_u8str_view_t key_view = { .ptr = (const proven_byte_t *)key_buf, .size = 5 };
    int value = 7;

    proven_err_t set_err = proven_map_set_u8_owned(&map, key_view, &value);
    PROVEN_TEST_ASSERT(set_err == PROVEN_OK, "Owned-key insert should succeed", "Inspect owned-key duplication and key-type validation if the insert fails.");
    PROVEN_TEST_ASSERT(g_tracker.owned_allocs == 1, "Owned-key insert should allocate one key copy", "Check the owned-key allocation wrapper if no byte-copy allocation is recorded.");

    key_buf[0] = 'z';
    const int *found = PROVEN_MAP_GET_U8_OWNED(&map, int, PROVEN_LIT("alpha"));
    PROVEN_TEST_ASSERT(found && *found == 7, "Owned key should survive source-buffer mutation", "Inspect the copied-key storage if the lookup follows the mutated source buffer.");

    proven_err_t remove_err = PROVEN_MAP_REMOVE_U8_OWNED(&map, PROVEN_LIT("alpha"));
    PROVEN_TEST_ASSERT(remove_err == PROVEN_OK, "Owned-key remove should succeed", "Inspect owned-key cleanup if remove fails or reports the key missing.");
    PROVEN_TEST_ASSERT(g_tracker.owned_frees == 1, "Owned-key remove should free the duplicated bytes once", "Inspect remove cleanup if the owned key is not released or is released twice.");

    PROVEN_MAP_DESTROY(&map);
    PROVEN_TEST_PASS("Owned-key copy and release checks passed.");
}

static void test_owned_key_rehash_and_destroy(void) {
    PROVEN_TEST_SECTION(
        "map owned-key rehash and destroy",
        "Verify owned U8 keys remain reachable after rehash and are all released exactly once on destroy.",
        "Inspect the rehash migration path if any key is lost, duplicated, or freed too early during growth."
    );

    tracker_reset();
    proven_allocator_t alloc = tracker_allocator();
    proven_result_map_t res = proven_map_create(alloc, 4, PROVEN_KEY_TYPE_U8_OWNED, sizeof(int), alignof(int));
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Owned-key map should initialize for rehash testing", "Check map creation if initialization fails.");

    proven_map_t map = res.value;
    char key0[] = "k0";
    char key1[] = "k1";
    char key2[] = "k2";
    char key3[] = "k3";
    char key4[] = "k4";
    char key5[] = "k5";
    char key6[] = "k6";

    proven_u8str_view_t keys[] = {
        { .ptr = (const proven_byte_t *)key0, .size = 2 },
        { .ptr = (const proven_byte_t *)key1, .size = 2 },
        { .ptr = (const proven_byte_t *)key2, .size = 2 },
        { .ptr = (const proven_byte_t *)key3, .size = 2 },
        { .ptr = (const proven_byte_t *)key4, .size = 2 },
        { .ptr = (const proven_byte_t *)key5, .size = 2 },
        { .ptr = (const proven_byte_t *)key6, .size = 2 },
    };

    for (int i = 0; i < 7; ++i) {
        proven_err_t err = proven_map_set_u8_owned(&map, keys[i], &i);
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "Owned-key insert during rehash setup should succeed", "Inspect the owned-key insert path if the growth-prep insert fails.");
    }

    PROVEN_TEST_ASSERT(map.cap == 16, "Owned-key map should rehash at the expected threshold", "Inspect map growth and load-factor logic if the capacity does not double.");
    PROVEN_TEST_ASSERT(g_tracker.owned_allocs == 7, "Each owned-key insert should allocate one copied key", "Inspect the key-duplication path if the allocation count drifts.");

    for (int i = 0; i < 7; ++i) {
        const int *found = PROVEN_MAP_GET_U8_OWNED(&map, int, keys[i]);
        PROVEN_TEST_ASSERT(found && *found == i, "Owned key should remain reachable after rehash", "Inspect key migration if a lookup fails after growth.");
    }

    PROVEN_MAP_DESTROY(&map);
    PROVEN_TEST_ASSERT(g_tracker.owned_frees == 7, "Destroy should free every duplicated owned key exactly once", "Inspect destroy and rehash migration cleanup if owned bytes leak or double-free.");
    PROVEN_TEST_PASS("Owned-key rehash and destroy checks passed.");
}

int main(void) {
    PROVEN_TEST_INFO("Running map owned-key checks...");
    test_owned_key_copy_and_release();
    test_owned_key_rehash_and_destroy();
    return 0;
}
