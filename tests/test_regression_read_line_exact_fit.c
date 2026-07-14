#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>

/*
 * proven_reader_read_line documented one rule - "a line longer than the buffer is
 * PROVEN_ERR_OUT_OF_BOUNDS, not a silently truncated line" - and enforced a stricter one. It
 * refused any line that FILLED the buffer, and the reason it had to was that it asked the
 * wrong question first:
 *
 *     if (cursor == 0 && len == buf.size) -> OUT_OF_BOUNDS
 *
 * ran BEFORE any attempt to fill, because a fill cannot distinguish "the buffer is full" from
 * "the source has ended" - it returns false for both. So a full buffer was assumed to mean an
 * over-long line. It does not. It means one of three things, and only one of them is an error:
 *
 *   - the next byte is a newline      -> the line is EXACTLY buffer-sized, and complete;
 *   - the source has ended            -> what is held IS the file's final line;
 *   - the next byte is anything else  -> the line really is longer than the buffer.
 *
 * The middle case was the data-loss one, and it is not exotic: a 4-byte file with no trailing
 * newline, read through a 4-byte buffer, came back OUT_OF_BOUNDS with its entire contents
 * unreachable. One byte of lookahead tells the three apart, so the documented rule is now the
 * rule that is actually enforced.
 *
 * Found by the standing adversarial audit of the standard-stream bridge, which put the line
 * reader under a reference splitter at every buffer size.
 */

static void write_file(const char *path, const char *content, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(content, 1, n, f); fclose(f); }
}

/* Read every line of `path` through a buffer of exactly `cap` bytes. */
static int read_all_lines(proven_allocator_t heap, const char *path, proven_size_t cap,
                          char out[][64], int max_lines, proven_err_t *last_err) {
    proven_result_file_t rf = proven_fs_open(heap, proven_u8str_view_from_cstr(path), PROVEN_FS_READ);
    if (!proven_is_ok(rf.err)) { *last_err = rf.err; return -1; }

    proven_byte_t buf[256];
    proven_sysio_lines_t lines;
    proven_err_t e = proven_sysio_lines_open(&lines, rf.value, (proven_mem_mut_t){ .ptr = buf, .size = cap });
    if (!proven_is_ok(e)) { (void)proven_fs_close(rf.value); *last_err = e; return -1; }

    int n = 0;
    for (;;) {
        proven_result_u8str_view_t ln = proven_sysio_read_line(&lines);
        if (ln.err == PROVEN_ERR_EOF) { *last_err = PROVEN_OK; break; }
        if (!proven_is_ok(ln.err)) { *last_err = ln.err; break; }
        if (n >= max_lines) { *last_err = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        memcpy(out[n], ln.val.ptr, ln.val.size);
        out[n][ln.val.size] = 0;
        ++n;
    }
    (void)proven_fs_close(rf.value);
    return n;
}

int main(void) {
    PROVEN_TEST_SUITE("a line that FITS the buffer is a line, not an error",
        "The reader documented that only a line LONGER than the buffer is OUT_OF_BOUNDS. A line exactly the size of the buffer - and, worse, a file whose entire unterminated contents fill it - was refused, and its data was unreachable.",
        "Inspect the buffer-full branch of proven_reader_read_line in src/proven/stream.c. It must not answer 'too long' before it has looked at the next byte: a full buffer can equally mean 'complete line, newline next' or 'this is the last line'.");

    proven_allocator_t heap = proven_heap_allocator();
    char got[8][64];
    proven_err_t last = PROVEN_OK;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a file with no trailing newline, exactly buffer-sized",
        "This is the data-loss case: the whole file is sitting in the buffer and there is nothing more to read, and it came back as an error.",
        "");
    // ---------------------------------------------------------------
    {
        write_file("rl_exact.tmp", "xxxx", 4);
        int n = read_all_lines(heap, "rl_exact.tmp", 4, got, 8, &last);
        PROVEN_TEST_ASSERT(n == 1 && strcmp(got[0], "xxxx") == 0 && proven_is_ok(last),
            "a 4-byte file read through a 4-byte buffer must return its 4 bytes",
            "OUT_OF_BOUNDS here means the entire contents of the file are unreachable through this API.");
        (void)proven_fs_remove(heap, PROVEN_LIT("rl_exact.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a line exactly the size of the buffer, terminated by a newline",
        "The line fits. The newline is the very next byte. The documented rule says only a LONGER line is an error.",
        "");
    // ---------------------------------------------------------------
    {
        write_file("rl_exact.tmp", "abcd\nef\n", 8);
        int n = read_all_lines(heap, "rl_exact.tmp", 4, got, 8, &last);
        PROVEN_TEST_ASSERT(n == 2 && strcmp(got[0], "abcd") == 0 && strcmp(got[1], "ef") == 0 && proven_is_ok(last),
            "a 4-byte line in a 4-byte buffer must be returned, and the stream must carry on",
            "The newline does not have to fit inside the buffer for the line to fit.");
        (void)proven_fs_remove(heap, PROVEN_LIT("rl_exact.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a line genuinely longer than the buffer is still refused",
        "The fix must not turn the real error into a silently truncated line - that is the corruption the check existed to prevent.",
        "");
    // ---------------------------------------------------------------
    {
        write_file("rl_exact.tmp", "abcde\n", 6);
        int n = read_all_lines(heap, "rl_exact.tmp", 4, got, 8, &last);
        PROVEN_TEST_ASSERT(n == 0 && last == PROVEN_ERR_OUT_OF_BOUNDS,
            "a 5-byte line in a 4-byte buffer is OUT_OF_BOUNDS, never a truncated \"abcd\"",
            "A truncated line returned as a success is a corruption the caller cannot detect.");
        (void)proven_fs_remove(heap, PROVEN_LIT("rl_exact.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("CRLF at the exact boundary",
        "The '\\r' is the last byte in the buffer and the '\\n' is the next one. The line is still 3 characters, and the '\\r' is not part of it.",
        "");
    // ---------------------------------------------------------------
    {
        write_file("rl_exact.tmp", "abc\r\nz\n", 7);
        int n = read_all_lines(heap, "rl_exact.tmp", 4, got, 8, &last);
        PROVEN_TEST_ASSERT(n == 2 && strcmp(got[0], "abc") == 0 && strcmp(got[1], "z") == 0,
            "\"abc\\r\" fills a 4-byte buffer; the line is \"abc\" and the stream continues", "");
        (void)proven_fs_remove(heap, PROVEN_LIT("rl_exact.tmp"));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the ordinary cases keep working at every buffer size",
        "The fix touches the buffer-full path, so the paths that were already right must stay right.",
        "");
    // ---------------------------------------------------------------
    {
        const char *content = "one\ntwo\nthree\n";
        write_file("rl_exact.tmp", content, strlen(content));
        for (proven_size_t cap = 6; cap <= 32; ++cap) {
            int n = read_all_lines(heap, "rl_exact.tmp", cap, got, 8, &last);
            PROVEN_TEST_ASSERT(n == 3 && strcmp(got[0], "one") == 0 &&
                               strcmp(got[1], "two") == 0 && strcmp(got[2], "three") == 0 &&
                               proven_is_ok(last),
                "three lines come back as three lines, at every buffer size that fits them", "");
        }
        (void)proven_fs_remove(heap, PROVEN_LIT("rl_exact.tmp"));
    }

    PROVEN_TEST_PASS("a line that fits is returned; only a line that does not fit is an error.");
    return 0;
}
