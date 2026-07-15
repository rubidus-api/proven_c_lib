#include "proven.h"
#include "proven_test.h"
#include <stdalign.h>

static void check_array_guarding(void) {
    PROVEN_TEST_SECTION(
        "array invariant guards",
        "Verify public array mutation entry points reject corrupted public structs instead of touching allocator callbacks.",
        "Inspect proven_array_reserve, proven_array_push, and proven_array_destroy if an invalid array reaches a realloc or free call."
    );

    proven_byte_t dummy = 0;
    proven_array_t arr = {
        .alloc = {0},
        .data = &dummy,
        .len = 1,
        .cap = 1,
        .elem_size = sizeof(int),
        .align = alignof(int)
    };

    proven_err_t reserve_err = proven_array_reserve(&arr, 2);
    PROVEN_TEST_ASSERT(reserve_err == PROVEN_ERR_INVALID_ARG, "Corrupted arrays should fail reserve with invalid arg", "Inspect the public invariant guard before reallocating.");

    int value = 42;
    proven_err_t push_err = proven_array_push(&arr, &value);
    PROVEN_TEST_ASSERT(push_err == PROVEN_ERR_INVALID_ARG, "Corrupted arrays should fail push with invalid arg", "Inspect the public invariant guard before touching allocator callbacks.");

    proven_array_destroy(&arr);
    PROVEN_TEST_ASSERT(arr.data == NULL && arr.len == 0 && arr.cap == 0 && arr.elem_size == 0 && arr.align == 0, "Destroy should clear corrupted arrays after a best-effort cleanup", "Inspect proven_array_destroy if stale state remains visible after cleanup.");
}

static void check_map_guarding(void) {
    PROVEN_TEST_SECTION(
        "map invariant guards",
        "Verify public map mutation entry points reject corrupted public structs instead of touching allocator callbacks.",
        "Inspect proven_map_reserve, proven_map_set, and proven_map_destroy if an invalid map reaches a rehash or free call."
    );

    proven_byte_t dummy = 0;
    proven_map_t map = {
        .alloc = {0},
        .internal.ptr = &dummy,
        .internal.size = 1,
        .cap = 8,
        .used = 1,
        .len = 1,
        .elem_size = sizeof(int),
        .align = alignof(int),
        .bucket_stride = 0,
        .payload_offset = 0,
        .key_type = PROVEN_KEY_TYPE_INT
    };

    proven_err_t reserve_err = proven_map_reserve(&map, 16);
    PROVEN_TEST_ASSERT(reserve_err == PROVEN_ERR_INVALID_ARG, "Corrupted maps should fail reserve with invalid arg", "Inspect the public invariant guard before rehashing.");

    int value = 7;
    proven_err_t set_err = proven_map_set(&map, (proven_map_key_t){ .id = 1 }, &value);
    PROVEN_TEST_ASSERT(set_err == PROVEN_ERR_INVALID_ARG, "Corrupted maps should fail set with invalid arg", "Inspect the public invariant guard before inserting or rehashing.");

    proven_map_destroy(&map);
    PROVEN_TEST_ASSERT(map.internal.ptr == NULL && map.cap == 0 && map.used == 0 && map.len == 0 && map.elem_size == 0 && map.align == 0 && map.bucket_stride == 0 && map.payload_offset == 0, "Destroy should clear corrupted maps after a best-effort cleanup", "Inspect proven_map_destroy if stale state remains visible after cleanup.");
}

static void check_append_open_flags(void) {
    PROVEN_TEST_SECTION(
        "filesystem append open flags",
        "Verify append-only creation opens a writable POSIX file and append-plus-truncation is rejected as a conflicting request.",
        "Inspect the filesystem open-flag translation if append behaves like a read-only open or if append and truncation are accepted together."
    );

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("test_regression_append_flags.txt");

    proven_result_file_t create_res = proven_fs_open(heap, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(proven_is_ok(create_res.err), "Setup file should open for writing", "Check the temp-file setup before testing append flags.");
    (void)proven_fs_close(create_res.value);

    proven_result_file_t append_res = proven_fs_open(heap, path, PROVEN_FS_APPEND | PROVEN_FS_CREATE);
    PROVEN_TEST_ASSERT(proven_is_ok(append_res.err), "Append plus create should open a writable file", "Inspect POSIX open flags so append is treated as write intent.");
    proven_err_t write_err = proven_fs_write_all(append_res.value, proven_mem_view_from_u8(PROVEN_LIT("abc")));
    PROVEN_TEST_ASSERT(write_err == PROVEN_OK, "Append-open file should accept writes", "Inspect POSIX open flags so O_APPEND is paired with write access.");
    (void)proven_fs_close(append_res.value);

    proven_result_file_t conflict_res = proven_fs_open(heap, path, PROVEN_FS_APPEND | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    if (proven_is_ok(conflict_res.err)) {
        (void)proven_fs_close(conflict_res.value);
        (void)proven_fs_remove(heap, path);
    }
    PROVEN_TEST_ASSERT(conflict_res.err == PROVEN_ERR_INVALID_ARG, "Append plus truncation should be rejected as a conflicting request", "Inspect the filesystem mode validation before changing the open-flag translation.");

    (void)proven_fs_remove(heap, path);
}

static void check_invalid_open_flags(void) {
    PROVEN_TEST_SECTION(
        "filesystem invalid open inputs",
        "Verify unsupported filesystem mode bits and empty paths are rejected before reaching the PAL layer.",
        "Inspect the filesystem path conversion or open-mode mask if an empty path or out-of-range flag reaches the platform open call."
    );

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t empty_path = {0};
    proven_result_file_t empty_res = proven_fs_open(heap, empty_path, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(empty_res.err == PROVEN_ERR_INVALID_ARG, "Empty filesystem paths should fail with invalid arg", "Inspect the shared path conversion helper if empty paths stop being rejected.");

    proven_u8str_view_t path = PROVEN_LIT("test_regression_invalid_fs_mode.txt");
    proven_fs_mode_t invalid_mode = (proven_fs_mode_t)(1u << 6);
    proven_result_file_t res = proven_fs_open(heap, path, invalid_mode);
    PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_INVALID_ARG, "Unsupported filesystem mode bits should fail with invalid arg", "Inspect the supported-mode mask in proven_fs_open before changing platform flag translation.");
}

static void check_trunc_requires_write(void) {
    PROVEN_TEST_SECTION(
        "filesystem truncation intent",
        "Verify truncation without write intent is rejected instead of being silently converted into a read-only open.",
        "Inspect the filesystem mode validation if truncation is accepted without a write-capable access mode."
    );

    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("test_regression_trunc_requires_write.txt");
    proven_result_file_t res = proven_fs_open(heap, path, PROVEN_FS_TRUNC);
    PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_INVALID_ARG, "Truncation without write intent should fail with invalid arg", "Inspect the truncation-mode validation in proven_fs_open before changing access defaults.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "regression public contracts",
        "Verify public array, map, and filesystem mutation entry points reject corrupted structs and conflicting open modes.",
        "Inspect the invariant guard or open-flag translation named by the failing section before touching unrelated code."
    );

    check_array_guarding();
    check_map_guarding();
    check_append_open_flags();
    check_invalid_open_flags();
    check_trunc_requires_write();

    PROVEN_TEST_PASS("Public contract regression checks passed.");
    return 0;
}
