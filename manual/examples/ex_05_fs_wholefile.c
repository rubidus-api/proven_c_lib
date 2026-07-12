#include "example.h"

/*
 * The whole-file API: one call in, one call out. It exists because the
 * open/read-loop/close dance is where most file-handling bugs live - a forgotten
 * close, a partial read treated as EOF, a truncated file left behind by a failed
 * write. If you are reading or writing a file in its entirety, this is the API.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A relative path in the current directory: the example must not depend on a
     * writable /tmp, and it removes what it creates before returning. */
    proven_u8str_view_t path = PROVEN_LIT("proven_example_wholefile.tmp");
    proven_u8str_view_t text = PROVEN_LIT("first line\nsecond line\n");

    /* --- write it in one call ---------------------------------------------- */
    /* Not atomic: a concurrent reader can see this file half-written. Fine here,
     * because nobody else is looking at it yet. */
    proven_err_t err = proven_fs_write_file(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole file should succeed");
    if (!proven_is_ok(err)) return 1;

    /* --- read it back as raw bytes ----------------------------------------- */
    /* proven_fs_read_all reads to EOF rather than to a pre-measured size, so it
     * also works on a pipe or a /proc entry, whose size cannot be known up front. */
    proven_result_mem_mut_t raw = proven_fs_read_all(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(raw.err), "reading the whole file should succeed");
    if (proven_is_ok(raw.err)) {
        EXAMPLE_REQUIRE(raw.value.size == text.size, "read_all should return every byte written");
        /* The block is plain allocator memory - hand it back to the allocator that
         * produced it. There is no proven_fs_read_all_destroy. */
        alloc.free_fn(alloc.ctx, raw.value.ptr);
    }

    /* --- read it back as a string ------------------------------------------ */
    /* This is the one most callers want: the result is NUL-terminated, so it can
     * be handed to a view, to as_cstr, or to the scanner with no second copy. The
     * terminator slot is reserved up front, so it costs no extra allocation. */
    proven_result_u8str_t s = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "reading the whole file as a string should succeed");
    if (!proven_is_ok(s.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), text),
                    "the file's contents should come back unchanged");
    printf("read back %zu bytes: %s", (size_t)proven_u8str_as_view(&s.value).size,
           proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- stat, and the perms round-trip ------------------------------------ */
    proven_fs_stat_t st = {0};
    err = proven_fs_stat(alloc, path, &st);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat on a file we just wrote should succeed");
    EXAMPLE_REQUIRE(st.type == PROVEN_FS_TYPE_FILE, "a regular file should stat as a FILE");
    EXAMPLE_REQUIRE(st.size == text.size, "stat should report the size we wrote");

    /* `perms` carries the nine permission bits and nothing else, so it can be fed
     * straight back to chmod. That is the whole point of the field: read a file's
     * mode, and later restore it. (It used to carry the raw POSIX st_mode, whose
     * file-type bits chmod rejects - so this obvious round-trip failed.) */
    err = proven_fs_chmod(alloc, path, st.perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a stat's perms must be accepted back by chmod");

    /* Now make the file owner-only, so the next check has something to prove. */
    proven_fs_perms_t private_perms = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W;
    err = proven_fs_chmod(alloc, path, private_perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "restricting the file to its owner should succeed");

    /* --- rewrite it atomically --------------------------------------------- */
    /* A sibling temp file plus a rename: a concurrent reader sees either the whole
     * old file or the whole new one, never a half-written mix. Atomic for readers,
     * not durable across power loss - proven exposes no fsync. */
    proven_u8str_view_t text2 = PROVEN_LIT("replacement\n");
    err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text2));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the atomic rewrite should succeed");

    proven_fs_stat_t st2 = {0};
    err = proven_fs_stat(alloc, path, &st2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat after the atomic rewrite should succeed");
    EXAMPLE_REQUIRE(st2.size == text2.size, "the file should now hold the replacement text");
    /* The rename writes a *new* inode over the old name, so the permissions would
     * be lost unless they were copied across. They are: rewriting a 0600 file does
     * not republish it as 0644. */
    EXAMPLE_REQUIRE(st2.perms == private_perms,
                    "the atomic rewrite must preserve the target's permissions");

    /* --- clean up ----------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
