#ifndef PROVEN_INTERNAL_QUIC_NET_H
#define PROVEN_INTERNAL_QUIC_NET_H

/*
 * Internal: a socket address as the QUIC engine keeps it, and back. The engine compares
 * addresses byte for byte, so every byte is set: a copy that differed only in padding would look
 * like the peer moving. For the event server and the event client, both hosted. Not public API.
 */

#include "proven/net.h"
#include "proven_internal_quic.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
/* Batched datagrams (RFC-0018 S-3), in net.c over the platform's segment calls; never waiting.
 * send: `data` as datagrams of `seg` bytes (the last may be shorter); `*sent` bytes went - all
 * on PROVEN_OK, a whole number of datagrams on PROVEN_ERR_TIMEOUT (the socket would block).
 * recv: one receive; `*seg` the size of each datagram when it holds several, else 0;
 * PROVEN_ERR_TIMEOUT when there is nothing. gro: whether the socket may coalesce. */
#define PROVEN_NET_UDP_BATCH_BYTES ((proven_size_t)65507)
#define PROVEN_NET_UDP_BATCH_COUNT 64
proven_err_t proven_net_udp_send_segments_(proven_net_udp_t *udp, proven_net_addr_t to, proven_mem_view_t data, proven_size_t seg, proven_size_t *sent);
proven_result_size_t proven_net_udp_recv_segments_(proven_net_udp_t *udp, proven_mem_mut_t dest, proven_size_t *seg, proven_net_addr_t *from);
bool proven_net_udp_gro_(proven_net_udp_t *udp);
void proven_net_udp_segments_off_for_test_(bool off);
#endif

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET) && !defined(PROVEN_NO_HTTP3)

static inline proven_quic_addr_t proven_quic_addr_of_(const proven_net_addr_t *a) {
    proven_quic_addr_t q;
    for (proven_size_t i = 0; i < sizeof q; ++i) ((proven_byte_t *)&q)[i] = 0;
    q.len = 23;
    q.b[0] = (proven_byte_t)a->family;
    q.b[1] = (proven_byte_t)(a->port >> 8);
    q.b[2] = (proven_byte_t)a->port;
    for (int i = 0; i < 4; ++i) q.b[3 + i] = (proven_byte_t)(a->scope_id >> (24 - 8 * i));
    const proven_size_t ip = a->family == PROVEN_NET_FAMILY_IPV4 ? 4 : 16;
    for (proven_size_t i = 0; i < ip; ++i) q.b[7 + i] = a->ip[i];
    return q;
}

static inline proven_net_addr_t proven_net_addr_of_quic_(const proven_quic_addr_t *q) {
    proven_net_addr_t a = { 0 };
    a.family = (proven_net_family_t)q->b[0];
    a.port = (proven_u16)((proven_u16)q->b[1] << 8 | q->b[2]);
    for (int i = 0; i < 4; ++i) a.scope_id = a.scope_id << 8 | q->b[3 + i];
    for (int i = 0; i < 16; ++i) a.ip[i] = q->b[7 + i];
    return a;
}

#endif
#endif
