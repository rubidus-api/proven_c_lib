#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_quic_net.h"
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/*
 * Batched datagrams (RFC-0018 S-3): proven_net_udp_send_segments_ sends datagrams of one size
 * (the last may be shorter) in one call where the platform can (Linux: UDP_SEGMENT) and one at a
 * time elsewhere; proven_net_udp_recv_segments_ reports the size when the kernel hands several
 * in one receive (Linux: UDP_GRO). Run with the platform's way and with the one-at-a-time way
 * forced, against a receiver with GRO asked for and one without: whatever the kernel does, the
 * receiving side must see the same datagrams - every size and every byte.
 *
 * Whether the kernel coalesced is printed, never asserted: it depends on the kernel and the
 * interface. Loopback does not lose datagrams under this load.
 */

static proven_byte_t pattern(unsigned run, unsigned dg, proven_size_t i) { return (proven_byte_t)(run * 31 + dg * 7 + i * 13 + (i >> 8)); }

/* Receive `want` datagrams' worth from `r`, splitting reported batches; their sizes into `sizes`. */
static bool receive_all(proven_net_udp_t *r, proven_byte_t *buf, proven_size_t want_bytes, unsigned run, proven_size_t *sizes, unsigned cap,
                        unsigned *count, unsigned *batches) {
    proven_size_t got = 0;
    *count = 0; *batches = 0;
    bool ok = true;
    for (int idle = 0; got < want_bytes && idle < 500;) {
        proven_size_t seg = 0;
        proven_net_addr_t from;
        const proven_result_size_t res = proven_net_udp_recv_segments_(r, (proven_mem_mut_t){ buf, 65536 }, &seg, &from);
        if (res.err == PROVEN_ERR_TIMEOUT) { proven_time_sleep(1); ++idle; continue; }
        if (res.err != PROVEN_OK) return false;
        idle = 0;
        if (seg) ++*batches;
        const proven_size_t step = seg ? seg : res.value;
        for (proven_size_t off = 0; off < res.value; off += step) {
            const proven_size_t n = res.value - off < step ? res.value - off : step;
            if (*count >= cap) return false;
            for (proven_size_t i = 0; i < n; ++i) if (buf[off + i] != pattern(run, *count, i)) ok = false;
            sizes[(*count)++] = n;
            got += n;
        }
    }
    return ok && got == want_bytes;
}

int main(void) {
    PROVEN_TEST_SUITE("net: batched datagrams",
        "Datagrams of one size sent in one call where the platform can, and one at a time where it cannot or is told not to; received whole, in order and byte for byte whether or not the kernel coalesced them.",
        "Inspect proven_sys_net_send_segments and proven_sys_net_recv_segments in platform/proven_sys_net.c, and their wrappers in src/proven/net.c.");

    proven_net_udp_t s, r;
    proven_net_addr_t r_at;
    proven_err_t err = proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &r, &r_at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        PROVEN_TEST_INFO("SKIP: this environment refuses to open a datagram socket (error {}).", PROVEN_ARG((int)err));
        PROVEN_TEST_PASS("skipped: no sockets here.");
        return 0;
    }
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "a receiving socket", "");
    PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &s, NULL) == PROVEN_OK, "a sending socket", "");

    static proven_byte_t out[PROVEN_NET_UDP_BATCH_BYTES], in[65536];
    static proven_size_t sizes[128];
    proven_size_t sent = 0;

    PROVEN_TEST_SECTION("the don't-fragment bit", "A socket asked for it reports it (Linux: IP_PMTUDISC_PROBE - the bit set, nothing fragmented here) and still carries datagrams of 1,452 bytes (RFC-0018 11).", "Inspect proven_sys_net_udp_dontfrag.");
    {
        const bool df = proven_net_udp_dontfrag_(&s);
#if defined(__linux__) || defined(_WIN32)
        PROVEN_TEST_ASSERT(df, "the platform sets it", "");
#endif
#if defined(__linux__) && defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_PROBE)
        int mode = -1;
        socklen_t ml = sizeof mode;
        PROVEN_TEST_ASSERT(getsockopt((int)proven_net_udp_handle(&s).raw, IPPROTO_IP, IP_MTU_DISCOVER, &mode, &ml) == 0 && mode == IP_PMTUDISC_PROBE,
                           "the socket reports IP_PMTUDISC_PROBE", "");
        proven_net_udp_t s6;
        proven_net_addr_t at6;
        if (proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV6, 0), &s6, &at6) == PROVEN_OK) {
            PROVEN_TEST_ASSERT(proven_net_udp_dontfrag_(&s6), "an IPv6 socket too", "");
            ml = sizeof mode;
            PROVEN_TEST_ASSERT(getsockopt((int)proven_net_udp_handle(&s6).raw, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &mode, &ml) == 0 && mode == IPV6_PMTUDISC_PROBE,
                               "and reports IPV6_PMTUDISC_PROBE", "");
            (void)proven_net_udp_close(&s6);
        }
#else
        (void)df;
#endif
        for (proven_size_t i = 0; i < 1452; ++i) out[i] = pattern(999, 0, i);
        PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ out, 1452 }, 1452, &sent) == PROVEN_OK && sent == 1452, "a datagram of 1,452 bytes goes", "");
        unsigned count = 0, batches = 0;
        PROVEN_TEST_ASSERT(receive_all(&r, in, 1452, 999, sizes, 128, &count, &batches) && count == 1 && sizes[0] == 1452, "and arrives whole", "");
    }

    PROVEN_TEST_SECTION("refusals", "A batch the platform cannot take in one call is refused before anything is sent.", "");
    PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ out, 100 }, 0, &sent) == PROVEN_ERR_INVALID_ARG && sent == 0, "a datagram size of 0", "");
    PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ out, 65 * 100 }, 100, &sent) == PROVEN_ERR_OUT_OF_BOUNDS && sent == 0, "65 datagrams: more than one call may carry", "");
    static proven_byte_t big[PROVEN_NET_UDP_BATCH_BYTES + 1];
    PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ big, sizeof big }, 1452, &sent) == PROVEN_ERR_OUT_OF_BOUNDS && sent == 0, "65,508 bytes: more than one UDP send may carry", "");
    PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ out, 0 }, 1452, &sent) == PROVEN_OK && sent == 0, "nothing to send is a success", "");

    struct { proven_size_t seg; unsigned count; proven_size_t tail; } shapes[] = {
        { 1452, 1, 0 }, { 1452, 2, 0 }, { 1452, 44, 0 }, { 1452, 44, 300 }, { 1452, 45, 0 },
        { 1200, 54, 0 }, { 1200, 53, 1 }, { 60, 64, 0 }, { 60, 63, 20 }, { 1452, 1, 700 },
    };
    unsigned run = 0, coalesced = 0;
    for (int forced = 0; forced < 2; ++forced) {
        proven_net_udp_segments_off_for_test_(forced == 1);
        for (int gro = 0; gro < 2; ++gro) {
            /* A fresh receiver for each mode: GRO, once asked for, stays on. */
            (void)proven_net_udp_close(&r);
            PROVEN_TEST_ASSERT(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &r, &r_at) == PROVEN_OK, "a receiver", "");
            const bool on = gro ? proven_net_udp_gro_(&r) : false;
            if (forced) PROVEN_TEST_ASSERT(!on, "with the one-at-a-time way forced, no GRO either", "");
            printf("  %s, receiver %s\n", forced ? "one at a time (forced)" : "the platform's way", on ? "with GRO" : "without GRO");
            for (unsigned k = 0; k < sizeof shapes / sizeof shapes[0]; ++k, ++run) {
                const proven_size_t seg = shapes[k].seg;
                const unsigned n = shapes[k].count + (shapes[k].tail ? 1u : 0u);
                proven_size_t total = 0;
                for (unsigned d = 0; d < n; ++d) {
                    const proven_size_t len = d < shapes[k].count ? seg : shapes[k].tail;
                    for (proven_size_t i = 0; i < len; ++i) out[total + i] = pattern(run, d, i);
                    total += len;
                }
                PROVEN_TEST_ASSERT(proven_net_udp_send_segments_(&s, r_at, (proven_mem_view_t){ out, total }, seg, &sent) == PROVEN_OK && sent == total,
                                   "a batch is sent whole", "");
                unsigned count = 0, batches = 0;
                const bool ok = receive_all(&r, in, total, run, sizes, 128, &count, &batches);
                bool shape = count == n;
                for (unsigned d = 0; shape && d < n; ++d) shape = sizes[d] == (d < shapes[k].count ? seg : shapes[k].tail);
                if (!ok || !shape) printf("  FAILED: %u x %zu + %zu: %u datagrams received\n", shapes[k].count, seg, shapes[k].tail, count);
                PROVEN_TEST_ASSERT(ok && shape, "the receiver sees every datagram, its size and every byte, in order", "");
                coalesced += batches;
            }
        }
    }
    proven_net_udp_segments_off_for_test_(false);
    printf("  receives that held several datagrams: %u (the kernel's choice; 0 off Linux)\n", coalesced);

    (void)proven_net_udp_close(&s);
    (void)proven_net_udp_close(&r);
    PROVEN_TEST_PASS("batched datagrams arrive as the datagrams they were");
    return 0;
}
