#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 코덱만으로 다루는 압축된 WebSocket 메시지: 소켓도 드라이버도 없다.
 *
 * permessage-deflate라는 확장은 세 가지로 이루어진다. 핸드셰이크가 그것에 합의한다.
 * 프레임 헤더의 비트 하나가 "이 메시지는 압축되었다"고 말한다. 그리고 페이로드는 마지막
 * 네 바이트를 뗀 DEFLATE다. 앞의 둘은 코덱이 하고, 셋째는 deflate.h가 한다.
 *
 * ws_conn.h나 ws_event.h를 쓰는 프로그램은 필드 하나를 정할 뿐 이것을 볼 일이 없다.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 핸드셰이크 ------------------------------------------------------------------
    /* 클라이언트는 헤더 하나로 요청한다. PROVEN_WS_DEFLATE_OFFER는 이 라이브러리가 말하는 형태다:
     * 양쪽 방향 모두, 메시지 하나하나를 따로 압축한다. */
    proven_http_header_t asked[] = {
        { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT(PROVEN_WS_DEFLATE_OFFER) },
    };
    proven_http_request_t request = { .headers = asked, .header_count = 1 };

    /* 서버는 제안된 것을 보고 답을 쓴다 - 또는 맞춰 줄 수 있는 것이 없으면
     * (PROVEN_ERR_NOT_FOUND) 연결은 압축 없이 이어진다. */
    proven_byte_t answer[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t answer_len = 0;
    int window_bits = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&request, (proven_mem_mut_t){ answer, sizeof answer }, &answer_len, &window_bits) == PROVEN_OK, "the offer can be met");
    printf("the server answers: %.*s\n", (int)answer_len, (const char *)answer);
    EXAMPLE_REQUIRE(window_bits == 15, "and may compress with the full window");

    /* 브라우저의 제안은 모양이 다르지만, 그것도 맞춰 준다. */
    proven_http_header_t browser[] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; client_max_window_bits") } };
    proven_http_request_t from_browser = { .headers = browser, .header_count = 1 };
    proven_size_t len2 = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&from_browser, (proven_mem_mut_t){ answer, sizeof answer }, &len2, &window_bits) == PROVEN_OK, "a browser's offer too");

    /* 클라이언트는 답 전체를 검사한다: 늘 보던 것들, 그리고 확장이 요청한 형태로 돌아왔는지.
     * (키와 accept 값은 RFC 6455에 실린 한 쌍이다.) */
    proven_http_header_t answered[] = {
        { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") },
        { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
        { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
        { PROVEN_LIT("Sec-WebSocket-Extensions"), { answer, answer_len } },
    };
    proven_http_response_t response = { .status = 101, .headers = answered, .header_count = 4 };
    proven_u8str_view_t protocol;
    bool compressed = false;
    EXAMPLE_REQUIRE(proven_ws_check_response_deflate(&response, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &compressed) == PROVEN_OK && compressed,
                    "the client finds the extension agreed");

    // ---- 압축된 메시지가 도착한다 ----------------------------------------------------
    /* 이 프레임은 RFC 7692에 있다: 서버가 보낸, 압축된 텍스트 "Hello". 압축하지 않은 텍스트
     * 프레임이라면 81일 첫 바이트가 c1이다: 더해진 비트가 RSV1이다. */
    proven_byte_t wire[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };

    proven_ws_decoder_t decoder;
    proven_ws_event_t event;
    proven_size_t used = 0;
    /* 확장에 대해 듣지 못한 디코더는 그 비트를 거부한다. 그래야 한다. */
    proven_byte_t copy[sizeof wire];
    memcpy(copy, wire, sizeof wire);
    proven_ws_decoder_init(&decoder, false, 0);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ copy, sizeof copy }, &used, &event) == PROVEN_ERR_INVALID_FORMAT, "without the extension, the bit is an error");

    proven_ws_decoder_init(&decoder, false, 0);
    proven_ws_decoder_allow_compressed(&decoder);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ wire, sizeof wire }, &used, &event) == PROVEN_OK &&
                    event.kind == PROVEN_WS_EVENT_DATA && event.compressed && event.last, "with it, the message is handed over, marked compressed");

    /* 디코더는 압축을 풀지 않는다: 아무것도 할당하지 않는다. 건네주는 바이트는 raw DEFLATE이고,
     * 보낸 쪽이 마지막 네 바이트 - 언제나 00 00 ff ff - 를 떼어 냈다. 그것을 다시 붙이고
     * 푼다. 나오는 양의 한도는 여러분 버퍼의 크기다. */
    proven_inflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "a decompressor for raw DEFLATE");
    static const proven_byte_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    proven_byte_t text[64];
    proven_size_t taken = 0, made = 0, total = 0;
    bool ended = false;
    EXAMPLE_REQUIRE(proven_inflate(z, event.data, &taken, (proven_mem_mut_t){ text, sizeof text }, &made, &ended) == PROVEN_OK && taken == event.data.size, "the payload");
    total = made;
    EXAMPLE_REQUIRE(proven_inflate(z, (proven_mem_view_t){ tail, 4 }, &taken, (proven_mem_mut_t){ text + total, sizeof text - total }, &made, &ended) == PROVEN_OK && taken == 4, "then the four bytes");
    total += made;
    EXAMPLE_REQUIRE(total == 5 && memcmp(text, "Hello", 5) == 0, "and the message is Hello");
    printf("the message: %.*s\n", (int)total, (const char *)text);
    /* 이제야 - 그 전이 아니라 - 텍스트이고, 검사는 여러분 몫이다: 디코더는 할 수 없었다. */
    proven_inflate_destroy(z);

    // ---- 압축된 메시지를 보낸다 ------------------------------------------------------
    /* 반대 방향: sync flush로 압축하면 같은 네 바이트로 끝나는데, 그것을 떼어 낸다.
     * 이것을 싣는 프레임에는 `rsv1`이 켜져 있다. */
    static const char said[] = "to be, or not to be, that is the question: to be, or not to be";
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_RAW };
    proven_deflate_t *c = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &c) == PROVEN_OK, "a compressor");
    proven_byte_t packed[128];
    EXAMPLE_REQUIRE(proven_deflate(c, (proven_mem_view_t){ (const proven_byte_t *)said, sizeof said - 1 }, &taken, (proven_mem_mut_t){ packed, sizeof packed }, &made,
                                   PROVEN_DEFLATE_FLUSH_SYNC, &ended) == PROVEN_OK && taken == sizeof said - 1 && made > 4, "compressed, with a sync flush");
    EXAMPLE_REQUIRE(memcmp(packed + made - 4, tail, 4) == 0, "which ends in 00 00 ff ff");
    proven_size_t payload = made - 4;
    proven_deflate_destroy(c);

    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .rsv1 = true, .length = payload };
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t header_len = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_write((proven_mem_mut_t){ header, sizeof header }, &header_len, &frame) == PROVEN_OK && header[0] == 0xc1, "a frame header with the bit");
    printf("%u bytes of text go out as %u\n", (unsigned)(sizeof said - 1), (unsigned)(header_len + payload));
    EXAMPLE_REQUIRE(header_len + payload < sizeof said - 1, "fewer than were said");
    return EXAMPLE_OK();
}
