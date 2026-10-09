#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Address text, both ways. Nothing here opens a socket or asks the system anything: the parser
 * and the formatter are plain code, so every case is deterministic on every platform.
 *
 * The IPv6 cases follow RFC 4291 section 2.2 (what is accepted) and RFC 5952 section 4 (the
 * one form that is written). The refusals are the ones other parsers get wrong: inet_aton reads
 * "010" as octal and "1.2.3" as a shorthand, and both are refused here.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool formats_as(const proven_net_addr_t *a, const char *want) {
    proven_byte_t buf[PROVEN_NET_ADDR_TEXT_MAX];
    proven_size_t n = 0;
    if (proven_net_addr_format(a, (proven_mem_mut_t){ buf, sizeof buf }, &n) != PROVEN_OK) return false;
    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

static bool round_trip(const char *text, proven_u16 port, const char *want) {
    proven_net_addr_t a;
    if (proven_net_addr_parse(sv(text), port, &a) != PROVEN_OK) return false;
    return formats_as(&a, want);
}

static bool refused(const char *text) {
    proven_net_addr_t a;
    memset(&a, 0x5a, sizeof a);
    proven_net_addr_t before = a;
    if (proven_net_addr_parse(sv(text), 1, &a) != PROVEN_ERR_INVALID_FORMAT) return false;
    return memcmp(&a, &before, sizeof a) == 0;     /* a refusal writes nothing */
}

int main(void) {
    PROVEN_TEST_SUITE("net: addresses as text",
        "Literal IPv4 and IPv6 addresses parse to the bytes they name and print in one canonical form.",
        "Inspect internal_parse_ipv4, internal_parse_ipv6 and internal_put_ipv6 in src/proven/net.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("IPv4: four decimal numbers, nothing else",
        "Accepted forms give the right bytes; every shorthand and every out-of-range part is refused.",
        "Leading zeros are refused because inet_aton reads them as octal: \"010\" would be eight.");
    // ---------------------------------------------------------------
    {
        proven_net_addr_t a;
        PROVEN_TEST_ASSERT(proven_net_addr_parse(sv("192.0.2.1"), 8080, &a) == PROVEN_OK &&
                           a.family == PROVEN_NET_FAMILY_IPV4 && a.port == 8080 &&
                           a.ip[0] == 192 && a.ip[1] == 0 && a.ip[2] == 2 && a.ip[3] == 1,
            "192.0.2.1 parses to its four bytes, in order", "");
        PROVEN_TEST_ASSERT(round_trip("0.0.0.0", 0, "0.0.0.0:0"), "0.0.0.0 round-trips", "");
        PROVEN_TEST_ASSERT(round_trip("255.255.255.255", 65535, "255.255.255.255:65535"), "the largest address and port round-trip", "");
        PROVEN_TEST_ASSERT(round_trip("127.0.0.1", 80, "127.0.0.1:80"), "loopback round-trips", "");

        static const char *bad[] = {
            "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256", "01.2.3.4", "1.2.3.04", "1..2.3",
            ".1.2.3", "1.2.3.", "1.2.3.4 ", " 1.2.3.4", "1.2.3.a", "1,2,3,4", "0x7f.0.0.1",
            "1234.1.1.1", "127.1", "2130706433", "localhost", "1.2.3.-4",
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (!refused(bad[i])) { all = false; PROVEN_TEST_INFO("accepted, or wrote on refusal: \"{}\"", PROVEN_ARG((const char *)bad[i])); }
        }
        PROVEN_TEST_ASSERT(all, "every malformed IPv4 literal is PROVEN_ERR_INVALID_FORMAT and leaves the output untouched", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("IPv6: the forms of RFC 4291, written as RFC 5952 says",
        "Full, compressed, bracketed, zoned and IPv4-tailed forms parse; output is the one canonical spelling.",
        "RFC 5952 section 4: lowercase, no leading zeros, the longest run of two or more zero groups as \"::\", the first run on a tie.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *in; const char *out; } v[] = {
            { "::", "[::]:443" },
            { "::1", "[::1]:443" },
            { "1::", "[1::]:443" },
            { "2001:db8::1", "[2001:db8::1]:443" },
            { "2001:DB8:0:0:0:0:0:1", "[2001:db8::1]:443" },
            { "2001:0db8:0000:0000:0000:0000:0000:0001", "[2001:db8::1]:443" },
            { "[2001:db8::1]", "[2001:db8::1]:443" },
            { "2001:db8:0:1:1:1:1:1", "[2001:db8:0:1:1:1:1:1]:443" },      /* one zero group is not compressed */
            { "2001:0:0:1:0:0:0:1", "[2001:0:0:1::1]:443" },               /* the longer run wins */
            { "2001:db8:0:0:1:0:0:1", "[2001:db8::1:0:0:1]:443" },         /* a tie: the first run */
            { "1:2:3:4:5:6:7:8", "[1:2:3:4:5:6:7:8]:443" },
            { "1:2:3:4:5:6:7::", "[1:2:3:4:5:6:7:0]:443" },
            { "::2:3:4:5:6:7:8", "[0:2:3:4:5:6:7:8]:443" },
            { "fe80::1%3", "[fe80::1%3]:443" },
            { "[fe80::1%12]", "[fe80::1%12]:443" },
            { "::ffff:192.0.2.1", "[::ffff:192.0.2.1]:443" },
            { "::ffff:c000:201", "[::ffff:192.0.2.1]:443" },
            { "64:ff9b::192.0.2.33", "[64:ff9b::c000:221]:443" },
            { "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:443" },
        };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            if (!round_trip(v[i].in, 443, v[i].out)) { all = false; PROVEN_TEST_INFO("wrong for \"{}\" (want {})", PROVEN_ARG((const char *)v[i].in), PROVEN_ARG((const char *)v[i].out)); }
        }
        PROVEN_TEST_ASSERT(all, "every accepted IPv6 form prints as its canonical spelling", "");

        proven_net_addr_t a;
        PROVEN_TEST_ASSERT(proven_net_addr_parse(sv("2001:db8::ff00:42:8329"), 1, &a) == PROVEN_OK &&
                           a.family == PROVEN_NET_FAMILY_IPV6 && a.ip[0] == 0x20 && a.ip[1] == 0x01 &&
                           a.ip[2] == 0x0d && a.ip[3] == 0xb8 && a.ip[10] == 0xff && a.ip[11] == 0x00 &&
                           a.ip[12] == 0x00 && a.ip[13] == 0x42 && a.ip[14] == 0x83 && a.ip[15] == 0x29,
            "the groups land in the right bytes, most significant first", "");
        PROVEN_TEST_ASSERT(proven_net_addr_parse(sv("fe80::1%7"), 1, &a) == PROVEN_OK && a.scope_id == 7,
            "a numeric zone is kept as the scope id", "");

        static const char *bad[] = {
            ":", ":::", "1:::2", "1::2::3", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "12345::", "g::1",
            ":1:2:3:4:5:6:7", "1:2:3:4:5:6:7:", "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8", "[::1", "::1]",
            "[]", "::1%", "::1%eth0", "::1%99999999999", "1:2:3:4:5:6:1.2.3.4.5", "1:2:3:4:5:6:7:1.2.3.4",
            "::1.2.3", "::1.2.3.256", "1.2.3.4::", "2001:db8::1 ", "::ffff:01.2.3.4",
        };
        all = true;
        for (proven_size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            if (!refused(bad[i])) { all = false; PROVEN_TEST_INFO("accepted, or wrote on refusal: \"{}\"", PROVEN_ARG((const char *)bad[i])); }
        }
        PROVEN_TEST_ASSERT(all, "every malformed IPv6 literal is PROVEN_ERR_INVALID_FORMAT and leaves the output untouched", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("constructors, equality, and a buffer that is too small",
        "The fixed addresses are what their names say; equality compares what identifies an endpoint.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_addr_t lo4 = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 9);
        proven_net_addr_t lo6 = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV6, 9);
        proven_net_addr_t any4 = proven_net_addr_any(PROVEN_NET_FAMILY_IPV4, 0);
        proven_net_addr_t any6 = proven_net_addr_any(PROVEN_NET_FAMILY_IPV6, 0);
        PROVEN_TEST_ASSERT(formats_as(&lo4, "127.0.0.1:9") && formats_as(&lo6, "[::1]:9"), "loopback is 127.0.0.1 and ::1", "");
        PROVEN_TEST_ASSERT(formats_as(&any4, "0.0.0.0:0") && formats_as(&any6, "[::]:0"), "any is 0.0.0.0 and ::", "");
        PROVEN_TEST_ASSERT(proven_net_addr_loopback(PROVEN_NET_FAMILY_UNIX, 1).family == PROVEN_NET_FAMILY_NONE,
            "there is no loopback address in a family that has none", "");

        proven_net_addr_t a = proven_net_addr_ipv4(10, 0, 0, 1, 80);
        proven_net_addr_t b = proven_net_addr_ipv4(10, 0, 0, 1, 80);
        proven_net_addr_t c = proven_net_addr_ipv4(10, 0, 0, 1, 81);
        PROVEN_TEST_ASSERT(proven_net_addr_eq(&a, &b) && !proven_net_addr_eq(&a, &c) && !proven_net_addr_eq(&a, &lo6),
            "equal addresses are equal; a different port or family is not", "");
        proven_byte_t raw[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        proven_net_addr_t v6 = proven_net_addr_ipv6(raw, 53, 0);
        proven_net_addr_t v6z = proven_net_addr_ipv6(raw, 53, 2);
        PROVEN_TEST_ASSERT(formats_as(&v6, "[2001:db8::1]:53") && !proven_net_addr_eq(&v6, &v6z),
            "sixteen bytes make the address they spell, and the zone is part of its identity", "");

        proven_byte_t small[8];
        proven_size_t n = 99;
        PROVEN_TEST_ASSERT(proven_net_addr_format(&a, (proven_mem_mut_t){ small, sizeof small }, &n) == PROVEN_ERR_OUT_OF_BOUNDS && n == 0,
            "a destination too small for \"10.0.0.1:80\" is PROVEN_ERR_OUT_OF_BOUNDS, with nothing reported written", "");
        proven_net_addr_t none = {0};
        PROVEN_TEST_ASSERT(proven_net_addr_format(&none, (proven_mem_mut_t){ small, sizeof small }, &n) == PROVEN_ERR_INVALID_ARG,
            "an address of no family has no text", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("Unix-domain paths",
        "A path becomes an address when it fits and holds no NUL.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_addr_t u, u2;
        PROVEN_TEST_ASSERT(proven_net_addr_unix(sv("/tmp/proven.sock"), &u) == PROVEN_OK &&
                           u.family == PROVEN_NET_FAMILY_UNIX && formats_as(&u, "/tmp/proven.sock"),
            "a path is kept and printed as itself", "");
        PROVEN_TEST_ASSERT(proven_net_addr_unix(sv("/tmp/proven.sock"), &u2) == PROVEN_OK && proven_net_addr_eq(&u, &u2),
            "the same path is the same address", "");
        PROVEN_TEST_ASSERT(proven_net_addr_unix(sv(""), &u2) == PROVEN_ERR_INVALID_ARG, "an empty path is refused", "");
        char longpath[200];
        memset(longpath, 'a', sizeof longpath);
        proven_u8str_view_t exact = { (const proven_byte_t *)longpath, PROVEN_NET_UNIX_PATH_MAX };
        proven_u8str_view_t over = { (const proven_byte_t *)longpath, PROVEN_NET_UNIX_PATH_MAX + 1 };
        PROVEN_TEST_ASSERT(proven_net_addr_unix(exact, &u2) == PROVEN_OK && u2.path_len == PROVEN_NET_UNIX_PATH_MAX,
            "a path of exactly the maximum length fits", "");
        PROVEN_TEST_ASSERT(proven_net_addr_unix(over, &u2) == PROVEN_ERR_OUT_OF_BOUNDS, "one byte longer does not", "");
        proven_u8str_view_t nul = { (const proven_byte_t *)"a\0b", 3 };
        PROVEN_TEST_ASSERT(proven_net_addr_unix(nul, &u2) == PROVEN_ERR_INVALID_ARG, "a NUL inside the path is refused", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("resolving a literal needs no resolver",
        "proven_net_resolve answers an address literal itself, and refuses names it cannot ask about.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_addr_t out[4];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_net_resolve(sv("192.0.2.7"), 25, out, 4, &n) == PROVEN_OK && n == 1 && formats_as(&out[0], "192.0.2.7:25"),
            "an IPv4 literal resolves to itself", "");
        PROVEN_TEST_ASSERT(proven_net_resolve(sv("::1"), 25, out, 4, &n) == PROVEN_OK && n == 1 && formats_as(&out[0], "[::1]:25"),
            "an IPv6 literal resolves to itself", "");
        PROVEN_TEST_ASSERT(proven_net_resolve(sv(""), 25, out, 4, &n) == PROVEN_ERR_INVALID_ARG && n == 0, "an empty name is refused", "");
        proven_u8str_view_t nul = { (const proven_byte_t *)"a\0b", 3 };
        PROVEN_TEST_ASSERT(proven_net_resolve(nul, 25, out, 4, &n) == PROVEN_ERR_INVALID_ARG, "a name with a NUL in it is refused", "");
        PROVEN_TEST_ASSERT(proven_net_resolve(sv("192.0.2.7"), 25, out, 0, &n) == PROVEN_ERR_OUT_OF_BOUNDS, "no room for an answer is refused", "");
    }

    PROVEN_TEST_PASS("addresses parse and print as specified.");
    return 0;
}
