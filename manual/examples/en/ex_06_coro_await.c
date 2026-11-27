#include "example.h"

/*
 * A coroutine that waits for another: the parent puts the child's step function
 * in PROVEN_CORO_AWAIT's condition. AWAIT evaluates the condition on every visit,
 * the first one included, so each call of the parent steps the child once, and
 * the parent goes on in the same call that completes the child.
 *
 * The child here reads a two-byte big-endian length as the bytes arrive. It takes
 * the first byte as soon as it is there and keeps it in its struct - a local would
 * not survive the suspension - then waits for the second.
 *
 * Two rules:
 *   - One driver. Only the parent steps the child. A caller that also stepped it
 *     would advance it twice for one input.
 *   - Done is sticky. After END the state is -1, which matches no case: a later
 *     call skips the body and returns 1 again.
 *
 * And one trap: AWAIT(PROVEN_CORO_IS_DONE(&child)) would never step the child,
 * since the caller steps only the parent. The child's return value is the signal,
 * and it says "done" even for a length of zero.
 */

typedef struct {
    proven_coro_t coro;
    const unsigned char *in;    /* the input so far, written by the caller */
    int available;              /* how many bytes of `in` there are */
    int taken;                  /* how many the reader has used */
    int high;                   /* the first byte, kept across the suspension */
    int length;                 /* the result */
    int steps;                  /* how many times it was stepped (for the checks) */
} length_reader_t;

static proven_i32 reader_step(length_reader_t *r) {
    r->steps += 1;
    PROVEN_CORO_BEGIN(&r->coro);
    PROVEN_CORO_AWAIT(&r->coro, r->available >= 1);
    r->high = r->in[0];
    r->taken = 1;
    PROVEN_CORO_AWAIT(&r->coro, r->available >= 2);
    r->length = (r->high << 8) | r->in[1];
    r->taken = 2;
    PROVEN_CORO_END(&r->coro);
}

typedef struct {
    proven_coro_t coro;
    length_reader_t reader;
    int length;                 /* published by the parent; -1 until then */
} request_t;

static proven_i32 request_step(request_t *q) {
    PROVEN_CORO_BEGIN(&q->coro);
    PROVEN_CORO_AWAIT(&q->coro, reader_step(&q->reader));
    q->length = q->reader.length;
    PROVEN_CORO_END(&q->coro);
}

static void request_init(request_t *q, const unsigned char *in, int available) {
    *q = (request_t){ .length = -1 };
    q->reader.in = in;
    q->reader.available = available;
    PROVEN_CORO_INIT(&q->coro);         /* both, once, before the first call */
    PROVEN_CORO_INIT(&q->reader.coro);
}

int main(void) {
    static const unsigned char bytes[] = { 0x01, 0x02 };
    request_t q;
    request_init(&q, bytes, 0);

    /* Each block is one call of the parent; the caller only adds input. */
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 0,
                    "no bytes: the parent waits, the reader has taken nothing");
    q.reader.available = 1;
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 1,
                    "one byte: the reader took it and waits for the second");
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 1,
                    "a call with nothing new changes nothing");
    q.reader.available = 2;
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258 && q.reader.taken == 2,
                    "both bytes: the child completes and the parent publishes 0x0102 in the same call");
    EXAMPLE_REQUIRE(q.reader.steps == 4, "the reader was stepped exactly once per call of the parent");
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258 && q.reader.steps == 4,
                    "done is sticky: a finished parent does not step the reader again");

    /* All the input there before the first call: one call does it all. */
    static const unsigned char zero[] = { 0x00, 0x00 };
    request_init(&q, zero, 2);
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 0,
                    "a length of zero is a result, not a pending child");
    request_init(&q, bytes, 2);
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258, "both bytes at once: 258 on the first call");

    printf("length: %d\n", q.length);
    return EXAMPLE_OK();
}
