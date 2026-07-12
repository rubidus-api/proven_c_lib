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


/* A counting allocator: the allocation *shape* of a whole-file read is part of
 * its contract, so the tests below measure it rather than trusting it. */
static long g_reallocs = 0;
static long g_peak = 0;
static long g_live = 0;

static proven_result_mem_mut_t counting_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx; (void)align;
    proven_result_mem_mut_t r = {0};
    proven_allocator_t h = proven_heap_allocator();
    r = h.alloc_fn(h.ctx, size, align);
    if (proven_is_ok(r.err)) {
        g_live += (long)size;
        if (g_live > g_peak) g_peak = g_live;
    }
    return r;
}
static proven_result_mem_mut_t counting_realloc(void *ctx, void *ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ctx;
    proven_allocator_t h = proven_heap_allocator();
    proven_result_mem_mut_t r = h.realloc_fn(h.ctx, ptr, old_size, new_size, align);
    if (proven_is_ok(r.err)) {
        g_reallocs++;
        g_live += (long)new_size;
        if (g_live > g_peak) g_peak = g_live;
        g_live -= (long)old_size;
    }
    return r;
}
static void counting_free_trait(void *ctx, void *ptr) {
    (void)ctx;
    proven_allocator_t h = proven_heap_allocator();
    h.free_fn(h.ctx, ptr);
}
static proven_allocator_t counting_allocator(void) {
    return (proven_allocator_t){ .ctx = 0, .alloc_fn = counting_alloc, .realloc_fn = counting_realloc, .free_fn = counting_free_trait };
}
static void counting_reset(void) { g_reallocs = 0; g_peak = 0; g_live = 0; }
static void counting_free(void *p) { proven_allocator_t h = proven_heap_allocator(); h.free_fn(h.ctx, p); }

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
    PROVEN_TEST_SECTION("read_all costs one allocation for a regular file",
        "The reported size must seed the buffer exactly; EOF is confirmed with a one-byte probe, not by doubling the buffer.",
        "If reallocs > 0 here, internal_read_to_eof is growing the buffer just to discover the file ended where its size said it would - doubling peak memory for every file read.");
    // ---------------------------------------------------------------
    {
        proven_size_t n = 64u * 1024u;
        proven_result_mem_mut_t blob = heap.alloc_fn(heap.ctx, n, 1);
        PROVEN_TEST_ASSERT(proven_is_ok(blob.err), "fixture allocation failed", "");
        for (proven_size_t i = 0; i < n; ++i) blob.value.ptr[i] = (proven_byte_t)(i & 0xFF);

        proven_u8str_view_t big = PROVEN_LIT("test_slurp_big.bin");
        w = proven_fs_write_file(heap, big, (proven_mem_view_t){ .ptr = blob.value.ptr, .size = n });
        PROVEN_TEST_ASSERT(proven_is_ok(w), "failed to write the large fixture", "");

        counting_reset();
        proven_result_mem_mut_t cr = proven_fs_read_all(counting_allocator(), big);
        PROVEN_TEST_ASSERT(proven_is_ok(cr.err) && cr.value.size == n, "read_all lost bytes on the large fixture", "");
        PROVEN_TEST_ASSERT(g_reallocs == 0,
            "read_all reallocated while reading a regular file of known size",
            "the buffer must be seeded to the reported size and EOF confirmed with a probe byte");
        PROVEN_TEST_ASSERT(g_peak <= (long)(n + 64),
            "read_all peaked above the file size",
            "a doubling here means peak memory is 2-3x the file, which a large file cannot afford");
        counting_free(cr.value.ptr);
        heap.free_fn(heap.ctx, blob.value.ptr);
        (void)proven_fs_remove(heap, big);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a size-0 source starts from the chunk size, not one byte",
        "read_all_u8str reserves a terminator byte, so a capacity test cannot stand in for a size test.",
        "If the realloc count is large, internal_slurp_path is testing cap == 0 (never true when extra == 1) instead of the reported size.");
    // ---------------------------------------------------------------
    {
        proven_result_mem_mut_t probe = proven_fs_read_all(heap, proc_path);
        if (proven_is_ok(probe.err) && probe.value.size > 0) {
            heap.free_fn(heap.ctx, probe.value.ptr);
            counting_reset();
            proven_result_u8str_t ps = proven_fs_read_all_u8str(counting_allocator(), proc_path);
            PROVEN_TEST_ASSERT(proven_is_ok(ps.err) && proven_u8str_as_view(&ps.value).size > 0,
                "read_all_u8str returned nothing for a size-0-reporting source", "");
            PROVEN_TEST_ASSERT(g_reallocs <= 2,
                "read_all_u8str doubled its way up from a tiny buffer",
                "a source that reports size 0 must start from the chunk size, not from the terminator reservation");
            counting_free(ps.value.internal.ptr);
        } else {
            PROVEN_TEST_INFO("no size-0 source available; skipping chunk-size check");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("write_file_atomic preserves the target's permissions",
        "Rewriting a file must not widen access to it: a 0600 file must not come back 0644.",
        "The temp sibling is created fresh with 0666 & ~umask, and rename carries its mode onto the target - so the target's mode must be copied onto the temp before the rename.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t secret = PROVEN_LIT("test_slurp_secret.key");
        w = proven_fs_write_file(heap, secret, proven_mem_view_from_u8(content));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "failed to write the secret fixture", "");
        proven_err_t ce = proven_fs_chmod(heap, secret, PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W);
        PROVEN_TEST_ASSERT(proven_is_ok(ce), "chmod 0600 failed", "");

        w = proven_fs_write_file_atomic(heap, secret, proven_mem_view_from_u8(replaced));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "atomic rewrite of a 0600 file failed", "");

        proven_fs_stat_t st = {0};
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_stat(heap, secret, &st)), "stat after atomic rewrite failed", "");
        PROVEN_TEST_ASSERT(st.perms == (PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W),
            "an atomic rewrite widened the file's permissions",
            "the target's mode must be copied onto the temp file before the rename");
        (void)proven_fs_remove(heap, secret);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("fs_stat reports permission bits, not the raw mode",
        "stat's perms must round-trip straight into chmod; that is the whole point of the field.",
        "The raw st_mode carries the file-type bits (S_IFREG). Handing those back in a proven_fs_perms_t makes chmod reject them as unsupported.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t pf = PROVEN_LIT("test_slurp_perm.txt");
        w = proven_fs_write_file(heap, pf, proven_mem_view_from_u8(content));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "failed to write the perm fixture", "");

        proven_fs_stat_t st = {0};
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_stat(heap, pf, &st)), "stat failed", "");

        const proven_fs_perms_t all9 =
            PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W | PROVEN_FS_PERM_OWNER_X |
            PROVEN_FS_PERM_GROUP_R | PROVEN_FS_PERM_GROUP_W | PROVEN_FS_PERM_GROUP_X |
            PROVEN_FS_PERM_OTHER_R | PROVEN_FS_PERM_OTHER_W | PROVEN_FS_PERM_OTHER_X;
        PROVEN_TEST_ASSERT((st.perms & ~all9) == 0,
            "fs_stat returned bits outside the nine permission bits",
            "perms must be masked to the low nine; the raw st_mode also holds the file-type bits");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(heap, pf, st.perms)),
            "a stat's perms could not be fed back to chmod",
            "this round-trip is the obvious use of the field and must work");
        (void)proven_fs_remove(heap, pf);
    }

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
