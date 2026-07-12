#include "example.h"

/*
 * There are two string handles here and the difference is ownership, not size:
 *
 *   proven_u8str_t      - a byte string you can edit. It either owns an
 *                         allocation (create) or borrows one of yours (borrow).
 *   proven_u8str_view_t - a pointer and a length into somebody else's bytes.
 *                         It owns nothing, it is not NUL-terminated, and it is
 *                         only valid while those bytes are.
 *
 * A view is what you pass to a function that reads. A u8str is what you keep.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- an OWNED string: the allocator's memory, yours to destroy ---------- */
    /* The capacity argument is content bytes; the NUL is extra, so as_cstr is
     * always O(1) and always safe. */
    proven_result_u8str_t r = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 16-byte string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t path = r.value;

    /* append is fixed-capacity: it fits or it fails, and on failure it has not
     * touched the string. It never reallocates, so it needs no allocator. */
    proven_err_t err = proven_u8str_append(&path, PROVEN_LIT("/etc/hosts"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "10 bytes fit in a 16-byte string");

    /* append_grow is the growable twin: give it the allocator the string was
     * created with and it reallocates when needed. Still failure-atomic - if the
     * allocation fails, the string is exactly as it was. */
    err = proven_u8str_append_grow(alloc, &path, PROVEN_LIT(".backup.original"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "append_grow must reallocate rather than fail");

    /* Edits in the middle. insert shifts the tail right; remove shifts it left. */
    err = proven_u8str_insert_grow(alloc, &path, 0, PROVEN_LIT("/mnt"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a prefix must succeed");

    err = proven_u8str_remove(&path, proven_u8str_as_view(&path).size - 9, 9);  /* drop ".original" */
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the trailing suffix must succeed");

    /* replace_first returns PROVEN_OK when the target is absent - "nothing to do"
     * is not an error. Search first when the difference matters to you. */
    err = proven_u8str_replace_first(&path, 0, PROVEN_LIT("hosts"), PROVEN_LIT("fstab"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "replacing an existing substring must succeed");

    /* --- reading it: borrow a view, do not copy ----------------------------- */
    /* as_view is free. The view is only good until the next edit: any growing
     * call may reallocate and leave the view (and any cstr) dangling. */
    proven_u8str_view_t v = proven_u8str_as_view(&path);

    EXAMPLE_REQUIRE(proven_u8str_view_eq(v, PROVEN_LIT("/mnt/etc/fstab.backup")),
                    "the edits above should have produced /mnt/etc/fstab.backup");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(v, PROVEN_LIT("/mnt")),
                    "the inserted prefix is at the front");

    proven_size_t dot = proven_u8str_view_find(v, 0, PROVEN_LIT(".backup"));
    EXAMPLE_REQUIRE(dot != PROVEN_INDEX_NOT_FOUND, "the suffix must be found");

    /* A slice is a view into the SAME bytes - no allocation, no copy. */
    proven_u8str_view_t stem = proven_u8str_view_slice(v, 0, dot);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(stem, PROVEN_LIT("/mnt/etc/fstab")),
                    "slicing at the suffix leaves the stem");

    /* as_cstr is the escape hatch to C APIs, and it is only valid because the
     * owned string keeps a NUL past its length. Do NOT do this with a view:
     * `stem.ptr` is not NUL-terminated - it just points into `path`. */
    printf("owned:  %s\n", proven_u8str_as_cstr(&path));

    /* --- a BORROWED string: your memory, no allocation at all --------------- */
    /* Same type, same operations - but the bytes are this stack buffer. `cap`
     * includes the NUL, so this holds 31 content bytes. */
    proven_byte_t line[32];
    proven_u8str_t status = proven_u8str_borrow(line, sizeof line);

    err = proven_u8str_append(&status, PROVEN_LIT("mounted "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into a borrowed buffer needs no allocator");
    err = proven_u8str_append(&status, stem);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a view can be appended just like a literal");

    /* The growing calls exist for a borrowed string, but they refuse to
     * reallocate memory they do not own: too much data is OUT_OF_BOUNDS, and
     * `line` is left untouched. A borrowed string cannot silently escape to the
     * heap behind your back. */
    err = proven_u8str_append_grow(alloc, &status,
                                   PROVEN_LIT(" ...and a great deal more text than fits"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a borrowed string reports overflow instead of reallocating caller memory");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&status), PROVEN_LIT("mounted /mnt/etc/fstab")),
                    "the failed append must have left the string unchanged");

    printf("borrowed: %s\n", proven_u8str_as_cstr(&status));

    /* reset truncates to empty and keeps the buffer, so the next frame reuses
     * the same 32 bytes with no allocation. */
    err = proven_u8str_reset(&status);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reset must succeed on a borrowed string");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&status).size == 0, "reset empties the string");

    /* --- destroy: the ownership rule, spelled out --------------------------- */
    /* destroy on the borrowed string is a no-op - `line` is not the library's to
     * free. Calling it anyway is correct and costs nothing, and it means the
     * teardown code does not have to know which kind of string it holds. */
    proven_u8str_destroy(alloc, &status);

    /* destroy on the owned string frees the allocation, and it must be given the
     * allocator the string was created with. */
    proven_u8str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
