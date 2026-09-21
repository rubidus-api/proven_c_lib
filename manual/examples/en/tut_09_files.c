#include "example.h"

/*
 * Lesson 9 - the outside world fails too, and says so the same way.
 *
 * Everything so far could only fail for reasons inside the program: not enough
 * room, a bad argument. A file can be missing, unreadable, or on a full disk,
 * and none of that is a bug in your code. The library reports those failures
 * with the same error values as before, so the checking you already learned
 * is all the checking there is.
 *
 * The calls here are the whole-file ones: one call writes a file, one call
 * reads it back. No open, no read loop, no close to forget.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A name in the current directory; the program removes it before it ends. */
    proven_u8str_view_t path = PROVEN_LIT("proven_tutorial_notes.tmp");
    proven_u8str_view_t text = PROVEN_LIT("buy milk\ncall home\n");

    /* Atomic: a reader sees the old file or the new one, never half of it.
     * The allocator is scratch space the call may need for the path. */
    proven_err_t err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing a small file in the current directory should work");
    if (!proven_is_ok(err)) return 1;

    /* Reading gives back an owned string - lesson 4's shape, lesson 5's rule:
     * it was made with `alloc`, so it is destroyed with `alloc`. */
    proven_result_u8str_t back = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(back.err), "the file we just wrote can be read");
    if (proven_is_ok(back.err)) {
        EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&back.value), text),
                        "the bytes come back exactly as written");
        proven_println("read {} bytes back", PROVEN_ARG(proven_u8str_as_view(&back.value).size));
        proven_u8str_destroy(alloc, &back.value);
    }

    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the file we created should work");

    /* Now it is gone, and reading it is a failure from outside the program.
     * It arrives as an ordinary value, and it says which failure it is. */
    proven_result_u8str_t missing = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(missing.err == PROVEN_ERR_NOT_FOUND, "a removed file reads as NOT_FOUND");
    if (proven_is_ok(missing.err)) proven_u8str_destroy(alloc, &missing.value);
    proven_println("reading it again: not found, as expected");

    return EXAMPLE_OK();
}
