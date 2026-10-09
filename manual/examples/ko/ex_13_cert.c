#include "example.h"
#include <string.h>

/*
 * 인증서는 어떤 키가 어떤 이름의 것이라는 서명된 진술이다. 여기서는 하나를 읽고, 어느 이름을 위한
 * 것인지 묻고, 믿을지 결정한다: 신뢰 앵커까지의 체인, 날짜, 그리고 이름 - 무엇이 실패했는지
 * 말해 주는 에러와 함께.
 */

/* 이 예제를 위해 만든 루트와, 그 루트가 example.test에 발급한 인증서. Ed25519라서 짧다.
 * 2026년부터 2036년까지 유효하다. */
static const char ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBAjCBtaADAgECAhQxTiZoH2JgH7bPWBktoxUyd0d3ZzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMRXhhbXBsZSBSb290MCowBQYDK2VwAyEAJcpfTb5GYzM+\n"
    "bgKssEvuWEC6urqf1Znqz9I8CY2RdLujEzARMA8GA1UdEwEB/wQFMAMBAf8wBQYD\n"
    "K2VwA0EARcXC/GhPDj4gAU3zpeSAhJ033zALpz2P91wtEqbf2R4Z7drW+RSRQGp2\n"
    "Ky/CDeEyx//WjylXEoHVNvixwKrxAw==\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char LEAF_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBPTCB8KADAgECAhRBU6wAvGvpDo28++Tq/BfZ/8DkNzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMZXhhbXBsZS50ZXN0MCowBQYDK2VwAyEA3dq7G1ndiCbb\n"
    "IAS6O7cRIlxdACAdLSsPSRROKyglnTOjTjBMMAwGA1UdEwEB/wQCMAAwJwYDVR0R\n"
    "BCAwHoIMZXhhbXBsZS50ZXN0gg4qLmV4YW1wbGUudGVzdDATBgNVHSUEDDAKBggr\n"
    "BgEFBQcDATAFBgMrZXADQQBVDpl9ZAqf4j2R3NTG8JuKfPJuh+PD99QVdpjnqPAR\n"
    "dgg3sGIps+UTADlifcSOVlqJF6T9ya5lHpO5FwIpLpMI\n"
    "-----END CERTIFICATE-----\n"
    ;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- PEM: 바이트를 감싼 텍스트 ------------------------------------------------
    /* 인증서는 PEM으로 다닌다: BEGIN과 END 줄 사이의 Base64. proven_pem_next는 다음 블록을 찾아
     * 디코딩한다. 안의 바이트는 DER이고, 나머지 모든 호출이 받는 것이 그것이다. */
    proven_byte_t leaf_der[1024];
    proven_size_t pos = 0, leaf_len = 0;
    proven_u8str_view_t label;
    proven_mem_view_t leaf_pem = { (const proven_byte_t *)LEAF_PEM, sizeof LEAF_PEM - 1 };
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, sizeof leaf_der }, &leaf_len) == PROVEN_OK,
                    "블록이 디코딩된다");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(label, PROVEN_LIT("CERTIFICATE")), "그리고 무엇인지 말한다");
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, 0 }, &(proven_size_t){ 0 }) == PROVEN_ERR_NOT_FOUND,
                    "두 번째 블록은 없다");

    // ---- 인증서 하나 읽기 ---------------------------------------------------------
    /* 파싱은 아무것도 복사하지 않는다: proven_cert_t의 모든 필드는 leaf_der를 가리키는 뷰이고,
     * 구조체를 쓰는 동안 leaf_der가 살아 있어야 한다. */
    proven_cert_t leaf;
    EXAMPLE_REQUIRE(proven_cert_parse((proven_mem_view_t){ leaf_der, leaf_len }, &leaf) == PROVEN_OK, "파싱된다");
    EXAMPLE_REQUIRE(leaf.version == 3 && leaf.key_kind == PROVEN_CERT_KEY_ED25519 && leaf.key.size == 32 && !leaf.is_ca,
                    "버전 3, Ed25519 키, CA 아님");
    EXAMPLE_REQUIRE(leaf.not_before < 1811808000 && leaf.not_after > 1811808000, "2027년 6월에 유효");

    proven_size_t at = 0, names = 0;
    proven_cert_name_kind_t kind;
    proven_mem_view_t name;
    while (proven_cert_alt_name_next(&leaf, &at, &kind, &name)) {
        if (kind == PROVEN_CERT_NAME_DNS) names++;
    }
    EXAMPLE_REQUIRE(names == 2, "subjectAltName에 DNS 이름 둘");

    /* 어느 호스트를 위한 것인가? subjectAltName만 센다. 와일드카드는 정확히 라벨 하나를 뜻한다. */
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("example.test")), "이름 그 자체");
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("www.example.test")), "와일드카드 아래 라벨 하나");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("a.b.example.test")), "둘은 아니다");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("example.org")), "다른 이름도 아니다");

    // ---- 신뢰 앵커 ----------------------------------------------------------------
    /* 저장소는 이미 믿는 인증서를 담는다. 받은 것을 복사하므로 ROOT_PEM은 이 뒤에 해제해도 된다.
     * 라이브러리는 루트를 싣고 다니지 않는다: 직접 공급한다. */
    proven_cert_store_t *anchors = NULL;
    proven_size_t added = 0;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK, "저장소");
    EXAMPLE_REQUIRE(proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &added) == PROVEN_OK &&
                    added == 1 && proven_cert_store_count(anchors) == 1, "번들에서 앵커 하나");

    // ---- 검증 ---------------------------------------------------------------------
    /* chain[0]은 상대의 인증서이고, 상대가 보낸 나머지가 순서 없이 뒤따른다. 시각은 호출자가
     * 공급한다: 스스로 시계를 읽는 라이브러리는 테스트할 수 없고, 시계가 없는 곳에서는 돌 수
     * 없다. */
    proven_mem_view_t chain[1] = { { leaf_der, leaf_len } };
    proven_cert_verify_options_t options = {
        .anchors = anchors,
        .host = PROVEN_LIT("www.example.test"),
        .now = 1811808000,                       /* 2027-06-01 */
        .use = PROVEN_CERT_USE_SERVER,
    };
    proven_cert_verify_result_t result;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK && result.depth == 2, "길이 2의 경로: 리프와 앵커");

    /* 틀리는 방식마다 답이 따로 있다. 에러 코드는 거친 답이고, 결과의 fault는 로그 한 줄을
     * 위한 것이다. */
    options.host = PROVEN_LIT("example.org");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_NAME_MISMATCH &&
                    result.fault == PROVEN_CERT_FAULT_NAME_MISMATCH, "체인은 좋고 이름은 아니다");
    options.host = PROVEN_LIT("www.example.test");
    options.now = 2127427200;                    /* 2037-06-01 */
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_EXPIRED, "십 년 뒤에는 만료되었다");
    options.now = 1811808000;
    leaf_der[leaf_len - 1] ^= 1;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_BAD_SIGNATURE, "비트 하나가 바뀌면 서명이 더는 검증되지 않는다");
    leaf_der[leaf_len - 1] ^= 1;

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "저장소");
    options.anchors = empty;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_NO_ISSUER, "앵커가 없으면 아무것도 믿지 않는다");

    // ---- 핀 ------------------------------------------------------------------------
    /* 핀은 인증서 공개 키(SubjectPublicKeyInfo)의 SHA-256이다. 어떤 키를 기대하는지 아는
     * 프로그램은 체인에 더해, 또는 체인 대신 이것을 비교한다. */
    proven_byte_t pin[32], again[32];
    EXAMPLE_REQUIRE(proven_cert_key_sha256(&leaf, pin) == PROVEN_OK, "핀");
    proven_sha256(leaf.public_key_info, again);
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ pin, 32 }, (proven_mem_view_t){ again, 32 }), "키 인코딩의 해시다");

    // ---- 시스템의 루트 ------------------------------------------------------------
    /* 공개 인터넷과 대화할 때 앵커는 운영체제의 것이다. 기계에 그것이 있는지는 기계의 사정이므로
     * 이 예제는 두 답을 모두 받아들인다. */
    proven_cert_store_t *system_roots = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &system_roots) == PROVEN_OK, "저장소");
    proven_err_t sys = proven_cert_store_add_system(system_roots, &added);
    EXAMPLE_REQUIRE(sys == PROVEN_OK || sys == PROVEN_ERR_NOT_FOUND, "시스템 저장소를 읽었거나, 없다");
    EXAMPLE_REQUIRE((sys == PROVEN_OK) == (proven_cert_store_count(system_roots) > 0), "읽었다면 비어 있지 않다");

    /* DER 인증서 하나도 더할 수 있다. */
    proven_byte_t root_der[1024];
    proven_size_t root_len = 0;
    pos = 0;
    EXAMPLE_REQUIRE(proven_pem_next((proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &pos, &label,
                                    (proven_mem_mut_t){ root_der, sizeof root_der }, &root_len) == PROVEN_OK, "블록이 디코딩된다");
    EXAMPLE_REQUIRE(proven_cert_store_add_der(empty, (proven_mem_view_t){ root_der, root_len }) == PROVEN_OK, "DER로 된 루트");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK, "이제 비어 있던 저장소가 리프를 믿는다");

    proven_cert_store_destroy(system_roots);
    proven_cert_store_destroy(empty);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
