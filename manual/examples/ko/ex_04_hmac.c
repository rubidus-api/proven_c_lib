#include "example.h"
#include <string.h>

/*
 * 다이제스트는 바이트가 무엇인지 말한다. MAC은 누가 그것을 보증했는지 말하고, KDF는 비밀 하나를
 * 프로토콜에 필요한 여러 키로 바꾼다. 그리고 어떤 비밀에든 따라붙는 호출 둘: 새지 않는 비교와,
 * 최적화로 지워지지 않는 지우기.
 */

static bool hex_is(const proven_byte_t *got, proven_size_t n, const char *want) {
    char text[129];
    proven_size_t len = 0;
    return proven_hex_encode((proven_mem_view_t){ got, n }, (proven_byte_t *)text, sizeof text, &len) == PROVEN_OK &&
           len == strlen(want) && memcmp(text, want, len) == 0;
}

int main(void) {
    // ---- SHA-512와 SHA-384 ---------------------------------------------------
    /* SHA-256과 같은 모양이다: 한 번에, 또는 메모리에 다 들어가지 않는 것에는 init / update / final.
     * SHA-384는 시작 값이 다른 SHA-512를 48바이트로 자른 것이다. */
    proven_byte_t d512[PROVEN_SHA512_SIZE], d384[PROVEN_SHA384_SIZE], again[PROVEN_SHA512_SIZE];
    proven_sha512(proven_mem_view_from_u8(PROVEN_LIT("abc")), d512);
    EXAMPLE_REQUIRE(hex_is(d512, 8, "ddaf35a193617aba"), "SHA-512 of abc begins as FIPS 180-4 prints it");
    proven_sha384(proven_mem_view_from_u8(PROVEN_LIT("abc")), d384);
    EXAMPLE_REQUIRE(hex_is(d384, 8, "cb00753f45a35e8b"), "and SHA-384");

    proven_sha512_t running;
    proven_sha512_init(&running);
    proven_sha512_update(&running, proven_mem_view_from_u8(PROVEN_LIT("a")));
    proven_sha512_update(&running, proven_mem_view_from_u8(PROVEN_LIT("bc")));
    proven_sha512_final(&running, again);
    EXAMPLE_REQUIRE(memcmp(d512, again, sizeof d512) == 0, "in pieces, the same digest");

    proven_sha384_t running384;
    proven_sha384_init(&running384);
    proven_sha384_update(&running384, proven_mem_view_from_u8(PROVEN_LIT("abc")));
    proven_sha384_final(&running384, again);
    EXAMPLE_REQUIRE(memcmp(d384, again, sizeof d384) == 0, "and for SHA-384");

    // ---- HMAC: 키를 가진 누군가가 보증한 메시지 ---------------------------------
    /* 서버가 토큰에 서명하고, 나중에 돌아온 것이 자기가 발급한 그것인지 확인한다. */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key), "a key from the system: 32 bytes for a 32-byte MAC");
    proven_mem_view_t token = proven_mem_view_from_u8(PROVEN_LIT("user=ada;expires=1767225600"));
    proven_byte_t mac[PROVEN_HMAC_MAX_SIZE];
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }, token, mac) == PROVEN_OK, "the token's MAC");
    EXAMPLE_REQUIRE(proven_hmac_size(PROVEN_HMAC_SHA256) == 32, "32 bytes of it; the buffer is sized for the largest");

    /* 확인하기: 다시 계산해서, 둘이 어디서 다른지 흘리지 "않고" 비교한다. memcmp는 처음 틀린
     * 바이트에서 멈추고, 걸린 시간이 위조자에게 어디까지 맞았는지 알려 준다. */
    proven_byte_t check[PROVEN_HMAC_MAX_SIZE];
    proven_hmac_t h;
    EXAMPLE_REQUIRE(proven_hmac_init(&h, PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }) == PROVEN_OK, "the streaming form, for a message in pieces");
    proven_hmac_update(&h, proven_mem_view_from_u8(PROVEN_LIT("user=ada;")));
    proven_hmac_update(&h, proven_mem_view_from_u8(PROVEN_LIT("expires=1767225600")));
    proven_hmac_final(&h, check);                   /* `h`에서 키도 지운다 */
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ mac, 32 }, (proven_mem_view_t){ check, 32 }), "the token is genuine");

    /* 누군가 토큰을 바꾼다. 그에 맞는 MAC은 만들 수 없다. */
    proven_mem_view_t forged = proven_mem_view_from_u8(PROVEN_LIT("user=eve;expires=1767225600"));
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ key, sizeof key }, forged, check) == PROVEN_OK &&
                    !proven_mem_equal_ct((proven_mem_view_t){ mac, 32 }, (proven_mem_view_t){ check, 32 }), "a changed token does not carry the old MAC");

    /* 공개된 벡터 하나. 이것이 남들이 계산하는 그 HMAC임을 보이려고: RFC 4231, 2번 사례. */
    EXAMPLE_REQUIRE(proven_hmac(PROVEN_HMAC_SHA256, proven_mem_view_from_u8(PROVEN_LIT("Jefe")),
                                proven_mem_view_from_u8(PROVEN_LIT("what do ya want for nothing?")), check) == PROVEN_OK &&
                    hex_is(check, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231, test case 2");

    // ---- HKDF: 비밀 하나, 키 여럿 ----------------------------------------------
    /* 양쪽이 공유 비밀에 합의했다. 그것은 아직 키가 아니다: 용도마다 레이블에 묶인 키를 하나씩
     * 유도해서, 하나를 알아도 다른 것에 대해 알 수 있는 것이 없게 한다. */
    proven_byte_t shared[32];
    EXAMPLE_REQUIRE(proven_random_bytes(shared, sizeof shared), "stands for the output of a key exchange");
    proven_mem_view_t secret = { shared, sizeof shared };
    proven_mem_view_t salt = proven_mem_view_from_u8(PROVEN_LIT("example-protocol v1"));       /* 비밀이 아니다. 비어도 된다 */

    proven_byte_t enc_key[32], mac_key[32];
    EXAMPLE_REQUIRE(proven_hkdf(PROVEN_HMAC_SHA256, salt, secret, proven_mem_view_from_u8(PROVEN_LIT("encryption")), (proven_mem_mut_t){ enc_key, sizeof enc_key }) == PROVEN_OK &&
                    proven_hkdf(PROVEN_HMAC_SHA256, salt, secret, proven_mem_view_from_u8(PROVEN_LIT("authentication")), (proven_mem_mut_t){ mac_key, sizeof mac_key }) == PROVEN_OK,
                    "two keys from one secret");
    EXAMPLE_REQUIRE(!proven_mem_equal_ct((proven_mem_view_t){ enc_key, 32 }, (proven_mem_view_t){ mac_key, 32 }), "different labels, unrelated keys");

    /* 두 단계를 따로: 한 번 추출하고, 필요한 만큼 확장한다. */
    proven_byte_t prk[PROVEN_HMAC_MAX_SIZE], enc_again[32];
    EXAMPLE_REQUIRE(proven_hkdf_extract(PROVEN_HMAC_SHA256, salt, secret, prk) == PROVEN_OK &&
                    proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, proven_mem_view_from_u8(PROVEN_LIT("encryption")),
                                       (proven_mem_mut_t){ enc_again, sizeof enc_again }) == PROVEN_OK &&
                    proven_mem_equal_ct((proven_mem_view_t){ enc_key, 32 }, (proven_mem_view_t){ enc_again, 32 }), "extract then expand is the same derivation");

    /* HKDF는 해시의 255블록까지만 준다. 더 달라고 하면 잘라 주는 것이 아니라 거절한다. */
    static proven_byte_t too_much[255 * 32 + 1];
    EXAMPLE_REQUIRE(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, proven_mem_view_from_u8(PROVEN_LIT("")),
                                       (proven_mem_mut_t){ too_much, sizeof too_much }) == PROVEN_ERR_OUT_OF_BOUNDS, "more than 8160 bytes from SHA-256: refused");

    // ---- 비밀을 다 썼을 때 -----------------------------------------------------
    /* 지운다. 여기에 평범한 루프를 쓰면 최적화기가 삭제한다 - 그 뒤로 버퍼를 읽는 것이 없으니까 - .
     * 그러면 키가 메모리에 남는다. 이 쓰기는 지워지지 않는다. */
    proven_mem_wipe((proven_mem_mut_t){ key, sizeof key });
    proven_mem_wipe((proven_mem_mut_t){ shared, sizeof shared });
    proven_mem_wipe((proven_mem_mut_t){ prk, sizeof prk });
    proven_mem_wipe((proven_mem_mut_t){ enc_key, sizeof enc_key });
    proven_mem_wipe((proven_mem_mut_t){ mac_key, sizeof mac_key });
    proven_byte_t zeros[32] = {0};
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ key, 32 }, (proven_mem_view_t){ zeros, 32 }), "the key is gone from its buffer");

    return EXAMPLE_OK();
}
