#include "example.h"

/*
 * 주소는 평범한 값이다. 만들고, 텍스트에서 파싱하고, 찍고, 둘을 비교한다.
 * 이 프로그램의 어느 줄도 소켓을 열거나 네트워크에 묻지 않는다.
 */

static bool prints_as(const proven_net_addr_t *addr, proven_u8str_view_t want) {
    proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
    proven_size_t n = 0;
    if (proven_net_addr_format(addr, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return false;
    return proven_u8str_view_eq((proven_u8str_view_t){ text, n }, want);
}

int main(void) {
    /* 부품으로 만든다. 포트는 종이에 적는 그 숫자다 - 호스트 바이트 순서. */
    proven_net_addr_t web = proven_net_addr_ipv4(192, 0, 2, 10, 80);
    EXAMPLE_REQUIRE(prints_as(&web, PROVEN_LIT("192.0.2.10:80")), "an IPv4 address prints as address:port");

    /* 프로그램에 가장 자주 필요한 고정 주소 둘: 이 기계만, 그리고 모든 인터페이스. */
    proven_net_addr_t local = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 8080);
    proven_net_addr_t everywhere = proven_net_addr_any(PROVEN_NET_FAMILY_IPV6, 8080);
    EXAMPLE_REQUIRE(prints_as(&local, PROVEN_LIT("127.0.0.1:8080")), "loopback is 127.0.0.1");
    EXAMPLE_REQUIRE(prints_as(&everywhere, PROVEN_LIT("[::]:8080")), "any is ::, and IPv6 is bracketed before a port");

    /* 텍스트에서 파싱한다. IPv6 주소 하나의 여러 표기가 같은 바이트로 파싱되고, 나갈 때의
     * 표기는 정확히 하나다. 그래서 텍스트를 비교하고 키로 쓸 수 있다. */
    proven_net_addr_t a, b;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("2001:DB8:0:0:0:0:0:1"), 443, &a) == PROVEN_OK, "the long form parses");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("[2001:db8::1]"), 443, &b) == PROVEN_OK, "the short, bracketed form parses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&a, &b), "and they are the same address");
    EXAMPLE_REQUIRE(prints_as(&a, PROVEN_LIT("[2001:db8::1]:443")), "printed in the one canonical form");

    /* 주소가 아닌 것은 거절한다 - 다른 파서들이 조용히 받아 주는 것까지. */
    proven_net_addr_t bad;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("010.0.0.1"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a leading zero is refused, not read as octal");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("example.com"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a name is not a literal: that is proven_net_resolve's job");

    /* 주소가 패킷이나 파일에서 올 때는 네트워크 순서의 원시 바이트 열여섯 개로. */
    proven_byte_t raw[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    proven_net_addr_t link_local = proven_net_addr_ipv6(raw, 22, 3);
    EXAMPLE_REQUIRE(prints_as(&link_local, PROVEN_LIT("[fe80::1%3]:22")), "a link-local address carries its zone");

    /* 유닉스 도메인 주소는 파일 시스템 경로다. */
    proven_net_addr_t sock;
    EXAMPLE_REQUIRE(proven_net_addr_unix(PROVEN_LIT("/run/app.sock"), &sock) == PROVEN_OK, "a path makes an address");
    EXAMPLE_REQUIRE(prints_as(&sock, PROVEN_LIT("/run/app.sock")), "and prints as the path");

    /* 리터럴의 해석은 질의 없이 답하므로, 이 줄은 네트워크가 없어도 동작한다.
     * 진짜 이름은 시스템 리졸버로 가고, 블록하며, 기한이 없다. 그래도 되는 자리에서
     * 불러라. */
    proven_net_addr_t found[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_resolve(PROVEN_LIT("192.0.2.10"), 80, found, 4, &count) == PROVEN_OK && count == 1,
                    "a literal resolves to itself");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&found[0], &web), "the same address as the one built by hand");

    return EXAMPLE_OK();
}
