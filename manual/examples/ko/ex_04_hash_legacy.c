#include "example.h"

/*
 * SHA-1과 MD5. 둘 다 상대가 입력에 손댈 수 있는 곳에서는 깨진 해시이고, 둘 다 아무도 바꿀 수
 * 없는 형식에 여전히 적혀 있다. 부를 이유는 그것 하나뿐이다.
 */

int main(void) {
    /* WebSocket 핸드셰이크(RFC 6455): 서버는 클라이언트의 키를 읽었다는 증거로
     * SHA-1(키 + 고정 GUID)을 Base64로 돌려준다. 형식이 SHA-1이라고 하니 SHA-1이다. */
    proven_sha1_t ctx;
    proven_sha1_init(&ctx);
    proven_sha1_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ==")));
    proven_sha1_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
    proven_byte_t sha1[PROVEN_SHA1_SIZE];
    proven_sha1_final(&ctx, sha1);

    proven_byte_t accept[32];
    proven_size_t accept_len = 0;
    proven_err_t err = proven_base64_encode((proven_mem_view_t){ .ptr = sha1, .size = sizeof sha1 },
                                            accept, sizeof accept, &accept_len);
    EXAMPLE_REQUIRE(proven_is_ok(err), "20 bytes encode to 28 Base64 characters");
    EXAMPLE_REQUIRE(proven_u8str_view_eq((proven_u8str_view_t){ .ptr = accept, .size = accept_len },
                                         PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")),
                    "the accept key printed in RFC 6455 section 1.3");

    /* 한 번에 계산하기, 그리고 sha1sum과 git이 찍는 hex 표기. */
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("abc"));
    proven_sha1(data, sha1);
    char sha1_hex[41];
    proven_sha1_to_hex(sha1, sha1_hex);
    EXAMPLE_REQUIRE(proven_cstr_len(sha1_hex) == 40, "a SHA-1 digest is 40 hex characters");

    /* MD5: 내려받은 파일을 md5sum 출력으로 공개된 체크섬 파일과 맞춰 본다. 손상된 다운로드는
     * 잡는다. 위조된 것은 못 잡는다 - 같은 MD5를 가진 두 번째 파일은 누구나 만들 수 있다. */
    proven_byte_t md5[PROVEN_MD5_SIZE];
    proven_md5(data, md5);
    char md5_hex[33];
    proven_md5_to_hex(md5, md5_hex);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_view_from_cstr(md5_hex),
                                         PROVEN_LIT("900150983cd24fb0d6963f7d28e17f72")),
                    "the digest md5sum prints for \"abc\"");

    /* MD5도 스트림으로 먹일 수 있다. 다이제스트는 바이트에만 달려 있다. */
    proven_md5_t mctx;
    proven_md5_init(&mctx);
    proven_md5_update(&mctx, proven_mem_view_from_u8(PROVEN_LIT("a")));
    proven_md5_update(&mctx, proven_mem_view_from_u8(PROVEN_LIT("bc")));
    proven_byte_t streamed[PROVEN_MD5_SIZE];
    proven_md5_final(&mctx, streamed);

    bool same = true;
    for (proven_size_t i = 0; i < PROVEN_MD5_SIZE; ++i) {
        if (streamed[i] != md5[i]) same = false;
    }
    EXAMPLE_REQUIRE(same, "two updates of the pieces equal one hash of the whole");

    return EXAMPLE_OK();
}
