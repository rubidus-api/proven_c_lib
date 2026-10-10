#include "example.h"
#include <string.h>

/*
 * 스트림으로서의 압축: 조각으로 들어와 조각으로 나가는 데이터를, 가진 크기 그대로의 버퍼로.
 * 같은 루프가 어떤 길이의 파일에도, 아직 만들어지고 있는 응답에도, 연결에도 통한다.
 */

/* 압축된 바이트가 가는 곳. 실제 프로그램이라면 파일이나 소켓에 쓴다. */
static proven_byte_t g_wire[8192];
static proven_size_t g_wire_len;

/* 압축기에 입력을 주고 가진 출력을 64바이트씩 받는다. 입력을 모두 가져갈 때까지 - 그리고
 * `flush`가 그 이상을 청하면 그것까지 모두 내놓을 때까지. */
static bool feed(proven_deflate_t *z, const char *text, proven_deflate_flush_t flush, bool *done) {
    proven_mem_view_t in = { (const proven_byte_t *)text, strlen(text) };
    for (;;) {
        proven_byte_t out[64];
        proven_size_t consumed = 0, produced = 0;
        if (proven_deflate(z, in, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, flush, done) != PROVEN_OK) return false;
        if (g_wire_len + produced > sizeof g_wire) return false;
        memcpy(g_wire + g_wire_len, out, produced);
        g_wire_len += produced;
        in.ptr += consumed; in.size -= consumed;
        /* 더 줄 것이 없고, 내놓은 것이 버퍼 하나에 못 미쳤다: 밀린 것이 없다. */
        if (in.size == 0 && produced < sizeof out) return true;
    }
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* 압축기는 한 번 만든다. 옵션의 0은 기본값을 뜻한다: 레벨 6, 32 KiB 윈도. 레벨 1은 더
     * 빠르고 조금 덜 압축하며, 9는 더 느리고 조금 더 압축한다. */
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_ZLIB, .level = 6 };
    proven_deflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &z) == PROVEN_OK, "압축기");

    /* 조각은 오는 대로 들어간다. FLUSH_NONE이면 압축기는 블록 하나만큼 모일 때까지 받은 것을
     * 쥐고 있다 - 오랫동안 아무것도 나오지 않을 수 있고, 그래서 잘 압축한다. */
    bool done = false;
    EXAMPLE_REQUIRE(feed(z, "first piece of the stream; ", PROVEN_DEFLATE_FLUSH_NONE, &done), "조각 하나가 들어간다");
    const proven_size_t after_first = g_wire_len;
    EXAMPLE_REQUIRE(after_first <= 2 && !done, "아직 두 바이트 헤더 말고는 나온 것이 없다");

    /* sync flush는 말한다: 상대가 여기까지를 지금 읽을 수 있어야 한다. 매번 몇 바이트가 든다 -
     * 누군가 기다릴 때 쓰고, 쓸 때마다 쓰지는 말 것. */
    EXAMPLE_REQUIRE(feed(z, "second piece of the stream; ", PROVEN_DEFLATE_FLUSH_SYNC, &done) && g_wire_len > after_first + 20 && !done,
                    "sync flush 뒤에는 두 조각 모두 선 위에 있다");
    const proven_size_t at_flush = g_wire_len;

    /* FINISH는 스트림을 끝낸다: 마지막 블록, 그리고 zlib와 gzip이라면 체크섬. */
    EXAMPLE_REQUIRE(feed(z, "third and last piece.", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "마지막 조각, 그리고 스트림이 끝났다");

    /* 받는 쪽: 10바이트씩 받아 16바이트 버퍼에 푸는 해제기. */
    proven_inflate_t *u = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_ZLIB, &u) == PROVEN_OK, "해제기");
    char text[200];
    proven_size_t text_len = 0, at = 0, seen_at_flush = 0;
    bool ended = false;
    /* 먼저 sync flush가 돌아왔을 때 쓰여 있던 것만, 그다음 나머지. */
    for (proven_size_t limit = at_flush; !ended; limit = g_wire_len) {
        for (;;) {
            proven_byte_t out[16];
            proven_size_t consumed = 0, produced = 0;
            const proven_size_t give = limit - at < 10 ? limit - at : 10;
            if (proven_inflate(u, (proven_mem_view_t){ g_wire + at, give }, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, &ended) != PROVEN_OK) break;
            if (text_len + produced > sizeof text) break;
            memcpy(text + text_len, out, produced);
            text_len += produced;
            at += consumed;
            /* 소비도 생산도 없는 PROVEN_OK: 받은 것을 다 쓰고 더 원한다. 오류가 아니다 -
             * 스트림은 그렇게 기다린다. */
            if (ended || (consumed == 0 && produced == 0)) break;
        }
        if (limit == at_flush) seen_at_flush = text_len;
        else break;
    }
    static const char whole[] = "first piece of the stream; second piece of the stream; third and last piece.";
    EXAMPLE_REQUIRE(ended && text_len == sizeof whole - 1 && memcmp(text, whole, text_len) == 0, "세 조각이 하나의 텍스트로 나온다");
    EXAMPLE_REQUIRE(at == g_wire_len, "스트림은 압축기가 쓰기를 멈춘 바로 그곳에서 끝났다");
    EXAMPLE_REQUIRE(seen_at_flush >= 55, "flush 지점에서 받는 쪽은 이미 앞의 두 조각을 모두 갖고 있었다");

    /* 두 객체는 다시 만들지 않고 다음 스트림에 다시 쓴다. */
    proven_deflate_reset(z);
    proven_inflate_reset(u);
    g_wire_len = 0;
    EXAMPLE_REQUIRE(feed(z, "another stream", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "같은 압축기, 새 스트림");
    proven_byte_t again[32];
    proven_size_t consumed = 0, produced = 0;
    EXAMPLE_REQUIRE(proven_inflate(u, (proven_mem_view_t){ g_wire, g_wire_len }, &consumed, (proven_mem_mut_t){ again, sizeof again }, &produced, &ended) == PROVEN_OK &&
                    ended && produced == 14 && memcmp(again, "another stream", 14) == 0, "같은 해제기가 그것을 읽는다");

    proven_deflate_destroy(z);
    proven_inflate_destroy(u);
    return EXAMPLE_OK();
}
