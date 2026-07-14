#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * A sweep of the public functions that no test had ever called.
 *
 * The list was found by diffing every `proven_*` symbol declared in include/proven/ against
 * every one mentioned anywhere in tests/ or manual/examples/. Most of the difference was
 * noise - `proven_arg_i32` and `proven_ring_push` are reached through macros, and a test that
 * writes PROVEN_ARG(x) exercises them without naming them. What was left is this file: real
 * public functions, shipped and documented, that nothing had ever run.
 *
 * Untested public API is where bugs live, because nothing has ever disagreed with it. These
 * are the ones where a bug would matter and is plausible: a path-converting syscall wrapper, a
 * bounds-checked slice, the formatter's caller-supplied-scratch path, a mutable lookup that
 * hands out a pointer into a table that rehashes, and three standard-stream bridges that
 * shipped one release ago with only their siblings covered.
 */

static int int_cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    PROVEN_TEST_SUITE("the public functions nothing had ever called",
        "Real, shipped, documented API that no test exercised: a symlink, a bounds-checked slice, the formatter's scratch path, mutable lookups, and the standard-stream bridges that were left uncovered.",
        "Each section names the function it covers. A failure here is a bug that shipped, in code nothing had ever run.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_symlink — a public filesystem call with no test",
        "It converts a path and makes a syscall; the tests that needed a symlink built theirs with POSIX directly and never went through this.",
        "");
    // ---------------------------------------------------------------
    {
        (void)proven_fs_remove(heap, PROVEN_LIT("psg_link"));
        (void)proven_fs_remove(heap, PROVEN_LIT("psg_target.txt"));

        proven_err_t w = proven_fs_write_file(heap, PROVEN_LIT("psg_target.txt"),
            proven_mem_view_from_u8(PROVEN_LIT("payload")));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "setup: write the target", "");

        proven_err_t e = proven_fs_symlink(heap, PROVEN_LIT("psg_target.txt"), PROVEN_LIT("psg_link"));
        PROVEN_TEST_ASSERT(proven_is_ok(e), "proven_fs_symlink must create the link", "");

        /* The link must resolve: reading through it gives the target's bytes. */
        proven_result_u8str_t r = proven_fs_read_all_u8str(heap, PROVEN_LIT("psg_link"));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "reading through the symlink must work", "");
        if (proven_is_ok(r.err)) {
            PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_u8str_as_view(&r.value), PROVEN_LIT("payload")),
                "and it must resolve to the target's contents", "");
            proven_u8str_destroy(heap, &r.value);
        }

        /* stat follows the link: it is a FILE, because that is what it points at. */
        proven_fs_stat_t st;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_stat(heap, PROVEN_LIT("psg_link"), &st)),
            "stat through the link must succeed", "");
        PROVEN_TEST_ASSERT(st.type == PROVEN_FS_TYPE_FILE,
            "stat follows the symlink, so the type is the target's", "");

        /* Creating it again over an existing name must fail rather than clobber. */
        proven_err_t again = proven_fs_symlink(heap, PROVEN_LIT("psg_target.txt"), PROVEN_LIT("psg_link"));
        PROVEN_TEST_ASSERT(!proven_is_ok(again),
            "symlinking over an existing name is an error, not a silent replace", "");

        (void)proven_fs_remove(heap, PROVEN_LIT("psg_link"));
        (void)proven_fs_remove(heap, PROVEN_LIT("psg_target.txt"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_mem_view_slice_checked / _mut_slice_checked — the bounds are the whole point",
        "A slice helper whose bounds check nothing has ever tried to break.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t bytes[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        proven_mem_view_t v = { .ptr = bytes, .size = sizeof bytes };

        proven_result_mem_view_t ok = proven_mem_view_slice_checked(v, 2, 3);
        PROVEN_TEST_ASSERT(proven_is_ok(ok.err) && ok.value.size == 3 && ok.value.ptr[0] == 2,
            "an in-range slice is the bytes it names", "");

        /* The exact end is in range; one past it is not. */
        PROVEN_TEST_ASSERT(proven_is_ok(proven_mem_view_slice_checked(v, 8, 0).err),
            "a zero-length slice at the very end is legal", "");
        PROVEN_TEST_ASSERT(!proven_is_ok(proven_mem_view_slice_checked(v, 0, 9).err),
            "a slice one byte past the end must be refused", "");
        PROVEN_TEST_ASSERT(!proven_is_ok(proven_mem_view_slice_checked(v, 9, 0).err),
            "an offset past the end must be refused", "");

        /* offset + size must not be allowed to wrap. */
        PROVEN_TEST_ASSERT(!proven_is_ok(proven_mem_view_slice_checked(v, 4, (proven_size_t)-1).err),
            "an offset+size that overflows must be refused, not wrapped",
            "A wrap here turns a bounds check into a pointer into nowhere.");

        proven_mem_mut_t m = { .ptr = bytes, .size = sizeof bytes };
        proven_result_mem_mut_t mo = proven_mem_mut_slice_checked(m, 6, 2);
        PROVEN_TEST_ASSERT(proven_is_ok(mo.err) && mo.value.size == 2,
            "the mutable form slices the same way", "");
        PROVEN_TEST_ASSERT(!proven_is_ok(proven_mem_mut_slice_checked(m, 7, 2).err),
            "and refuses the same overrun", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_u8str_append_fmt_with_scratch — the formatter's caller-scratch path",
        "The formatter can be handed a scratch allocator instead of allocating one. Nothing had ever passed it one.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t arena_mem[1024];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ .ptr = arena_mem, .size = sizeof arena_mem });
        proven_allocator_t scratch = proven_arena_as_allocator(&arena);

        proven_result_u8str_t rs = proven_u8str_create(heap, 64);
        PROVEN_TEST_ASSERT(proven_is_ok(rs.err), "setup", "");
        proven_u8str_t s = rs.value;

        proven_fmt_result_t fr = proven_u8str_append_fmt_with_scratch(
            heap, &s, "{} and {}", scratch, PROVEN_ARG(42), PROVEN_ARG("text"));
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(fr),
            "formatting through a caller-supplied scratch must succeed", "");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s), "42 and text") == 0,
            "and must render exactly what the ordinary path renders",
            "A different answer here means the scratch path is a second formatter, not the same one.");

        proven_u8str_destroy(heap, &s);
        proven_arena_destroy(&arena);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_map_get_mut and proven_array_get_mut — the mutable lookups",
        "The const forms are covered everywhere; the mutable ones, which hand out a pointer INTO the storage, were not.",
        "");
    // ---------------------------------------------------------------
    {
        proven_result_map_t mr = proven_map_create(heap, 16, PROVEN_KEY_TYPE_INT, sizeof(int), alignof(int));
        PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup", "");
        proven_map_t m = mr.value;

        int v = 10;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_map_set(&m, (proven_map_key_t){ .id = 1 }, &v)), "set", "");

        int *p = (int *)proven_map_get_mut(&m, (proven_map_key_t){ .id = 1 });
        PROVEN_TEST_ASSERT(p != NULL && *p == 10, "get_mut must find the value", "");
        if (p) *p = 99;   /* mutate through it */

        const int *c = (const int *)proven_map_get(&m, (proven_map_key_t){ .id = 1 });
        PROVEN_TEST_ASSERT(c && *c == 99,
            "a write through get_mut must be visible in the map",
            "If it is not, get_mut is handing out a copy, and every caller mutating through it is silently doing nothing.");

        PROVEN_TEST_ASSERT(proven_map_get_mut(&m, (proven_map_key_t){ .id = 7 }) == NULL,
            "a missing key is NULL, not a pointer to somewhere", "");
        PROVEN_TEST_ASSERT(proven_map_is_valid(&m), "the map is still valid", "");
        proven_map_destroy(&m);

        proven_result_array_t ar = PROVEN_ARRAY_INIT(heap, int, 4);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "setup", "");
        proven_array_t arr = ar.value;
        int x = 5;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_array_push(&arr, &x)), "push", "");

        int *ap = (int *)proven_array_get_mut(&arr, 0);
        PROVEN_TEST_ASSERT(ap && *ap == 5, "array get_mut must find the element", "");
        if (ap) *ap = 77;
        const int *ac = (const int *)proven_array_get(&arr, 0);
        PROVEN_TEST_ASSERT(ac && *ac == 77, "and the write must stick", "");
        PROVEN_TEST_ASSERT(proven_array_get_mut(&arr, 99) == NULL, "out of range is NULL", "");

        /* linear_search, the sibling of the binary search everything else uses. Unlike the
         * binary one it does not need the array sorted - which is the only reason to have it. */
        int more[3] = { 3, 1, 2 };   /* deliberately unsorted */
        for (int i = 0; i < 3; ++i) {
            PROVEN_TEST_ASSERT(proven_is_ok(proven_array_push(&arr, &more[i])), "push", "");
        }

        int key = 1;
        int *hit = (int *)proven_array_linear_search(&arr, &key, int_cmp);
        PROVEN_TEST_ASSERT(hit != NULL && *hit == 1,
            "linear_search must find a present key in an UNSORTED array",
            "That it does not need a sorted array is the only reason it exists next to the binary search.");

        int absent = 12345;
        PROVEN_TEST_ASSERT(proven_array_linear_search(&arr, &absent, int_cmp) == NULL,
            "and an absent key is NULL, not a pointer to the nearest element", "");

        proven_array_destroy(&arr);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_u16str_create_from_view — the U16 constructor nothing built with",
        "The u16 seal bug lived one function away from this one.",
        "");
    // ---------------------------------------------------------------
    {
        proven_result_u16str_t r = proven_u16str_create_from_view(heap, PROVEN_U16_LIT("hello"));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "creating from a view must succeed", "");
        if (proven_is_ok(r.err)) {
            proven_u16str_t s = r.value;
            PROVEN_TEST_ASSERT(proven_u16str_len(&s) == 5, "the length is in UNITS, not bytes", "");
            const proven_u16 *p = proven_u16str_as_ptr(&s);
            PROVEN_TEST_ASSERT(p[0] == u'h' && p[4] == u'o', "the content must round-trip", "");
            PROVEN_TEST_ASSERT(p[5] == 0,
                "and it must be sealed at the right UNIT index",
                "This is exactly where the seal bug was: writing the terminator at the byte offset lands twice as far out.");
            proven_u16str_destroy(heap, &s);
        }

        /* An empty view is a legal, sealed, empty string. */
        proven_result_u16str_t e = proven_u16str_create_from_view(heap, (proven_u16str_view_t){ NULL, 0 });
        PROVEN_TEST_ASSERT(proven_is_ok(e.err) && proven_u16str_len(&e.value) == 0,
            "an empty view gives an empty string, not an error", "");
        if (proven_is_ok(e.err)) proven_u16str_destroy(heap, &e.value);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the standard-stream bridges that shipped without a test",
        "stdout_writer and stdin_lines were covered; stderr_writer, stdin_reader and file_buffered were not.",
        "");
    // ---------------------------------------------------------------
    {
        /* stderr_writer: valid, and it writes. Aim it at a file rather than the terminal by
         * using the file form of the same bridge, so the bytes can be checked. */
        proven_sysio_std_t est;
        proven_writer_t ew = proven_sysio_stderr_writer(&est);
        PROVEN_TEST_ASSERT(proven_writer_is_valid(ew), "proven_sysio_stderr_writer must be valid", "");
        PROVEN_TEST_ASSERT(!proven_writer_is_valid(proven_sysio_stderr_writer(NULL)),
            "and a NULL state gives an invalid writer, not a crash", "");

        proven_sysio_std_t ist;
        proven_reader_t ir = proven_sysio_stdin_reader(&ist);
        PROVEN_TEST_ASSERT(proven_reader_is_valid(ir), "proven_sysio_stdin_reader must be valid", "");
        PROVEN_TEST_ASSERT(!proven_reader_is_valid(proven_sysio_stdin_reader(NULL)),
            "and a NULL state gives an invalid reader", "");

        /* file_buffered: the whole point is that the bytes land in the FILE, and only on flush. */
        (void)proven_fs_remove(heap, PROVEN_LIT("psg_buf.txt"));
        proven_result_file_t of = proven_fs_open(heap, PROVEN_LIT("psg_buf.txt"),
            PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
        PROVEN_TEST_ASSERT(proven_is_ok(of.err), "setup: open a file", "");

        proven_byte_t obuf[128];
        proven_sysio_out_t out;
        proven_writer_t w = proven_sysio_file_buffered(&out, of.value,
            (proven_mem_mut_t){ .ptr = obuf, .size = sizeof obuf });
        PROVEN_TEST_ASSERT(proven_writer_is_valid(w), "file_buffered must give a valid writer", "");

        for (int i = 0; i < 3; ++i) {
            PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_fprintln(w, "row {}", PROVEN_ARG(i))),
                "the formatter must write into the buffered file writer", "");
        }
        PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_flush(w)), "the flush must succeed", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_close(of.value)), "close must succeed", "");

        proven_result_u8str_t rd = proven_fs_read_all_u8str(heap, PROVEN_LIT("psg_buf.txt"));
        PROVEN_TEST_ASSERT(proven_is_ok(rd.err), "read the file back", "");
        if (proven_is_ok(rd.err)) {
            PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&rd.value), "row 0\nrow 1\nrow 2\n") == 0,
                "every buffered line must be in the file, in order, after the flush", "");
            proven_u8str_destroy(heap, &rd.value);
        }
        (void)proven_fs_remove(heap, PROVEN_LIT("psg_buf.txt"));

        /* An invalid buffer must be refused rather than half-wired. */
        proven_sysio_out_t bad;
        PROVEN_TEST_ASSERT(!proven_writer_is_valid(
            proven_sysio_file_buffered(&bad, of.value, (proven_mem_mut_t){ .ptr = NULL, .size = 0 })),
            "an empty buffer gives an invalid writer", "");
    }

    PROVEN_TEST_PASS("the public surface nothing had ever called now has something calling it.");
    return 0;
}
