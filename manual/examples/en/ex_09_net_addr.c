#include "example.h"

/*
 * Addresses are plain values: build one, parse one from text, print it, compare two.
 * Nothing in this program opens a socket or asks the network anything.
 */

static bool prints_as(const proven_net_addr_t *addr, proven_u8str_view_t want) {
    proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
    proven_size_t n = 0;
    if (proven_net_addr_format(addr, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return false;
    return proven_u8str_view_eq((proven_u8str_view_t){ text, n }, want);
}

int main(void) {
    /* Built from parts. The port is the number you would write down - host byte order. */
    proven_net_addr_t web = proven_net_addr_ipv4(192, 0, 2, 10, 80);
    EXAMPLE_REQUIRE(prints_as(&web, PROVEN_LIT("192.0.2.10:80")), "an IPv4 address prints as address:port");

    /* The two fixed addresses a program needs most: this machine only, and every interface. */
    proven_net_addr_t local = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 8080);
    proven_net_addr_t everywhere = proven_net_addr_any(PROVEN_NET_FAMILY_IPV6, 8080);
    EXAMPLE_REQUIRE(prints_as(&local, PROVEN_LIT("127.0.0.1:8080")), "loopback is 127.0.0.1");
    EXAMPLE_REQUIRE(prints_as(&everywhere, PROVEN_LIT("[::]:8080")), "any is ::, and IPv6 is bracketed before a port");

    /* Parsed from text. Many spellings of one IPv6 address parse to the same bytes, and there
     * is exactly one spelling on the way out, so the text can be compared and used as a key. */
    proven_net_addr_t a, b;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("2001:DB8:0:0:0:0:0:1"), 443, &a) == PROVEN_OK, "the long form parses");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("[2001:db8::1]"), 443, &b) == PROVEN_OK, "the short, bracketed form parses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&a, &b), "and they are the same address");
    EXAMPLE_REQUIRE(prints_as(&a, PROVEN_LIT("[2001:db8::1]:443")), "printed in the one canonical form");

    /* What is not an address is refused - including what other parsers quietly accept. */
    proven_net_addr_t bad;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("010.0.0.1"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a leading zero is refused, not read as octal");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("example.com"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a name is not a literal: that is proven_net_resolve's job");

    /* Sixteen raw bytes, in network order, when the address comes from a packet or a file. */
    proven_byte_t raw[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    proven_net_addr_t link_local = proven_net_addr_ipv6(raw, 22, 3);
    EXAMPLE_REQUIRE(prints_as(&link_local, PROVEN_LIT("[fe80::1%3]:22")), "a link-local address carries its zone");

    /* A Unix-domain address is a filesystem path. */
    proven_net_addr_t sock;
    EXAMPLE_REQUIRE(proven_net_addr_unix(PROVEN_LIT("/run/app.sock"), &sock) == PROVEN_OK, "a path makes an address");
    EXAMPLE_REQUIRE(prints_as(&sock, PROVEN_LIT("/run/app.sock")), "and prints as the path");

    /* Resolving a literal is answered without a query, so this line works with no network.
     * A real name goes to the system resolver, blocks, and has no deadline: call it where
     * that is acceptable. */
    proven_net_addr_t found[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_resolve(PROVEN_LIT("192.0.2.10"), 80, found, 4, &count) == PROVEN_OK && count == 1,
                    "a literal resolves to itself");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&found[0], &web), "the same address as the one built by hand");

    return EXAMPLE_OK();
}
