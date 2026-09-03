#include "example.h"
#include <string.h>

/*
 * One owned buffer, three ways of talking about it.
 *
 * The program holds a small table of fixed-size records in a single block of
 * memory it owns, and then does the four things every such program has to do:
 *
 *   1. hand the table to a reader that must not modify it (a read-only view),
 *   2. hand one row to a writer that may modify only that row (a slice),
 *   3. delete a row by moving the rows after it down over it (an overlapping
 *      copy, which plain copying is not allowed to do),
 *   4. take a pointer somebody else returned and decide whether it even points
 *      into the table before using it as a row index.
 *
 * Every step is a place where a bare `char *` loses the length and the program
 * finds out later. The types here carry the length with the pointer, so the
 * bounds question is answered where the mistake would be made.
 */

#define ROW_SIZE  8u
#define ROW_COUNT 4u

/* A reader gets a view: it can read every byte and write none of them. The
 * const in the type is not advice - assigning through it does not compile. */
static proven_size_t count_nonzero(proven_mem_view_t table) {
    proven_size_t n = 0;
    for (proven_size_t i = 0; i < table.size; ++i) {
        if (table.ptr[i] != 0) {
            ++n;
        }
    }
    return n;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* Allocate the table. The allocator returns a read-write slice, which is
     * what an owner holds: a pointer and the size that goes with it. */
    proven_result_mem_mut_t got = alloc.alloc_fn(alloc.ctx, ROW_SIZE * ROW_COUNT, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(got.err), "allocating the record table must succeed");
    if (!proven_is_ok(got.err)) {
        return 1;
    }

    /* proven_mem_t is the "I own this" type. Keeping the owned block in one
     * variable, and handing out views and slices derived from it, is the whole
     * ownership discipline this library asks for. */
    proven_mem_t owned = { .ptr = got.value.ptr, .size = got.value.size };
    proven_mem_mut_t table = proven_mem_mut_from_owned(owned);

    /* Fill row i with the byte value i + 1, so a moved row is recognisable. */
    for (proven_u32 row = 0; row < ROW_COUNT; ++row) {
        proven_result_mem_mut_t slot = proven_mem_mut_slice_checked(table, row * ROW_SIZE, ROW_SIZE);
        EXAMPLE_REQUIRE(proven_is_ok(slot.err), "every row of the table must be in bounds");
        if (!proven_is_ok(slot.err)) {
            alloc.free_fn(alloc.ctx, owned.ptr);
            return 1;
        }
        memset(slot.value.ptr, (int)(row + 1), slot.value.size);
    }

    /* 1. Read-only use. The reader cannot modify the table and cannot run off
     *    the end of it, because it was handed both facts at once. */
    proven_mem_view_t read_only = proven_mem_view_from_owned(owned);
    EXAMPLE_REQUIRE(count_nonzero(read_only) == ROW_SIZE * ROW_COUNT,
                    "every byte of every row was written");

    /* 2. Asking for a row that does not exist is an error you can handle, not a
     *    crash you have to debug. Row 4 of a 4-row table starts one row past
     *    the end. */
    proven_result_mem_mut_t past_end = proven_mem_mut_slice_checked(table, ROW_COUNT * ROW_SIZE, ROW_SIZE);
    EXAMPLE_REQUIRE(past_end.err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "slicing past the end must report OUT_OF_BOUNDS, not return a bad slice");

    /* 3. Delete row 1 by moving rows 2 and 3 down one row. Source and
     *    destination overlap, so this is the one case plain copying is not
     *    allowed to handle: proven_mem_copy documents non-overlapping regions,
     *    proven_mem_move is the one that accepts them. */
    proven_mem_view_t tail = {
        .ptr  = table.ptr + 2 * ROW_SIZE,
        .size = 2 * ROW_SIZE
    };
    proven_err_t moved = proven_mem_move(table.ptr + 1 * ROW_SIZE, table.size - 1 * ROW_SIZE, tail);
    EXAMPLE_REQUIRE(proven_is_ok(moved), "moving the tail of the table down must succeed");
    EXAMPLE_REQUIRE(table.ptr[1 * ROW_SIZE] == 3, "row 2 moved into row 1's place");
    EXAMPLE_REQUIRE(table.ptr[2 * ROW_SIZE] == 4, "row 3 moved into row 2's place");

    /* The move is bounded like everything else: a destination too small to hold
     * the source is refused, and nothing is written. */
    proven_err_t refused = proven_mem_move(table.ptr, ROW_SIZE, tail);
    EXAMPLE_REQUIRE(refused == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a move that would not fit must be refused before it writes");

    /* 4. A pointer from elsewhere. Comparing unrelated pointers with < is
     *    undefined behaviour in C - the compiler is allowed to assume it never
     *    happens - so the question "does this pointer point into my buffer?"
     *    has to be asked with a function that does the comparison correctly. */
    proven_size_t offset = 0;
    const proven_byte_t *from_elsewhere = table.ptr + 2 * ROW_SIZE;
    bool inside = proven_range_contains_ptr(table.ptr, table.size, from_elsewhere, ROW_SIZE, &offset);
    EXAMPLE_REQUIRE(inside, "a pointer to row 2 is inside the table");
    EXAMPLE_REQUIRE(offset / ROW_SIZE == 2, "and its offset recovers the row index");

    proven_u8 unrelated[ROW_SIZE] = { 0 };
    EXAMPLE_REQUIRE(!proven_range_contains_ptr(table.ptr, table.size, unrelated, ROW_SIZE, NULL),
                    "a pointer into a different object is not inside the table");

    /* A row that starts inside but ends outside is also outside: the check is
     * about the whole range, not just where it begins. */
    EXAMPLE_REQUIRE(!proven_range_contains_ptr(table.ptr, table.size,
                                               table.ptr + table.size - 1, ROW_SIZE, NULL),
                    "a range that starts inside but runs past the end is refused");

    /* 5. Once the range check has proved the row is in bounds, the unchecked
     *    slice is the right call: it does no work, and the proof is the line
     *    above it rather than a comment. Never write it without that proof. */
    proven_mem_mut_t row2 = proven_mem_mut_slice_unchecked(table, offset, ROW_SIZE);
    EXAMPLE_REQUIRE(row2.size == ROW_SIZE && row2.ptr[0] == 4,
                    "the unchecked slice names the row the checked test just proved");

    /* 6. Alignment, in the one place a caller meets it: laying two differently
     *    aligned things out inside one block. The address of the second one has
     *    to be rounded up, and rounding up is only defined for a power-of-two
     *    boundary - so the boundary is checked first. */
    proven_size_t want_align = alignof(proven_u32);
    EXAMPLE_REQUIRE(proven_is_pow2(want_align), "an alignment boundary must be a power of two");
    EXAMPLE_REQUIRE(!proven_is_pow2(24u), "24 is not a power of two, so it is not a valid boundary");

    proven_uintptr_t raw     = (proven_uintptr_t)(table.ptr + 1);   /* deliberately odd */
    proven_uintptr_t aligned = proven_uintptr_align_up(raw, want_align);
    EXAMPLE_REQUIRE(aligned >= raw, "aligning up never moves backwards");
    EXAMPLE_REQUIRE(aligned % want_align == 0, "and the result is on the boundary asked for");
    EXAMPLE_REQUIRE(aligned - raw < want_align, "the padding is never more than one boundary");

    /* A boundary that is not a power of two returns 0 rather than a plausible
     * wrong address, so the mistake stops here instead of downstream. */
    EXAMPLE_REQUIRE(proven_uintptr_align_up(raw, 24u) == 0,
                    "aligning to a non-power-of-two returns 0, not a wrong address");

    alloc.free_fn(alloc.ctx, owned.ptr);
    return EXAMPLE_OK();
}
