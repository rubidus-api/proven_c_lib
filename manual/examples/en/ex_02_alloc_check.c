#include "example.h"
#include <string.h>

/*
 * Catching the wrong allocator where the mistake is made.
 *
 * Nothing in this library remembers which allocator a block came from: the caller
 * passes one to create and one to destroy, and if they differ the allocator's
 * bookkeeping is corrupted and the program breaks later, somewhere else. The
 * alloc_check wrapper goes in front of any allocator, keeps a record of the
 * blocks it handed out, and refuses - with a panic, at the call - anything else.
 *
 * It is for tests and bug hunts. In your own code, write proven_alloc_checked()
 * and switch it with -DPROVEN_ALLOC_CHECK: without the macro it returns the
 * allocator it was given and costs nothing. (A #define after the first proven
 * #include is too late - the switch is read with the header.) This example
 * calls proven_alloc_check_wrap(), which checks unconditionally, so it shows
 * the checks whatever the build flags.
 */

/* A panic handler that returns, so this example can show a refusal and go on. A
 * real program keeps the default, which stops at the mistake. */
static int g_refused;
static void note_panic(const char *msg) {
    ++g_refused;
    printf("refused: %s\n", msg);
}

int main(void) {
    proven_set_panic_handler(note_panic);

    /* Two allocators, each behind its own checker with room to record 16 blocks. */
    static proven_byte_t arena_mem[2048];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ arena_mem, sizeof arena_mem });
    proven_alloc_check_t arena_chk, heap_chk;
    proven_alloc_check_entry_t arena_rec[16], heap_rec[16];
    proven_allocator_t scratch = proven_alloc_check_wrap(&arena_chk, proven_arena_as_allocator(&arena), arena_rec, 16);
    proven_allocator_t heap = proven_alloc_check_wrap(&heap_chk, proven_heap_allocator(), heap_rec, 16);

    /* --- correct use passes straight through --------------------------------- */

    proven_result_u8str_t name = proven_u8str_create_from_view(heap, PROVEN_LIT("report"));
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "a string from the checked heap");
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_append_grow(heap, &name.value, PROVEN_LIT(".txt"))), "grown by the same allocator");
    EXAMPLE_REQUIRE(proven_alloc_check_owns(&heap_chk, name.value.internal.ptr), "the heap checker owns its block");

    /* --- the mistake, caught at the call ------------------------------------ */

    /* A temporary built in the scratch arena... */
    proven_result_u8str_t tmp = proven_u8str_create(scratch, 64);
    EXAMPLE_REQUIRE(proven_is_ok(tmp.err), "a temporary in the arena");
    proven_u8str_t t = tmp.value;
    /* ...and destroyed through the heap. Unchecked, the heap would free arena memory. */
    proven_u8str_destroy(heap, &t);
    EXAMPLE_REQUIRE(g_refused == 1 && heap_chk.faults == 1, "the heap refuses a block it never gave out");
    EXAMPLE_REQUIRE(proven_alloc_check_live(&arena_chk) == 1, "and the arena block is untouched");
    t = tmp.value;
    proven_u8str_destroy(scratch, &t);   /* the right one */

    /* --- a leak check at the end of a test ---------------------------------- */

    proven_u8str_destroy(heap, &name.value);
    EXAMPLE_REQUIRE(proven_alloc_check_live(&heap_chk) == 0 && proven_alloc_check_live(&arena_chk) == 0,
                    "nothing is left live");
    printf("peak %zu live block(s) on the heap, %zu refusal(s)\n", (size_t)heap_chk.peak_live, (size_t)heap_chk.faults);

    proven_set_panic_handler(NULL);
    return EXAMPLE_OK();
}
