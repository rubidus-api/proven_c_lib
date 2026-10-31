#include "example.h"
#include <string.h>

/*
 * 사전을 쓴 Zstandard: 서로 닮은 짧은 메시지를 각각 한 프레임으로, 양쪽이 같은 과거에서 시작한다.
 * 여기서는 사전이 원시 사전이다 - 전형적인 메시지 몇 개를 그대로 - 그래서 학습이 필요 없다;
 * `zstd --train`이 만드는 형식 사전도 같은 방법으로 쓴다.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char dict[] =
        "{\"event\": \"login\", \"user\": \"alice\", \"ok\": true, \"region\": \"eu-west\"}\n"
        "{\"event\": \"logout\", \"user\": \"bob\", \"ok\": true, \"region\": \"us-east\"}\n"
        "{\"event\": \"purchase\", \"user\": \"carol\", \"ok\": false, \"region\": \"ap-south\"}\n";
    static const char msg[] = "{\"event\": \"login\", \"user\": \"dave\", \"ok\": false, \"region\": \"us-east\"}\n";
    const proven_mem_view_t dv = { (const proven_byte_t *)dict, sizeof dict - 1 };
    EXAMPLE_REQUIRE(proven_zstd_dictionary_id(dv) == 0, "원시 사전에는 ID가 없다");

    /* 메시지 하나를 사전과 함께, 그리고 사전 없이. */
    static proven_byte_t frame[256], plain[256], back[256];
    proven_size_t n = 0, p = 0;
    proven_zstd_encoder_options_t o = { .alloc = heap, .level = 3, .dictionary = dv };
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ (const proven_byte_t *)msg, sizeof msg - 1 }, (proven_mem_mut_t){ frame, sizeof frame }, &n) == PROVEN_OK,
                    "사전으로 압축한 메시지");
    o.dictionary = (proven_mem_view_t){ NULL, 0 };
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ (const proven_byte_t *)msg, sizeof msg - 1 }, (proven_mem_mut_t){ plain, sizeof plain }, &p) == PROVEN_OK,
                    "사전 없이");
    EXAMPLE_REQUIRE(n * 2 < p, "사전이 있으면 73바이트가 사전 없을 때의 절반도 안 된다");

    /* 받는 쪽: 같은 사전을 디코더에 한 번 걸어 두면 그 뒤의 모든 프레임에 쓰인다. */
    proven_zstd_decoder_t *z = NULL;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &z) == PROVEN_OK && proven_zstd_decoder_use_dictionary(z, dv) == PROVEN_OK, "사전을 건 디코더");
    for (int round = 0; round < 2; ++round) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ frame, n }, &used, (proven_mem_mut_t){ back, sizeof back }, &made, &done) == PROVEN_OK && done &&
                        made == sizeof msg - 1 && memcmp(back, msg, made) == 0, "메시지가 돌아온다 - 같은 디코더에서 한 번 더");
    }
    /* 사전 없이는 되돌릴 수 없다: 프레임에는 같은 과거가 필요하다. */
    EXAMPLE_REQUIRE(proven_zstd_decoder_use_dictionary(z, (proven_mem_view_t){ NULL, 0 }) == PROVEN_OK, "사전을 뗀다");
    proven_size_t used = 0, made = 0;
    bool done = false;
    const proven_err_t e = proven_zstd_decode(z, (proven_mem_view_t){ frame, n }, &used, (proven_mem_mut_t){ back, sizeof back }, &made, &done);
    EXAMPLE_REQUIRE(e != PROVEN_OK || made != sizeof msg - 1 || memcmp(back, msg, made) != 0, "원시 사전의 프레임은 사전을 밝히지 않으므로 다른 내용으로 풀리거나 실패한다");
    proven_zstd_decoder_destroy(z);
    return EXAMPLE_OK();
}
