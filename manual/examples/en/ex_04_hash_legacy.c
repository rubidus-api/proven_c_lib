#include "example.h"

/*
 * SHA-1 and MD5. Both are broken for anything an adversary can influence, and both are still
 * written into formats nobody can change - which is the only reason to call them.
 */

int main(void) {
    /* The WebSocket handshake (RFC 6455): the server proves it read the client's key by
     * returning SHA-1(key + a fixed GUID), in Base64. The format says SHA-1, so SHA-1 it is. */
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

    /* One-shot, and the hex spelling sha1sum and git print. */
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("abc"));
    proven_sha1(data, sha1);
    char sha1_hex[41];
    proven_sha1_to_hex(sha1, sha1_hex);
    EXAMPLE_REQUIRE(proven_cstr_len(sha1_hex) == 40, "a SHA-1 digest is 40 hex characters");

    /* MD5: checking a download against a checksum file published as md5sum output. That
     * catches a damaged download. It does not catch a forged one - anyone can build a second
     * file with the same MD5. */
    proven_byte_t md5[PROVEN_MD5_SIZE];
    proven_md5(data, md5);
    char md5_hex[33];
    proven_md5_to_hex(md5, md5_hex);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_view_from_cstr(md5_hex),
                                         PROVEN_LIT("900150983cd24fb0d6963f7d28e17f72")),
                    "the digest md5sum prints for \"abc\"");

    /* MD5 streams too; the digest depends only on the bytes. */
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
