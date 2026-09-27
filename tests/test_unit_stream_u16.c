#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the contracts in stream.h, fmt.h and sysio.h for UTF-16 text (docs/BACKLOG.md
 * B-039). Output: u16 text reaches every sink the formatter reaches, and a writer in any of the
 * three encodings, byte-exact, with malformed text writing nothing. Input: the u16 reader
 * decodes all three encodings, carries a character split across reads of the source - checked
 * with a source that hands out ONE byte per read, the worst case a pipe can produce - and keeps
 * proven_reader_read_line's rules for newlines, full buffers and the last line.
 */

static proven_u16str_view_t u16v(const proven_u16 *p, proven_size_t n) {
    return (proven_u16str_view_t){ p, n };
}

/* A source that returns one byte per read. */
typedef struct { const proven_byte_t *p; proven_size_t n, at; } drip_t;
static proven_result_size_t drip_read(void *ctx, proven_mem_mut_t dest) {
    drip_t *d = ctx;
    if (d->at >= d->n) return (proven_result_size_t){ PROVEN_ERR_EOF, 0 };
    if (dest.size == 0) return (proven_result_size_t){ PROVEN_OK, 0 };
    dest.ptr[0] = d->p[d->at++];
    return (proven_result_size_t){ PROVEN_OK, 1 };
}

static bool line_is(proven_result_u16str_view_t r, const proven_u16 *want, proven_size_t n) {
    return proven_is_ok(r.err) && r.val.size == n && (n == 0 || memcmp(r.val.ptr, want, n * sizeof(proven_u16)) == 0);
}

/* U+AC00 CR LF, "A" U+1F600 LF, U+B05D - Hangul, CRLF, a surrogate pair, a final line with no newline. */
static const proven_u16 L1[] = { 0xAC00 };
static const proven_u16 L2[] = { 'A', 0xD83D, 0xDE00 };
static const proven_u16 L3[] = { 0xB05D };
static const proven_u16 TEXT[] = { 0xAC00, '\r', '\n', 'A', 0xD83D, 0xDE00, '\n', 0xB05D };
#define TEXT_N (sizeof TEXT / sizeof TEXT[0])

static proven_size_t encode(proven_text_encoding_t enc, proven_byte_t *out, proven_size_t cap, bool bom) {
    proven_writer_buf_t wb = { .buf = { out, cap } };
    proven_writer_t w = proven_writer_from_buffer(&wb);
    if (bom) PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_bom(w, enc)), "BOM writes", "");
    PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_u16(w, u16v(TEXT, TEXT_N), enc)), "text writes", "");
    return wb.len;
}

static void expect_three_lines(proven_u16_reader_t *r) {
    PROVEN_TEST_ASSERT(line_is(proven_u16_reader_read_line(r), L1, 1), "line 1 is the Hangul syllable, CR removed", "");
    PROVEN_TEST_ASSERT(line_is(proven_u16_reader_read_line(r), L2, 3), "line 2 keeps the surrogate pair whole", "");
    PROVEN_TEST_ASSERT(line_is(proven_u16_reader_read_line(r), L3, 1), "a final line with no newline is still a line", "");
    PROVEN_TEST_ASSERT(proven_u16_reader_read_line(r).err == PROVEN_ERR_EOF, "then EOF", "");
    PROVEN_TEST_ASSERT(proven_u16_reader_read_line(r).err == PROVEN_ERR_EOF, "and EOF again", "");
}

int main(void) {
    PROVEN_TEST_SUITE("UTF-16 text through writers, readers and the formatter",
        "u16 text is written byte-exact in UTF-8, UTF-16LE and UTF-16BE, read back line by line from all three with a character split across source reads carried, and formatted into any sink; malformed text is refused everywhere.",
        "Inspect the UTF-16 section of src/proven/stream.c and render_u16 in src/proven/fmt.c.");

    proven_allocator_t heap = proven_heap_allocator();
    proven_byte_t bytes[256];
    proven_u16 lbuf[16];

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the formatter takes u16 text wherever it takes u8 text",
        "PROVEN_ARG on a proven_u16str_view_t renders UTF-8, width counts UTF-8 bytes as it does for a u8 view, and an unpaired surrogate fails the format.",
        "");
    // ---------------------------------------------------------------
    {
        proven_result_u8str_t r = proven_u8str_create(heap, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "setup", "");
        proven_u8str_t s = r.value;
        proven_u16str_view_t han = u16v(L1, 1);
        proven_fmt_result_t f = proven_u8str_append_fmt_grow(heap, &s, "[{}|{:*>5}]", PROVEN_ARG(han), PROVEN_ARG(u16v(L2, 3)));
        PROVEN_TEST_ASSERT(proven_is_ok(f.err), "formatting u16 views succeeds", "");
        const char *want = "[\xEA\xB0\x80|A\xF0\x9F\x98\x80]";   /* 'A' + 4-byte emoji = 5 bytes: no padding */
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&s), want) == 0, "the u16 text comes out as its UTF-8", "");

        PROVEN_TEST_ASSERT(proven_is_ok(proven_u8str_reset(&s)), "reset", "");
        f = proven_u8str_append_fmt_grow(heap, &s, "{:-<5}|", PROVEN_ARG(han));
        PROVEN_TEST_ASSERT(proven_is_ok(f.err) && strcmp(proven_u8str_as_cstr(&s), "\xEA\xB0\x80--|") == 0,
            "width 5 around a 3-byte syllable pads 2: width is in UTF-8 bytes, as for a u8 view", "");

        const proven_u16 lone[] = { 'x', 0xDC00 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u8str_reset(&s)), "reset", "");
        f = proven_u8str_append_fmt_grow(heap, &s, "a{}b", PROVEN_ARG(u16v(lone, 2)));
        PROVEN_TEST_ASSERT(f.err == PROVEN_ERR_INVALID_ENCODING, "an unpaired surrogate fails the format", "");
        proven_u8str_destroy(heap, &s);

        proven_writer_buf_t wb = { .buf = { bytes, sizeof bytes } };
        proven_writer_t w = proven_writer_from_buffer(&wb);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fprintln(w, "u16: {}", PROVEN_ARG(han)).err), "fprintln takes it", "");
        PROVEN_TEST_ASSERT(wb.len == 9 && memcmp(bytes, "u16: \xEA\xB0\x80\n", 9) == 0, "into any writer", "");

        /* The console path: exercised for real in the Windows check, here only for its result. */
        PROVEN_TEST_ASSERT(proven_is_ok(proven_println("stdout takes u16 too: {}", PROVEN_ARG(han))), "println takes it", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a writer writes u16 text byte-exact in each encoding",
        "UTF-8 bytes, UTF-16LE and UTF-16BE code units in their byte order regardless of the host's, and the three byte order marks on request.",
        "");
    // ---------------------------------------------------------------
    {
        proven_size_t n = encode(PROVEN_TEXT_UTF16LE, bytes, sizeof bytes, true);
        PROVEN_TEST_ASSERT(n == 2 + 2 * TEXT_N && bytes[0] == 0xFF && bytes[1] == 0xFE &&
                           bytes[2] == 0x00 && bytes[3] == 0xAC && bytes[10] == 0x3D && bytes[11] == 0xD8,
            "UTF-16LE: BOM FF FE, then low byte first", "");
        n = encode(PROVEN_TEXT_UTF16BE, bytes, sizeof bytes, true);
        PROVEN_TEST_ASSERT(n == 2 + 2 * TEXT_N && bytes[0] == 0xFE && bytes[1] == 0xFF &&
                           bytes[2] == 0xAC && bytes[3] == 0x00 && bytes[10] == 0xD8 && bytes[11] == 0x3D,
            "UTF-16BE: BOM FE FF, then high byte first", "");
        n = encode(PROVEN_TEXT_UTF8, bytes, sizeof bytes, false);
        const char *want = "\xEA\xB0\x80\r\nA\xF0\x9F\x98\x80\n\xEB\x81\x9D";
        PROVEN_TEST_ASSERT(n == strlen(want) && memcmp(bytes, want, n) == 0, "UTF-8 with no BOM unless asked", "");
        n = encode(PROVEN_TEXT_UTF8, bytes, sizeof bytes, true);
        PROVEN_TEST_ASSERT(bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF, "the UTF-8 BOM on request", "");

        proven_writer_buf_t wb = { .buf = { bytes, sizeof bytes } };
        proven_writer_t w = proven_writer_from_buffer(&wb);
        const proven_u16 bad[] = { 'o', 'k', 0xD83D };
        PROVEN_TEST_ASSERT(proven_writer_write_u16(w, u16v(bad, 3), PROVEN_TEXT_UTF16LE) == PROVEN_ERR_INVALID_ENCODING &&
                           wb.len == 0, "malformed text writes nothing, not even its valid start", "");
        PROVEN_TEST_ASSERT(proven_writer_write_u16(w, u16v(bad, 2), PROVEN_TEXT_AUTO) == PROVEN_ERR_INVALID_ARG,
            "AUTO is for readers", "");
        PROVEN_TEST_ASSERT(proven_writer_write_bom(w, PROVEN_TEXT_AUTO) == PROVEN_ERR_INVALID_ARG, "and has no BOM", "");

        /* A long text crosses the writer's internal chunk in every encoding. */
        static proven_u16 longtext[700];
        for (size_t i = 0; i < 700; i += 2) { longtext[i] = 0xD83D; longtext[i + 1] = 0xDE00; }
        static proven_byte_t big[4096];
        for (int e = 0; e < 3; ++e) {
            proven_writer_buf_t wl = { .buf = { big, sizeof big } };
            proven_err_t err = proven_writer_write_u16(proven_writer_from_buffer(&wl), u16v(longtext, 700), (proven_text_encoding_t)e);
            PROVEN_TEST_ASSERT(proven_is_ok(err) && wl.len == 1400, "350 emoji are 1400 bytes in every encoding", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the reader decodes all three encodings, one byte per read",
        "Every character arrives split across source reads; each is carried until complete. AUTO finds each BOM, and no BOM means UTF-8.",
        "A failure only with the drip source is the carry between reads.");
    // ---------------------------------------------------------------
    for (int e = 0; e < 3; ++e) {
        for (int mode = 0; mode < 2; ++mode) {           /* explicit, then AUTO with a BOM */
            proven_size_t n = encode((proven_text_encoding_t)e, bytes, sizeof bytes, mode == 1);
            drip_t d = { bytes, n, 0 };
            proven_u16_reader_t r;
            proven_text_encoding_t open_as = mode == 1 ? PROVEN_TEXT_AUTO : (proven_text_encoding_t)e;
            PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, (proven_reader_t){ &d, drip_read }, open_as, lbuf, 16)),
                "init", "");
            expect_three_lines(&r);
            PROVEN_TEST_ASSERT(r.enc == (proven_text_encoding_t)e, "AUTO resolves to the BOM's encoding", "");
        }
    }
    {
        proven_size_t n = encode(PROVEN_TEXT_UTF8, bytes, sizeof bytes, false);
        proven_reader_view_t rv;
        proven_u16_reader_t r;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ bytes, n }),
                                                               PROVEN_TEXT_AUTO, lbuf, 16)), "init", "");
        expect_three_lines(&r);
        PROVEN_TEST_ASSERT(r.enc == PROVEN_TEXT_UTF8, "no BOM: AUTO reads UTF-8", "");

        n = encode(PROVEN_TEXT_UTF16LE, bytes, sizeof bytes, true);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ bytes, n }),
                                                               PROVEN_TEXT_UTF16LE, lbuf, 16)), "init", "");
        proven_result_u16str_view_t l = proven_u16_reader_read_line(&r);
        PROVEN_TEST_ASSERT(proven_is_ok(l.err) && l.val.size == 2 && l.val.ptr[0] == 0xFEFF,
            "an explicit encoding delivers a BOM as the character U+FEFF", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("full buffers, long lines and pairs at the edge",
        "A line that exactly fills the buffer is a line; one unit longer is OUT_OF_BOUNDS and stays so; a surrogate pair that would straddle the end of the buffer is not split.",
        "Compare with the buffer-full branch of proven_reader_read_line: the rules are the same.");
    // ---------------------------------------------------------------
    {
        proven_reader_view_t rv;
        proven_u16_reader_t r;
        proven_u16 small[4];
        const char *exact = "abcd\nefgh";
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)exact, 9 }),
                                                               PROVEN_TEXT_UTF8, small, 4)), "init", "");
        proven_result_u16str_view_t l = proven_u16_reader_read_line(&r);
        PROVEN_TEST_ASSERT(proven_is_ok(l.err) && l.val.size == 4 && l.val.ptr[3] == 'd', "a 4-unit line fits a 4-unit buffer", "");
        l = proven_u16_reader_read_line(&r);
        PROVEN_TEST_ASSERT(proven_is_ok(l.err) && l.val.size == 4 && l.val.ptr[0] == 'e', "and so does a final one with no newline", "");
        PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_EOF, "EOF", "");

        const char *longer = "abcde\n";
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)longer, 6 }),
                                                               PROVEN_TEXT_UTF8, small, 4)), "init", "");
        PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_OUT_OF_BOUNDS, "5 units do not fit 4", "");
        PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_OUT_OF_BOUNDS, "and the reader stays on that line", "");

        const char *pair_edge = "abc\xF0\x9F\x98\x80\n";   /* 3 units, then a pair: 5 units */
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)pair_edge, 8 }),
                                                               PROVEN_TEXT_UTF8, small, 4)), "init", "");
        PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_OUT_OF_BOUNDS,
            "a pair that would need units 4 and 5 makes the line too long; it is not cut in half", "");

        /* read() in chunks of 2 over a pair-heavy text returns whole pairs only. */
        const char *emoji3 = "x\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
        PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)emoji3, 9 }),
                                                               PROVEN_TEXT_UTF8, lbuf, 16)), "init", "");
        proven_u16 two[2];
        proven_size_t total = 0;
        for (;;) {
            proven_result_size_t rr = proven_u16_reader_read(&r, two, 2);
            if (rr.err == PROVEN_ERR_EOF) break;
            PROVEN_TEST_ASSERT(proven_is_ok(rr.err) && rr.value > 0, "read makes progress", "");
            PROVEN_TEST_ASSERT(!(two[rr.value - 1] >= 0xD800 && two[rr.value - 1] <= 0xDBFF), "and never ends on a high surrogate", "");
            total += rr.value;
        }
        PROVEN_TEST_ASSERT(total == 5, "1 + 2 + 2 units in all", "");
        PROVEN_TEST_ASSERT(proven_u16_reader_read(&r, two, 1).err == PROVEN_ERR_INVALID_ARG, "a 1-unit destination is refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("malformed input stops the reader after the valid text before it",
        "Bad UTF-8, a lone surrogate in UTF-16, an odd trailing byte, and a source that ends mid-character: each INVALID_ENCODING, sticky.",
        "");
    // ---------------------------------------------------------------
    {
        struct { const char *b; proven_size_t n; proven_text_encoding_t enc; proven_size_t good; } cases[] = {
            { "ok\n\xC0\x80\n", 6, PROVEN_TEXT_UTF8, 2 },
            { "ok\n\xED\x95", 5, PROVEN_TEXT_UTF8, 2 },
            { "o\0k\0\n\0\x00\xDC\n\0", 10, PROVEN_TEXT_UTF16LE, 2 },
            { "o\0k\0\n\0\x41", 7, PROVEN_TEXT_UTF16LE, 2 },
            { "\0o\0k\0\n\xD8\x3D", 8, PROVEN_TEXT_UTF16BE, 2 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            drip_t d = { (const proven_byte_t *)cases[i].b, cases[i].n, 0 };
            proven_u16_reader_t r;
            PROVEN_TEST_ASSERT(proven_is_ok(proven_u16_reader_init(&r, (proven_reader_t){ &d, drip_read }, cases[i].enc, lbuf, 16)), "init", "");
            proven_result_u16str_view_t l = proven_u16_reader_read_line(&r);
            PROVEN_TEST_ASSERT(proven_is_ok(l.err) && l.val.size == cases[i].good && l.val.ptr[0] == 'o',
                "the valid first line is delivered", "");
            PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_INVALID_ENCODING, "then the reader refuses", "");
            PROVEN_TEST_ASSERT(proven_u16_reader_read_line(&r).err == PROVEN_ERR_INVALID_ENCODING, "and keeps refusing", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a file round trip through sysio, in each encoding",
        "Written with proven_writer_write_u16 through a file writer, read back with proven_sysio_u16_lines_open(AUTO).",
        "");
    // ---------------------------------------------------------------
    {
        proven_u8str_view_t path = PROVEN_LIT("build/test_unit_stream_u16.tmp");
        for (int e = 0; e < 3; ++e) {
            proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
            PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open for writing", "");
            proven_writer_t w = proven_writer_from_file(&f.value);
            PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_bom(w, (proven_text_encoding_t)e)), "BOM", "");
            PROVEN_TEST_ASSERT(proven_is_ok(proven_writer_write_u16(w, u16v(TEXT, TEXT_N), (proven_text_encoding_t)e)), "text", "");
            PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_close(f.value)), "close", "");

            f = proven_fs_open(heap, path, PROVEN_FS_READ);
            PROVEN_TEST_ASSERT(proven_is_ok(f.err), "open for reading", "");
            proven_sysio_u16_lines_t lines;
            PROVEN_TEST_ASSERT(proven_is_ok(proven_sysio_u16_lines_open(&lines, f.value, PROVEN_TEXT_AUTO, lbuf, 16)), "open lines", "");
            proven_sysio_u16_lines_t moved = lines;   /* the wrapper re-binds, so moving it is allowed */
            PROVEN_TEST_ASSERT(line_is(proven_sysio_read_u16_line(&moved), L1, 1), "line 1", "");
            PROVEN_TEST_ASSERT(line_is(proven_sysio_read_u16_line(&moved), L2, 3), "line 2", "");
            PROVEN_TEST_ASSERT(line_is(proven_sysio_read_u16_line(&moved), L3, 1), "line 3", "");
            PROVEN_TEST_ASSERT(proven_sysio_read_u16_line(&moved).err == PROVEN_ERR_EOF, "EOF", "");
            PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_close(f.value)), "close", "");
        }
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_remove(heap, path)), "cleanup", "");
    }

    PROVEN_TEST_PASS("UTF-16 text through writers, readers and the formatter");
    return 0;
}
