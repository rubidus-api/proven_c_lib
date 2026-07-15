#include "proven.h"
#include "proven_test.h"

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("scanner over a pipe",
        "A short read is not an end of input.",
        "This check needs POSIX pipe() and pthreads; it is skipped on Windows.");
    PROVEN_TEST_INFO("POSIX-only; skipped on this platform");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>

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
typedef struct { int fd; const char *first; const char *second; } split_writer_t;

static void *split_writer(void *arg) {
    split_writer_t *w = (split_writer_t *)arg;
    ssize_t n = write(w->fd, w->first, strlen(w->first));
    (void)n;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 120 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    n = write(w->fd, w->second, strlen(w->second));
    (void)n;
    close(w->fd);
    return NULL;
}

/* A writer that dribbles its message one byte at a time. */
typedef struct { int fd; const char *msg; } byte_writer_t;

static void *byte_writer(void *arg) {
    byte_writer_t *w = (byte_writer_t *)arg;
    for (const char *p = w->msg; *p; ++p) {
        ssize_t n = write(w->fd, p, 1);
        (void)n;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 };  /* 0.2 ms */
        nanosleep(&ts, NULL);
    }
    close(w->fd);
    return NULL;
}

/* Scan one int from a pipe fed in two pieces. */
static proven_err_t scan_split(proven_allocator_t heap, const char *first, const char *second,
                               const char *fmt, proven_i32 *out) {
    int fds[2];
    if (pipe(fds) != 0) return PROVEN_ERR_IO;

    split_writer_t w = { .fd = fds[1], .first = first, .second = second };
    pthread_t th;
    if (pthread_create(&th, NULL, split_writer, &w) != 0) { close(fds[0]); close(fds[1]); return PROVEN_ERR_IO; }

    proven_file_t pf = {0};
    pf.internal.fd = fds[0];

    proven_sysio_scanner_t sc;
    proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 64);
    if (proven_is_ok(e)) {
        e = proven_sysio_scanner_scan(&sc, fmt, PROVEN_SCAN_ARG(out));
        proven_sysio_scanner_deinit(&sc);
    }
    (void)pthread_join(th, NULL);
    close(fds[0]);
    return e;
}

static void *slow_writer(void *arg) {
    int fd = *(int *)arg;
    ssize_t n = write(fd, "123", 3);
    (void)n;

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 150 * 1000 * 1000 };  /* 150 ms */
    nanosleep(&ts, NULL);

    n = write(fd, "456 789\n", 8);
    (void)n;
    close(fd);
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
        int fds[2];
        PROVEN_TEST_ASSERT(pipe(fds) == 0, "setup: a pipe", "");

        pthread_t th;
        int wfd = fds[1];
        PROVEN_TEST_ASSERT(pthread_create(&th, NULL, slow_writer, &wfd) == 0, "setup: a writer thread", "");

        proven_file_t pf = {0};
        pf.internal.fd = fds[0];

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
        (void)pthread_join(th, NULL);
        close(fds[0]);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a read that FAILS is an I/O error, not an end of input",
        "proven_sys_io_read_once reports a failed read() as {PROVEN_ERR_IO, 0}, and the old test for end-of-input accepted any zero-byte result.",
        "A stream that broke halfway through was indistinguishable from one that finished.");
    // ---------------------------------------------------------------
    {
        /* A write-only pipe end: every read() on it fails with EBADF. */
        int fds[2];
        PROVEN_TEST_ASSERT(pipe(fds) == 0, "setup: a pipe", "");
        close(fds[0]);

        proven_file_t pf = {0};
        pf.internal.fd = fds[1];   /* the WRITE end - reading it is an error */

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 64);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: the scanner", "");

        proven_i32 v = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&v));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO,
            "a failed read must surface as PROVEN_ERR_IO",
            "It used to be PROVEN_ERR_EOF: the caller was told the stream ended cleanly when in fact it never opened.");

        proven_sysio_scanner_deinit(&sc);
        close(fds[1]);
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
        int fds[2];
        PROVEN_TEST_ASSERT(pipe(fds) == 0, "setup: a pipe", "");

        pthread_t th;
        static const char *msg = "  -12345 key=678   9.5\n";
        static byte_writer_t bw;
        bw.fd = fds[1];
        bw.msg = msg;
        PROVEN_TEST_ASSERT(pthread_create(&th, NULL, byte_writer, &bw) == 0, "setup: a dribbling writer", "");

        proven_file_t pf = {0};
        pf.internal.fd = fds[0];

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
        (void)pthread_join(th, NULL);
        close(fds[0]);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a token too big for the buffer fails; it does not wait forever",
        "Refilling on \"the token is not complete yet\" must not become an infinite wait when the token can never fit.",
        "scanner_fill returns PROVEN_ERR_OUT_OF_BOUNDS once the buffer is full and the token still is not finished. The buffer is the caller's; size it for the input.");
    // ---------------------------------------------------------------
    {
        int fds[2];
        PROVEN_TEST_ASSERT(pipe(fds) == 0, "setup: a pipe", "");

        pthread_t th;
        static split_writer_t w2;
        w2.fd = fds[1];
        w2.first = "123456789012345678 ";
        w2.second = "42\n";
        PROVEN_TEST_ASSERT(pthread_create(&th, NULL, split_writer, &w2) == 0, "setup: a writer", "");

        proven_file_t pf = {0};
        pf.internal.fd = fds[0];

        proven_sysio_scanner_t sc;
        proven_err_t e = proven_sysio_scanner_init(&sc, pf, heap, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "setup: an 8-byte scanner", "");

        proven_i32 v = -1;
        e = proven_sysio_scanner_scan(&sc, "{}", PROVEN_SCAN_ARG(&v));
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS,
            "an 18-digit token in an 8-byte buffer must be an error",
            "If this test hangs instead, the refill loop is waiting for input that cannot help.");

        proven_sysio_scanner_deinit(&sc);
        (void)pthread_join(th, NULL);
        close(fds[0]);
    }

    PROVEN_TEST_PASS("the scanner reads pipes the way pipes actually behave.");
    return 0;
}
#endif
