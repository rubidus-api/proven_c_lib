#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_console.h"
#include <string.h>

/*
 * The Windows console edge (docs/BACKLOG.md B-039), run against a FAKE console so that it runs
 * on every host. A console takes and gives UTF-16 code units; the program writes and reads
 * UTF-8 in pieces that ignore character boundaries. This test drives the conversion in
 * src/proven/proven_internal_console.h through every way those pieces can fall:
 *
 *   - a text written as two writes split at EVERY byte offset, one byte at a time, and through
 *     a buffered writer whose 5-byte buffer flushes mid-character;
 *   - a console that hands out 1, 2 or 3 units per read, read into destinations of 1 to 10
 *     bytes, so that characters straddle both kinds of boundary.
 *
 * What the real console does - that WriteConsoleW shows Hangul on a code-page-949 console, and
 * that ReadConsoleW returns what a person typed - only a Windows run can say; see
 * docs/b039-console-check.c.
 */

typedef struct {
    proven_u16 out[256];
    proven_size_t out_len;
    proven_size_t fail_after;      /* write_units fails once this many units are out */
    const proven_u16 *in;
    proven_size_t in_len, in_at, chunk;
} fake_console_t;

static proven_result_size_t fake_write(void *ctx, const proven_u16 *u, proven_size_t n) {
    fake_console_t *f = ctx;
    proven_size_t i = 0;
    for (; i < n; ++i) {
        if (f->out_len >= f->fail_after) return (proven_result_size_t){ PROVEN_ERR_IO, i };
        f->out[f->out_len++] = u[i];
    }
    return (proven_result_size_t){ PROVEN_OK, n };
}

static proven_result_size_t fake_read(void *ctx, proven_u16 *u, proven_size_t cap) {
    fake_console_t *f = ctx;
    if (f->in_at >= f->in_len) return (proven_result_size_t){ PROVEN_ERR_EOF, 0 };
    proven_size_t n = f->in_len - f->in_at;
    if (n > cap) n = cap;
    if (n > f->chunk) n = f->chunk;
    for (proven_size_t i = 0; i < n; ++i) u[i] = f->in[f->in_at + i];
    f->in_at += n;
    return (proven_result_size_t){ PROVEN_OK, n };
}

static proven_console_io_t io_of(fake_console_t *f) {
    return (proven_console_io_t){ f, fake_write, fake_read };
}

/* "A", U+AC00, U+1F600, "b", CR LF */
static const char TEXT8[] = "A\xEA\xB0\x80\xF0\x9F\x98\x80" "b\r\n";
static const proven_u16 TEXT16[] = { 'A', 0xAC00, 0xD83D, 0xDE00, 'b', '\r', '\n' };
#define TEXT8_N (sizeof TEXT8 - 1)
#define TEXT16_N (sizeof TEXT16 / sizeof TEXT16[0])

/* A writer over the fake console, as sysio.c builds one over the real console. */
typedef struct { fake_console_t *f; proven_sysio_carry_t carry; } fake_std_t;
static proven_result_size_t fake_std_write(void *ctx, proven_mem_view_t chunk) {
    fake_std_t *s = ctx;
    return proven_console_write_utf8(io_of(s->f), &s->carry, chunk.ptr, chunk.size);
}
static proven_err_t fake_std_flush(void *ctx) {
    fake_std_t *s = ctx;
    return proven_console_finish(&s->carry);
}

static proven_result_size_t fake_std_read(void *ctx, proven_mem_mut_t dest) {
    fake_std_t *s = ctx;
    return proven_console_read_utf8(io_of(s->f), &s->carry, dest.ptr, dest.size);
}

static const proven_byte_t *B(const char *s) { return (const proven_byte_t *)s; }

int main(void) {
    PROVEN_TEST_SUITE("UTF-8 text through a UTF-16 console",
        "Text written in pieces reaches the console as exactly the right UTF-16 wherever the pieces are cut; console input comes back as exactly the right UTF-8 whatever the read sizes; malformed text is refused after the valid text before it.",
        "Inspect src/proven/proven_internal_console.h. A failure at one split offset is the write carry; at one destination size, the read carry.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a write split at every byte offset",
        "Two writes, cut at each of the 13 offsets, and 13 one-byte writes: the console receives the same seven units every time.",
        "");
    // ---------------------------------------------------------------
    for (proven_size_t cut = 0; cut <= TEXT8_N; ++cut) {
        fake_console_t f = { .fail_after = 1000 };
        proven_sysio_carry_t c = {0};
        proven_result_size_t a = proven_console_write_utf8(io_of(&f), &c, B(TEXT8), cut);
        proven_result_size_t b = proven_console_write_utf8(io_of(&f), &c, B(TEXT8) + cut, TEXT8_N - cut);
        PROVEN_TEST_ASSERT(proven_is_ok(a.err) && a.value == cut && proven_is_ok(b.err) && b.value == TEXT8_N - cut,
            "both halves are consumed whole", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_console_finish(&c)), "nothing is left open", "");
        PROVEN_TEST_ASSERT(f.out_len == TEXT16_N && memcmp(f.out, TEXT16, sizeof TEXT16) == 0,
            "the console receives the text exactly", "");
    }
    {
        fake_console_t f = { .fail_after = 1000 };
        proven_sysio_carry_t c = {0};
        for (proven_size_t i = 0; i < TEXT8_N; ++i) {
            proven_result_size_t r = proven_console_write_utf8(io_of(&f), &c, B(TEXT8) + i, 1);
            PROVEN_TEST_ASSERT(proven_is_ok(r.err) && r.value == 1, "each byte is taken", "");
        }
        PROVEN_TEST_ASSERT(f.out_len == TEXT16_N && memcmp(f.out, TEXT16, sizeof TEXT16) == 0,
            "one byte at a time gives the same text", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a buffered writer flushing mid-character",
        "A 5-byte buffer over the console writer flushes inside the Hangul syllable and inside the emoji; the carry completes both.",
        "This is the shape proven_sysio_stdout_buffered produces on a Windows console.");
    // ---------------------------------------------------------------
    {
        fake_console_t f = { .fail_after = 1000 };
        fake_std_t s = { .f = &f };
        proven_writer_buffered_t bs;
        proven_byte_t buf[5];
        proven_writer_t w = proven_writer_buffered(&bs, (proven_writer_t){ &s, fake_std_write, fake_std_flush },
                                                   (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write(w, (proven_mem_view_t){ B(TEXT8), TEXT8_N })), "write", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_flush(w)), "flush", "");
        PROVEN_TEST_ASSERT(f.out_len == TEXT16_N && memcmp(f.out, TEXT16, sizeof TEXT16) == 0,
            "the console receives the text exactly", "");

        /* Chapter 5: "a buffered writer whose text ends inside a character reports it at
         * proven_writer_flush" - the buffered flush must reach the console writer's finish. */
        fake_console_t g = { .fail_after = 1000 };
        fake_std_t t = { .f = &g };
        proven_writer_buffered_t bt;
        proven_writer_t wt = proven_writer_buffered(&bt, (proven_writer_t){ &t, fake_std_write, fake_std_flush },
                                                    (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write(wt, (proven_mem_view_t){ B("ok\xEA\xB0"), 4 })), "write", "");
        PROVEN_TEST_ASSERT(proven_writer_flush(wt) == PROVEN_ERR_INVALID_ENCODING && g.out_len == 2,
            "a text ending inside a character is reported by the flush, after 'ok' went out", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("malformed and unfinished text is refused, after the valid part",
        "Bad UTF-8 stops the write where it starts; a character still open when the text ends is reported by finish (the flush); a carried character completed by a wrong byte is malformed.",
        "");
    // ---------------------------------------------------------------
    {
        fake_console_t f = { .fail_after = 1000 };
        proven_sysio_carry_t c = {0};
        proven_result_size_t r = proven_console_write_utf8(io_of(&f), &c, B("ok\xFFzz"), 5);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ENCODING && r.value == 2 && f.out_len == 2,
            "'ok' goes out, the bad byte stops the write", "");

        f.out_len = 0;
        r = proven_console_write_utf8(io_of(&f), &c, B("ok\xEA\xB0"), 4);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && r.value == 4 && f.out_len == 2, "an open character is carried", "");
        PROVEN_TEST_ASSERT(proven_console_finish(&c) == PROVEN_ERR_INVALID_ENCODING, "and refused if the text ends there", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_console_finish(&c)), "once", "");

        r = proven_console_write_utf8(io_of(&f), &c, B("\xEA"), 1);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "a lead byte is carried", "");
        r = proven_console_write_utf8(io_of(&f), &c, B("A"), 1);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_ENCODING, "and an ASCII byte cannot complete it", "");

        /* The console itself fails after three units: the report counts the bytes those
         * units came from - 'A' and the syllable, four bytes - not the units. */
        fake_console_t g = { .fail_after = 3 };
        proven_sysio_carry_t c2 = {0};
        r = proven_console_write_utf8(io_of(&g), &c2, B(TEXT8), TEXT8_N);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_IO && r.value == 4,
            "a failed console write reports the input bytes that did go out", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("reading, whatever the read sizes",
        "A console handing out 1, 2 or 3 units per read, into destinations of 1 to 10 bytes: the bytes read are exactly the UTF-8 of what was typed.",
        "A failure at chunk 1 is the pending high surrogate; at small destinations, the carry of converted bytes.");
    // ---------------------------------------------------------------
    for (proven_size_t chunk = 1; chunk <= 3; ++chunk) {
        for (proven_size_t cap = 1; cap <= 10; ++cap) {
            fake_console_t f = { .in = TEXT16, .in_len = TEXT16_N, .chunk = chunk };
            proven_sysio_carry_t c = {0};
            proven_byte_t got[64];
            proven_size_t n = 0;
            for (int guard = 0; guard < 200; ++guard) {
                proven_result_size_t r = proven_console_read_utf8(io_of(&f), &c, got + n, cap);
                if (r.err == PROVEN_ERR_EOF) break;
                PROVEN_TEST_ASSERT(proven_is_ok(r.err) && r.value > 0 && r.value <= cap, "each read returns something that fits", "");
                n += r.value;
            }
            if (n != TEXT8_N || memcmp(got, TEXT8, n) != 0) {
                PROVEN_TEST_INFO("chunk {} cap {} read {} bytes", PROVEN_ARG(chunk), PROVEN_ARG(cap), PROVEN_ARG(n));
                PROVEN_TEST_ASSERT(false, "the bytes read must be the UTF-8 of the console text", "");
            }
        }
    }
    {
        /* Through the line reader, as proven_sysio_stdin_lines builds it: the '\r' a console
         * line ends with is removed, and the line is the UTF-8 of what was typed. */
        fake_console_t f = { .in = TEXT16, .in_len = TEXT16_N, .chunk = 2 };
        fake_std_t s = { .f = &f };
        proven_reader_buffered_t rb;
        proven_byte_t lb[32];
        proven_reader_t r = proven_reader_buffered(&rb, (proven_reader_t){ &s, fake_std_read }, (proven_mem_mut_t){ lb, sizeof lb });
        PROVEN_TEST_ASSERT(proven_reader_is_valid(r), "line reader", "");
        proven_result_u8str_view_t line = proven_reader_read_line(&rb);
        PROVEN_TEST_ASSERT(proven_is_ok(line.err) && line.val.size == TEXT8_N - 2 && memcmp(line.val.ptr, TEXT8, TEXT8_N - 2) == 0,
            "one line, CR LF removed", "");
        PROVEN_TEST_ASSERT(proven_reader_read_line(&rb).err == PROVEN_ERR_EOF, "then EOF", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("console input that is over, or broken",
        "Ctrl+Z as the first unit of a read is end of input. A lone low surrogate, and a high surrogate followed by the end, are malformed - after the valid text before them.",
        "");
    // ---------------------------------------------------------------
    {
        const proven_u16 ctrlz[] = { 0x1A, '\r', '\n' };
        fake_console_t f = { .in = ctrlz, .in_len = 3, .chunk = 8 };
        proven_sysio_carry_t c = {0};
        proven_byte_t got[16];
        PROVEN_TEST_ASSERT(proven_console_read_utf8(io_of(&f), &c, got, sizeof got).err == PROVEN_ERR_EOF, "Ctrl+Z ends input", "");

        const proven_u16 lone[] = { 'o', 'k', 0xDC00, 'x' };
        fake_console_t g = { .in = lone, .in_len = 4, .chunk = 8 };
        proven_sysio_carry_t c2 = {0};
        proven_result_size_t r = proven_console_read_utf8(io_of(&g), &c2, got, sizeof got);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && r.value == 2 && got[0] == 'o', "'ok' is delivered first", "");
        PROVEN_TEST_ASSERT(proven_console_read_utf8(io_of(&g), &c2, got, sizeof got).err == PROVEN_ERR_INVALID_ENCODING,
            "then the lone low surrogate is refused", "");
        PROVEN_TEST_ASSERT(proven_console_read_utf8(io_of(&g), &c2, got, sizeof got).err == PROVEN_ERR_INVALID_ENCODING,
            "and stays refused", "");

        const proven_u16 hi_end[] = { 'o', 0xD83D };
        fake_console_t h = { .in = hi_end, .in_len = 2, .chunk = 8 };
        proven_sysio_carry_t c3 = {0};
        r = proven_console_read_utf8(io_of(&h), &c3, got, sizeof got);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && r.value == 1, "'o' first", "");
        PROVEN_TEST_ASSERT(proven_console_read_utf8(io_of(&h), &c3, got, sizeof got).err == PROVEN_ERR_INVALID_ENCODING,
            "a high surrogate and then the end of input is half a character", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("off Windows, nothing is a console",
        "The standard streams are never consoles on POSIX, so every sysio path stays byte-exact there.",
        "");
    // ---------------------------------------------------------------
#if !defined(_WIN32) && !defined(_WIN64)
    {
        proven_sysio_std_t st;
        (void)proven_sysio_stdout_writer(&st);
        PROVEN_TEST_ASSERT(!st.console, "stdout is not a console on POSIX", "");
        (void)proven_sysio_stdin_reader(&st);
        PROVEN_TEST_ASSERT(!st.console, "nor is stdin", "");
    }
#endif

    PROVEN_TEST_PASS("UTF-8 text through a UTF-16 console");
    return 0;
}
