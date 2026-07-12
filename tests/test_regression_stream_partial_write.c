#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Three defects an adversarial audit reproduced in the streams code the day it was
 * written. All three share one shape: the code was correct for the sink it was tested
 * against, and silently wrong for a sink that behaves the way real sinks behave.
 *
 *   1. A writer that accepts only part of a chunk. The trait said "write_fn must
 *      consume the whole chunk or fail" - which a pipe, a socket, or a filling disk
 *      cannot promise. When such a sink took 4096 of 6000 bytes and then failed, the
 *      buffered writer kept the WHOLE buffer and re-sent it on the next flush, so the
 *      receiver got the first 4096 bytes twice: 10096 bytes for a 6000-byte payload.
 *
 *   2. A reader whose source fails mid-file. The buffered reader stored no error, so
 *      a short read set eof and the caller saw a clean end-of-file. A file truncated
 *      by a disk error was indistinguishable from a complete one.
 *
 *   3. `{:f}` on a large value. fmt.h promises `{:f}` forces the fixed form; the float
 *      engine switched to scientific above 1e18 regardless, so `{:f}` on 1e20 gave
 *      "1.000000e+20" and `{}` and `{:f}` were byte-identical for every input.
 */

/* ---- a sink that accepts N bytes at most, then fails: i.e. every real pipe ---- */
typedef struct {
    proven_byte_t got[32768];
    proven_size_t got_len;
    proven_size_t accept_per_call; /* 0 = unlimited */
    int fail_after_calls;          /* -1 = never */
    int stall_after;               /* 0 = never: after N calls, take nothing and report OK */
    int calls;
} throttled_sink_t;

static proven_result_size_t throttled_write(void *ctx, proven_mem_view_t chunk) {
    throttled_sink_t *s = ctx;
    proven_result_size_t r = {0};
    proven_size_t take = chunk.size;

    if (s->stall_after && s->calls >= s->stall_after) {
        /* The stall: no bytes, no error. Every sink that has ever wedged looks like this. */
        s->calls++;
        r.err = PROVEN_OK;
        r.value = 0;
        return r;
    }

    if (s->accept_per_call && take > s->accept_per_call) take = s->accept_per_call;

    if (s->fail_after_calls >= 0 && s->calls >= s->fail_after_calls) {
        /* The honest partial failure: some bytes landed, then the sink died. */
        if (take > 8) take = 8;
        memcpy(s->got + s->got_len, chunk.ptr, take);
        s->got_len += take;
        s->calls++;
        r.err = PROVEN_ERR_IO;
        r.value = take; /* and it says how many */
        return r;
    }

    memcpy(s->got + s->got_len, chunk.ptr, take);
    s->got_len += take;
    s->calls++;
    r.err = PROVEN_OK;
    r.value = take;
    return r;
}

static proven_err_t throttled_flush(void *ctx) { (void)ctx; return PROVEN_OK; }

/* Format one argument into a borrowed buffer; the returned pointer is buf. */
static const char *fmt_one(proven_byte_t *buf, proven_size_t cap, const char *f, proven_arg_t a) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), a };
    proven_fmt_result_t r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f,
                                                      (proven_allocator_t){0}, args, 2);
    if (!proven_is_ok(r.err)) return "<format failed>";
    return proven_u8str_as_cstr(&s);
}

int main(void) {
    PROVEN_TEST_SUITE("streams and {:f}: three defects found by adversarial audit",
        "A partial write must not duplicate bytes; a failed read must not look like EOF; {:f} must force the fixed form.",
        "Inspect writer_buffered_flush, reader_buffered_fill (src/proven/stream.c), and the never_scientific flag (src/proven/float_format.c).");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a sink that takes part of a chunk gets each byte exactly once",
        "The buffered writer used to keep the whole buffer after a partial write and re-send it.",
        "writer_buffered_flush must advance past the bytes the sink acknowledged and keep only the unsent tail.");
    // ---------------------------------------------------------------
    {
        static throttled_sink_t sink;
        memset(&sink, 0, sizeof sink);
        sink.accept_per_call = 700;   /* never takes a whole 4096-byte flush */
        sink.fail_after_calls = -1;

        proven_writer_t raw = {
            .ctx = &sink, .write_fn = throttled_write, .flush_fn = throttled_flush,
        };
        static proven_byte_t bufmem[4096];
        proven_writer_buffered_t bw;
        proven_writer_t w = proven_writer_buffered(&bw, raw,
            (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

        static proven_byte_t payload[6000];
        for (proven_size_t i = 0; i < sizeof payload; ++i) payload[i] = (proven_byte_t)(i & 0xff);

        proven_err_t err = proven_writer_write(w, (proven_mem_view_t){ .ptr = payload, .size = sizeof payload });
        PROVEN_TEST_ASSERT(proven_is_ok(err), "writing 6000 bytes through a slow sink must succeed", "");
        err = proven_writer_flush(w);
        PROVEN_TEST_ASSERT(proven_is_ok(err), "the flush must succeed", "");

        PROVEN_TEST_ASSERT(sink.got_len == sizeof payload,
            "the sink must receive exactly 6000 bytes",
            "It used to receive 10096: the buffer was re-sent from the start after every partial write.");
        PROVEN_TEST_ASSERT(memcmp(sink.got, payload, sizeof payload) == 0,
            "and they must be the payload, in order",
            "Duplicated bytes would corrupt the stream even if the count happened to match.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a sink that dies mid-chunk reports the failure and loses nothing silently",
        "The caller must be told, and the bytes the sink took must not be sent twice by a retry.",
        "proven_writer_write_partial exists so a caller can see how far it got.");
    // ---------------------------------------------------------------
    {
        static throttled_sink_t sink;
        memset(&sink, 0, sizeof sink);
        sink.accept_per_call = 0;
        sink.fail_after_calls = 0;   /* fail on the very first write, after taking 8 bytes */

        proven_writer_t raw = {
            .ctx = &sink, .write_fn = throttled_write, .flush_fn = throttled_flush,
        };
        proven_byte_t payload[64];
        memset(payload, 'A', sizeof payload);

        proven_result_size_t pr = proven_writer_write_partial(raw,
            (proven_mem_view_t){ .ptr = payload, .size = sizeof payload });
        PROVEN_TEST_ASSERT(pr.err == PROVEN_ERR_IO,
            "a failing sink must surface its error",
            "");
        PROVEN_TEST_ASSERT(pr.value == 8,
            "and must report the 8 bytes it did accept",
            "A caller that cannot see the partial count has no safe way to resume: it either drops those bytes or duplicates them.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a read that fails is an error, not an end of file",
        "A file truncated by a disk error used to be indistinguishable from a complete file.",
        "The buffered reader state carries an err field; reader_buffered_fill stores it instead of setting eof.");
    // ---------------------------------------------------------------
    {
        /* A reader whose source yields a few bytes and then fails. */
        struct failing_src { int calls; } src = { 0 };

        extern proven_result_size_t proven_test_failing_read(void *ctx, proven_mem_mut_t out);
        proven_reader_t raw = { .ctx = &src, .read_fn = proven_test_failing_read };

        static proven_byte_t bufmem[64];
        proven_reader_buffered_t br;
        proven_reader_t r = proven_reader_buffered(&br, raw,
            (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

        proven_byte_t out[16];
        proven_result_size_t rr = proven_reader_read(r, (proven_mem_mut_t){ .ptr = out, .size = sizeof out });
        PROVEN_TEST_ASSERT(proven_is_ok(rr.err) && rr.value == 4,
            "the first read must deliver the 4 bytes the source produced", "");

        rr = proven_reader_read(r, (proven_mem_mut_t){ .ptr = out, .size = sizeof out });
        PROVEN_TEST_ASSERT(rr.err == PROVEN_ERR_IO,
            "the second read must report the source's failure",
            "It used to report 0 bytes and PROVEN_OK - a clean end of file. A caller reading a config file would have acted on half of it.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("{:f} forces the fixed form at any magnitude",
        "fmt.h promises it. The engine ignored the policy and switched to scientific above 1e18.",
        "Inspect never_scientific in proven_float_format_options_t.");
    // ---------------------------------------------------------------
    {
        proven_byte_t out[128];

        PROVEN_TEST_ASSERT(strcmp(fmt_one(out, sizeof out, "{:.1f}", PROVEN_ARG(1e20)),
                                  "100000000000000000000.0") == 0,
            "1e20 with {:.1f} must be 100000000000000000000.0, with no exponent",
            "It used to be \"1.000000e+20\": the exact fixed-point engine was there all along, and the dispatcher refused to call it.");

        /* And at the other end: {:f} must not escape into scientific for a tiny value either. */
        PROVEN_TEST_ASSERT(strcmp(fmt_one(out, sizeof out, "{:.8f}", PROVEN_ARG(1e-7)),
                                  "0.00000010") == 0,
            "1e-7 with {:.8f} must be 0.00000010",
            "The scientific escape hatch fired below 1e-4 as well.");

        /* The default form is unchanged: it still picks the shorter spelling. */
        PROVEN_TEST_ASSERT(strchr(fmt_one(out, sizeof out, "{}", PROVEN_ARG(1e20)), 'e') != NULL,
            "and {} on 1e20 must still choose the scientific spelling",
            "Forcing fixed is what {:f} is FOR. It must not change what {} does.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a writer that has failed stays failed",
        "flush() used to answer PROVEN_OK after a write had already failed - the buffer was empty, so there was nothing left to fail on.",
        "The shape \"write, write, write, check the flush\" is how almost everyone uses a buffered writer. On a full disk it reported success on a stream missing every byte.");
    // ---------------------------------------------------------------
    {
        static throttled_sink_t sink;
        memset(&sink, 0, sizeof sink);
        sink.accept_per_call = 0;
        sink.fail_after_calls = 0;   /* fails immediately, like a full disk */

        proven_writer_t raw = {
            .ctx = &sink, .write_fn = throttled_write, .flush_fn = throttled_flush,
        };
        static proven_byte_t bufmem[4096];
        proven_writer_buffered_t bw;
        proven_writer_t w = proven_writer_buffered(&bw, raw,
            (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

        /* Bigger than the buffer, so it goes straight to the sink - and the sink dies. */
        static proven_byte_t payload[10000];
        memset(payload, 'x', sizeof payload);

        proven_err_t err = proven_writer_write(w, (proven_mem_view_t){ .ptr = payload, .size = sizeof payload });
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_IO, "the write must fail", "");

        err = proven_writer_flush(w);
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_IO,
            "and the flush must NOT report success",
            "It used to return PROVEN_OK: nothing was left in the buffer, so nothing failed. A caller that checks only the flush - which is most callers - was told a stream missing 10,000 bytes had been written.");

        /* And it does not quietly recover, either. */
        err = proven_writer_write(w, (proven_mem_view_t){ .ptr = payload, .size = 8 });
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_IO,
            "a later write on a failed writer must also fail",
            "The byte stream has a hole in it. Continuing to write into it produces a file that looks complete and is not.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an EOF that carries bytes does not lose them",
        "A reader may return {PROVEN_ERR_EOF, N} with N nonzero - the library's own read_all does - and the last N bytes of the file are still bytes.",
        "reader_buffered_fill must add r.value to the buffer before latching eof. It used to drop them, so the final line of a file vanished whenever the source reported its end and its last bytes in the same breath.");
    // ---------------------------------------------------------------
    {
        extern proven_result_size_t proven_test_eof_with_bytes(void *ctx, proven_mem_mut_t out);
        int calls = 0;
        proven_reader_t raw = { .ctx = &calls, .read_fn = proven_test_eof_with_bytes };

        static proven_byte_t bufmem[64];
        proven_reader_buffered_t br;
        proven_reader_t r = proven_reader_buffered(&br, raw,
            (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

        proven_u8str_view_t line = {0};
        proven_result_u8str_view_t lr = proven_reader_read_line(&br);
        PROVEN_TEST_ASSERT(proven_is_ok(lr.err), "the first line must be readable", "");
        line = lr.val;
        PROVEN_TEST_ASSERT(line.size == 5 && memcmp(line.ptr, "alpha", 5) == 0,
            "the first line must be \"alpha\"",
            "The reader used to answer EOF here: the source handed over eleven bytes together with its EOF, and they were thrown away.");

        lr = proven_reader_read_line(&br);
        PROVEN_TEST_ASSERT(proven_is_ok(lr.err) && lr.val.size == 4 && memcmp(lr.val.ptr, "beta", 4) == 0,
            "and the second line must be \"beta\"", "");

        (void)r;
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("{:f} works at every magnitude a double can hold",
        "The fixed form of 1e308 is 309 digits. It used to be rendered into a 128-byte scratch and refused - as PROVEN_ERR_INVALID_FORMAT, a *bad format string*.",
        "A caller could not fix that by supplying a bigger output buffer, because the limit was not the output buffer. Inspect the float case in render_arg.");
    // ---------------------------------------------------------------
    {
        proven_allocator_t heap = proven_heap_allocator();

        proven_result_u8str_t s = proven_u8str_create(heap, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(s.err), "setup", "");
        proven_fmt_result_t f = proven_u8str_append_fmt_grow(heap, &s.value, "{:.1f}", PROVEN_ARG(1e121));
        PROVEN_TEST_ASSERT(proven_is_ok(f.err),
            "{:.1f} on 1e121 must format",
            "It used to be PROVEN_ERR_INVALID_FORMAT - indistinguishable from a typo in the spec.");
        PROVEN_TEST_ASSERT(proven_u8str_as_view(&s.value).size == 124,
            "and produce all 124 characters of it", "");
        proven_u8str_destroy(heap, &s.value);

        s = proven_u8str_create(heap, 16);
        f = proven_u8str_append_fmt_grow(heap, &s.value, "{:.60f}", PROVEN_ARG(1e308));
        PROVEN_TEST_ASSERT(proven_is_ok(f.err) && proven_u8str_as_view(&s.value).size == 370,
            "the widest thing the grammar allows - 1e308 with 60 decimals - must render whole",
            "309 integer digits, a point, and 60 decimals.");
        proven_u8str_destroy(heap, &s.value);

        /* And a precision the grammar does NOT allow is an out-of-bounds request, not a
         * malformed format: the spec is well-formed, it is simply asking for too much. */
        s = proven_u8str_create(heap, 16);
        f = proven_u8str_append_fmt_grow(heap, &s.value, "{:.61f}", PROVEN_ARG(1.0));
        PROVEN_TEST_ASSERT(f.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a precision past the grammar's limit must be PROVEN_ERR_OUT_OF_BOUNDS", "");
        proven_u8str_destroy(heap, &s.value);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a fixed buffer that overflowed reports it on flush too",
        "The same disease as the buffered writer: the write that overflowed said so, but a caller who checks only the flush - which is most callers - was told everything was fine.",
        "And a later, shorter chunk must not sneak in behind the hole: a buffer that looks like valid output and is missing a piece in the middle is undetectable downstream.");
    // ---------------------------------------------------------------
    {
        proven_byte_t small[8];
        proven_writer_buf_t st = { .buf = { .ptr = small, .size = sizeof small } };
        proven_writer_t w = proven_writer_from_buffer(&st);

        proven_err_t e = proven_writer_write_str(w, PROVEN_LIT("way too long for eight bytes"));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS, "the oversized write must be refused", "");
        PROVEN_TEST_ASSERT(st.overflowed, "and the writer must record it", "");

        e = proven_writer_flush(w);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS,
            "the flush must NOT report success after data was dropped",
            "It used to have no flush function at all, so proven_writer_flush returned PROVEN_OK.");

        e = proven_writer_write_str(w, PROVEN_LIT("ok"));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS,
            "and a later chunk that WOULD fit must still be refused",
            "Two bytes fit in the remaining eight. Writing them would put them after the hole, and the result would look like complete output.");
        PROVEN_TEST_ASSERT(st.len == 0, "nothing must have been written at all", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a sink that STALLS - takes nothing, reports no error - is a failure too",
        "The buffered writer remembered an inner error but not an inner stall, so it went on accepting writes after bytes had been lost.",
        "The receiver then got 'ABC' + a 27-byte hole + 'XYZ', with the second write reporting success. The flush path already treated {OK, 0} as a failure; the write path did not, and the two disagreed.");
    // ---------------------------------------------------------------
    {
        static throttled_sink_t sink;
        memset(&sink, 0, sizeof sink);
        sink.accept_per_call = 3;     /* takes 3 bytes... */
        sink.stall_after = 1;         /* ...once, then takes nothing and reports OK */
        sink.fail_after_calls = -1;

        proven_writer_t raw = {
            .ctx = &sink, .write_fn = throttled_write, .flush_fn = throttled_flush,
        };
        static proven_byte_t bufmem[16];
        proven_writer_buffered_t bw;
        proven_writer_t w = proven_writer_buffered(&bw, raw,
            (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

        proven_byte_t payload[30];
        memset(payload, 'A', sizeof payload);

        proven_err_t e = proven_writer_write(w, (proven_mem_view_t){ .ptr = payload, .size = sizeof payload });
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO, "a stalled sink must be an I/O failure", "");

        e = proven_writer_write(w, (proven_mem_view_t){ .ptr = payload, .size = 3 });
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO,
            "and a later write must be refused, not appended after the hole",
            "The writer used to think it was healthy: it only remembered an inner ERROR, and a stall is not an error - it is worse, because it looks like nothing happened.");

        e = proven_writer_flush(w);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO, "and the flush must say so", "");
    }

    PROVEN_TEST_PASS("partial writes, failed reads, and {:f} all behave.");
    return 0;
}

/* Hands over eleven bytes and its EOF in the same breath - which is exactly what the
 * library's own proven_sys_io_read_all does. */
proven_result_size_t proven_test_eof_with_bytes(void *ctx, proven_mem_mut_t out) {
    int *calls = (int *)ctx;
    proven_result_size_t r = {0};
    static const char payload[] = "alpha\nbeta\n";
    proven_size_t n = sizeof payload - 1u;

    if ((*calls)++ == 0 && out.size >= n) {
        memcpy(out.ptr, payload, n);
        r.err = PROVEN_ERR_EOF;   /* the end AND the bytes, together */
        r.value = n;
        return r;
    }
    r.err = PROVEN_ERR_EOF;
    r.value = 0;
    return r;
}

/* Yields 4 bytes once, then fails. Defined after main so the test body reads top-down. */
proven_result_size_t proven_test_failing_read(void *ctx, proven_mem_mut_t out) {
    struct failing_src { int calls; } *s = ctx;
    proven_result_size_t r = {0};
    if (s->calls++ == 0) {
        proven_size_t n = out.size < 4u ? out.size : 4u;
        memset(out.ptr, 'x', n);
        r.err = PROVEN_OK;
        r.value = n;
        return r;
    }
    r.err = PROVEN_ERR_IO;
    r.value = 0;
    return r;
}
