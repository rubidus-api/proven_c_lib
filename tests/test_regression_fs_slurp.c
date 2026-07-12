#include "proven/fs.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <stdbool.h>

/*
 * Whole-file read and whole-file write.
 *
 * The regression this suite exists for: proven_fs_size reports 0 for anything
 * that is not a regular file, and proven_fs_read_all used that 0 as the buffer
 * size. Reading a FIFO or a /proc entry therefore returned an empty buffer with
 * PROVEN_OK - every byte silently dropped. read_all now reads to EOF.
 */

static bool bytes_eq(proven_mem_mut_t got, proven_u8str_view_t want) {
    if (got.size != want.size) return false;
    for (proven_size_t i = 0; i < want.size; ++i) {
        if (got.ptr[i] != want.ptr[i]) return false;
    }
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("fs whole-file read/write",
        "Verify whole-file read and write: read to EOF (not to a reported size), NUL-terminated string form, and atomic replace.",
        "Inspect internal_slurp_path / internal_read_to_eof in src/proven/fs.c and the write_file / write_file_atomic entry points.");
    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("read_all: regular file round-trips",
        "A regular file must read back byte-for-byte what write_file wrote.",
        "Inspect the single-pass path: proven_fs_size seeds capacity, internal_read_to_eof fills it.");
    // ---------------------------------------------------------------
    proven_u8str_view_t path = PROVEN_LIT("test_slurp.txt");
    proven_u8str_view_t content = PROVEN_LIT("line one\nline two\n");

    proven_err_t w = proven_fs_write_file(heap, path, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "write_file failed", "check open flags WRITE|CREATE|TRUNC");

    proven_result_mem_mut_t r = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "read_all failed on regular file", "");
    PROVEN_TEST_ASSERT(bytes_eq(r.value, content), "read_all returned wrong bytes", "");
    heap.free_fn(heap.ctx, r.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("write_file truncates a longer existing file",
        "Writing shorter content must not leave a tail of the previous content behind.",
        "Inspect the open flags in proven_fs_write_file; PROVEN_FS_TRUNC must be set.");
    // ---------------------------------------------------------------
    proven_u8str_view_t shorter = PROVEN_LIT("hi");
    w = proven_fs_write_file(heap, path, proven_mem_view_from_u8(shorter));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "write_file (truncating) failed", "");
    r = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "read_all after truncate failed", "");
    PROVEN_TEST_ASSERT(bytes_eq(r.value, shorter), "file was not truncated to the new content", "TRUNC flag missing?");
    heap.free_fn(heap.ctx, r.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("read_all: empty file yields NULL/0 with OK",
        "An empty file is not an error and must not allocate.",
        "Inspect the len == 0 branch of proven_fs_read_all.");
    // ---------------------------------------------------------------
    proven_u8str_view_t empty_path = PROVEN_LIT("test_slurp_empty.txt");
    proven_mem_view_t nothing = { .ptr = (const proven_byte_t*)0, .size = 0 };
    w = proven_fs_write_file(heap, empty_path, nothing);
    PROVEN_TEST_ASSERT(proven_is_ok(w), "write_file of empty content failed", "");

    r = proven_fs_read_all(heap, empty_path);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "read_all on empty file must succeed", "");
    PROVEN_TEST_ASSERT(r.value.size == 0, "empty file must read as 0 bytes", "");
    PROVEN_TEST_ASSERT(r.value.ptr == 0, "empty file must return a NULL pointer", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("read_all: source with no knowable size (regression)",
        "proven_fs_size reports 0 for non-regular files; read_all must still return their bytes.",
        "The reported size may only seed capacity. If this fails, read_all is bounding the read by the reported size again.");
    // ---------------------------------------------------------------
    /* /proc/self/status is a regular-looking file whose st_size is 0. Before the
     * fix this returned OK with zero bytes. It only exists on Linux; where it is
     * absent the open fails and there is nothing to regress. */
    proven_u8str_view_t proc_path = PROVEN_LIT("/proc/self/status");
    proven_result_mem_mut_t pr = proven_fs_read_all(heap, proc_path);
    if (proven_is_ok(pr.err)) {
        PROVEN_TEST_ASSERT(pr.value.size > 0,
            "read_all returned 0 bytes for a size-0-reporting source",
            "proven_fs_size reports 0 for non-regular files; read_all must read to EOF, not to the reported size");
        heap.free_fn(heap.ctx, pr.value.ptr);
        PROVEN_TEST_INFO("/proc/self/status read to EOF (size-0-reporting source)");
    } else {
        PROVEN_TEST_INFO("/proc/self/status unavailable; skipping unknown-size source check");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("read_all_u8str: NUL-terminated owned string",
        "The string form must satisfy the u8str NUL invariant that proven_u8str_as_cstr relies on.",
        "Inspect the +1 terminator reservation and the s.ptr[s.len] = 0 store in proven_fs_read_all_u8str.");
    // ---------------------------------------------------------------
    w = proven_fs_write_file(heap, path, proven_mem_view_from_u8(content));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "write_file failed", "");

    proven_result_u8str_t s = proven_fs_read_all_u8str(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(s.err), "read_all_u8str failed", "");
    PROVEN_TEST_ASSERT(proven_u8str_is_valid(&s.value), "read_all_u8str produced an invalid string", "");
    proven_u8str_view_t got = proven_u8str_as_view(&s.value);
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(got, content), "read_all_u8str returned wrong contents", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_cstr(&s.value)[content.size] == '\0',
        "read_all_u8str result is not NUL-terminated",
        "the +1 terminator slot must be reserved and written");
    proven_u8str_destroy(heap, &s.value);

    /* Empty file must still yield a usable, terminated, destroyable string. */
    s = proven_fs_read_all_u8str(heap, empty_path);
    PROVEN_TEST_ASSERT(proven_is_ok(s.err), "read_all_u8str failed on empty file", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&s.value).size == 0, "empty file must yield an empty string", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_cstr(&s.value)[0] == '\0', "empty string must be NUL-terminated", "");
    proven_u8str_destroy(heap, &s.value);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("write_file_atomic: replaces content, leaves no debris",
        "The rename must replace the target and consume the temp sibling.",
        "Inspect the CREATE_NEW temp open, the rename, and the cleanup remove in proven_fs_write_file_atomic.");
    // ---------------------------------------------------------------
    proven_u8str_view_t replaced = PROVEN_LIT("replaced atomically");
    w = proven_fs_write_file_atomic(heap, path, proven_mem_view_from_u8(replaced));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "write_file_atomic failed", "check CREATE_NEW temp open and rename");

    r = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "read_all after atomic write failed", "");
    PROVEN_TEST_ASSERT(bytes_eq(r.value, replaced), "atomic write did not replace the contents", "");
    heap.free_fn(heap.ctx, r.value.ptr);

    /* The temp sibling must be gone: the rename consumed it. */
    proven_u8str_view_t tmp0 = PROVEN_LIT("test_slurp.txt.pvtmp00");
    proven_result_file_t leftover = proven_fs_open(heap, tmp0, PROVEN_FS_READ);
    PROVEN_TEST_ASSERT(!proven_is_ok(leftover.err),
        "atomic write left its temp file behind",
        "the rename should have consumed the temp file");

    /* Atomic write to a path that does not exist yet must create it. */
    proven_u8str_view_t fresh = PROVEN_LIT("test_slurp_fresh.txt");
    (void)proven_fs_remove(heap, fresh);
    w = proven_fs_write_file_atomic(heap, fresh, proven_mem_view_from_u8(replaced));
    PROVEN_TEST_ASSERT(proven_is_ok(w), "atomic write to a new path failed", "");
    r = proven_fs_read_all(heap, fresh);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && bytes_eq(r.value, replaced), "atomic write to a new path lost content", "");
    heap.free_fn(heap.ctx, r.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("invalid arguments are rejected",
        "An invalid allocator or a missing file must fail as a value, never crash.",
        "Inspect the proven_alloc_is_valid guards at the top of each entry point.");
    // ---------------------------------------------------------------
    proven_allocator_t bad = {0};
    proven_result_mem_mut_t br = proven_fs_read_all(bad, path);
    PROVEN_TEST_ASSERT(br.err == PROVEN_ERR_INVALID_ARG, "read_all must reject an invalid allocator", "");
    proven_result_u8str_t bs = proven_fs_read_all_u8str(bad, path);
    PROVEN_TEST_ASSERT(bs.err == PROVEN_ERR_INVALID_ARG, "read_all_u8str must reject an invalid allocator", "");
    PROVEN_TEST_ASSERT(proven_fs_write_file(bad, path, proven_mem_view_from_u8(content)) == PROVEN_ERR_INVALID_ARG,
        "write_file must reject an invalid allocator", "");
    PROVEN_TEST_ASSERT(proven_fs_write_file_atomic(bad, path, proven_mem_view_from_u8(content)) == PROVEN_ERR_INVALID_ARG,
        "write_file_atomic must reject an invalid allocator", "");

    proven_result_mem_mut_t missing = proven_fs_read_all(heap, PROVEN_LIT("test_slurp_does_not_exist.txt"));
    PROVEN_TEST_ASSERT(!proven_is_ok(missing.err), "read_all of a missing file must fail", "");

    // cleanup
    (void)proven_fs_remove(heap, path);
    (void)proven_fs_remove(heap, empty_path);
    (void)proven_fs_remove(heap, fresh);

    PROVEN_TEST_PASS("fs whole-file read/write behavior passed.");
    return 0;
}
