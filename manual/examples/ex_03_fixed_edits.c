#include "example.h"

/*
 * The previous example grew a string whenever it ran out of room. This one is
 * about the case where growing is not allowed - a fixed-size record, a log line
 * with a hard length limit, a buffer in an arena that must not be reallocated -
 * and about the two honest answers a call can give when the data does not fit:
 *
 *   "no, and I changed nothing"      - the atomic calls: append, insert,
 *                                      replace_at. They check the capacity
 *                                      first, so a refusal leaves the string
 *                                      exactly as it was.
 *   "some of it, and here is how much" - the best-effort call:
 *                                      append_partial. It fills what it can and
 *                                      tells you the byte count it wrote.
 *
 * Both are useful; picking the wrong one silently truncates a record or
 * silently drops one. The `_grow` variants are the third answer - "yes, I found
 * more room" - and appear at the end for contrast.
 */

#define FIELD_CAP 32u

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the field buffer must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t field = r.value;

    /* is_valid checks the handle's own structure - a pointer with a capacity and
     * a length that do not contradict each other. Worth asserting once at the
     * boundary of your code when a string arrives from somewhere else; it is not
     * a check you need after every edit, because every edit maintains it. */
    EXAMPLE_REQUIRE(proven_u8str_is_valid(&field), "a freshly created string must be structurally valid");

    /* --- reserving room up front ------------------------------------------ */

    /* reserve raises the capacity now, so later growth does not reallocate. On
     * the heap that saves copies; in an arena it saves something worse, because
     * every reallocation there leaks the old block until the next reset. Ask for
     * what you expect to need, once. */
    proven_err_t err = proven_u8str_reserve(alloc, &field, 64);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving 64 bytes must succeed");
    EXAMPLE_REQUIRE(field.internal.cap >= 64, "the capacity must actually be at least what was asked for");

    /* --- the atomic calls: fit, or change nothing -------------------------- */

    err = proven_u8str_append(&field, PROVEN_LIT("2026-01-01 level=info "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the prefix fits in the reserved capacity");

    /* append_byte adds one byte, which is what separators, terminators and
     * escape characters are. It takes the allocator because it is a growing
     * call - one byte is exactly the case where a capacity check would fail on
     * a boundary you did not think about. */
    err = proven_u8str_append_byte(alloc, &field, (proven_u8)'[');
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending a single separator byte must succeed");

    err = proven_u8str_append(&field, PROVEN_LIT("disk full"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the message fits");

    err = proven_u8str_append_byte(alloc, &field, (proven_u8)']');
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the bracket must succeed");

    /* Now ask for more than the capacity can hold. The atomic append refuses and
     * - this is the property worth relying on - the string still holds exactly
     * what it held before the call. */
    proven_size_t before = proven_u8str_as_view(&field).size;
    err = proven_u8str_append(&field, PROVEN_LIT(" and a very long trailing explanation that certainly does not fit"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized atomic append must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before, "and must leave the string untouched");

    /* --- the best-effort call: as much as fits, and the count -------------- */

    /* A fixed-width column in a report is the case for this one: write what
     * fits, and know how much was written so the caller can mark the value as
     * truncated instead of pretending it is complete. */
    proven_result_size_t part = proven_u8str_append_partial(&field, PROVEN_LIT(" ...more text than there is room for"));
    EXAMPLE_REQUIRE(part.err == PROVEN_ERR_OUT_OF_BOUNDS, "a partial append that truncates still reports the truncation");
    EXAMPLE_REQUIRE(part.value > 0, "but it wrote what it could");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before + part.value,
                    "and the string grew by exactly the number of bytes it reports");
    printf("partial append wrote %zu byte(s) before the buffer was full\n", (size_t)part.value);

    /* --- editing in the middle without growing ----------------------------- */

    proven_result_u8str_t r2 = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the second buffer must succeed");
    proven_u8str_t path = r2.value;
    err = proven_u8str_append(&path, PROVEN_LIT("var/log/service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the path fits");

    /* insert shifts the tail right. Fixed-capacity: it fits or it refuses. */
    err = proven_u8str_insert(&path, 0, PROVEN_LIT("/"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a leading slash must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/service.log")),
                    "the insert lands at index 0");

    /* replace_at replaces old_len bytes at an index with data of any length, as
     * long as the result still fits. Replacing "service" (7) with "daemon" (6)
     * shrinks the string, so this cannot fail on capacity. */
    proven_size_t at = proven_u8str_view_find(proven_u8str_as_view(&path), 0, PROVEN_LIT("service"));
    EXAMPLE_REQUIRE(at != PROVEN_SIZE_MAX, "the substring must be found before it can be replaced");
    err = proven_u8str_replace_at(&path, at, 7, PROVEN_LIT("daemon"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "a shortening replacement must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/daemon.log")),
                    "and produce the expected path");

    /* The same edit the other way round overflows a 32-byte buffer, and the
     * fixed-capacity call refuses it rather than truncating a path - which is
     * the failure that silently writes to the wrong file. */
    before = proven_u8str_as_view(&path).size;
    err = proven_u8str_replace_at(&path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "a replacement that does not fit must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&path).size == before, "and must leave the path unchanged");

    /* replace_at_grow is the same edit with permission to reallocate. Use it
     * when the buffer is heap-backed and the length is genuinely unbounded;
     * prefer the fixed-capacity call when the limit is part of the format. */
    err = proven_u8str_replace_at_grow(alloc, &path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the growing variant makes room instead of refusing");
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".log")),
                    "the extension is still at the end after the edit");

    /* ends_with answers the question an extension check actually asks. Doing it
     * with an index computed by hand is where the off-by-one lives; doing it
     * with strcmp on a pointer requires a NUL that a view does not have. */
    EXAMPLE_REQUIRE(!proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".txt")),
                    "and it is not a .txt file");

    /* An empty suffix is a suffix of everything, which is the answer that keeps
     * loops over a list of suffixes from needing a special case. */
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT("")),
                    "every string ends with the empty suffix");

    printf("log line: %s\n", proven_u8str_as_cstr(&field));
    printf("path:     %s\n", proven_u8str_as_cstr(&path));

    proven_u8str_destroy(alloc, &path);
    proven_u8str_destroy(alloc, &field);
    return EXAMPLE_OK();
}
