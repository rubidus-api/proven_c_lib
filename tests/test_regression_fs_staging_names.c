#include "proven.h"
#include "proven_test.h"

#include <stdio.h>
#include <string.h>

/*
 * RFC-0009 D-001. proven_fs_write_file_atomic and proven_fs_write_file_durable stage into a
 * sibling file and rename it over the target. The staging names used to be a fixed list,
 * "<path>.pvtmp00" .. "<path>.pvtmp07": eight writers killed mid-write, or anyone who could
 * write the directory, left all eight taken, and every later replacement of that path failed
 * with PROVEN_ERR_IO - for good. The names are now random, and only a collision is retried.
 */

#define DIR_NAME "test_staging_names.d"
#define TARGET   DIR_NAME "/target.txt"

static proven_allocator_t heap;

static void put(const char *path, const char *text) {
    proven_err_t e = proven_fs_write_file(heap, proven_u8str_view_from_cstr(path),
                                          proven_mem_view_from_u8(proven_u8str_view_from_cstr(text)));
    PROVEN_TEST_ASSERT(proven_is_ok(e), "a fixture file is written", path);
}

static bool holds(const char *path, const char *text) {
    proven_result_mem_mut_t r = proven_fs_read_all(heap, proven_u8str_view_from_cstr(path));
    if (!proven_is_ok(r.err)) return false;
    bool same = r.value.size == strlen(text) && (r.value.size == 0 || memcmp(r.value.ptr, text, r.value.size) == 0);
    if (r.value.ptr) heap.free_fn(heap.ctx, r.value.ptr);
    return same;
}

/* Entries in the directory whose names have the staging shape. */
static int count_staging(void) {
    proven_result_dir_t d = proven_fs_dir_open(heap, PROVEN_LIT(DIR_NAME));
    PROVEN_TEST_ASSERT(proven_is_ok(d.err), "the fixture directory opens", "");
    int n = 0;
    proven_fs_dir_entry_t e;
    while (proven_is_ok(proven_fs_dir_next(&d.value, &e))) {
        if (proven_fs_is_staging_name(e.name)) ++n;
    }
    proven_fs_dir_close(&d.value);
    return n;
}

static void remove_staging(void) {
    char names[128][64];
    int n = 0;
    proven_result_dir_t d = proven_fs_dir_open(heap, PROVEN_LIT(DIR_NAME));
    if (!proven_is_ok(d.err)) return;
    proven_fs_dir_entry_t e;
    while (n < 128 && proven_is_ok(proven_fs_dir_next(&d.value, &e))) {
        if (proven_fs_is_staging_name(e.name) && e.name.size < 40) {
            snprintf(names[n++], sizeof names[0], DIR_NAME "/%.*s", (int)e.name.size, (const char *)e.name.ptr);
        }
    }
    proven_fs_dir_close(&d.value);
    for (int i = 0; i < n; ++i) (void)proven_fs_remove(heap, proven_u8str_view_from_cstr(names[i]));
}

int main(void) {
    PROVEN_TEST_SUITE("random staging names (RFC-0009 D-001)",
        "Leftover or planted staging files never block an atomic or durable write, and the writes leave none of their own.",
        "Inspect internal_write_file_atomic and internal_tmp_bits in src/proven/fs.c.");
    heap = proven_heap_allocator();

    (void)proven_fs_remove(heap, PROVEN_LIT(TARGET));
    remove_staging();
    (void)proven_fs_rmdir(heap, PROVEN_LIT(DIR_NAME));
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_mkdir(heap, PROVEN_LIT(DIR_NAME))), "the fixture directory is created", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the name test",
        "proven_fs_is_staging_name accepts today's 13-character suffix and the old two-digit one, and nothing else.",
        "Inspect proven_fs_is_staging_name.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp0123456789abc")), "13 characters from 0-9a-v", "");
    PROVEN_TEST_ASSERT(proven_fs_is_staging_name(PROVEN_LIT("dir/a.txt.pvtmpvvvvvvvvvvvvv")), "a whole path, only the end examined", "");
    PROVEN_TEST_ASSERT(proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp07")), "the names before 0.6.0", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp0123456789abw")), "w is outside the alphabet", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp0123456789AbC")), "upper case is outside it", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp0123456789ab")), "12 characters", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("a.txt.pvtmp7")), "one digit", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT(".pvtmp07")), "a suffix with no stem", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("a.txt")), "an ordinary name", "");
    PROVEN_TEST_ASSERT(!proven_fs_is_staging_name(PROVEN_LIT("")), "the empty name", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the eight old names, taken",
        "The reproducer: with <path>.pvtmp00 .. 07 present, both writes failed with PROVEN_ERR_IO on every call.",
        "If this fails with PROVEN_ERR_IO or PROVEN_ERR_EXISTS, the staging names are predictable again.");
    // ---------------------------------------------------------------
    put(TARGET, "v1");
    char name[96];
    for (int i = 0; i < 8; ++i) {
        snprintf(name, sizeof name, TARGET ".pvtmp%02d", i);
        put(name, "stale");
    }
    proven_err_t e = proven_fs_write_file_atomic(heap, PROVEN_LIT(TARGET), proven_mem_view_from_u8(PROVEN_LIT("v2")));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && holds(TARGET, "v2"), "the atomic write succeeds past the eight old names", "");
    e = proven_fs_write_file_durable(heap, PROVEN_LIT(TARGET), proven_mem_view_from_u8(PROVEN_LIT("v3")));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && holds(TARGET, "v3"), "the durable write succeeds past them", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("many names of today's shape, taken",
        "64 planted names of the new shape do not block the write either: the suffix has 64 random bits.",
        "A failure here means the random suffix is not random - check that internal_tmp_bits is called per attempt.");
    // ---------------------------------------------------------------
    for (int i = 0; i < 64; ++i) {
        snprintf(name, sizeof name, TARGET ".pvtmp%013d", i);
        put(name, "planted");
    }
    for (int round = 0; round < 32; ++round) {
        e = proven_fs_write_file_atomic(heap, PROVEN_LIT(TARGET), proven_mem_view_from_u8(PROVEN_LIT("v4")));
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "every atomic write succeeds", "");
    }
    PROVEN_TEST_ASSERT(holds(TARGET, "v4"), "and the target holds the last one", "");
    PROVEN_TEST_ASSERT(holds(TARGET ".pvtmp00", "stale") && holds(TARGET ".pvtmp0000000000063", "planted"),
        "files the library did not create are left alone", "Nothing may remove a staging file it did not just create: it may be another writer's.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("no staging file of its own is left behind",
        "Each successful write renames its staging file away; the count of staging-shaped names is exactly what was planted.",
        "A higher count means a rename or cleanup path lost track of its temp file.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(count_staging() == 8 + 64, "only the 72 planted names have the staging shape", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a failure that is not a collision is not retried",
        "A missing directory fails the same way on every attempt, so it is reported at once, as what it is.",
        "Before D-001 every failure was retried; now only PROVEN_ERR_EXISTS is.");
    // ---------------------------------------------------------------
    e = proven_fs_write_file_atomic(heap, PROVEN_LIT(DIR_NAME "/absent/x.txt"), proven_mem_view_from_u8(PROVEN_LIT("x")));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "a write into a missing directory is PROVEN_ERR_NOT_FOUND", "");

    remove_staging();
    (void)proven_fs_remove(heap, PROVEN_LIT(TARGET));
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_rmdir(heap, PROVEN_LIT(DIR_NAME))), "the fixture directory is empty at the end", "");
    PROVEN_TEST_PASS("random staging names");
    return 0;
}
