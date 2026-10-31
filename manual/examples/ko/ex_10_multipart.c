#include "example.h"
#include <string.h>

/*
 * 폼이 보내는 것 - multipart/form-data 본문 - 을 도착하는 대로 조각조각 읽기, 그리고 여러 범위를
 * 묻는 요청에 multipart/byteranges 본문으로 답하기.
 */

static bool view_is(proven_u8str_view_t v, const char *s) { return v.size == strlen(s) && memcmp(v.ptr, s, v.size) == 0; }

static bool holds(const proven_byte_t *p, proven_size_t n, const char *s) {
    const proven_size_t k = strlen(s);
    for (proven_size_t i = 0; i + k <= n; ++i) if (memcmp(p + i, s, k) == 0) return true;
    return false;
}

int main(void) {
    /* 제목과 파일 하나가 있는 폼에 브라우저가 보내는 것. */
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
    EXAMPLE_REQUIRE(proven_http_multipart_boundary_parse(proven_u8str_view_from_cstr(ctype), &boundary) == PROVEN_OK, "요청의 Content-Type에서 경계");

    /* 읽개: 파트 하나의 머리를 담을 버퍼와 필드 자리; 그 밖에는 아무것도 쥐지 않는다. */
    proven_http_multipart_t r;
    static proven_byte_t head[1024];
    proven_http_header_t fields[8];
    EXAMPLE_REQUIRE(proven_http_multipart_init(&r, boundary, (proven_mem_mut_t){ head, sizeof head }, fields, 8) == PROVEN_OK, "읽개");

    /* 느린 연결이 건네듯 일곱 바이트씩 먹인다. */
    char title[32] = { 0 }, file[64] = { 0 }, filename[32] = { 0 };
    char *into = NULL;
    proven_size_t into_len = 0, into_cap = 0;
    for (proven_size_t at = 0; at < sizeof body - 1; at += 7) {
        proven_mem_view_t in = { (const proven_byte_t *)body + at, sizeof body - 1 - at < 7 ? sizeof body - 1 - at : 7 };
        while (in.size > 0) {
            proven_size_t used = 0;
            proven_http_multipart_event_t ev;
            EXAMPLE_REQUIRE(proven_http_multipart_feed(&r, in, &used, &ev) == PROVEN_OK, "조각을 읽는다");
            in.ptr += used;
            in.size -= used;
            if (ev.kind == PROVEN_HTTP_MULTIPART_PART) {
                /* 어느 필드인가? Content-Disposition이 말해 준다. */
                proven_u8str_view_t cd = { 0 };
                for (proven_size_t i = 0; i < ev.field_count; ++i)
                    if (proven_u8str_view_eq_ascii_ci(ev.fields[i].name, PROVEN_LIT("Content-Disposition"))) cd = ev.fields[i].value;
                proven_http_disposition_t d;
                EXAMPLE_REQUIRE(proven_http_content_disposition_parse(cd, &d) == PROVEN_OK && d.has_name, "파트마다 필드 이름이 있다");
                into = view_is(d.name, "title") ? title : file;
                into_cap = view_is(d.name, "title") ? sizeof title - 1 : sizeof file - 1;
                into_len = 0;
                if (d.has_filename && d.filename.size < sizeof filename) memcpy(filename, d.filename.ptr, d.filename.size);
            } else if (ev.kind == PROVEN_HTTP_MULTIPART_DATA) {
                /* 파트의 바이트, 한 조각씩: 여기서는 모아 두고, 파일이라면 디스크로 보낸다. */
                EXAMPLE_REQUIRE(into && into_len + ev.data.size <= into_cap, "파트가 마련해 둔 자리에 들어간다");
                memcpy(into + into_len, ev.data.ptr, ev.data.size);
                into_len += ev.data.size;
            }
        }
    }
    EXAMPLE_REQUIRE(proven_http_multipart_end(&r) == PROVEN_OK, "본문이 온전했다: 닫는 경계가 왔다");
    EXAMPLE_REQUIRE(strcmp(title, "Holiday") == 0 && strcmp(filename, "beach.txt") == 0 && strcmp(file, "sand\r\n--and sea") == 0,
                    "제목, 파일 이름, 파일 바이트 - 그 안의 CR LF와 대시 둘까지");

    /* 여러 범위: 합치고 정렬한 뒤 multipart/byteranges 본문으로. */
    static const char doc[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    proven_http_range_t ranges[4];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_range_parse_list(PROVEN_LIT("bytes=30-, 0-3, 2-5"), sizeof doc - 1, ranges, 4, &n) == PROVEN_OK &&
                    n == 2 && ranges[0].first == 0 && ranges[0].last == 5 && ranges[1].first == 30 && ranges[1].last == 35,
                    "0-3과 2-5는 겹쳐서 0-5가 되고, 30부터 끝까지는 30-35");
    static proven_byte_t buf[512];
    proven_mem_mut_t out = { buf, sizeof buf };
    proven_size_t len = 0;
    const proven_u8str_view_t b = PROVEN_LIT("ranges-1f0b");
    static proven_byte_t ct[96];
    proven_size_t ctl = 0;
    EXAMPLE_REQUIRE(proven_http_byteranges_write_content_type((proven_mem_mut_t){ ct, sizeof ct }, &ctl, b) == PROVEN_OK, "206 자신의 Content-Type: 경계가 붙은 multipart/byteranges");
    proven_err_t e = PROVEN_OK;
    for (proven_size_t i = 0; i < n && e == PROVEN_OK; ++i) {
        e = proven_http_byteranges_write_part(out, &len, b, PROVEN_LIT("text/plain"), ranges[i].first, ranges[i].last, sizeof doc - 1);
        const proven_size_t k = (proven_size_t)(ranges[i].last - ranges[i].first + 1);
        if (e == PROVEN_OK && len + k <= sizeof buf) { memcpy(buf + len, doc + ranges[i].first, k); len += k; }
        if (e == PROVEN_OK) e = proven_http_multipart_write_part_end(out, &len);
    }
    if (e == PROVEN_OK) e = proven_http_multipart_write_end(out, &len, b);
    EXAMPLE_REQUIRE(e == PROVEN_OK && len > 0, "206의 본문: 범위마다 Content-Range가 붙은 파트");
    const char *second = "--ranges-1f0b\r\nContent-Type: text/plain\r\nContent-Range: bytes 30-35/36\r\n\r\nuvwxyz\r\n";
    EXAMPLE_REQUIRE(holds(buf, len, second), "두 번째 파트, 쓰인 그대로");
    return EXAMPLE_OK();
}
