#include "example.h"

/*
 * A ring buffer is a fixed-size queue that never moves its contents and never
 * grows. You give it a capacity once; push adds at the tail, pop removes from
 * the head, and when it is full, push REFUSES.
 *
 * The C you would otherwise write is an array plus two indices plus the modulo
 * arithmetic to wrap them, and the bug is always the same: "is it full or is it
 * empty?" - both states have head == tail unless you keep a count or waste a
 * slot. This ring keeps the count, so the question does not arise.
 *
 * Use one when a producer and a consumer run at different speeds and you want a
 * hard bound on how far ahead the producer may get: an event queue, a log ring,
 * an audio buffer. The refusal on a full ring is the feature - it is
 * backpressure. A growable queue would answer a burst by eating memory until
 * something worse happens.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* Capacity of 4 events. It will never be 5. */
    proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 4-slot ring should succeed");
    proven_ring_t ring = r.value;

    /* --- push until full ---------------------------------------------- */
    for (int i = 1; i <= 4; ++i) {
        proven_err_t err = PROVEN_RING_PUSH(&ring, int, i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the first four pushes fit");
    }

    /* The fifth push is refused. It does not overwrite the oldest entry, and it
     * does not grow: a full ring is a full ring, and the caller decides what to
     * do about it - wait, drop the new event, or report backpressure. */
    proven_err_t full = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(full == PROVEN_ERR_OUT_OF_BOUNDS,
                    "pushing into a full ring must be refused, not silently absorbed");

    /* --- pop in the order they were pushed ----------------------------- */
    int out = 0;
    proven_err_t err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(proven_is_ok(err) && out == 1, "pop returns the oldest entry first");

    /* Now there is room again, and the ring wraps around its storage without
     * moving anything. */
    err = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(proven_is_ok(err), "after a pop there is room for one more");

    int expected[] = { 2, 3, 4, 5 };
    for (int i = 0; i < 4; ++i) {
        err = PROVEN_RING_POP(&ring, int, &out);
        EXAMPLE_REQUIRE(proven_is_ok(err) && out == expected[i],
                        "entries come out in the order they went in");
    }

    /* --- empty behaves like full: it refuses, it does not invent data --- */
    err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(!proven_is_ok(err), "popping an empty ring must fail rather than return junk");

    PROVEN_RING_DESTROY(&ring);
    return EXAMPLE_OK();
}
