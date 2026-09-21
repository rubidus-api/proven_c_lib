#include "example.h"

/*
 * Lesson 8 - a container remembers its allocator.
 *
 * A string is created with an allocator and destroyed with the same one; you
 * pass it both times. A growable array has to allocate again later, whenever a
 * push outgrows its storage, so it keeps the allocator it was created with.
 * That is the one new thing: you hand the allocator over once, at creation,
 * and after that push, get and destroy take nothing but the array.
 *
 * The PROVEN_ARRAY_* macros take the element type as an argument, so they can
 * check it: pushing a double into an array of int does not compile.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 2 is a starting capacity, not a limit. */
    proven_result_array_t made = PROVEN_ARRAY_INIT(alloc, int, 2);
    EXAMPLE_REQUIRE(proven_is_ok(made.err), "creating an empty array must succeed");
    if (!proven_is_ok(made.err)) return 1;
    proven_array_t squares = made.value;

    /* Ten pushes into room for two. Each push that does not fit grows the
     * storage through the allocator the array remembered - no allocator here. */
    for (int i = 1; i <= 10; ++i) {
        proven_err_t err = PROVEN_ARRAY_PUSH(&squares, int, i * i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the heap can grow a ten-int array");
        if (!proven_is_ok(err)) {
            PROVEN_ARRAY_DESTROY(&squares);
            return 1;
        }
    }
    EXAMPLE_REQUIRE(squares.len == 10, "ten pushes, ten elements");

    /* get returns a pointer to the element, or NULL when the index is past the
     * end. An index out of range is an answer you can test, not a wild read. */
    const int *third = PROVEN_ARRAY_GET(&squares, int, 2);
    EXAMPLE_REQUIRE(third && *third == 9, "element 2 is 3 * 3");
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&squares, int, 10) == NULL, "index 10 is past the end");

    int sum = 0;
    for (proven_size_t i = 0; i < squares.len; ++i) {
        sum += *PROVEN_ARRAY_GET(&squares, int, i);
    }
    EXAMPLE_REQUIRE(sum == 385, "1 + 4 + 9 + ... + 100");
    proven_println("{} squares, sum {}", PROVEN_ARG(squares.len), PROVEN_ARG(sum));

    /* Destroy takes only the array: it frees through the allocator it kept. */
    PROVEN_ARRAY_DESTROY(&squares);
    return EXAMPLE_OK();
}
