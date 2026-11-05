#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <string.h>

/*
 * Reading multipart bodies (RFC 2046 section 5.1, RFC 7578), Content-Disposition, and several byte
 * ranges (RFC 9110 sections 14.2 and 14.6): the reader fed in every piece size against bodies whose
 * parts are known - written by this library's writers, or by hand from the RFC's grammar; the
 * disposition and range rows from the RFCs' rules. A private check reads 300 bodies Python's email
 * parser agreed with, in eight piece sizes (TEST.md names it).
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

/* Read `body` in pieces of `step` bytes (0: whole), writing what was found as text into `out`:
 * "P" and the fields for a part, its bytes, "E" at its end, "Z" at the end. Returns the error, if any. */
static proven_err_t read_all(const char *boundary, const proven_byte_t *body, proven_size_t n, proven_size_t step, char *out, proven_size_t cap,
                             proven_size_t head_cap, proven_size_t field_cap, bool *whole) {
    proven_http_multipart_t r;
    static proven_byte_t head[4096];
    proven_http_header_t fields[16];
    proven_err_t e = proven_http_multipart_init(&r, sv(boundary), (proven_mem_mut_t){ head, head_cap }, fields, field_cap);
    if (e != PROVEN_OK) return e;
    proven_size_t o = 0, at = 0;
    out[0] = 0;
    while (at < n) {
        proven_size_t give = step == 0 || step > n - at ? n - at : step;
        proven_mem_view_t in = { body + at, give };
        for (;;) {
            proven_size_t used = 0;
            proven_http_multipart_event_t ev;
            e = proven_http_multipart_feed(&r, in, &used, &ev);
            if (e != PROVEN_OK) return e;
            if (used > in.size) return PROVEN_ERR_INVALID_STATE;
            in.ptr += used; in.size -= used;
            if (ev.kind == PROVEN_HTTP_MULTIPART_PART) {
                o += (proven_size_t)snprintf(out + o, cap - o, "P");
                for (proven_size_t i = 0; i < ev.field_count; ++i)
                    o += (proven_size_t)snprintf(out + o, cap - o, "[%.*s=%.*s]", (int)ev.fields[i].name.size, (const char *)ev.fields[i].name.ptr, (int)ev.fields[i].value.size, (const char *)ev.fields[i].value.ptr);
                o += (proven_size_t)snprintf(out + o, cap - o, ":");
            } else if (ev.kind == PROVEN_HTTP_MULTIPART_DATA) {
                if (ev.data.size == 0) return PROVEN_ERR_INVALID_STATE;
                o += (proven_size_t)snprintf(out + o, cap - o, "%.*s", (int)ev.data.size, (const char *)ev.data.ptr);
            } else if (ev.kind == PROVEN_HTTP_MULTIPART_PART_END) {
                o += (proven_size_t)snprintf(out + o, cap - o, "E");
            } else if (ev.kind == PROVEN_HTTP_MULTIPART_END) {
                o += (proven_size_t)snprintf(out + o, cap - o, "Z");
            } else if (in.size > 0) {
                return PROVEN_ERR_INVALID_STATE;          /* NONE must mean all was taken */
            }
            if (in.size == 0 && ev.kind == PROVEN_HTTP_MULTIPART_NONE) break;
        }
        at += give;
    }
    *whole = proven_http_multipart_end(&r) == PROVEN_OK;
    return PROVEN_OK;
}

/* The same in every piece size from 1 to 20 and whole: the same text each time, and the one wanted. */
static bool reads_as(const char *boundary, const char *body, const char *want) {
    static char got[8192];
    const proven_size_t n = strlen(body);
    for (proven_size_t step = 0; step <= 20; ++step) {
        bool whole = false;
        if (read_all(boundary, (const proven_byte_t *)body, n, step, got, sizeof got, 4096, 16, &whole) != PROVEN_OK || !whole || strcmp(got, want) != 0) {
            printf("  in pieces of %zu: \"%s\"\n", step, got);
            return false;
        }
    }
    return true;
}

static proven_err_t read_err(const char *boundary, const char *body, proven_size_t head_cap, proven_size_t field_cap) {
    static char got[8192];
    bool whole = false;
    proven_err_t e = read_all(boundary, (const proven_byte_t *)body, strlen(body), 0, got, sizeof got, head_cap, field_cap, &whole);
    if (e == PROVEN_OK && !whole) return PROVEN_ERR_EOF;          /* the test's word for "ended too soon" */
    return e;
}

int main(void) {
    PROVEN_TEST_SUITE("http multipart: reading bodies, Content-Disposition, several ranges",
        "Multipart bodies read in pieces of every size to the parts they hold; a part's head and Content-Disposition read to their fields; Range lists resolved, merged and bounded as RFC 9110 section 14.2 suggests.",
        "Inspect the multipart reader, proven_http_content_disposition_parse and proven_http_range_parse_list in net/src/http.c.");

    PROVEN_TEST_SECTION("the boundary from a Content-Type", "Any multipart subtype, a token or a quoted boundary, parameters in any order; what is not multipart, has no boundary or a bad one.", "");
    {
        proven_u8str_view_t b;
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv("multipart/form-data; boundary=----abc"), &b) == PROVEN_OK && proven_u8str_view_eq(b, sv("----abc")), "a token", "");
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv("Multipart/Mixed;charset=x; BOUNDARY=\"a b:c\""), &b) == PROVEN_OK && proven_u8str_view_eq(b, sv("a b:c")),
                           "quoted, with a space and a colon; names and the type compared without case", "");
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv("multipart/byteranges; boundary=x"), &b) == PROVEN_OK && b.size == 1, "byteranges, one character", "");
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv("text/plain; boundary=x"), &b) == PROVEN_ERR_NOT_FOUND &&
                           proven_http_multipart_boundary_parse(sv("multipart/form-data"), &b) == PROVEN_ERR_NOT_FOUND &&
                           proven_http_multipart_boundary_parse(sv("multipart/; boundary=x"), &b) == PROVEN_ERR_NOT_FOUND, "not multipart, or no boundary: PROVEN_ERR_NOT_FOUND", "");
        static char longb[100];
        memset(longb, 'a', sizeof longb - 1);
        char ct[160];
        (void)snprintf(ct, sizeof ct, "multipart/mixed; boundary=%.71s", longb);
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv(ct), &b) == PROVEN_ERR_INVALID_FORMAT, "71 characters: PROVEN_ERR_INVALID_FORMAT", "");
        (void)snprintf(ct, sizeof ct, "multipart/mixed; boundary=%.70s", longb);
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv(ct), &b) == PROVEN_OK && b.size == 70, "70 is the most", "");
        PROVEN_TEST_ASSERT(proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary=\"\""), &b) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary=\"a \""), &b) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary=\"a\\\\b\""), &b) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary=\"a;b\""), &b) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary=a; boundary=b"), &b) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_boundary_parse(sv("multipart/mixed; boundary"), &b) == PROVEN_ERR_INVALID_FORMAT,
                           "empty, ending in a space, an escape, a character RFC 2046 does not allow, given twice, no value: PROVEN_ERR_INVALID_FORMAT", "");
    }

    PROVEN_TEST_SECTION("bodies written by this library's writers", "A form with a field and a file, and a byteranges body: read back in every piece size from 1 to 20 and whole.", "");
    {
        static proven_byte_t body[2048];
        proven_size_t n = 0;
        const proven_u8str_view_t b = sv("----proven1234");
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part((proven_mem_mut_t){ body, sizeof body }, &n, b, sv("title"), sv(""), sv("")) == PROVEN_OK, "a field", "");
        (void)memcpy(body + n, "Hello", 5); n += 5;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part_end((proven_mem_mut_t){ body, sizeof body }, &n) == PROVEN_OK &&
                           proven_http_multipart_write_part((proven_mem_mut_t){ body, sizeof body }, &n, b, sv("up"), sv("a\"b.txt"), sv("text/plain")) == PROVEN_OK, "a file", "");
        (void)memcpy(body + n, "line one\r\n--proven\r\n", 20); n += 20;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part_end((proven_mem_mut_t){ body, sizeof body }, &n) == PROVEN_OK &&
                           proven_http_multipart_write_end((proven_mem_mut_t){ body, sizeof body }, &n, b) == PROVEN_OK, "the end", "");
        body[n] = 0;
        PROVEN_TEST_ASSERT(reads_as("----proven1234", (const char *)body,
                                    "P[Content-Disposition=form-data; name=\"title\"]:HelloEP[Content-Disposition=form-data; name=\"up\"; filename=\"a%22b.txt\"][Content-Type=text/plain]:line one\r\n--proven\r\nEZ"),
                           "the field and the file - whose bytes hold a CR LF and two dashes - come back as written", "");
        /* byteranges: two parts, read back and their Content-Range read. */
        n = 0;
        PROVEN_TEST_ASSERT(proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &n, b, sv("text/html"), 0, 4, 100) == PROVEN_OK, "a range's part", "");
        (void)memcpy(body + n, "01234", 5); n += 5;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part_end((proven_mem_mut_t){ body, sizeof body }, &n) == PROVEN_OK &&
                           proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &n, b, sv(""), 90, 99, 100) == PROVEN_OK, "a second, without a type", "");
        (void)memcpy(body + n, "abcdefghij", 10); n += 10;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part_end((proven_mem_mut_t){ body, sizeof body }, &n) == PROVEN_OK &&
                           proven_http_multipart_write_end((proven_mem_mut_t){ body, sizeof body }, &n, b) == PROVEN_OK, "the end", "");
        body[n] = 0;
        PROVEN_TEST_ASSERT(reads_as("----proven1234", (const char *)body, "P[Content-Type=text/html][Content-Range=bytes 0-4/100]:01234EP[Content-Range=bytes 90-99/100]:abcdefghijEZ"), "both ranges", "");
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_http_byteranges_write_content_type((proven_mem_mut_t){ body, sizeof body }, &len, b) == PROVEN_OK &&
                           len == 61 && memcmp(body, "Content-Type: multipart/byteranges; boundary=----proven1234\r\n", 61) == 0, "the body's own Content-Type", "");
        PROVEN_TEST_ASSERT(proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &len, b, sv(""), 5, 4, 100) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &len, b, sv(""), 0, 100, 100) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &len, b, sv("a\rb"), 0, 1, 100) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_byteranges_write_part((proven_mem_mut_t){ body, sizeof body }, &len, sv("a b"), sv(""), 0, 1, 100) == PROVEN_ERR_INVALID_ARG,
                           "a range backwards or past the size, a control character, a boundary that needs quoting: PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_SECTION("what RFC 2046 allows around the parts", "A preamble, an epilogue, white space after a boundary, an empty part, a part with no fields, CR and LF that are not a boundary line, and a boundary that is a prefix of another's.", "");
    {
        PROVEN_TEST_ASSERT(reads_as("b", "preamble\r\n--b\r\nA: 1\r\n\r\nx\r\n--b--\r\nepilogue\r\n--b\r\n", "P[A=1]:xEZ"), "the preamble and the epilogue are passed over, even a boundary line in the epilogue", "");
        PROVEN_TEST_ASSERT(reads_as("b", "--b \t\r\n\r\n\r\n--b--", "P:EZ"), "white space after the boundary; a part with no fields and no bytes", "");
        PROVEN_TEST_ASSERT(reads_as("b", "--b\r\n\r\n\r\n\r\n--b\r\n\r\n\r\n--b--", "P:\r\nEP:EZ"), "a part whose bytes are a CR LF, and an empty one", "");
        PROVEN_TEST_ASSERT(reads_as("bb", "--bb\r\n\r\n\r\n--b\r\n-\r\n-\r--bb\r\n\r\n--bb--", "P:\r\n--b\r\n-\r\n-\r--bb\r\nEZ"),
                           "CR, LF and dashes that begin a boundary line and stop short of it, and the boundary after a CR alone, are data - held across pieces and given back", "");
        PROVEN_TEST_ASSERT(reads_as("a", "--a\r\nX: y\r\n\r\n\r\r\n\r\n-\r\n--\r\n--a--", "P[X=y]:\r\r\n\r\n-\r\n--E" "Z"), "runs of CR LF before the boundary line", "");
    }

    PROVEN_TEST_SECTION("bodies refused", "A boundary line followed by other text (RFC 2046 does not let it appear inside a part: Python's email reads such a line as data, this reader does not), a head that is not fields, a head over the buffer, more fields than there is room for, and bodies that end too soon.", "");
    {
        PROVEN_TEST_ASSERT(read_err("b", "--b\r\n\r\nx\r\n--bX\r\n--b--", 4096, 16) == PROVEN_ERR_INVALID_FORMAT, "a boundary line followed by more text", "");
        PROVEN_TEST_ASSERT(read_err("b", "--b\r\nno colon\r\n\r\nx\r\n--b--", 4096, 16) == PROVEN_ERR_INVALID_FORMAT &&
                           read_err("b", "--b\r\nA: 1\r\n folded\r\n\r\nx\r\n--b--", 4096, 16) == PROVEN_ERR_INVALID_FORMAT &&
                           read_err("b", "--b\r\nA: 1\n\r\nx\r\n--b--", 4096, 16) == PROVEN_ERR_INVALID_FORMAT, "a line that is not a field, a folded one, a bare LF", "");
        PROVEN_TEST_ASSERT(read_err("b", "--b\r\nA: 1234567890\r\n\r\nx\r\n--b--", 16, 16) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           read_err("b", "--b\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\nx\r\n--b--", 4096, 2) == PROVEN_ERR_OUT_OF_BOUNDS, "a head over the buffer, three fields with room for two: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(read_err("b", "--b\r\n\r\nx\r\n--b", 4096, 16) == PROVEN_ERR_EOF && read_err("b", "--b\r\n\r\nx", 4096, 16) == PROVEN_ERR_EOF &&
                           read_err("b", "no boundary at all", 4096, 16) == PROVEN_ERR_EOF, "a body without its closing boundary is not whole: proven_http_multipart_end says so", "");
        /* Every prefix of a body, and every single-bit change: refused or read, never past its input. */
        const char *base = "pre\r\n--bnd\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nvalue\r\n--bnd\r\nX: 1\r\n\r\n\r\n--bn\r\n--bnd--\r\n";
        static char mut[256];
        int odd = 0;
        const proven_size_t bl = strlen(base);
        for (proven_size_t cut = 0; cut <= bl; ++cut) {
            memcpy(mut, base, cut); mut[cut] = 0;
            const proven_err_t e = read_err("bnd", mut, 4096, 16);
            if (e != PROVEN_OK && e != PROVEN_ERR_EOF && e != PROVEN_ERR_INVALID_FORMAT) odd++;
            if (cut == bl && e != PROVEN_OK) odd++;
        }
        for (proven_size_t i = 0; i < bl; ++i) for (int bit = 0; bit < 8; ++bit) {
            memcpy(mut, base, bl + 1);
            mut[i] = (char)(mut[i] ^ (1 << bit));
            if (mut[i] == 0) continue;
            const proven_err_t e = read_err("bnd", mut, 4096, 16);
            if (e != PROVEN_OK && e != PROVEN_ERR_EOF && e != PROVEN_ERR_INVALID_FORMAT && e != PROVEN_ERR_OUT_OF_BOUNDS) odd++;
        }
        PROVEN_TEST_ASSERT(odd == 0, "every prefix ends too soon or is refused, the whole is read; every single-bit change is read or refused with a reason", "");
        proven_http_multipart_t r;
        proven_byte_t h[8];
        proven_http_header_t f[1];
        proven_size_t used = 0;
        proven_http_multipart_event_t ev;
        PROVEN_TEST_ASSERT(proven_http_multipart_init(&r, sv(""), (proven_mem_mut_t){ h, 8 }, f, 1) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_init(&r, sv("b"), (proven_mem_mut_t){ NULL, 0 }, f, 1) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_init(&r, sv("b"), (proven_mem_mut_t){ h, 8 }, f, 0) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_feed(&r, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_multipart_init(NULL, sv("b"), (proven_mem_mut_t){ h, 8 }, f, 1) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_multipart_end(NULL) == PROVEN_ERR_INVALID_ARG,
                           "no boundary, no head buffer, no room for a field: PROVEN_ERR_INVALID_ARG, and a reader that did not start reads nothing", "");
    }

    PROVEN_TEST_SECTION("Content-Disposition", "The type and the name, filename and filename* parameters, as views; case, order, other parameters, and what is refused.", "");
    {
        proven_http_disposition_t d;
        PROVEN_TEST_ASSERT(proven_http_content_disposition_parse(sv("form-data; name=\"field1\"; filename=\"a b.txt\""), &d) == PROVEN_OK &&
                           proven_u8str_view_eq(d.type, sv("form-data")) && d.has_name && proven_u8str_view_eq(d.name, sv("field1")) && d.has_filename &&
                           proven_u8str_view_eq(d.filename, sv("a b.txt")) && !d.has_filename_ext, "a browser's file part", "");
        PROVEN_TEST_ASSERT(proven_http_content_disposition_parse(sv("attachment;FILENAME*=UTF-8''%e2%82%ac%20rates.txt; size=10 ;Name=tok"), &d) == PROVEN_OK &&
                           proven_u8str_view_eq(d.type, sv("attachment")) && d.has_filename_ext && proven_u8str_view_eq(d.filename_ext, sv("UTF-8''%e2%82%ac%20rates.txt")) &&
                           d.has_name && proven_u8str_view_eq(d.name, sv("tok")) && !d.has_filename, "filename* undecoded, a parameter not known passed over, names without case", "");
        PROVEN_TEST_ASSERT(proven_http_content_disposition_parse(sv("inline"), &d) == PROVEN_OK && !d.has_name && d.name.size == 0, "a type alone", "");
        PROVEN_TEST_ASSERT(proven_http_content_disposition_parse(sv("form-data; name=\"a\\\"b\""), &d) == PROVEN_ERR_UNSUPPORTED, "a backslash escape: PROVEN_ERR_UNSUPPORTED", "");
        PROVEN_TEST_ASSERT(proven_http_content_disposition_parse(sv(""), &d) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_disposition_parse(sv("form-data name=a"), &d) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_disposition_parse(sv("form-data; name=\"a"), &d) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_disposition_parse(sv("form-data; name=a; name=b"), &d) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_content_disposition_parse(sv("form-data; =a"), &d) == PROVEN_ERR_INVALID_FORMAT,
                           "nothing, no semicolon, an open quote, a name twice, no parameter name: PROVEN_ERR_INVALID_FORMAT", "");
    }

    PROVEN_TEST_SECTION("several ranges", "Every spelling, merged and sorted; unsatisfiable ones dropped; more than the room, or more than the resource, refused.", "");
    {
        proven_http_range_t r[8];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=0-99, 200-299"), 1000, r, 8, &n) == PROVEN_OK && n == 2 &&
                           r[0].first == 0 && r[0].last == 99 && r[1].first == 200 && r[1].last == 299, "two ranges", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=500-599,0-9,10-19,-100"), 1000, r, 8, &n) == PROVEN_OK && n == 3 &&
                           r[0].first == 0 && r[0].last == 19 && r[1].first == 500 && r[1].last == 599 && r[2].first == 900 && r[2].last == 999,
                           "sorted; touching ones merged; a suffix resolved", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=0-499,400-"), 1000, r, 8, &n) == PROVEN_ERR_UNSUPPORTED,
                           "overlapping ranges that together ask for more than the resource: PROVEN_ERR_UNSUPPORTED", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=0-0,,  , 5000-6000"), 1000, r, 8, &n) == PROVEN_OK && n == 1 && r[0].last == 0,
                           "empty elements allowed; one past the end dropped", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=2000-,3000-4000"), 1000, r, 8, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0, "none satisfiable: PROVEN_ERR_OUT_OF_BOUNDS (416)", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=0-1,2-3,4-5"), 1000, r, 2, &n) == PROVEN_ERR_UNSUPPORTED &&
                           proven_http_range_parse_list(sv("bytes=0-999,0-999"), 1000, r, 8, &n) == PROVEN_ERR_UNSUPPORTED &&
                           proven_http_range_parse_list(sv("items=0-1"), 1000, r, 8, &n) == PROVEN_ERR_UNSUPPORTED,
                           "more ranges than room, ranges that ask for more than the resource, another unit: PROVEN_ERR_UNSUPPORTED (send it all)", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=5-1"), 1000, r, 8, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_range_parse_list(sv("bytes=-"), 1000, r, 8, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_range_parse_list(sv("bytes="), 1000, r, 8, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_range_parse_list(sv("bytes=1-2 x"), 1000, r, 8, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_http_range_parse_list(sv("0-1"), 1000, r, 8, &n) == PROVEN_ERR_INVALID_FORMAT,
                           "backwards, no numbers, nothing, trailing text, no unit: PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=-0"), 1000, r, 8, &n) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_http_range_parse_list(sv("bytes=0-"), 0, r, 8, &n) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           proven_http_range_parse_list(sv("bytes=-5000"), 1000, r, 8, &n) == PROVEN_OK && n == 1 && r[0].first == 0 && r[0].last == 999,
                           "a suffix of nothing, anything of an empty resource: unsatisfiable; a suffix longer than the resource is all of it", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse_list(sv("bytes=0-1"), 1000, NULL, 8, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_range_parse_list(sv("bytes=0-1"), 1000, r, 0, &n) == PROVEN_ERR_INVALID_ARG, "no room: PROVEN_ERR_INVALID_ARG", "");
    }

    PROVEN_TEST_PASS("multipart bodies, dispositions and range lists read as their RFCs say.");
    return 0;
}
