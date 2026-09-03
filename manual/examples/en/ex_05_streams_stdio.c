#include "example.h"

/*
 * A writer is "somewhere bytes go" and a reader is "somewhere bytes come from",
 * each of them two pointers: a small table of functions, and the state those
 * functions work on. That is all. The value of the arrangement is that code
 * written against a writer does not know or care whether the bytes end up in a
 * file, in a string, on the terminal, or in a test buffer - and code written
 * against a reader can be tested against a string in memory instead of a file
 * on disk.
 *
 * This program shows both, and the standard-stream and file plumbing that hangs
 * off them:
 *
 *   - a reader over a view (an in-memory string), which is how you test a
 *     parser without touching the filesystem,
 *   - the unbuffered stdout and stderr writers, and the difference between
 *     "write all of this" and "write what you can",
 *   - a buffered writer over a file, which turns many small writes into few
 *     large ones,
 *   - a line reader over that same file,
 *   - reading formatted values straight out of a file handle.
 *
 * A note that costs people an afternoon: the state structs below must stay
 * where they are declared. A writer holds a pointer INTO its state struct, so
 * copying the struct leaves the copy inert and the original addressed.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 1. a reader over bytes you already have -------------------------- */

    /* The parser below does not know this is a string. It would work the same
     * over a file, a pipe or a socket - which is exactly why the test can use
     * the cheap one. */
    proven_reader_view_t src_state;
    proven_reader_t src = proven_reader_from_view(&src_state, PROVEN_LIT("id=41\nid=42\n"));

    /* is_valid asks whether the reader was actually built - a zero-initialised
     * handle is not a reader, and this is the check that says so at the
     * boundary instead of at the first read. */
    EXAMPLE_REQUIRE(proven_reader_is_valid(src), "a reader made from a view must be usable");
    proven_reader_t never_made = {0};
    EXAMPLE_REQUIRE(!proven_reader_is_valid(never_made), "a zero-initialised reader handle is not");

    /* read fills up to dest.size bytes and reports how many it actually got.
     * A short read is normal - it is not end of file, and treating it as one is
     * the classic way to lose the tail of an input. End of file has its own
     * code, PROVEN_ERR_EOF. */
    proven_byte_t chunk[5];
    proven_mem_mut_t into = { .ptr = chunk, .size = sizeof chunk };
    proven_result_size_t got = proven_reader_read(src, into);
    EXAMPLE_REQUIRE(proven_is_ok(got.err), "the first read must succeed");
    EXAMPLE_REQUIRE(got.value == 5, "and fill the buffer from the view");

    proven_size_t total = got.value;
    for (;;) {
        got = proven_reader_read(src, into);
        if (got.err == PROVEN_ERR_EOF) {
            break;
        }
        EXAMPLE_REQUIRE(proven_is_ok(got.err), "reads before end of file must succeed");
        total += got.value;
    }
    EXAMPLE_REQUIRE(total == 12, "the loop must consume the whole view, however it was chunked");

    /* --- 2. the standard streams ------------------------------------------ */

    /* The state struct holds the handle the writer points at, so it has to
     * outlive the writer. Declaring the two next to each other is the habit
     * that keeps that true. */
    proven_sysio_std_t out_state;
    proven_writer_t out = proven_sysio_stdout_writer(&out_state);
    EXAMPLE_REQUIRE(proven_writer_is_valid(out), "the stdout writer must be usable");

    /* write means "all of it, or an error". It loops internally, because a
     * single system-level write may move fewer bytes than asked. */
    proven_err_t err = proven_writer_write(out, proven_mem_view_from_u8(PROVEN_LIT("stream example: start\n")));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing a whole line to stdout must succeed");

    /* write_partial is the honest low-level twin: it moves what it can and
     * reports the count. Use it when you are managing your own retry or
     * back-pressure; use write when you just want the bytes out. */
    proven_result_size_t part = proven_writer_write_partial(out, proven_mem_view_from_u8(PROVEN_LIT("partial write\n")));
    EXAMPLE_REQUIRE(proven_is_ok(part.err), "a partial write to a terminal or pipe must succeed");
    EXAMPLE_REQUIRE(part.value > 0, "and report how many bytes it moved");

    /* stderr is unbuffered on purpose: a diagnostic is out before the next line
     * of code runs, which is what you need when the next line is the one that
     * crashes. */
    proven_sysio_std_t err_state;
    proven_writer_t diag = proven_sysio_stderr_writer(&err_state);
    EXAMPLE_REQUIRE(proven_writer_is_valid(diag), "the stderr writer must be usable");
    err = proven_writer_write(diag, proven_mem_view_from_u8(PROVEN_LIT("stream example: diagnostics go here\n")));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing to stderr must succeed");

    /* A reader over stdin is built the same way. This example does not read
     * from it - a test run has no one typing - but building it shows the shape,
     * and it is the handle a filter program would loop over. */
    proven_sysio_std_t in_state;
    proven_reader_t stdin_reader = proven_sysio_stdin_reader(&in_state);
    EXAMPLE_REQUIRE(proven_reader_is_valid(stdin_reader), "the stdin reader must be usable");

    /* --- 3. buffered output to a file ------------------------------------- */

    proven_u8str_view_t path = PROVEN_LIT("proven_example_streams.txt");
    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the output file must succeed");
    if (!proven_is_ok(f.err)) return 1;

    /* The buffer is yours: the library does not allocate one behind your back,
     * so the memory cost of buffering is a number you chose and can see. Sixty
     * lines through a 256-byte buffer is a handful of writes instead of sixty. */
    proven_byte_t outbuf[256];
    proven_sysio_out_t file_out;
    proven_writer_t w = proven_sysio_file_buffered(&file_out, f.value,
                                                   (proven_mem_mut_t){ .ptr = outbuf, .size = sizeof outbuf });
    EXAMPLE_REQUIRE(proven_writer_is_valid(w), "the buffered file writer must be usable");

    for (int i = 0; i < 20; ++i) {
        proven_fmt_result_t line_out = proven_fprintln(w, "reading {} = {}",
                                                       proven_arg_i32(i), proven_arg_i32(i * i));
        EXAMPLE_REQUIRE(proven_is_ok(line_out.err), "writing a formatted line must succeed");
    }

    /* Buffered output that is never flushed is output that never happened.
     * Nothing here flushes at exit on your behalf. */
    err = proven_writer_flush(w);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the flush is what actually writes the file");
    err = proven_fs_close(f.value);
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the file must succeed");

    /* --- 4. reading it back a line at a time ------------------------------ */

    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopening for reading must succeed");
    if (!proven_is_ok(rf.err)) return 1;

    /* Size the buffer for the longest LINE you expect. A longer line is
     * reported as OUT_OF_BOUNDS - never silently cut in half, which is the
     * failure that turns one bad record into two plausible ones. */
    proven_byte_t linebuf[128];
    proven_sysio_lines_t lines;
    err = proven_sysio_lines_open(&lines, rf.value, (proven_mem_mut_t){ .ptr = linebuf, .size = sizeof linebuf });
    EXAMPLE_REQUIRE(proven_is_ok(err), "opening a line reader over the file must succeed");

    proven_size_t count = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) {
            break;
        }
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "reading a line must succeed until end of file");
        /* The view points into linebuf and is good only until the next call.
         * That is what makes a million lines cost one buffer instead of a
         * million allocations - and why you copy a line you want to keep. */
        if (count == 0) {
            EXAMPLE_REQUIRE(proven_u8str_view_eq(line.val, PROVEN_LIT("reading 0 = 0")),
                            "the first line reads back exactly as it was written");
        }
        ++count;
    }
    EXAMPLE_REQUIRE(count == 20, "every line written must be read back");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(rf.value)), "closing the read handle must succeed");

    /* --- 5. reading values, not lines, from a file ------------------------ */

    /* When the input is a known shape rather than free text, scanning it
     * directly saves writing the split-and-convert loop by hand. This reads one
     * chunk from the file handle and pulls the two numbers out of it. */
    proven_result_file_t sf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(sf.err), "opening the file for scanning must succeed");

    proven_i32 index = -1, square = -1;
    err = proven_scan_fmt_from_file(sf.value, "reading {} = {}",
                                    proven_scan_arg_i32(&index), proven_scan_arg_i32(&square));
    EXAMPLE_REQUIRE(proven_is_ok(err), "scanning the first record must succeed");
    EXAMPLE_REQUIRE(index == 0 && square == 0, "and produce the values that were written");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(sf.value)), "closing the scan handle must succeed");

    printf("streams: %zu byte(s) read from the view, %zu line(s) round-tripped\n",
           (size_t)total, (size_t)count);

    (void)proven_fs_remove(alloc, path);
    return EXAMPLE_OK();
}
