#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * A regression from the read_line-exact-fit fix, found by auditing that fix.
 *
 * The fix gave the buffered reader one byte of lookahead: when a line overruns the buffer,
 * read_line peeks at one more byte to decide whether the line is really too long, and if it
 * is, it STASHES that byte (s->peek / s->has_peek) because the byte belongs to the stream.
 *
 * reader_buffered_fill correctly re-inserts the stashed byte before reading more. But then it
 * returned `r.value > 0` - true only if the SOURCE handed over new bytes this call. When the
 * source is at EOF and returns zero bytes, that is false - even though the peek byte it just
 * put back is sitting in the buffer, unread. The caller reads false as "nothing left" and
 * returns EOF with a stream byte still buffered. A read-to-EOF loop never looks again, so that
 * byte is silently lost.
 *
 * The fix: fill() reports whether it made the buffer non-empty, not whether the source
 * specifically contributed - re-inserting a peek byte is progress too.
 */

/* A reader over a fixed view whose read_fn hands out AT MOST `chunk` bytes per call - so the
 * peek/refill boundaries are forced, exactly as a small pipe would force them. */
typedef struct {
    const proven_byte_t *data;
    proven_size_t size;
    proven_size_t pos;
    proven_size_t chunk;
} dribble_t;

static proven_result_size_t dribble_read(void *ctx, proven_mem_mut_t dest) {
    dribble_t *d = (dribble_t *)ctx;
    if (d->pos >= d->size) return (proven_result_size_t){ .err = PROVEN_ERR_EOF, .value = 0 };
    proven_size_t avail = d->size - d->pos;
    proven_size_t n = avail < dest.size ? avail : dest.size;
    if (n > d->chunk) n = d->chunk;
    memcpy(dest.ptr, d->data + d->pos, n);
    d->pos += n;
    return (proven_result_size_t){ .err = PROVEN_OK, .value = n };
}

static proven_reader_t dribble_reader(dribble_t *d) {
    return (proven_reader_t){ .ctx = d, .read_fn = dribble_read };
}

int main(void) {
    PROVEN_TEST_SUITE("a stream byte stranded after a too-long line is not lost",
        "After read_line reports OUT_OF_BOUNDS it stashes one lookahead byte. A later raw read must be able to reach that byte; it must not come back EOF while a buffered byte is unread.",
        "Inspect reader_buffered_fill in src/proven/stream.c: it must report whether it made the buffer non-empty (re-inserting a peek byte is progress), not merely whether the source handed over new bytes this call.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the minimal trigger: read_line OOB, then read to the end",
        "\"aaa\" through a 2-byte buffer. The line is too long; the third 'a' is peeked and stashed; the raw reads that follow must yield all three bytes and only then EOF.",
        "");
    // ---------------------------------------------------------------
    {
        const proven_byte_t data[] = { 'a', 'a', 'a' };
        dribble_t d = { .data = data, .size = 3, .pos = 0, .chunk = 64 };

        proven_byte_t buf[2];
        proven_reader_buffered_t st;
        proven_reader_t r = proven_reader_buffered(&st, dribble_reader(&d),
            (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        PROVEN_TEST_ASSERT(proven_reader_is_valid(r), "setup", "");

        /* The line "aaa" does not fit a 2-byte buffer: OUT_OF_BOUNDS, and the 3rd byte is stashed. */
        proven_result_u8str_view_t ln = proven_reader_read_line(&st);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a 3-byte line in a 2-byte buffer is OUT_OF_BOUNDS", "");

        /* Now read the raw bytes. All three 'a's must come out before EOF - none may be lost to
         * the stash. */
        proven_byte_t out[8];
        proven_size_t total = 0;
        for (int guard = 0; guard < 16; ++guard) {
            proven_result_size_t rr = proven_reader_read(r, (proven_mem_mut_t){ .ptr = out + total, .size = sizeof out - total });
            if (rr.err == PROVEN_ERR_EOF) break;
            PROVEN_TEST_ASSERT(proven_is_ok(rr.err), "raw read must not fail", "");
            total += rr.value;
        }

        PROVEN_TEST_ASSERT(total == 3,
            "all three bytes of the stream must be read back, none stranded in the stash",
            "total == 2 means the peeked third byte was lost: fill() reported EOF while it held an unread byte.");
        PROVEN_TEST_ASSERT(out[0] == 'a' && out[1] == 'a' && out[2] == 'a',
            "and they must be the right bytes", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the stranded byte is not lost even when it is the whole remainder",
        "A longer over-long line, then the raw tail: every byte past the buffer must still be reachable.",
        "");
    // ---------------------------------------------------------------
    {
        /* "abcdef" with no newline, 3-byte buffer: the line overruns, 'd' is peeked/stashed,
         * and d,e,f must all be readable afterwards. */
        const proven_byte_t data[] = { 'a', 'b', 'c', 'd', 'e', 'f' };
        dribble_t d = { .data = data, .size = 6, .pos = 0, .chunk = 1 };  /* one byte per read, like a slow pipe */

        proven_byte_t buf[3];
        proven_reader_buffered_t st;
        proven_reader_t r = proven_reader_buffered(&st, dribble_reader(&d),
            (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });

        proven_result_u8str_view_t ln = proven_reader_read_line(&st);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_OUT_OF_BOUNDS, "the line overruns the buffer", "");

        proven_byte_t out[16];
        proven_size_t total = 0;
        for (int guard = 0; guard < 32; ++guard) {
            proven_result_size_t rr = proven_reader_read(r, (proven_mem_mut_t){ .ptr = out + total, .size = sizeof out - total });
            if (rr.err == PROVEN_ERR_EOF) break;
            PROVEN_TEST_ASSERT(proven_is_ok(rr.err), "raw read must not fail", "");
            total += rr.value;
        }
        PROVEN_TEST_ASSERT(total == 6 && memcmp(out, data, 6) == 0,
            "every byte of \"abcdef\" must survive the read_line overrun, in order", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the ordinary read-after-read_line case still works",
        "The fix must not break the case where the peeked byte is followed by more from the source.",
        "");
    // ---------------------------------------------------------------
    {
        const proven_byte_t data[] = { 'x', 'x', 'y', 'y', 'z', 'z', 'z', 'z' };
        dribble_t d = { .data = data, .size = 8, .pos = 0, .chunk = 64 };

        proven_byte_t buf[2];
        proven_reader_buffered_t st;
        proven_reader_t r = proven_reader_buffered(&st, dribble_reader(&d),
            (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });

        proven_result_u8str_view_t ln = proven_reader_read_line(&st);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_OUT_OF_BOUNDS, "overruns 2 bytes", "");

        proven_byte_t out[16];
        proven_size_t total = 0;
        for (int guard = 0; guard < 32; ++guard) {
            proven_result_size_t rr = proven_reader_read(r, (proven_mem_mut_t){ .ptr = out + total, .size = sizeof out - total });
            if (rr.err == PROVEN_ERR_EOF) break;
            if (!proven_is_ok(rr.err)) break;
            total += rr.value;
        }
        PROVEN_TEST_ASSERT(total == 8 && memcmp(out, data, 8) == 0,
            "all eight bytes must be read back in order", "");
    }

    PROVEN_TEST_PASS("a byte peeked past a too-long line is read back, never stranded by a false EOF.");
    return 0;
}
