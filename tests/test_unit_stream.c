#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Writers and readers.
 *
 * The point of the abstraction is that one piece of code can move bytes without
 * knowing where they are going, so the tests below write the SAME function's output
 * into a string, a fixed buffer, and a file, and check all three agree.
 *
 * The contracts that are easy to get wrong, and are pinned here:
 *   - a full fixed buffer REFUSES, it does not truncate;
 *   - a buffered writer holds bytes until flushed, and loses nothing;
 *   - a line reader returns the last line even without a trailing newline;
 *   - a line longer than the buffer is an error, not a silently truncated line.
 */

/* One serializer. Three destinations. That is the whole idea. */
static proven_err_t emit_report(proven_writer_t w) {
    proven_err_t e = proven_writer_write_str(w, PROVEN_LIT("id="));
    if (!proven_is_ok(e)) return e;
    proven_fmt_result_t r = proven_fprint(w, "{} name={}", PROVEN_ARG(7), PROVEN_ARG("ada"));
    return r.err;
}

int main(void) {
    PROVEN_TEST_SUITE("writers and readers",
        "One piece of code must be able to move bytes without knowing where they go, and a sink must refuse rather than truncate.",
        "Inspect src/proven/stream.c. Buffering uses caller-supplied memory: there is no hidden global state and no allocation the caller did not ask for.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the same code writes to a string, a buffer, and a file",
        "That is what the abstraction is for; if the three disagree, the writers are not interchangeable.",
        "Compare the three write_fn implementations in stream.c.");
    // ---------------------------------------------------------------
    const char *expected = "id=7 name=ada";

    /* into an owned string */
    proven_result_u8str_t s = proven_u8str_create(heap, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(s.err), "string create failed", "");
    proven_writer_u8str_t sw_state;
    proven_writer_t sw = proven_writer_from_u8str(&sw_state, &s.value, heap);
    PROVEN_TEST_ASSERT(proven_is_ok(emit_report(sw)), "writing to a string failed", "");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s.value), expected) == 0,
        "the string sink produced the wrong bytes", "");
    proven_u8str_destroy(heap, &s.value);

    /* into a fixed caller buffer - no allocation at all */
    proven_byte_t fixed[64];
    proven_writer_buf_t bw_state = { .buf = { .ptr = fixed, .size = sizeof fixed }, .len = 0, .overflowed = false };
    proven_writer_t bw = proven_writer_from_buffer(&bw_state);
    PROVEN_TEST_ASSERT(proven_is_ok(emit_report(bw)), "writing to a buffer failed", "");
    PROVEN_TEST_ASSERT(bw_state.len == strlen(expected) &&
                       memcmp(fixed, expected, bw_state.len) == 0,
        "the buffer sink produced the wrong bytes", "");

    /* into a file */
    proven_u8str_view_t path = PROVEN_LIT("test_stream_out.txt");
    proven_result_file_t f = proven_fs_open(heap, path,
        (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
    PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open failed", "");
    proven_file_t file = f.value;
    proven_writer_t fw = proven_writer_from_file(&file);
    PROVEN_TEST_ASSERT(proven_is_ok(emit_report(fw)), "writing to a file failed", "");
    (void)proven_fs_close(file);

    proven_result_mem_mut_t back = proven_fs_read_all(heap, path);
    PROVEN_TEST_ASSERT(proven_is_ok(back.err) && back.value.size == strlen(expected) &&
                       memcmp(back.value.ptr, expected, back.value.size) == 0,
        "the file sink produced the wrong bytes",
        "All three sinks must agree; that is the point of the interface.");
    heap.free_fn(heap.ctx, back.value.ptr);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a full fixed buffer refuses; it does not truncate",
        "A sink that silently drops the end of your data is worse than one that says it cannot take it.",
        "Inspect writer_buf_write: overflow must return PROVEN_ERR_OUT_OF_BOUNDS and set `overflowed`.");
    // ---------------------------------------------------------------
    proven_byte_t tiny[4];
    proven_writer_buf_t tw_state = { .buf = { .ptr = tiny, .size = sizeof tiny }, .len = 0, .overflowed = false };
    proven_writer_t tw = proven_writer_from_buffer(&tw_state);
    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_str(tw, PROVEN_LIT("abcd"))), "exactly filling the buffer is fine", "");
    proven_err_t over = proven_writer_write_str(tw, PROVEN_LIT("e"));
    PROVEN_TEST_ASSERT(over == PROVEN_ERR_OUT_OF_BOUNDS, "one byte too many must be refused", "");
    PROVEN_TEST_ASSERT(tw_state.overflowed, "the overflow must be recorded, not hidden", "");
    PROVEN_TEST_ASSERT(tw_state.len == 4, "a refused write must not have been partially applied", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a buffered writer holds bytes until flushed, and loses none",
        "This is what turns ten thousand one-line writes into a handful of syscalls.",
        "Inspect writer_buffered_write and writer_buffered_flush. The backing memory is the caller's - there is no destructor to flush for them.");
    // ---------------------------------------------------------------
    proven_result_u8str_t sink = proven_u8str_create(heap, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(sink.err), "string create failed", "");
    proven_writer_u8str_t inner_state;
    proven_writer_t inner = proven_writer_from_u8str(&inner_state, &sink.value, heap);

    proven_byte_t bufmem[16];
    proven_writer_buffered_t buffered_state;
    proven_writer_t buffered = proven_writer_buffered(&buffered_state, inner,
        (proven_mem_mut_t){ .ptr = bufmem, .size = sizeof bufmem });

    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_str(buffered, PROVEN_LIT("abc"))), "buffered write failed", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&sink.value).size == 0,
        "a buffered write must not reach the inner writer yet",
        "If it did, the buffer is not buffering.");

    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_flush(buffered)), "flush failed", "");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_u8str_as_view(&sink.value), PROVEN_LIT("abc")),
        "flush must deliver exactly what was held", "");

    /* Overflowing the buffer flushes automatically rather than failing. */
    for (int i = 0; i < 10; ++i) {
        PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_str(buffered, PROVEN_LIT("0123456789"))),
            "writing past the buffer size must auto-flush, not fail", "");
    }
    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_flush(buffered)), "final flush failed", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&sink.value).size == 3 + 100,
        "nothing may be lost across auto-flushes",
        "A buffered writer that drops bytes when it wraps is worse than no buffer.");

    /* A chunk bigger than the whole buffer goes straight through. */
    proven_byte_t big[64];
    for (int i = 0; i < 64; ++i) big[i] = (proven_byte_t)'x';
    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write(buffered, (proven_mem_view_t){ .ptr = big, .size = 64 })),
        "a chunk larger than the buffer must pass through, not fail", "");
    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_flush(buffered)), "flush failed", "");
    PROVEN_TEST_ASSERT(proven_u8str_as_view(&sink.value).size == 3 + 100 + 64, "the oversized chunk arrived intact", "");
    proven_u8str_destroy(heap, &sink.value);

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reading lines",
        "There was no way to read a file line by line at all - the only route was loading the whole thing.",
        "Inspect proven_reader_read_line. The returned view points into the caller's buffer and is valid only until the next call.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t lines_path = PROVEN_LIT("test_stream_lines.txt");
        /* Note the last line has NO trailing newline: dropping it is how the last
         * record of a file goes missing. And a \r\n line, which must not keep its \r. */
        proven_err_t w = proven_fs_write_file(heap, lines_path,
            proven_mem_view_from_u8(PROVEN_LIT("alpha\nbeta\r\n\ngamma")));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "writing the fixture failed", "");

        proven_result_file_t lf = proven_fs_open(heap, lines_path, PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(proven_is_ok(lf.err), "open failed", "");
        proven_file_t lfile = lf.value;

        proven_byte_t rbuf[32];
        proven_reader_buffered_t rstate;
        (void)proven_reader_buffered(&rstate, proven_reader_from_file(&lfile),
                                     (proven_mem_mut_t){ .ptr = rbuf, .size = sizeof rbuf });

        proven_result_u8str_view_t ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(proven_is_ok(ln.err) && proven_u8str_view_eq(ln.val, PROVEN_LIT("alpha")), "line 1", "");

        ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(proven_is_ok(ln.err) && proven_u8str_view_eq(ln.val, PROVEN_LIT("beta")),
            "a \\r\\n line must not keep its carriage return", "");

        ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(proven_is_ok(ln.err) && ln.val.size == 0, "an empty line is a line", "");

        ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(proven_is_ok(ln.err) && proven_u8str_view_eq(ln.val, PROVEN_LIT("gamma")),
            "the final line must be returned even with no trailing newline",
            "Dropping it is how the last record of a file goes missing.");

        ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_EOF, "and then EOF", "");

        (void)proven_fs_close(lfile);
        (void)proven_fs_remove(heap, lines_path);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a line longer than the buffer is refused, not truncated",
        "A truncated line handed back as if it were whole is a corruption the caller cannot detect.",
        "Inspect the buffer-full branch of proven_reader_read_line.");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t lp = PROVEN_LIT("test_stream_longline.txt");
        proven_err_t w = proven_fs_write_file(heap, lp,
            proven_mem_view_from_u8(PROVEN_LIT("this single line is definitely longer than eight bytes\n")));
        PROVEN_TEST_ASSERT(proven_is_ok(w), "writing the fixture failed", "");

        proven_result_file_t lf = proven_fs_open(heap, lp, PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(proven_is_ok(lf.err), "open failed", "");
        proven_file_t lfile = lf.value;

        proven_byte_t small[8];
        proven_reader_buffered_t rstate;
        (void)proven_reader_buffered(&rstate, proven_reader_from_file(&lfile),
                                     (proven_mem_mut_t){ .ptr = small, .size = sizeof small });

        proven_result_u8str_view_t ln = proven_reader_read_line(&rstate);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a line that cannot fit the buffer must be refused",
            "Returning the first 8 bytes as if they were the line is a corruption nobody can see.");

        (void)proven_fs_close(lfile);
        (void)proven_fs_remove(heap, lp);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a reader over bytes you already have",
        "The same reader interface, with no file behind it.",
        "Inspect reader_view_read.");
    // ---------------------------------------------------------------
    {
        proven_reader_view_t vstate;
        proven_reader_t vr = proven_reader_from_view(&vstate, PROVEN_LIT("hello"));
        proven_byte_t got[8] = {0};
        proven_result_size_t rr = proven_reader_read(vr, (proven_mem_mut_t){ .ptr = got, .size = sizeof got });
        PROVEN_TEST_ASSERT(proven_is_ok(rr.err) && rr.value == 5 && memcmp(got, "hello", 5) == 0,
            "reading a view yields its bytes", "");
        rr = proven_reader_read(vr, (proven_mem_mut_t){ .ptr = got, .size = sizeof got });
        PROVEN_TEST_ASSERT(rr.err == PROVEN_ERR_EOF,
            "end of input is PROVEN_ERR_EOF, never a zero-byte success",
            "\"I read nothing and everything is fine\" is the shape of a loop that never terminates.");
    }

    (void)proven_fs_remove(heap, path);
    PROVEN_TEST_PASS("writers and readers behave.");
    return 0;
}
