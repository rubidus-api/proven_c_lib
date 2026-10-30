#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_quic.h"
#include "test_unit_quic_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * QUIC version 1's packet headers and frames (RFC 9000 sections 12, 17 and 19): the headers of
 * RFC 9001 appendix A's packets read and written byte for byte, the Retry packet built to the
 * RFC's bytes, version negotiation, packet number lengths against RFC 9000 appendix A.2; every
 * frame type written and read back; ACK ranges both ways; every truncation of a header and of
 * a frame refused, and each malformed frame refused with its code.
 */

#ifndef PROVEN_NO_HTTP3

static proven_size_t unhex(const char *hex, proven_byte_t *out, proven_size_t cap) {
    proven_size_t n = strlen(hex) / 2;
    if (n > cap) { fprintf(stderr, "vector too long\n"); exit(1); }
    for (proven_size_t i = 0; i < n; ++i) {
        char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        out[i] = (proven_byte_t)strtoul(c, NULL, 16);
    }
    return n;
}

static bool equals_hex(const proven_byte_t *p, proven_size_t n, const char *hex) {
    static proven_byte_t want[2048];
    return unhex(hex, want, sizeof want) == n && memcmp(p, want, n) == 0;
}

static proven_quic_cid_t cid_hex(const char *hex) {
    proven_quic_cid_t c = { 0 };
    c.len = (proven_byte_t)unhex(hex, c.id, sizeof c.id);
    return c;
}

static bool cid_eq(const proven_quic_cid_t *a, const char *hex) {
    return equals_hex(a->id, a->len, hex);
}

/* Read one frame from `n` bytes; the frame must use them all. */
static proven_u64 read_all(const proven_byte_t *p, proven_size_t n, proven_quic_frame_t *f) {
    proven_quic_rd_t r = { p, n, true };
    proven_u64 e = proven_quic_frame_read(&r, f);
    if (e == 0 && r.n != 0) return 0xffff;
    return e;
}

static proven_u64 read_hex(const char *hex, proven_quic_frame_t *f) {
    static proven_byte_t b[256];
    proven_size_t n = unhex(hex, b, sizeof b);
    return read_all(b, n, f);
}

static bool frame_eq(const proven_quic_frame_t *a, const proven_quic_frame_t *b) {
    return a->type == b->type && a->id == b->id && a->offset == b->offset && a->value == b->value && a->final_size == b->final_size
        && a->frame_type == b->frame_type && a->fin == b->fin && a->data.size == b->data.size
        && (a->data.size == 0 || memcmp(a->data.ptr, b->data.ptr, a->data.size) == 0)
        && a->cid.len == b->cid.len && memcmp(a->cid.id, b->cid.id, a->cid.len) == 0
        && memcmp(a->reset_token, b->reset_token, sizeof a->reset_token) == 0;
}

int main(void) {
    PROVEN_TEST_SUITE("QUIC packet headers and frames",
        "RFC 9000's headers and frames read and written: RFC 9001 appendix A's headers byte for byte, every frame type both ways, and every malformed one refused with its code.",
        "Inspect src/proven/quic_frame.c.");

    static proven_byte_t pkt[2048], buf[2048];
    proven_quic_hdr_t h;

    PROVEN_TEST_SECTION("RFC 9001 appendix A's headers", "The client's and server's Initial, the Retry and the short header located; the long and short headers and the Retry written to the RFC's bytes.", "");
    {
        proven_size_t n = unhex(QV_CLIENT_PACKET, pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_INITIAL && h.version == 1 &&
                           cid_eq(&h.dcid, QV_DCID) && h.scid.len == 0 && h.token.size == 0 && h.pn_offset == 18 && h.len == 1200 && n == 1200,
            "the client's Initial: version 1, its connection IDs, no token, the packet number at 18, 1,200 bytes", "");
        n = unhex(QV_SERVER_PACKET, pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_INITIAL &&
                           h.dcid.len == 0 && cid_eq(&h.scid, "f067a5502a4262b5") && h.pn_offset == 18 && h.len == n,
            "the server's Initial", "");
        /* A datagram of two packets: each is located by its Length. */
        memcpy(pkt + n, pkt, n);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, 2 * n }, 8, &h) && h.len == n &&
                           proven_quic_hdr_parse((proven_mem_view_t){ pkt + h.len, 2 * n - h.len }, 8, &h) && h.len == n,
            "coalesced packets are found one after another", "");
        n = unhex(QV_RETRY_PACKET, pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_RETRY &&
                           equals_hex(h.token.ptr, h.token.size, "746f6b656e") && cid_eq(&h.scid, "f067a5502a4262b5") && h.len == n,
            "the Retry: its token between the connection IDs and the tag", "");
        n = unhex(QV_CHACHA_PACKET, pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 0, &h) && h.type == PROVEN_QUIC_PKT_SHORT && h.dcid.len == 0 && h.pn_offset == 1 && h.len == n,
            "the short header, with this side's zero-length connection ID", "");

        proven_quic_cid_t dcid = cid_hex(QV_DCID), none = { 0 }, scid = cid_hex("f067a5502a4262b5");
        n = proven_quic_long_header_put(PROVEN_QUIC_PKT_INITIAL, &dcid, &none, (proven_mem_view_t){ 0 }, 2, 4, 1162, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(n > 0 && equals_hex(buf, n, QV_CLIENT_HEADER), "the client's Initial header, written", "");
        n = proven_quic_long_header_put(PROVEN_QUIC_PKT_INITIAL, &none, &scid, (proven_mem_view_t){ 0 }, 1, 2, 99, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(n > 0 && equals_hex(buf, n, QV_SERVER_HEADER), "the server's", "");
        n = proven_quic_short_header_put(&none, false, QV_CHACHA_PN, 3, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(n > 0 && equals_hex(buf, n, QV_CHACHA_HEADER), "the short header", "");
        n = proven_quic_retry_build(&dcid, &none, &scid, (proven_mem_view_t){ (const proven_byte_t *)"token", 5 }, 0x0f, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(n > 0 && equals_hex(buf, n, QV_RETRY_PACKET), "the Retry, its tag included", "");
        PROVEN_TEST_ASSERT(proven_quic_retry_build(&dcid, &none, &scid, (proven_mem_view_t){ (const proven_byte_t *)"token", 5 }, 0x0f, (proven_mem_mut_t){ buf, n - 1 }) == 0,
            "and nothing when it does not fit", "");
        PROVEN_TEST_ASSERT(proven_quic_long_header_put(PROVEN_QUIC_PKT_HANDSHAKE, &dcid, &scid, (proven_mem_view_t){ 0 }, 0, 1, 16384, (proven_mem_mut_t){ buf, sizeof buf }) == 0 &&
                           proven_quic_long_header_put(PROVEN_QUIC_PKT_RETRY, &dcid, &scid, (proven_mem_view_t){ 0 }, 0, 1, 10, (proven_mem_mut_t){ buf, sizeof buf }) == 0 &&
                           proven_quic_long_header_put(PROVEN_QUIC_PKT_HANDSHAKE, &dcid, &scid, (proven_mem_view_t){ 0 }, 0, 5, 10, (proven_mem_mut_t){ buf, sizeof buf }) == 0,
            "a Length past two bytes, a Retry, or a packet number of five bytes is not written", "");
        n = proven_quic_long_header_put(PROVEN_QUIC_PKT_HANDSHAKE, &dcid, &scid, (proven_mem_view_t){ 0 }, 0x1234, 2, 30, (proven_mem_mut_t){ buf, sizeof buf });
        memset(buf + n, 0, 46);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ buf, n + 30 + 16 }, 8, &h) && h.type == PROVEN_QUIC_PKT_HANDSHAKE &&
                           h.pn_offset == n - 2 && h.len == n + 30 + 16 && cid_eq(&h.dcid, QV_DCID),
            "a Handshake header read back", "");
    }

    PROVEN_TEST_SECTION("headers refused", "Every truncation of a long header, a clear fixed bit, a connection ID of 21 bytes, a Retry with no token, a Length past the datagram.", "");
    {
        proven_size_t n = unhex(QV_CLIENT_PACKET, pkt, sizeof pkt);
        bool all = true;
        for (proven_size_t cut = 0; cut < 20; ++cut) all = all && !proven_quic_hdr_parse((proven_mem_view_t){ pkt, cut }, 8, &h);
        PROVEN_TEST_ASSERT(all, "the Initial's header cut at any byte up to its packet number", "");
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ pkt, n - 1 }, 8, &h), "a Length one byte past the datagram", "");
        pkt[0] &= (proven_byte_t)~0x40u;
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h), "the fixed bit clear", "");
        unhex(QV_CLIENT_PACKET, pkt, sizeof pkt);
        pkt[5] = 21;
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h), "a connection ID of 21 bytes", "");
        n = unhex("f0000000010008f067a5502a4262b5" "04a265ba2eff4d829058fb3f0f2496ba", pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h), "a Retry with no token", "");
        n = unhex("4000", pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h) && proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 1, &h) &&
                           !proven_quic_hdr_parse((proven_mem_view_t){ (const proven_byte_t *)"\x00\x01", 2 }, 1, &h),
            "a short header shorter than this side's connection ID, or with the fixed bit clear", "");
    }

    PROVEN_TEST_SECTION("version negotiation and other versions", "An answer built and read; a long header of another version located by the invariants only.", "");
    {
        proven_quic_cid_t a = cid_hex("0102030405"), b = cid_hex("a1a2a3a4a5a6a7a8");
        proven_size_t n = proven_quic_vn_build(&a, &b, 0x55, (proven_mem_mut_t){ buf, sizeof buf });
        PROVEN_TEST_ASSERT(n == 1 + 4 + 1 + 5 + 1 + 8 + 8 && (buf[0] & 0x80) && proven_quic_hdr_parse((proven_mem_view_t){ buf, n }, 8, &h) &&
                           h.type == PROVEN_QUIC_PKT_VN && cid_eq(&h.dcid, "0102030405") && cid_eq(&h.scid, "a1a2a3a4a5a6a7a8") &&
                           equals_hex(h.token.ptr, h.token.size, "000000016b3343cf"),
            "version negotiation lists version 1 and version 2 (RFC-0036)", "");
        PROVEN_TEST_ASSERT(!proven_quic_hdr_parse((proven_mem_view_t){ buf, n - 1 }, 8, &h) && !proven_quic_hdr_parse((proven_mem_view_t){ buf, n - 8 }, 8, &h),
            "a version list that is not whole, or empty, is refused", "");
        n = unhex("c0" "1a2a3a4a" "04" "01020304" "00" "ffffffff", pkt, sizeof pkt);
        PROVEN_TEST_ASSERT(proven_quic_hdr_parse((proven_mem_view_t){ pkt, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_OTHER_VERSION && h.version == 0x1a2a3a4a &&
                           cid_eq(&h.dcid, "01020304") && h.len == n,
            "another version: its connection IDs, nothing more", "");
    }

    PROVEN_TEST_SECTION("packet number lengths", "RFC 9000 appendix A.2's examples, and room for twice what is unacknowledged.", "");
    {
        PROVEN_TEST_ASSERT(proven_quic_pn_len(0xac5c02, 0xabe8b3 + 1) == 2 && proven_quic_pn_len(0xace8fe, 0xabe8b3 + 1) == 3,
            "appendix A.2: two bytes, then three", "");
        PROVEN_TEST_ASSERT(proven_quic_pn_len(0, 0) == 1 && proven_quic_pn_len(127, 0) == 1 && proven_quic_pn_len(128, 0) == 2 && proven_quic_pn_len(129, 2) == 1 && proven_quic_pn_len(130, 2) == 2 &&
                           proven_quic_pn_len(1000000000, 0) == 4 && proven_quic_pn_len(200, 100) == 1,
            "nothing acknowledged yet counts from zero", "");
        for (proven_u64 largest = 0; largest < 3000; largest += 7) for (proven_u64 pn = largest + 1; pn < largest + 70000; pn += 997) {
            proven_size_t len = proven_quic_pn_len(pn, largest + 1);
            proven_u64 trunc = pn & ((((proven_u64)1) << (8 * len)) - 1);
            PROVEN_TEST_ASSERT(proven_quic_pn_decode(largest + 1, trunc, (unsigned)(8 * len)) == pn, "what is written decodes again at the receiver", "");
        }
    }

    PROVEN_TEST_SECTION("every frame type both ways", "Each written and read back the same; a STREAM frame without its length runs to the end of the packet.", "");
    {
        static const proven_byte_t data[] = "a little data for a stream";
        proven_quic_frame_t frames[24];
        proven_size_t k = 0;
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_PADDING };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_PING };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_RESET_STREAM, .id = 4, .value = 0x10c, .final_size = 123456789 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STOP_SENDING, .id = 1ull << 40, .value = 7 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_CRYPTO, .offset = 70000, .data = { data, 20 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_NEW_TOKEN, .data = { data, 9 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STREAM, .id = 8, .offset = 0, .data = { data, 10 }, .fin = false };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STREAM, .id = 3, .offset = 1ull << 40, .data = { data, 0 }, .fin = true };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_MAX_DATA, .value = 1ull << 50 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_MAX_STREAM_DATA, .id = 2, .value = 65536 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_MAX_STREAMS_BIDI, .value = 1ull << 60 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_MAX_STREAMS_UNI, .value = 3 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_DATA_BLOCKED, .value = 99 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STREAM_DATA_BLOCKED, .id = 6, .value = 1 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STREAMS_BLOCKED_BIDI, .value = 100 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_STREAMS_BLOCKED_UNI, .value = 0 };
        proven_quic_frame_t ncid = { .type = PROVEN_QUIC_FT_NEW_CONNECTION_ID, .id = 5, .value = 2, .cid = cid_hex("0102030405060708090a0b0c0d0e0f1011121314") };
        for (int i = 0; i < 16; ++i) ncid.reset_token[i] = (proven_byte_t)(0xe0 + i);
        frames[k++] = ncid;
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_RETIRE_CONNECTION_ID, .id = 1 };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_PATH_CHALLENGE, .data = { data, 8 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_PATH_RESPONSE, .data = { data + 8, 8 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_CONNECTION_CLOSE, .value = 0x0a, .frame_type = 0x08, .data = { data, 6 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_CONNECTION_CLOSE_APP, .value = 0x0100, .data = { data, 0 } };
        frames[k++] = (proven_quic_frame_t){ .type = PROVEN_QUIC_FT_HANDSHAKE_DONE };
        bool round = true, cuts = true;
        for (proven_size_t i = 0; i < k; ++i) {
            proven_quic_wr_t w = { buf, 0, sizeof buf, true };
            proven_quic_frame_t back;
            if (!proven_quic_frame_put(&w, &frames[i]) || read_all(buf, w.n, &back) != 0 || !frame_eq(&frames[i], &back)) {
                round = false; printf("frame %zu (type %llx) did not come back\n", i, (unsigned long long)frames[i].type);
            }
            /* Every shorter prefix is a frame cut short (the STREAM frames carry their length). */
            for (proven_size_t cut = 0; cut < w.n; ++cut) {
                proven_quic_rd_t r = { buf, cut, true };
                if (proven_quic_frame_read(&r, &back) != PROVEN_QUIC_FRAME_ENCODING_ERROR) { cuts = false; printf("frame %zu cut at %zu was taken\n", i, cut); }
            }
            /* And a writer one byte too small writes nothing. */
            proven_quic_wr_t small = { buf, 0, w.n - 1, true };
            if (proven_quic_frame_put(&small, &frames[i]) || small.n != 0) { round = false; printf("frame %zu written into too little room\n", i); }
        }
        PROVEN_TEST_ASSERT(round, "each frame type written and read back the same; too little room writes nothing", "");
        PROVEN_TEST_ASSERT(cuts, "every frame cut at any byte is FRAME_ENCODING_ERROR", "");
        proven_quic_frame_t f;
        PROVEN_TEST_ASSERT(read_hex("0904" "68656c6c6f", &f) == 0 && f.type == PROVEN_QUIC_FT_STREAM && f.id == 4 && f.fin && f.offset == 0 &&
                           equals_hex(f.data.ptr, f.data.size, "68656c6c6f"),
            "STREAM without a length: to the end of the packet", "");
        PROVEN_TEST_ASSERT(read_hex("0f00" "05" "02" "6869", &f) == 0 && f.offset == 5 && f.fin && f.data.size == 2,
            "STREAM with all three bits", "");
        PROVEN_TEST_ASSERT(proven_quic_stream_overhead(4, 0, 100) == 1 + 1 + 2 && proven_quic_stream_overhead(1ull << 40, 70000, 5) == 1 + 8 + 4 + 1,
            "a STREAM frame's header size", "");
        PROVEN_TEST_ASSERT(proven_quic_frame_in_handshake(PROVEN_QUIC_FT_CRYPTO) && proven_quic_frame_in_handshake(PROVEN_QUIC_FT_CONNECTION_CLOSE) &&
                           !proven_quic_frame_in_handshake(PROVEN_QUIC_FT_STREAM) && !proven_quic_frame_in_handshake(PROVEN_QUIC_FT_CONNECTION_CLOSE_APP) &&
                           !proven_quic_frame_in_handshake(PROVEN_QUIC_FT_HANDSHAKE_DONE) && !proven_quic_frame_in_handshake(PROVEN_QUIC_FT_NEW_TOKEN),
            "frames allowed in Initial and Handshake packets (RFC 9000 table 3)", "");
        PROVEN_TEST_ASSERT(!proven_quic_frame_ack_eliciting(PROVEN_QUIC_FT_ACK) && !proven_quic_frame_ack_eliciting(PROVEN_QUIC_FT_PADDING) &&
                           !proven_quic_frame_ack_eliciting(PROVEN_QUIC_FT_CONNECTION_CLOSE_APP) && proven_quic_frame_ack_eliciting(PROVEN_QUIC_FT_PING) &&
                           proven_quic_frame_probing(PROVEN_QUIC_FT_PATH_CHALLENGE) && !proven_quic_frame_probing(PROVEN_QUIC_FT_PING),
            "ack-eliciting and probing frames", "");
    }

    PROVEN_TEST_SECTION("ACK ranges", "Ranges written highest first and walked back the same; as many as fit; ranges below zero refused.", "");
    {
        const proven_u64 ranges[5][2] = { { 1000, 1010 }, { 900, 990 }, { 500, 500 }, { 2, 498 }, { 0, 0 } };
        proven_quic_wr_t w = { buf, 0, sizeof buf, true };
        PROVEN_TEST_ASSERT(proven_quic_ack_put(&w, ranges, 5, 77) == 5, "five ranges written", "");
        proven_quic_frame_t f;
        PROVEN_TEST_ASSERT(read_all(buf, w.n, &f) == 0 && f.type == PROVEN_QUIC_FT_ACK && f.largest == 1010 && f.ack_delay == 77 && f.range_count == 4,
            "read back: largest, delay, count", "");
        proven_quic_ack_iter_t it = proven_quic_ack_iter(&f);
        proven_u64 lo, hi;
        proven_size_t i = 0;
        bool same = true;
        while (proven_quic_ack_next(&it, &lo, &hi)) { same = same && i < 5 && lo == ranges[i][0] && hi == ranges[i][1]; ++i; }
        PROVEN_TEST_ASSERT(same && i == 5, "and the ranges walked back the same", "");
        /* Room for fewer: the highest ranges are kept. */
        for (proven_size_t cap = 1; cap < w.n; ++cap) {
            proven_quic_wr_t s = { buf, 0, cap, true };
            proven_size_t got = proven_quic_ack_put(&s, ranges, 5, 77);
            if (got == 0) { PROVEN_TEST_ASSERT(s.n == 0, "nothing written when not even the first range fits", ""); continue; }
            PROVEN_TEST_ASSERT(read_all(buf, s.n, &f) == 0 && f.range_count + 1 == got && f.largest == 1010, "the ranges that fit, readable", "");
        }
        PROVEN_TEST_ASSERT(read_hex("02" "05" "00" "00" "06", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "a first range below zero", "");
        PROVEN_TEST_ASSERT(read_hex("02" "05" "00" "01" "00" "04" "00", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "a gap below zero", "");
        PROVEN_TEST_ASSERT(read_hex("02" "05" "00" "01" "00" "03" "01", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "a range below zero", "");
        PROVEN_TEST_ASSERT(read_hex("02" "05" "00" "01" "00" "03" "00", &f) == 0, "a range ending at zero is fine", "");
        PROVEN_TEST_ASSERT(read_hex("03" "05" "00" "00" "00" "01" "02" "03", &f) == 0 && f.type == PROVEN_QUIC_FT_ACK_ECN && f.ecn[2] == 3,
            "ACK with ECN counts", "");
        PROVEN_TEST_ASSERT(read_hex("02" "05" "00" "40ff" "00", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "a range count past the frame", "");
    }

    PROVEN_TEST_SECTION("malformed frames refused with their code", "RFC 9000 section 19's rules, each.", "");
    {
        proven_quic_frame_t f;
        PROVEN_TEST_ASSERT(read_hex("4001", &f) == PROVEN_QUIC_PROTOCOL_VIOLATION && read_hex("800000" "1e", &f) == PROVEN_QUIC_PROTOCOL_VIOLATION,
            "a frame type in a longer encoding than it needs: PROTOCOL_VIOLATION", "");
        PROVEN_TEST_ASSERT(read_hex("1f", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR && read_hex("30", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR &&
                           read_hex("4040", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR,
            "an unknown type - DATAGRAM (0x30) among them, never offered", "");
        PROVEN_TEST_ASSERT(read_hex("0e" "04" "ffffffffffffffff" "02" "6869", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR &&
                           read_hex("0e" "04" "fffffffffffffffe" "01" "68", &f) == 0,
            "STREAM past 2^62 - 1, and up to it", "");
        PROVEN_TEST_ASSERT(read_hex("06" "ffffffffffffffff" "01" "68", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "CRYPTO past 2^62 - 1", "");
        PROVEN_TEST_ASSERT(read_hex("07" "00", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "an empty NEW_TOKEN", "");
        PROVEN_TEST_ASSERT(read_hex("12" "c000000000000001" "00" , &f) != 0 && read_hex("12" "d000000000000001", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR &&
                           read_hex("12" "d000000000000000", &f) == 0 && read_hex("17" "d000000000000001", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR,
            "MAX_STREAMS and STREAMS_BLOCKED above 2^60", "");
        const char *tok = "000102030405060708090a0b0c0d0e0f";
        static char hex[256];
        snprintf(hex, sizeof hex, "18" "01" "00" "00" "%s", tok);
        PROVEN_TEST_ASSERT(read_hex(hex, &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "NEW_CONNECTION_ID with no connection ID", "");
        snprintf(hex, sizeof hex, "18" "01" "00" "15" "000102030405060708090a0b0c0d0e0f1011121314" "%s", tok);
        PROVEN_TEST_ASSERT(read_hex(hex, &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "with 21 bytes", "");
        snprintf(hex, sizeof hex, "18" "01" "02" "04" "01020304" "%s", tok);
        PROVEN_TEST_ASSERT(read_hex(hex, &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "retire_prior_to above its own sequence number", "");
        snprintf(hex, sizeof hex, "18" "02" "02" "04" "01020304" "%s", tok);
        PROVEN_TEST_ASSERT(read_hex(hex, &f) == 0 && f.value == 2 && f.cid.len == 4, "and equal to it is fine", "");
        PROVEN_TEST_ASSERT(read_hex("1a" "01020304050607", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "PATH_CHALLENGE with seven bytes", "");
        PROVEN_TEST_ASSERT(read_hex("1c" "0a" "08" "05" "6869", &f) == PROVEN_QUIC_FRAME_ENCODING_ERROR, "CONNECTION_CLOSE whose reason runs past the packet", "");
        proven_quic_wr_t w = { buf, 0, sizeof buf, true };
        proven_quic_frame_t bad = { .type = PROVEN_QUIC_FT_PATH_CHALLENGE, .data = { buf, 7 } }, ack = { .type = PROVEN_QUIC_FT_ACK }, odd = { .type = 0x30 };
        PROVEN_TEST_ASSERT(!proven_quic_frame_put(&w, &bad) && !proven_quic_frame_put(&w, &ack) && !proven_quic_frame_put(&w, &odd) && w.n == 0 && w.ok,
            "writing a PATH_CHALLENGE of seven bytes, an ACK through the general writer, or an unknown type: nothing", "");
    }

    PROVEN_TEST_PASS("QUIC packet headers and frames are read and written as RFC 9000 says, and malformed ones refused.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("QUIC packet headers and frames", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
