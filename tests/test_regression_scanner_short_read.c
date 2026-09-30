#include "proven.h"
#include "proven_test.h"

#include "proven_sys_thread.h"
#include <string.h>
#include <stdbool.h>

/* A pipe, a writer thread and a pause, on either platform: an anonymous pipe is CreatePipe on
 * Windows and pipe() on POSIX, and it delivers short reads the same way on both. */
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
typedef HANDLE wend_t;
static bool make_pipe(proven_file_t *rd, wend_t *wr) {
    HANDLE r = NULL, w = NULL;
    if (!CreatePipe(&r, &w, NULL, 0)) return false;
    rd->internal.ptr = r;
    *wr = w;
    return true;
}
static void wend_write(wend_t w, const char *p, size_t n) { DWORD k = 0; (void)WriteFile(w, p, (DWORD)n, &k, NULL); }
static void wend_close(wend_t w) { CloseHandle(w); }
static void rd_close(proven_file_t f) { CloseHandle((HANDLE)f.internal.ptr); }
/* The write end, read from: ReadFile fails (ACCESS_DENIED), as read() on it fails on POSIX. */
static proven_file_t wend_as_file(wend_t w) { proven_file_t f = {0}; f.internal.ptr = w; return f; }
static void pause_ms(unsigned ms) { Sleep(ms ? ms : 1); }
#else
#include <unistd.h>
#include <time.h>
typedef int wend_t;
static bool make_pipe(proven_file_t *rd, wend_t *wr) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    rd->internal.fd = fds[0];
    *wr = fds[1];
    return true;
}
static void wend_write(wend_t w, const char *p, size_t n) { ssize_t k = write(w, p, n); (void)k; }
static void wend_close(wend_t w) { close(w); }
static void rd_close(proven_file_t f) { close(f.internal.fd); }
static proven_file_t wend_as_file(wend_t w) { proven_file_t f = {0}; f.internal.fd = w; return f; }
static void pause_ms(unsigned ms) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)(ms ? ms : 1) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}
#endif

/*
 * The buffered scanner exists FOR pipes - the header says so: "safe scanning for both
 * seekable and non-seekable streams (pipes, stdin)". It was the one thing it could not
 * do.
 *
 * read() on a pipe returns whatever has arrived so far. That is its contract, not an
 * error and not an end of file. scanner_fill treated any read shorter than the request
 * as end-of-input, and LATCHED it, so:
 *
 *   - a token straddling the read boundary was committed TRUNCATED. A writer that sent
 *     "123", paused, then "456 789\n" produced the integer 123. Not an error - the
 *     number 123, reported as a successful scan.
 *   - and every later scan returned PROVEN_ERR_EOF while the rest of the stream was
 *     still sitting in the pipe, unread, forever.
 *
 * Regular files hid it completely: a file read is short only at real EOF, so the whole
 * test suite passed. The bug lived exactly where the feature was supposed to live.
 */

/* A writer that sends `first`, pauses, then `second` - i.e. every pipe. */
typedef struct { wend_t fd; const char *first; const char *second; } split_writer_t;

static void *split_writer(void *arg) {
    split_writer_t *w = (split_writer_t *)arg;
    wend_write(w->fd, w->first, strlen(w->first));
    pause_ms(120);
    wend_write(w->fd, w->second, strlen(w->second));
    wend_close(w->fd);
    return NULL;
}

/* A writer that dribbles its message one byte at a time. */
typedef struct { wend_t fd; const char *msg; } byte_writer_t;

static void *byte_writer(void *arg) {
    byte_writer_t *w = (byte_writer_t *)arg;
    for (const char *p = w->msg; *p; ++p) {
        wend_write(w->fd, p, 1);
        pause_ms(1);
    }
    wend_close(w->fd);
    return NULL;
}

/* Scan one int from a pipe fed in two pieces. */
static proven_err_t scan_split(proven_allocator_t heap, const char *first, const char *second,
                               const char *fmt, proven_i32 *out) {
    proven_file_t pf = {0};
    wend_t wr;
    if (!make_pipe(&pf, &wr)) return PROVEN_ERR_IO;

    split_writer_t w = { .fd = wr, .first = first, .second = second };
    proven_sys_thread_t th = proven_sys_thread_create(split_writer, &w);

    proven_sysio_scanner_t sc;
    proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 64);
    if (proven_is_ok(e)) {
        e = proven_sysio_scanner_scan(&sc, fmt, PROVEN_SCAN_ARG(out));
        proven_sysio_scanner_deinit(&sc);
    }
    proven_sys_thread_join(th);
    rd_close(pf);
    return e;
}

static void *slow_writer(void *arg) {
    wend_t fd = *(wend_t *)arg;
    wend_write(fd, "123", 3);
    pause_ms(150);
    wend_write(fd, "456 789\n", 8);
    wend_close(fd);
    return NULL;
}

int main(void) {
    PROVEN_TEST_SUITE("the scanner over a pipe: a short read is not an end of input",
        "A token that straddles a read boundary must be scanned whole, and the stream must go on.",
        "Inspect scanner_fill in src/proven/sysio.c. Only a zero-byte read means the input ended - which is what read() itself has always meant by it.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a token split across two writes arrives whole",
        "The writer sends \"123\", pauses, then \"456 789\\n\". The first token is 123456, not 123.",
        "If this reads 123, scanner_fill latched eof on a short read and the scan committed a truncated token as a success.");
    // ---------------------------------------------------------------
    {
        proven_file_t pf = {0};
        static wend_t wfd;
        PROVEN_TEST_ASSERT(make_pipe(&pf, &wfd), "setup: a pipe", "");
        proven_sys_thread_t th = proven_sys_thread_create(slow_writer, &wfd);

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 64);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: the scanner", "");

        proven_i32 a = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&a));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && a == 123456,
            "a token split across two writes must scan as 123456",
            "It used to scan as 123: the pause between the writes made read() return 3 bytes, the scanner called that end-of-input, and the token was committed truncated - with PROVEN_OK.");

        proven_i32 b = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&b));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && b == 789,
            "and the rest of the stream must still be readable",
            "It used to return PROVEN_ERR_EOF with the data still in the pipe: eof was latched by the first short read and never cleared.");

        proven_i32 c = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&c));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_EOF,
            "and the real end of the stream is still an end of the stream",
            "A zero-byte read - the writer closed - is what an end of input actually is.");

        proven_sysio_scanner_deinit(&sc);
        proven_sys_thread_join(th);
        rd_close(pf);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a read that FAILS is an I/O error, not an end of input",
        "proven_sys_io_read_once reports a failed read() as {PROVEN_ERR_IO, 0}, and the old test for end-of-input accepted any zero-byte result.",
        "A stream that broke halfway through was indistinguishable from one that finished.");
    // ---------------------------------------------------------------
    {
        /* A write-only pipe end: every read on it fails (EBADF; ACCESS_DENIED on Windows). */
        proven_file_t rd = {0};
        wend_t wr;
        PROVEN_TEST_ASSERT(make_pipe(&rd, &wr), "setup: a pipe", "");
        rd_close(rd);

        proven_file_t pf = wend_as_file(wr);   /* the WRITE end - reading it is an error */

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 64);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: the scanner", "");

        proven_i32 v = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&v));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO,
            "a failed read must surface as PROVEN_ERR_IO",
            "It used to be PROVEN_ERR_EOF: the caller was told the stream ended cleanly when in fact it never opened.");

        proven_sysio_scanner_deinit(&sc);
        wend_close(wr);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a token cut in half by the read boundary is waited for, not rejected",
        "The scan engine works on a complete view, where \"the input ran out\" and \"the input is wrong\" are the same fact. Over a stream they are opposites.",
        "proven_scan_t::needs_more is how the engine says which one happened; scan_impl turns it into a refill. Without it, a minus sign that arrived before its digits was a malformed number.");
    // ---------------------------------------------------------------
    {
        proven_i32 v = -999;
        proven_err_t e = scan_split(heap, "-", "12 ", "{}", &v);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && v == -12,
            "a minus sign that arrives before its digits must scan as -12",
            "It used to be PROVEN_ERR_INVALID_ARG: a sign with nothing after it looked like garbage rather than like a number still on its way.");

        v = -999;
        e = scan_split(heap, "ke", "y=7 ", "key={}", &v);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && v == 7,
            "a literal split across the boundary must match once the rest arrives",
            "It used to be PROVEN_ERR_NOT_FOUND: the literal was not absent, it had not arrived.");

        v = -999;
        e = scan_split(heap, "12", "34 ", "{}", &v);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && v == 1234,
            "and digits split across the boundary still join up", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("input that is genuinely wrong is still wrong",
        "Waiting for more input must not become a way to wait forever on garbage.",
        "needs_more is set only when the parse ran off the END of the buffer. A non-digit sitting in front of the cursor is a malformed number no matter how much more arrives.");
    // ---------------------------------------------------------------
    {
        proven_i32 v = -999;
        proven_err_t e = scan_split(heap, "abc", "def ", "{}", &v);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG,
            "letters where a number was expected must fail, not block",
            "If this hangs, the scanner is refilling on a failure that more input cannot fix.");

        v = -999;
        e = scan_split(heap, "x=", "1 ", "key={}", &v);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND,
            "a literal that is present and WRONG must fail, not block", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a pipe delivering ONE BYTE at a time, into an 8-byte scanner buffer",
        "The worst case the refill loop can be given: every token crosses several read boundaries, and the buffer is barely bigger than the tokens.",
        "If this hangs, the refill loop has a state it cannot leave. If a value is wrong, a token was committed across a compaction that moved it.");
    // ---------------------------------------------------------------
    {
        proven_file_t pf = {0};
        static byte_writer_t bw;
        PROVEN_TEST_ASSERT(make_pipe(&pf, &bw.fd), "setup: a pipe", "");
        bw.msg = "  -12345 key=678   9.5\n";
        proven_sys_thread_t th = proven_sys_thread_create(byte_writer, &bw);

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 8);   /* barely a token wide */
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: an 8-byte scanner", "");

        proven_i32 a = 0, b = 0;
        double d = 0.0;

        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&a));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && a == -12345,
            "a six-character signed number arriving one byte at a time must scan whole", "");

        /* The leading space in the format is what skips the space in the input: a literal
         * matches exactly, like scanf's, and does not skip whitespace on its own. */
        e = proven_sysio_scanner_scan(&sc, " key={}", PROVEN_SCAN_ARG(&b));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && b == 678,
            "and a literal plus a number, also one byte at a time", "");

        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&d));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && d == 9.5,
            "and a float", "");

        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&a));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_EOF, "and then the stream ends", "");

        proven_sysio_scanner_deinit(&sc);
        proven_sys_thread_join(th);
        rd_close(pf);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a token too big for the buffer fails; it does not wait forever",
        "Refilling on \"the token is not complete yet\" must not become an infinite wait when the token can never fit.",
        "scanner_fill returns PROVEN_ERR_OUT_OF_BOUNDS once the buffer is full and the token still is not finished. The buffer is the caller's; size it for the input.");
    // ---------------------------------------------------------------
    {
        proven_file_t pf = {0};
        static split_writer_t w2;
        PROVEN_TEST_ASSERT(make_pipe(&pf, &w2.fd), "setup: a pipe", "");
        w2.first = "123456789012345678 ";
        w2.second = "42\n";
        proven_sys_thread_t th = proven_sys_thread_create(split_writer, &w2);

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: an 8-byte scanner", "");

        proven_i32 v = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&v));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS,
            "an 18-digit token in an 8-byte buffer must be an error",
            "If this test hangs instead, the refill loop is waiting for input that cannot help.");

        proven_sysio_scanner_deinit(&sc);
        proven_sys_thread_join(th);
        rd_close(pf);
    }

    PROVEN_TEST_PASS("the scanner reads pipes the way pipes actually behave.");
    return 0;
}
