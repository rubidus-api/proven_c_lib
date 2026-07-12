#include "example.h"

/*
 * Writers and readers: one interface for "where bytes go" and one for "where bytes
 * come from".
 *
 * The point is that the code below - render_row - does not know and does not care
 * whether it is writing to a string, to a fixed buffer, or to a file. That was
 * impossible before: the formatter's only sink was a proven_u8str_t.
 */

/* One serializer. It takes a sink, not a destination. */
static proven_err_t render_row(proven_writer_t w, int id, const char *name) {
    proven_fmt_result_t r = proven_fprintln(w, "{:>4} | {}", PROVEN_ARG(id), PROVEN_ARG(name));
    return r.err;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- the same code, into a growing string ----------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "string create");

    proven_writer_u8str_t s_state;
    proven_writer_t to_string = proven_writer_from_u8str(&s_state, &s.value, alloc);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_string, 7, "ada")), "render into a string");

    printf("into a string:\n%s", proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- the same code, into memory you own: zero allocations ------------- */
    proven_byte_t fixed[64];
    proven_writer_buf_t b_state = { .buf = { .ptr = fixed, .size = sizeof fixed } };
    proven_writer_t to_buffer = proven_writer_from_buffer(&b_state);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_buffer, 8, "grace")), "render into a buffer");
    EXAMPLE_REQUIRE(b_state.len > 0, "the buffer received the row");

    /* A full buffer REFUSES; it does not truncate. A sink that silently drops the
     * end of your data is worse than one that says it cannot take it. */
    proven_byte_t tiny[4];
    proven_writer_buf_t t_state = { .buf = { .ptr = tiny, .size = sizeof tiny } };
    proven_writer_t to_tiny = proven_writer_from_buffer(&t_state);
    EXAMPLE_REQUIRE(proven_writer_write_str(to_tiny, PROVEN_LIT("far too long")) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a full buffer refuses rather than truncating");
    EXAMPLE_REQUIRE(t_state.overflowed, "and it records that it did");

    /* --- the same code, into a file, buffered ------------------------------ */
    /*
     * The buffer is memory YOU supply, exactly like an arena's. This library has no
     * hidden global state, so it cannot flush for you at exit - which is why you must
     * flush before the buffer goes out of scope. In exchange, your logging path never
     * allocates: ten thousand lines here cost 0 mallocs and a couple of dozen write
     * syscalls, where ten thousand proven_println calls cost 10,000 syscalls.
     */
    proven_u8str_view_t path = PROVEN_LIT("example_stream_rows.txt");
    proven_result_file_t f = proven_fs_open(alloc, path,
        (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "open the output file");
    proven_file_t file = f.value;

    proven_byte_t out_buf[4096];
    proven_writer_buffered_t w_state;
    proven_writer_t to_file = proven_writer_buffered(&w_state,
        proven_writer_from_file(&file),
        (proven_mem_mut_t){ .ptr = out_buf, .size = sizeof out_buf });

    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_is_ok(render_row(to_file, i, "row")), "render into the file");
    }
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(to_file)),
                    "flush: nothing is written until you say so");
    proven_fs_close(file);

    /* --- reading it back, a line at a time -------------------------------- */
    /* Reading a file line by line was simply not possible before: the only route was
     * loading the entire file into memory and splitting it by hand. */
    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopen for reading");
    proven_file_t rfile = rf.value;

    proven_byte_t in_buf[128];
    proven_reader_buffered_t r_state;
    (void)proven_reader_buffered(&r_state, proven_reader_from_file(&rfile),
                                 (proven_mem_mut_t){ .ptr = in_buf, .size = sizeof in_buf });

    int lines = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_reader_read_line(&r_state);
        if (line.err == PROVEN_ERR_EOF) break;
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "read a line");
        /* The view points INTO the reader's buffer, and is valid only until the next
         * call. Copy it if it has to outlive that. */
        printf("line %d: %.*s\n", lines, (int)line.val.size, (const char *)line.val.ptr);
        ++lines;
    }
    EXAMPLE_REQUIRE(lines == 3, "three rows in, three lines out");

    proven_fs_close(rfile);
    (void)proven_fs_remove(alloc, path);

    return EXAMPLE_OK();
}
