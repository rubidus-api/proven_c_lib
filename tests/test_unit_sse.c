#include "proven.h"
#include "proven_test.h"
#include <stdlib.h>
#include <string.h>

/*
 * The Server-Sent Events parser, against the examples and the rules of the HTML standard
 * (section 9.2.6, "Interpreting an event stream").
 *
 * As with the HTTP body decoder, the property that matters is that the result does not depend
 * on how the stream was cut: each stream is parsed whole, split in two at every position, and
 * one byte at a time, and the events must be identical.
 */

typedef struct { char event[64]; char data[256]; char id[128]; bool has_retry; proven_u32 retry; } ev_t;
typedef struct { ev_t ev[8]; int count; proven_err_t err; } run_t;

static run_t run(const proven_byte_t *stream, proven_size_t len, proven_size_t first, proven_size_t step) {
    run_t out = {0};
    proven_byte_t work[512];
    proven_sse_t sse;
    out.err = proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work });
    if (out.err != PROVEN_OK) return out;
    proven_size_t pos = 0;
    proven_size_t piece = first ? first : (step ? step : len);
    while (pos < len) {
        proven_size_t avail = len - pos < piece ? len - pos : piece;
        proven_byte_t *copy = malloc(avail ? avail : 1);
        memcpy(copy, stream + pos, avail);
        proven_size_t off = 0;
        while (off < avail) {
            proven_size_t used = 0;
            proven_sse_event_t e;
            bool have = false;
            out.err = proven_sse_feed(&sse, (proven_mem_view_t){ copy + off, avail - off }, &used, &e, &have);
            if (out.err != PROVEN_OK) { free(copy); return out; }
            off += used;
            if (have && out.count < 8) {
                ev_t *v = &out.ev[out.count++];
                memcpy(v->event, e.event.ptr, e.event.size); v->event[e.event.size] = '\0';
                memcpy(v->data, e.data.ptr, e.data.size); v->data[e.data.size] = '\0';
                memcpy(v->id, e.id.ptr, e.id.size); v->id[e.id.size] = '\0';
                v->has_retry = e.has_retry;
                v->retry = e.retry_ms;
            }
        }
        free(copy);
        pos += avail;
        piece = step ? step : len;
    }
    return out;
}

static bool same(const run_t *a, const run_t *b) {
    if (a->err != b->err || a->count != b->count) return false;
    for (int i = 0; i < a->count; ++i) {
        if (strcmp(a->ev[i].event, b->ev[i].event) || strcmp(a->ev[i].data, b->ev[i].data) || strcmp(a->ev[i].id, b->ev[i].id) ||
            a->ev[i].has_retry != b->ev[i].has_retry || a->ev[i].retry != b->ev[i].retry) return false;
    }
    return true;
}

/* Parse `stream` every way and require agreement; returns the whole-stream result. */
static run_t every_way(const char *stream, bool *consistent) {
    proven_size_t len = strlen(stream);
    run_t whole = run((const proven_byte_t *)stream, len, 0, 0);
    *consistent = true;
    for (proven_size_t cut = 1; cut < len; ++cut) {
        run_t split = run((const proven_byte_t *)stream, len, cut, 0);
        if (!same(&whole, &split)) *consistent = false;
    }
    run_t bytes = run((const proven_byte_t *)stream, len, 0, 1);
    if (!same(&whole, &bytes)) *consistent = false;
    return whole;
}

int main(void) {
    PROVEN_TEST_SUITE("sse: the event-stream parser",
        "Events come out as the HTML standard says they should, however the stream is cut.",
        "Inspect src/proven/sse.c.");

    bool consistent = false;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the standard's examples",
        "Data lines join with newlines; comments and unknown fields vanish; an event without data is not delivered.",
        "");
    // ---------------------------------------------------------------
    {
        run_t r = every_way("data: This is the first message.\n\ndata: This is the second message, it\ndata: has two lines.\n\ndata: This is the third message.\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.err == PROVEN_OK && r.count == 3 &&
                           strcmp(r.ev[0].data, "This is the first message.") == 0 &&
                           strcmp(r.ev[1].data, "This is the second message, it\nhas two lines.") == 0 &&
                           strcmp(r.ev[2].data, "This is the third message.") == 0 && r.ev[0].event[0] == '\0',
            "three messages, the second with two data lines joined by a newline", "");

        r = every_way("event: add\ndata: 73857293\n\nevent: remove\ndata: 2153\n\nevent: add\ndata: 113411\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 3 && strcmp(r.ev[0].event, "add") == 0 && strcmp(r.ev[1].event, "remove") == 0 &&
                           strcmp(r.ev[1].data, "2153") == 0 && strcmp(r.ev[2].event, "add") == 0,
            "the event type is per event and does not carry over", "");

        r = every_way(": test stream\n\ndata: first event\nid: 1\n\ndata:second event\nid\n\ndata:  third event\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 3 && strcmp(r.ev[0].data, "first event") == 0 && strcmp(r.ev[0].id, "1") == 0 &&
                           strcmp(r.ev[1].data, "second event") == 0 && r.ev[1].id[0] == '\0' &&
                           strcmp(r.ev[2].data, " third event") == 0 && r.ev[2].id[0] == '\0',
            "a comment is skipped; an id sticks until reset by an empty id; exactly one space after the colon is dropped", "");

        r = every_way("data\n\ndata\ndata\n\ndata:", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 2 && r.ev[0].data[0] == '\0' && strcmp(r.ev[1].data, "\n") == 0,
            "a bare \"data\" line is an empty line of data; the unterminated last event is never delivered", "");

        r = every_way("data:test\n\ndata: test\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 2 && strcmp(r.ev[0].data, "test") == 0 && strcmp(r.ev[1].data, "test") == 0,
            "\"data:test\" and \"data: test\" are the same event", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("line endings, the byte order mark, and odd fields",
        "CRLF, LF and CR all end a line; a leading BOM is skipped; retry must be digits; an id with a NUL is ignored.",
        "A CR at the end of one piece and an LF at the start of the next are ONE line ending, which is where a split-sensitive parser fails.");
    // ---------------------------------------------------------------
    {
        run_t lf = every_way("event: e\ndata: a\ndata: b\nid: 7\n\n", &consistent);
        bool ok = consistent;
        run_t crlf = every_way("event: e\r\ndata: a\r\ndata: b\r\nid: 7\r\n\r\n", &consistent);
        ok = ok && consistent;
        run_t cr = every_way("event: e\rdata: a\rdata: b\rid: 7\r\r", &consistent);
        ok = ok && consistent;
        run_t mixed = every_way("event: e\r\ndata: a\rdata: b\nid: 7\r\n\n", &consistent);
        ok = ok && consistent;
        PROVEN_TEST_ASSERT(ok && lf.count == 1 && strcmp(lf.ev[0].data, "a\nb") == 0 && strcmp(lf.ev[0].id, "7") == 0 &&
                           same(&lf, &crlf) && same(&lf, &cr) && same(&lf, &mixed),
            "the same event with LF, CRLF, CR and a mixture of the three, cut anywhere", "");

        run_t r = every_way("\xef\xbb\xbf" "data: after bom\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 1 && strcmp(r.ev[0].data, "after bom") == 0, "a UTF-8 byte order mark at the start is skipped", "");
        r = every_way("\xef\xbb" "data: x\n\ndata: y\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 1 && strcmp(r.ev[0].data, "y") == 0,
            "two bytes that only begin a BOM are part of the first line, whose field name is then unknown", "");

        r = every_way("retry: 3000\ndata: a\n\nretry: soon\ndata: b\n\nretry:\ndata: c\n\nretry: 99999999999\ndata: d\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 4 && r.ev[0].has_retry && r.ev[0].retry == 3000 && !r.ev[1].has_retry && !r.ev[2].has_retry &&
                           r.ev[3].has_retry && r.ev[3].retry == 0xffffffffu,
            "retry is taken when it is all digits (saturating), and ignored otherwise", "");

        r = every_way("unknown: x\nDATA: wrong case\n data: leading space\ndata: real\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 1 && strcmp(r.ev[0].data, "real") == 0,
            "field names are case-sensitive and exact; anything else is ignored", "");

        r = every_way("id: 5\n\ndata: x\n\nevent: only\n\ndata: y\n\n", &consistent);
        PROVEN_TEST_ASSERT(consistent && r.count == 2 && strcmp(r.ev[0].id, "5") == 0 && r.ev[1].event[0] == '\0' && strcmp(r.ev[1].id, "5") == 0,
            "an id set by an event with no data is remembered; an event type set by one is not", "");

        const proven_byte_t with_nul[] = "id: a\0b\ndata: x\n\n";
        run_t n = run(with_nul, sizeof with_nul - 1, 0, 0);
        PROVEN_TEST_ASSERT(n.count == 1 && n.ev[0].id[0] == '\0', "an id containing a NUL is ignored", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("bounds, and the last id",
        "A line or an event too large for the working memory is an error; the last id is available for reconnecting.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t work[64];
        proven_sse_t sse;
        proven_size_t used = 0;
        proven_sse_event_t e;
        bool have = false;
        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ work, 63 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_sse_init(&sse, (proven_mem_mut_t){ NULL, 64 }) == PROVEN_ERR_INVALID_ARG,
            "working memory under 64 bytes, or none, is refused", "");
        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work }) == PROVEN_OK, "64 bytes: 32 for a line, 32 for data", "");
        const char *fits = "data: 0123456789012345678901234\n\n";            /* a 31-byte line, 25+1 bytes of data */
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)fits, strlen(fits) }, &used, &e, &have) == PROVEN_OK && have && e.data.size == 25,
            "a line that just fits is parsed", "");
        const char *long_line = "data: 01234567890123456789012345678901234567890\n\n";
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)long_line, strlen(long_line) }, &used, &e, &have) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a line longer than its half of the memory is PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)"data: x\n\n", 9 }, &used, &e, &have) == PROVEN_ERR_INVALID_STATE,
            "and the parser is finished: PROVEN_ERR_INVALID_STATE", "");

        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work }) == PROVEN_OK, "a fresh parser", "");
        const char *many = "data: 0123456789\ndata: 0123456789\ndata: 0123456789\n\n";
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)many, strlen(many) }, &used, &e, &have) == PROVEN_ERR_OUT_OF_BOUNDS,
            "three data lines that together outgrow the data half are PROVEN_ERR_OUT_OF_BOUNDS", "");

        proven_byte_t big[256];
        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ big, sizeof big }) == PROVEN_OK && proven_sse_last_id(&sse).size == 0, "no id yet", "");
        const char *s = "id: evt-41\ndata: a\n\nid: evt-42\n";
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)s, strlen(s) }, &used, &e, &have) == PROVEN_OK && have &&
                           proven_u8str_view_eq(proven_sse_last_id(&sse), PROVEN_LIT("evt-41")),
            "after the first event the last id is evt-41", "");
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)s + used, strlen(s) - used }, &used, &e, &have) == PROVEN_OK && !have &&
                           proven_u8str_view_eq(proven_sse_last_id(&sse), PROVEN_LIT("evt-42")),
            "and an id line read since then already counts, though its event has not ended", "");
    }

    PROVEN_TEST_PASS("the event-stream parser follows the standard, however the stream is cut.");
    return 0;
}
