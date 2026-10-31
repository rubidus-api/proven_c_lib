#include "example.h"
#include <string.h>

/*
 * Reading what a form sends - a multipart/form-data body, in pieces as it arrives - and answering
 * a request for several ranges with a multipart/byteranges body.
 */

static bool view_is(proven_u8str_view_t v, const char *s) { return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0; }

static bool holds(const proven_byte_t *p, proven_size_t n, const char *s) {
    const proven_size_t k = strlen(s);
    for (proven_size_t i = 0; i + k <= n; ++i) if (memcmp(p + i, s, k) == 0) return true;
    return false;
}

int main(void) {
    /* What a browser sends for a form with a title and a file. */
    static const char ctype[] = "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxk";
    static const char body[] =
        "------WebKitFormBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
        "Holiday\r\n"
        "------WebKitFormBoundary7MA4YWxk\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"beach.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "sand\r\n--and sea\r\n"
        "------WebKitFormBoundary7MA4YWxk--\r\n";
    proven_u8str_view_t boundary;
    EXAMPLE_REQUIRE(proven_http_multipart_boundary_parse(proven_u8str_view_from_cstr(ctype), &boundary) == PROVEN_OK, "the boundary, from the request's Content-Type");

    /* A reader: a buffer for one part's head and room for its fields; nothing else is held. */
    proven_http_multipart_t r;
    static proven_byte_t head[1024];
    proven_http_header_t fields[8];
    EXAMPLE_REQUIRE(proven_http_multipart_init(&r, boundary, (proven_mem_mut_t){ head, sizeof head }, fields, 8) == PROVEN_OK, "a reader");

    /* Fed seven bytes at a time, as a slow connection would deliver them. */
    char title[32] = { 0 }, file[64] = { 0 }, filename[32] = { 0 };
    char *into = NULL;
    proven_size_t into_len = 0, into_cap = 0;
    for (proven_size_t at = 0; at < sizeof body - 1; at += 7) {
        proven_mem_view_t in = { (const proven_byte_t *)body + at, sizeof body - 1 - at < 7 ? sizeof body - 1 - at : 7 };
        while (in.size > 0) {
            proven_size_t used = 0;
            proven_http_multipart_event_t ev;
            EXAMPLE_REQUIRE(proven_http_multipart_feed(&r, in, &used, &ev) == PROVEN_OK, "a piece is read");
            in.ptr += used;
            in.size -= used;
            if (ev.kind == PROVEN_HTTP_MULTIPART_PART) {
                /* Which field is this? Its Content-Disposition says. */
                proven_u8str_view_t cd = { 0 };
                for (proven_size_t i = 0; i < ev.field_count; ++i)
                    if (proven_u8str_view_eq_ascii_ci(ev.fields[i].name, PROVEN_LIT("Content-Disposition"))) cd = ev.fields[i].value;
                proven_http_disposition_t d;
                EXAMPLE_REQUIRE(proven_http_content_disposition_parse(cd, &d) == PROVEN_OK && d.has_name, "each part names its field");
                into = view_is(d.name, "title") ? title : file;
                into_cap = view_is(d.name, "title") ? sizeof title - 1 : sizeof file - 1;
                into_len = 0;
                if (d.has_filename && d.filename.size < sizeof filename) memcpy(filename, d.filename.ptr, d.filename.size);
            } else if (ev.kind == PROVEN_HTTP_MULTIPART_DATA) {
                /* The part's bytes, a piece at a time: here, kept; a file would go to disk. */
                EXAMPLE_REQUIRE(into && into_len + ev.data.size <= into_cap, "the part fits what was set aside for it");
                memcpy(into + into_len, ev.data.ptr, ev.data.size);
                into_len += ev.data.size;
            }
        }
    }
    EXAMPLE_REQUIRE(proven_http_multipart_end(&r) == PROVEN_OK, "the body was whole: its closing boundary came");
    EXAMPLE_REQUIRE(strcmp(title, "Holiday") == 0 && strcmp(filename, "beach.txt") == 0 && strcmp(file, "sand\r\n--and sea") == 0,
                    "the title, the file's name, and its bytes - a CR LF and two dashes among them");

    /* Several ranges: merged and sorted, then a multipart/byteranges body. */
    static const char doc[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    proven_http_range_t ranges[4];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_range_parse_list(PROVEN_LIT("bytes=30-, 0-3, 2-5"), sizeof doc - 1, ranges, 4, &n) == PROVEN_OK &&
                    n == 2 && ranges[0].first == 0 && ranges[0].last == 5 && ranges[1].first == 30 && ranges[1].last == 35,
                    "0-3 and 2-5 overlap and become 0-5; 30 to the end is 30-35");
    static proven_byte_t buf[512];
    proven_mem_mut_t out = { buf, sizeof buf };
    proven_size_t len = 0;
    const proven_u8str_view_t b = PROVEN_LIT("ranges-1f0b");
    static proven_byte_t ct[96];
    proven_size_t ctl = 0;
    EXAMPLE_REQUIRE(proven_http_byteranges_write_content_type((proven_mem_mut_t){ ct, sizeof ct }, &ctl, b) == PROVEN_OK, "the 206's own Content-Type: multipart/byteranges, with the boundary");
    proven_err_t e = PROVEN_OK;
    for (proven_size_t i = 0; i < n && e == PROVEN_OK; ++i) {
        e = proven_http_byteranges_write_part(out, &len, b, PROVEN_LIT("text/plain"), ranges[i].first, ranges[i].last, sizeof doc - 1);
        const proven_size_t k = (proven_size_t)(ranges[i].last - ranges[i].first + 1);
        if (e == PROVEN_OK && len + k <= sizeof buf) { memcpy(buf + len, doc + ranges[i].first, k); len += k; }
        if (e == PROVEN_OK) e = proven_http_multipart_write_part_end(out, &len);
    }
    if (e == PROVEN_OK) e = proven_http_multipart_write_end(out, &len, b);
    EXAMPLE_REQUIRE(e == PROVEN_OK && len > 0, "the body of the 206: each range a part with its Content-Range");
    const char *second = "--ranges-1f0b\r\nContent-Type: text/plain\r\nContent-Range: bytes 30-35/36\r\n\r\nuvwxyz\r\n";
    EXAMPLE_REQUIRE(holds(buf, len, second), "the second part, as it is written");
    return EXAMPLE_OK();
}
