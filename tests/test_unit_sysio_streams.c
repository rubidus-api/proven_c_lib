#include "proven.h"
#include "proven_test.h"
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("the standard streams as writers and readers",
        "stdin/stdout/stderr bridged onto the stream layer.",
        "Skipped on Windows: the test drives the real fd 0 and fd 1 through pipes, which is POSIX.");
    PROVEN_TEST_INFO("skipped on Windows");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

#include <unistd.h>
#include <fcntl.h>

/*
 * Written from the contract in include/proven/sysio.h before the bridge existed
 * (docs/TESTING.md §5.1). Two things were missing, and one thing was a lie:
 *
 *   - There was NO WAY to read stdin a line at a time. The standard streams were
 *     proven_file_t and nothing else, so the buffered reader and the line reader in stream.h -
 *     which do exactly this - could not be pointed at them. The single most common thing a
 *     program does with stdin had no route through the library.
 *
 *   - There was no way to print to a BUFFERED stdout. Every proven_print was a write syscall,
 *     so a thousand small lines cost a thousand syscalls, and the formatter could not be aimed
 *     at a standard stream at all (proven_fprint takes a writer; stdout was not one).
 *
 *   - proven_sysio_flush claimed to flush a buffer. There was no buffer. It did nothing on
 *     POSIX and a disk sync on Windows (B-004). The test below is what "flush" now means: the
 *     bytes are NOT out before it, and they ARE out after it. That assertion is only writable
 *     because there is finally something to flush.
 *
 * The test drives the real standard streams: it dup2's a pipe over fd 0 and fd 1, so what is
 * exercised is proven_sysio_stdin()/stdout() themselves, not a stand-in.
 */

/* Replace fd `target` with the read/write end of a fresh pipe; give back the other end. */
static int hijack(int target, int *other_end, int *saved) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    *saved = dup(target);
    if (*saved < 0) return -1;

    if (target == 0) {                 /* stdin: the pipe's READ end becomes fd 0 */
        if (dup2(fds[0], 0) < 0) return -1;
        close(fds[0]);
        *other_end = fds[1];           /* we write into this */
    } else {                           /* stdout: the pipe's WRITE end becomes fd 1 */
        if (dup2(fds[1], target) < 0) return -1;
        close(fds[1]);
        *other_end = fds[0];           /* we read from this */
    }
    return 0;
}

static void restore(int target, int saved) {
    dup2(saved, target);
    close(saved);
}

/* Read whatever is currently readable, without blocking. */
static proven_size_t drain(int fd, char *out, proven_size_t cap) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    proven_size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - total);
        if (n <= 0) break;
        total += (proven_size_t)n;
        if (total >= cap) break;
    }
    fcntl(fd, F_SETFL, fl);
    out[total] = 0;
    return total;
}

int main(void) {
    PROVEN_TEST_SUITE("the standard streams are writers and readers",
        "stdin, stdout and stderr bridged onto stream.h: a line reader over stdin, a buffered writer over stdout, and a flush that finally means something.",
        "Inspect the bridge in src/proven/sysio.c. The test drives the real fd 0 and fd 1 through pipes, so a pass means the standard streams themselves work, not a stand-in.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reading stdin a line at a time",
        "The most common thing a program does with stdin, and there was no route to it.",
        "A pipe is dup2'd over fd 0, so this is proven_sysio_stdin() for real - including the short reads a pipe delivers.");
    // ---------------------------------------------------------------
    {
        int w = -1, saved = -1;
        PROVEN_TEST_ASSERT(hijack(0, &w, &saved) == 0, "setup: hijack fd 0", "");

        const char *input = "first\nsecond\r\nthird line with spaces\nno trailing newline";
        PROVEN_TEST_ASSERT(write(w, input, strlen(input)) == (ssize_t)strlen(input), "setup: write the input", "");
        close(w);   /* EOF for the reader */

        proven_byte_t buf[128];
        proven_sysio_lines_t lines;
        proven_err_t e = proven_sysio_stdin_lines(&lines, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
        PROVEN_TEST_ASSERT(proven_is_ok(e), "a line reader must open over stdin", "");

        const char *want[] = { "first", "second", "third line with spaces", "no trailing newline" };
        for (int i = 0; i < 4; ++i) {
            proven_result_u8str_view_t ln = proven_sysio_read_line(&lines);
            PROVEN_TEST_ASSERT(proven_is_ok(ln.err), "each line must be read", "");
            PROVEN_TEST_ASSERT(ln.val.size == strlen(want[i]) &&
                               memcmp(ln.val.ptr, want[i], ln.val.size) == 0,
                "the line must come back without its newline, and \\r\\n must not leave a \\r",
                "A trailing '\\r' here means the CRLF case is not handled; a missing final line means a line without a trailing newline is being dropped.");
        }

        proven_result_u8str_view_t eof = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(eof.err == PROVEN_ERR_EOF,
            "the end of input is EOF, not an empty line forever", "");

        restore(0, saved);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a buffered stdout: nothing is written until it is flushed",
        "This is what flush MEANS now. proven_sysio_flush used to claim to flush a buffer that did not exist - it did nothing on POSIX (B-004). There is a buffer now, and the assertion below is only writable because of it.",
        "The bytes must NOT be in the pipe before the flush, and they MUST be after it.");
    // ---------------------------------------------------------------
    {
        int r = -1, saved = -1;
        PROVEN_TEST_ASSERT(hijack(1, &r, &saved) == 0, "setup: hijack fd 1", "");

        proven_byte_t obuf[256];
        proven_sysio_out_t out;
        proven_writer_t w = proven_sysio_stdout_buffered(&out, (proven_mem_mut_t){ .ptr = obuf, .size = sizeof obuf });

        bool wrote_ok = true;
        for (int i = 0; i < 5; ++i) {
            /* The formatter, aimed straight at a standard stream. This could not be done. */
            if (!PROVEN_FMT_IS_OK(proven_fprintln(w, "line {}", PROVEN_ARG(i)))) wrote_ok = false;
        }

        char seen[512];
        proven_size_t n_before = drain(r, seen, sizeof seen - 1);

        proven_err_t fe = proven_writer_flush(w);

        char after[512];
        proven_size_t n_after = drain(r, after, sizeof after - 1);

        restore(1, saved);

        PROVEN_TEST_ASSERT(wrote_ok, "every formatted line must be accepted by the writer", "");
        PROVEN_TEST_ASSERT(n_before == 0,
            "nothing may reach the OS before the flush - that is what buffered means",
            "If bytes are already in the pipe, the writer is not buffering and the flush means nothing.");
        PROVEN_TEST_ASSERT(proven_is_ok(fe), "the flush must succeed", "");
        PROVEN_TEST_ASSERT(n_after > 0 &&
                           strcmp(after, "line 0\nline 1\nline 2\nline 3\nline 4\n") == 0,
            "and after the flush, every buffered byte must be out, in order",
            "This is the honest meaning flush was waiting for (B-004).");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an unbuffered writer over a standard stream is out immediately",
        "stderr must not wait for anything: an error is out before the next line of code runs.",
        "");
    // ---------------------------------------------------------------
    {
        int r = -1, saved = -1;
        PROVEN_TEST_ASSERT(hijack(1, &r, &saved) == 0, "setup: hijack fd 1", "");

        proven_sysio_std_t st;
        proven_writer_t w = proven_sysio_stdout_writer(&st);
        PROVEN_TEST_ASSERT(proven_writer_is_valid(w), "the writer must be valid", "");

        proven_err_t e = proven_writer_write_str(w, PROVEN_LIT("straight out"));

        char seen[128];
        proven_size_t n = drain(r, seen, sizeof seen - 1);

        restore(1, saved);

        PROVEN_TEST_ASSERT(proven_is_ok(e), "the write must succeed", "");
        PROVEN_TEST_ASSERT(n == 12 && strcmp(seen, "straight out") == 0,
            "an unbuffered write is in the pipe with no flush at all", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a line longer than the buffer is refused, not truncated",
        "A silently shortened line is a wrong answer that looks like a right one.",
        "");
    // ---------------------------------------------------------------
    {
        int w = -1, saved = -1;
        PROVEN_TEST_ASSERT(hijack(0, &w, &saved) == 0, "setup: hijack fd 0", "");

        char big[200];
        memset(big, 'x', sizeof big);
        big[sizeof big - 1] = '\n';
        PROVEN_TEST_ASSERT(write(w, big, sizeof big) == (ssize_t)sizeof big, "setup: write a long line", "");
        close(w);

        proven_byte_t small[32];
        proven_sysio_lines_t lines;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_sysio_stdin_lines(&lines, (proven_mem_mut_t){ .ptr = small, .size = sizeof small })),
            "the reader opens", "");

        proven_result_u8str_view_t ln = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(ln.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a line that does not fit the buffer is OUT_OF_BOUNDS, never a truncated line", "");

        restore(0, saved);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the arguments are checked",
        "A null state or an empty buffer is a caller error, and must be reported rather than crashed on.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[16];
        proven_sysio_lines_t lines;
        PROVEN_TEST_ASSERT(proven_sysio_stdin_lines(NULL, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }) == PROVEN_ERR_INVALID_ARG,
            "a null state is INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_sysio_stdin_lines(&lines, (proven_mem_mut_t){ .ptr = NULL, .size = 0 }) == PROVEN_ERR_INVALID_ARG,
            "an empty buffer is INVALID_ARG", "");
    }

    PROVEN_TEST_PASS("the standard streams are writers and readers, and flush finally means something.");
    return 0;
}
#endif
