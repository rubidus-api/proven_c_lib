#include "example.h"

/*
 * 쓰임새로 나눈 해시. 이 모듈은 할 일마다 함수를 정확히 하나씩 준다. 그러니 고를 것은
 * "내가 할 일이 무엇인가" 하나뿐이고, *그것*을 틀리는 것이 위험의 전부다.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* 할 일 1: 믿을 수 있는 입력을 내 표의 자리로. 빠르고, 암호용이 아니다. */
    proven_u64 table_hash = proven_hash_bytes(data);
    EXAMPLE_REQUIRE(table_hash != 0, "FNV-1a produces a spread-out 64-bit value");

    /* 할 일 2: *믿을 수 없는* 입력을 표의 자리로. 목적은 같지만, 입력을 고르는 공격자도
     * 모두를 충돌시킬 수 없다. 키를 모르기 때문이다. 키는 시작할 때 진짜 난수로 한 번
     * 고른다. 고정된 키를 쓰면 이 방법의 뜻이 사라진다. */
    proven_byte_t key[16] = { 0 };   /* 실제 코드에서는 난수원에서 한 번 채운다 */
    proven_u64 safe_hash = proven_hash_keyed(data, key);
    EXAMPLE_REQUIRE(safe_hash != table_hash, "a keyed hash is a different function");

    /* 할 일 3: 이 바이트들이 상했는가? 해시가 아니라 검사합이다. gzip/zlib/PNG 와
     * 그대로 맞물린다 - 셋 다 정확히 이 CRC-32 를 쓴다. */
    proven_u32 checksum = proven_crc32(data);
    /* 널리 쓰이는 CRC-32 확인값이다. 진짜 그 함수라는 것을 눈으로 볼 수 있다. */
    EXAMPLE_REQUIRE(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
                    "CRC-32 of \"123456789\" is the shared check value");
    (void)checksum;

    /* 할 일 4: 내용의 지문 - 중복 제거, 내용 주소화, "이 둘이 같은 파일인가". 일치를
     * 위조하려는 상대 앞에서도 안전하게 답한다. 답이 속아 넘어가면 안 되는 자리에서
     * 집는 것이 이것이다. */
    proven_byte_t digest[PROVEN_SHA256_SIZE];
    proven_sha256(data, digest);

    char hex[65];
    proven_sha256_to_hex(digest, hex);
    /* sha256sum 과 git 이 찍는 것과 같은 표기라, 그대로 맞물린다. */
    EXAMPLE_REQUIRE(hex[64] == '\0' && proven_cstr_len(hex) == 64,
                    "a SHA-256 fingerprint is 64 lowercase hex characters");

    /* SHA-256 은 흘려 넣을 수도 있다. 한 번에 기억에 담을 수 없는 내용을 위해서다 -
     * 다이제스트는 바이트에만 달렸지, 몇 토막으로 나눠 넣었는지에는 달리지 않는다. */
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("the quick ")));
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("brown fox")));
    proven_byte_t streamed[PROVEN_SHA256_SIZE];
    proven_sha256_final(&ctx, streamed);

    bool same = true;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        if (streamed[i] != digest[i]) same = false;
    }
    EXAMPLE_REQUIRE(same, "two updates of the halves equal one hash of the whole");

    return EXAMPLE_OK();
}
