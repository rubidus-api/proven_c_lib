#include "example.h"
#include <string.h>

/*
 * UTF-16 text in and out: to the standard streams, to a file in the encoding
 * its reader expects, and back in a line at a time.
 *
 * Output goes through the formatter, so every sink that takes u8 text takes u16
 * text too: PROVEN_ARG picks the u16 renderer for a proven_u16str_view_t and
 * writes UTF-8. When a consumer wants UTF-16 bytes - a Windows tool that reads
 * UTF-16LE files, say - a writer writes them in the byte order you name.
 *
 * Input is a reader that decodes any of the three encodings into your u16
 * buffer. It carries a character split between two reads of the source, and it
 * keeps the byte line reader's rules: "\r\n" loses its '\r', the last line needs
 * no newline, and a line too long for the buffer is an error, not a cut line.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* Two Hangul syllables ("hello"), a space, and an emoji - which is a surrogate pair. */
    static const proven_u16 hello[] = { 0xC548, 0xB155, ' ', 0xD83D, 0xDE42 };
    proven_u16str_view_t text = { hello, 5 };

    /* --- to the standard streams --------------------------------------- */

    /* UTF-8 on stdout and stderr. On a Windows console the sysio layer hands
     * it to the console as UTF-16, so it shows correctly whatever the code page. */
    proven_err_t err = proven_println("stdout: {}", PROVEN_ARG(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "u16 text prints like u8 text");
    err = proven_eprintln("stderr: {}", proven_arg_u16(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "and to stderr");

    /* A buffered writer takes it too; flush it, or it never happened. */
    proven_sysio_out_t out;
    proven_byte_t buf[256];
    proven_writer_t w = proven_sysio_stdout_buffered(&out, (proven_mem_mut_t){ buf, sizeof buf });
    EXAMPLE_REQUIRE(proven_is_ok(proven_fprintln(w, "buffered: {}", PROVEN_ARG(text)).err), "fprintln takes u16");
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(w)), "and the flush sends it");

    /* Strict: an unpaired surrogate fails the whole line instead of printing junk. */
    static const proven_u16 torn_units[] = { 'o', 'k', 0xD83D };
    proven_u16str_view_t torn = { torn_units, 3 };
    err = proven_println("{}", PROVEN_ARG(torn));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING, "half a surrogate pair is refused");

    /* --- to a file, as UTF-16LE with a BOM --------------------------------- */

    proven_u8str_view_t path = PROVEN_LIT("proven_example_u16.txt");
    proven_result_file_t f = proven_fs_open(alloc, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "creating the file must succeed");
    if (!proven_is_ok(f.err)) return EXAMPLE_OK();

    proven_sysio_out_t fout;
    proven_writer_t fw = proven_sysio_file_buffered(&fout, f.value, (proven_mem_mut_t){ buf, sizeof buf });
    /* The BOM is written only because it is asked for. */
    err = proven_writer_write_bom(fw, PROVEN_TEXT_UTF16LE);
    for (int i = 0; i < 2 && proven_is_ok(err); ++i) {
        err = proven_writer_write_u16(fw, text, PROVEN_TEXT_UTF16LE);
        if (proven_is_ok(err)) err = proven_writer_write_u16(fw, (proven_u16str_view_t){ u"\r\n", 2 }, PROVEN_TEXT_UTF16LE);
    }
    if (proven_is_ok(err)) err = proven_writer_flush(fw);
    EXAMPLE_REQUIRE(proven_is_ok(err), "two lines of UTF-16LE are written");
    (void)proven_fs_close(f.value);

    /* --- back in, a line at a time ----------------------------------------- */

    f = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "opening the file again must succeed");
    if (proven_is_ok(f.err)) {
        /* AUTO: the BOM says UTF-16LE, and is consumed. The buffer is in code
         * units and must hold the longest line. */
        proven_u16 line_buf[64];
        proven_sysio_u16_lines_t lines;
        err = proven_sysio_u16_lines_open(&lines, f.value, PROVEN_TEXT_AUTO, line_buf, 64);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line reader opens");
        int n = 0;
        for (;;) {
            proven_result_u16str_view_t line = proven_sysio_read_u16_line(&lines);
            if (line.err == PROVEN_ERR_EOF) break;
            EXAMPLE_REQUIRE(proven_is_ok(line.err), "each line reads");
            if (!proven_is_ok(line.err)) break;
            /* The view points into line_buf until the next call. */
            EXAMPLE_REQUIRE(line.val.size == 5 && memcmp(line.val.ptr, hello, sizeof hello) == 0,
                            "each line is the text, without its CR LF");
            ++n;
        }
        EXAMPLE_REQUIRE(n == 2 && lines.reader.enc == PROVEN_TEXT_UTF16LE, "two lines, read as UTF-16LE");
        (void)proven_fs_close(f.value);
    }
    (void)proven_fs_remove(alloc, path);

    /* --- any reader, any encoding ------------------------------------------ */

    /* The same reader over bytes already in memory, here UTF-8. read() hands out
     * code units whatever lines they belong to, and never half a pair. */
    proven_reader_view_t src;
    proven_u16 rbuf[16];
    proven_u16_reader_t rd;
    err = proven_u16_reader_init(&rd, proven_reader_from_view(&src, PROVEN_LIT("A\xF0\x9F\x99\x82\nB")),
                                 PROVEN_TEXT_UTF8, rbuf, 16);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a u16 reader over UTF-8 bytes");
    proven_result_u16str_view_t first = proven_u16_reader_read_line(&rd);
    EXAMPLE_REQUIRE(proven_is_ok(first.err) && first.val.size == 3, "'A' and the emoji's two units");
    proven_u16 rest[4];
    proven_result_size_t got = proven_u16_reader_read(&rd, rest, 4);
    EXAMPLE_REQUIRE(proven_is_ok(got.err) && got.value == 1 && rest[0] == 'B', "the last line, without a newline");
    EXAMPLE_REQUIRE(proven_u16_reader_read(&rd, rest, 4).err == PROVEN_ERR_EOF, "then the end");

    return EXAMPLE_OK();
}
