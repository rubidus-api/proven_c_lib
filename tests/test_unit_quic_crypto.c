#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_quic.h"
#include "test_unit_quic_vectors.h"
#include "test_unit_quic_vectors_v2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * QUIC version 1 below the transport: variable-length integers and packet numbers against
 * RFC 9000's appendix A, packet protection and the Retry tag against RFC 9001's appendix A,
 * byte for byte, then every bit change and truncation of those packets refused, and the
 * transport parameters of RFC 9000 section 18 - those of the RFC's own ClientHello decoded,
 * every parameter encoded and decoded, and each refusal.
 *
 * test_unit_quic_vectors.h is the RFC's text turned into strings by a private generator.
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

/* The same block under two AES contexts: how a derived key is compared with the RFC's when
 * the library keeps only the expanded schedule. */
static bool same_aes(const proven_crypto_aes_gcm_t *derived, const char *key_hex) {
    proven_byte_t key[32], a[16], b[16], in[16];
    proven_size_t n = unhex(key_hex, key, sizeof key);
    proven_crypto_aes_gcm_t ctx;
    if (!proven_crypto_aes_gcm_init(&ctx, key, n)) return false;
    for (int i = 0; i < 16; ++i) in[i] = (proven_byte_t)(i * 17 + 3);
    proven_crypto_aes_encrypt_block(&ctx, in, a);
    proven_crypto_aes_encrypt_block(derived, in, b);
    return memcmp(a, b, 16) == 0;
}

/* The same for header protection's context, which is AES alone. */
static bool same_hp(const proven_crypto_aes_t *derived, const char *key_hex) {
    proven_byte_t key[32], a[16], b[16], in[16];
    proven_size_t n = unhex(key_hex, key, sizeof key);
    proven_crypto_aes_gcm_t ctx;
    if (!proven_crypto_aes_gcm_init(&ctx, key, n)) return false;
    for (int i = 0; i < 16; ++i) in[i] = (proven_byte_t)(i * 17 + 3);
    proven_crypto_aes_encrypt_block(&ctx, in, a);
    proven_crypto_aes_block(derived, in, b);
    return memcmp(a, b, 16) == 0;
}

/* Protect `payload` behind `header` with `keys` and compare with `packet`; then open the
 * result again. pn_len comes from the header's first byte. */
static bool protect_and_open(const proven_quic_keys_t *keys, const char *header_hex, const proven_byte_t *payload, proven_size_t payload_len,
                             proven_u64 pn, const char *packet_hex, proven_u64 largest_plus_one) {
    static proven_byte_t buf[1600];
    proven_size_t hlen = unhex(header_hex, buf, sizeof buf);
    proven_size_t pn_len = (proven_size_t)(buf[0] & 3) + 1;
    memcpy(buf + hlen, payload, payload_len);
    proven_size_t n = proven_quic_protect(keys, buf, hlen, pn_len, pn, payload_len);
    if (n != hlen + payload_len + 16 || !equals_hex(buf, n, packet_hex)) return false;
    proven_u64 got = 0;
    proven_size_t at = 0;
    proven_mem_mut_t plain;
    if (!proven_quic_unprotect(keys, buf, hlen - pn_len, n, largest_plus_one, &got, &at, &plain)) return false;
    return got == pn && at == hlen && plain.size == payload_len && memcmp(plain.ptr, payload, payload_len) == 0 && equals_hex(buf, hlen, header_hex);
}

/* Every single-bit change and every truncation of a protected packet is refused. */
static bool every_change_refused(const proven_quic_keys_t *keys, const char *packet_hex, proven_size_t pn_offset, proven_u64 largest_plus_one,
                                 unsigned *tried) {
    static proven_byte_t orig[1600], buf[1600];
    proven_size_t n = unhex(packet_hex, orig, sizeof orig);
    proven_u64 pn;
    proven_size_t at;
    proven_mem_mut_t plain;
    memcpy(buf, orig, n);
    if (!proven_quic_unprotect(keys, buf, pn_offset, n, largest_plus_one, &pn, &at, &plain)) return false;
    for (proven_size_t bit = 0; bit < 8 * n; ++bit) {
        memcpy(buf, orig, n);
        buf[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
        if (proven_quic_unprotect(keys, buf, pn_offset, n, largest_plus_one, &pn, &at, &plain)) return false;
        ++*tried;
    }
    for (proven_size_t len = 0; len < n; ++len) {
        memcpy(buf, orig, n);
        if (proven_quic_unprotect(keys, buf, pn_offset, len, largest_plus_one, &pn, &at, &plain)) return false;
        ++*tried;
    }
    return true;
}

static proven_u64 tp_decode_hex(const char *hex, bool from_server, proven_quic_tp_t *tp) {
    proven_byte_t b[512];
    proven_size_t n = unhex(hex, b, sizeof b);
    return proven_quic_tp_decode((proven_mem_view_t){ b, n }, from_server, tp);
}

int main(void) {
    PROVEN_TEST_SUITE("QUIC packet protection and transport parameters",
        "RFC 9000 appendix A and section 18, RFC 9001 appendix A, byte for byte; then every change refused.",
        "Inspect src/proven/quic_crypto.c. The section names the RFC part.");

    PROVEN_TEST_SECTION("variable-length integers (RFC 9000 section 16, appendix A.1)", "The four examples, every boundary, and short input.", "");
    {
        static const struct { const char *hex; proven_u64 v; } ex[] = {
            { "c2197c5eff14e88c", 151288809941952652ull }, { "9d7f3e7d", 494878333ull }, { "7bbd", 15293 }, { "25", 37 }, { "4025", 37 } };
        bool all = true;
        for (proven_size_t i = 0; i < sizeof ex / sizeof ex[0]; ++i) {
            proven_byte_t b[8];
            proven_size_t n = unhex(ex[i].hex, b, 8);
            proven_quic_rd_t r = { b, n, true };
            all = all && proven_quic_rd_varint(&r) == ex[i].v && r.ok && r.n == 0;
            proven_quic_rd_t s = { b, n - 1, true };
            (void)proven_quic_rd_varint(&s);
            all = all && !s.ok;
            proven_byte_t w[8];
            /* "4025" is a longer form of 37; the writer uses the shortest. */
            if (i < 4) all = all && proven_quic_varint_put(ex[i].v, w) == n && memcmp(w, b, n) == 0;
        }
        PROVEN_TEST_ASSERT(all, "the RFC's four examples decode, the two-byte 37 too, and one byte short is refused", "");
        static const proven_u64 edge[] = { 0, 63, 64, 16383, 16384, 1073741823, 1073741824, PROVEN_QUIC_VARINT_MAX };
        static const proven_size_t len[] = { 1, 1, 2, 2, 4, 4, 8, 8 };
        for (proven_size_t i = 0; i < 8; ++i) {
            proven_byte_t w[8];
            proven_size_t n = proven_quic_varint_put(edge[i], w);
            proven_quic_rd_t r = { w, n, true };
            all = all && n == len[i] && proven_quic_varint_len(edge[i]) == len[i] && proven_quic_rd_varint(&r) == edge[i] && r.ok;
        }
        proven_byte_t w[8] = { 0 };
        all = all && proven_quic_varint_put(PROVEN_QUIC_VARINT_MAX + 1, w) == 0 && proven_quic_varint_len(PROVEN_QUIC_VARINT_MAX + 1) == 0;
        proven_quic_rd_t empty = { w, 0, true };
        (void)proven_quic_rd_varint(&empty);
        all = all && !empty.ok;
        PROVEN_TEST_ASSERT(all, "each length's first and last value round-trip at their lengths; 2^62 has none", "");
    }

    PROVEN_TEST_SECTION("packet numbers (RFC 9000 appendix A.3)", "The RFC's example, the first packet, and both wraps.", "Check proven_quic_pn_decode.");
    {
        bool all = proven_quic_pn_decode(0xa82f30eaull + 1, 0x9b32, 16) == 0xa82f9b32ull;
        all = all && proven_quic_pn_decode(0, 0, 8) == 0 && proven_quic_pn_decode(0, 2, 32) == 2;
        /* Just past a window: the low bits mean the next window up, or the one below. */
        all = all && proven_quic_pn_decode(0x100 + 1, 0x01, 8) == 0x101 && proven_quic_pn_decode(0xff + 1, 0x01, 8) == 0x101;
        all = all && proven_quic_pn_decode(0x180 + 1, 0xff, 8) == 0x1ff && proven_quic_pn_decode(0x200 + 1, 0xff, 8) == 0x1ff;
        all = all && proven_quic_pn_decode(0x17e + 1, 0x00, 8) == 0x100 && proven_quic_pn_decode(0x17f + 1, 0x00, 8) == 0x200 && proven_quic_pn_decode(0x10 + 1, 0xf0, 8) == 0xf0;
        /* Near 2^62 no candidate above the space is taken. */
        all = all && proven_quic_pn_decode(PROVEN_QUIC_VARINT_MAX, 0x00, 8) == (PROVEN_QUIC_VARINT_MAX & ~(proven_u64)0xff);
        PROVEN_TEST_ASSERT(all, "the full number is the one closest to the next expected", "");
    }

    const proven_tls_suite_t *aes128 = proven_quic_initial_suite();
    proven_byte_t dcid[8], client_secret[32], server_secret[32], out[64];
    unhex(QV_DCID, dcid, sizeof dcid);
    proven_quic_keys_t ck, sk;

    PROVEN_TEST_SECTION("keys (RFC 9001 appendix A.1)", "Initial secrets from the connection ID, then each side's key, IV and header protection key.", "Check proven_quic_initial_secrets and proven_quic_keys_set.");
    {
        PROVEN_TEST_ASSERT(aes128 && aes128->id == PROVEN_TLS_AES_128_GCM_SHA256, "Initial packets use TLS_AES_128_GCM_SHA256", "");
        proven_byte_t initial[64];
        (void)proven_hkdf_extract(PROVEN_HMAC_SHA256, (proven_mem_view_t){ (const proven_byte_t *)"\x38\x76\x2c\xf7\xf5\x59\x34\xb3\x4d\x17\x9a\xe6\xa4\xc8\x0c\xad\xcc\xbb\x7f\x0a", 20 },
                                  (proven_mem_view_t){ dcid, 8 }, initial);
        PROVEN_TEST_ASSERT(equals_hex(initial, 32, QV_INITIAL_SECRET), "initial_secret from the version 1 salt", "");
        proven_quic_initial_secrets((proven_mem_view_t){ dcid, 8 }, client_secret, server_secret);
        PROVEN_TEST_ASSERT(equals_hex(client_secret, 32, QV_CLIENT_SECRET) && equals_hex(server_secret, 32, QV_SERVER_SECRET), "client_initial_secret and server_initial_secret", "");
        proven_mem_view_t none = { out, 0 };
        proven_tls13_expand_label(PROVEN_HMAC_SHA256, client_secret, "quic key", none, out, 16);
        bool labels = equals_hex(out, 16, QV_CLIENT_KEY);
        proven_tls13_expand_label(PROVEN_HMAC_SHA256, server_secret, "quic hp", none, out, 16);
        labels = labels && equals_hex(out, 16, QV_SERVER_HP);
        PROVEN_TEST_ASSERT(labels, "the labels \"quic key\" and \"quic hp\" behind the TLS 1.3 prefix", "");
        proven_quic_keys_set(&ck, aes128, client_secret);
        proven_quic_keys_set(&sk, aes128, server_secret);
        PROVEN_TEST_ASSERT(!ck.chacha && ck.key_len == 16 && equals_hex(ck.iv, 12, QV_CLIENT_IV) && same_aes(&ck.aead, QV_CLIENT_KEY) && same_hp(&ck.hp, QV_CLIENT_HP),
            "the client's key, IV and header protection key", "");
        PROVEN_TEST_ASSERT(equals_hex(sk.iv, 12, QV_SERVER_IV) && same_aes(&sk.aead, QV_SERVER_KEY) && same_hp(&sk.hp, QV_SERVER_HP),
            "the server's key, IV and header protection key", "");
    }

    PROVEN_TEST_SECTION("the client's Initial (RFC 9001 appendix A.2)", "A CRYPTO frame padded to 1162 bytes, packet number 2 in four bytes: the 1200-byte packet as printed.", "Check proven_quic_protect.");
    static proven_byte_t client_payload[1162];
    {
        memset(client_payload, 0, sizeof client_payload);
        unhex(QV_CLIENT_CRYPTO, client_payload, sizeof client_payload);
        proven_byte_t sample[16], mask[5], want[5];
        unhex(QV_CLIENT_SAMPLE, sample, 16);
        unhex(QV_CLIENT_MASK, want, 5);
        proven_quic_hp_mask(&ck, sample, mask);
        PROVEN_TEST_ASSERT(memcmp(mask, want, 5) == 0, "AES-ECB(hp, sample)[0..4]", "");
        PROVEN_TEST_ASSERT(protect_and_open(&ck, QV_CLIENT_HEADER, client_payload, sizeof client_payload, 2, QV_CLIENT_PACKET, 0),
            "protected as printed, and opened again to packet number 2 and the same payload", "");
    }

    PROVEN_TEST_SECTION("the server's Initial (RFC 9001 appendix A.3)", "An ACK and a CRYPTO frame, packet number 1 in two bytes.", "");
    {
        proven_byte_t payload[256], sample[16], mask[5], want[5];
        proven_size_t n = unhex(QV_SERVER_PAYLOAD, payload, sizeof payload);
        unhex(QV_SERVER_SAMPLE, sample, 16);
        unhex(QV_SERVER_MASK, want, 5);
        proven_quic_hp_mask(&sk, sample, mask);
        PROVEN_TEST_ASSERT(memcmp(mask, want, 5) == 0, "the server's mask", "");
        PROVEN_TEST_ASSERT(protect_and_open(&sk, QV_SERVER_HEADER, payload, n, 1, QV_SERVER_PACKET, 0), "protected as printed and opened again", "");
        /* The client has seen nothing from the server yet, or has seen packet 0. */
        static proven_byte_t buf[256];
        proven_u64 pn;
        proven_size_t at;
        proven_mem_mut_t plain;
        proven_size_t len = unhex(QV_SERVER_PACKET, buf, sizeof buf);
        PROVEN_TEST_ASSERT(proven_quic_unprotect(&sk, buf, 18, len, 1, &pn, &at, &plain) && pn == 1, "the same number after packet 0", "");
        len = unhex(QV_SERVER_PACKET, buf, sizeof buf);
        PROVEN_TEST_ASSERT(!proven_quic_unprotect(&ck, buf, 18, len, 0, &pn, &at, &plain), "the client's keys do not open the server's packet", "");
    }

    PROVEN_TEST_SECTION("Retry (RFC 9001 appendix A.4)", "The integrity tag over the client's connection ID and the packet.", "Check proven_quic_retry_tag.");
    {
        proven_byte_t pkt[128], tag[16];
        proven_size_t n = unhex(QV_RETRY_PACKET, pkt, sizeof pkt);
        proven_quic_retry_tag((proven_mem_view_t){ dcid, 8 }, (proven_mem_view_t){ pkt, n - 16 }, tag);
        PROVEN_TEST_ASSERT(memcmp(tag, pkt + n - 16, 16) == 0, "the tag as printed", "");
        PROVEN_TEST_ASSERT(proven_quic_retry_check((proven_mem_view_t){ dcid, 8 }, (proven_mem_view_t){ pkt, n }), "and it checks", "");
        bool none = true;
        for (proven_size_t bit = 0; bit < 8 * n; ++bit) {
            pkt[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
            none = none && !proven_quic_retry_check((proven_mem_view_t){ dcid, 8 }, (proven_mem_view_t){ pkt, n });
            pkt[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
        }
        for (proven_size_t bit = 0; bit < 64; ++bit) {
            dcid[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
            none = none && !proven_quic_retry_check((proven_mem_view_t){ dcid, 8 }, (proven_mem_view_t){ pkt, n });
            dcid[bit / 8] ^= (proven_byte_t)(1u << (bit % 8));
        }
        for (proven_size_t len = 0; len < n; ++len) none = none && !proven_quic_retry_check((proven_mem_view_t){ dcid, 8 }, (proven_mem_view_t){ pkt, len });
        none = none && !proven_quic_retry_check((proven_mem_view_t){ dcid, 7 }, (proven_mem_view_t){ pkt, n });
        PROVEN_TEST_ASSERT(none, "every bit change of the packet or of the connection ID, and every truncation, fails the check", "");
    }

    PROVEN_TEST_SECTION("ChaCha20-Poly1305, short header (RFC 9001 appendix A.5)", "Key, IV, header protection key and the next key phase's secret, then the 21-byte packet.", "");
    proven_quic_keys_t chk;
    {
        const proven_tls_suite_t *chacha = proven_tls_suite_find(PROVEN_TLS_CHACHA20_POLY1305_SHA256);
        proven_byte_t secret[32], key[32];
        unhex(QV_CHACHA_SECRET, secret, 32);
        proven_quic_keys_set(&chk, chacha, secret);
        unhex(QV_CHACHA_KEY, key, 32);
        PROVEN_TEST_ASSERT(chk.chacha && chk.key_len == 32 && memcmp(chk.aead.rk, key, 32) == 0 && equals_hex(chk.iv, 12, QV_CHACHA_IV) &&
                           equals_hex(chk.hp.rk, 32, QV_CHACHA_HP), "key, IV and header protection key", "");
        proven_quic_secret_update(chacha, secret);
        PROVEN_TEST_ASSERT(equals_hex(secret, 32, QV_CHACHA_KU), "\"quic ku\": the next key phase's secret", "");
        proven_byte_t sample[16], mask[5], want[5];
        unhex(QV_CHACHA_SAMPLE, sample, 16);
        unhex(QV_CHACHA_MASK, want, 5);
        proven_quic_hp_mask(&chk, sample, mask);
        PROVEN_TEST_ASSERT(memcmp(mask, want, 5) == 0, "the ChaCha20 mask: counter from the sample's first four bytes, little endian", "");
        proven_byte_t plain[1];
        unhex(QV_CHACHA_PLAIN, plain, 1);
        PROVEN_TEST_ASSERT(protect_and_open(&chk, QV_CHACHA_HEADER, plain, 1, QV_CHACHA_PN, QV_CHACHA_PACKET, QV_CHACHA_PN - 1000),
            "a PING in the smallest packet there is, packet number 654360564 in three bytes", "");
        proven_byte_t buf[32];
        unhex(QV_CHACHA_HEADER, buf, sizeof buf);
        PROVEN_TEST_ASSERT(proven_quic_protect(&chk, buf, 2, 1, 0, 2) == 0 && proven_quic_protect(&chk, buf, 4, 3, 0, 0) == 0 && proven_quic_protect(&chk, buf, 4, 5, 0, 4) == 0,
            "a packet too short to sample, or a packet number of five bytes, is not protected", "");
    }

    PROVEN_TEST_SECTION("every change refused", "Each single-bit change and each truncation of the three protected packets fails to open.", "A change that opens means the header is not all in the associated data, or the mask is applied to the wrong bits.");
    {
        unsigned tried = 0;
        PROVEN_TEST_ASSERT(every_change_refused(&ck, QV_CLIENT_PACKET, 18, 0, &tried), "the client's Initial", "");
        PROVEN_TEST_ASSERT(every_change_refused(&sk, QV_SERVER_PACKET, 18, 0, &tried), "the server's Initial", "");
        PROVEN_TEST_ASSERT(every_change_refused(&chk, QV_CHACHA_PACKET, 1, QV_CHACHA_PN - 1000, &tried), "the short header packet", "");
        const proven_size_t bytes = (strlen(QV_CLIENT_PACKET) + strlen(QV_SERVER_PACKET) + strlen(QV_CHACHA_PACKET)) / 2;
        PROVEN_TEST_ASSERT(tried == 9 * bytes, "every bit and every length was tried", "");
        PROVEN_TEST_INFO("{} changed packets refused", PROVEN_ARG(tried));
        /* The bits header protection covers: the low four of a long header's first byte, the
         * low five of a short header's (RFC 9001 section 5.4.1). The RFC's two long-header
         * masks both leave bit 4 alone, so this is checked over packet numbers whose masks
         * do not. */
        static proven_byte_t pkt[1300];
        unsigned long_bit4 = 0, short_bit5 = 0;
        bool kept = true;
        for (proven_u64 pn = 0; pn < 64; ++pn) {
            proven_byte_t mask[5];
            unhex(QV_CLIENT_HEADER, pkt, sizeof pkt);
            pkt[18] = 0; pkt[19] = 0; pkt[20] = 0; pkt[21] = (proven_byte_t)pn;
            memcpy(pkt + 22, client_payload, sizeof client_payload);
            const proven_byte_t first = pkt[0];
            kept = kept && proven_quic_protect(&ck, pkt, 22, 4, pn, sizeof client_payload) == 1200 && (pkt[0] & 0xf0) == (first & 0xf0);
            proven_quic_hp_mask(&ck, pkt + 22, mask);
            long_bit4 += (mask[0] & 0x10) != 0;
            pkt[0] = 0x42; pkt[1] = 0x00; pkt[2] = 0x00; pkt[3] = (proven_byte_t)pn;
            memset(pkt + 4, 0x01, 20);
            kept = kept && proven_quic_protect(&chk, pkt, 4, 3, pn, 20) == 40 && (pkt[0] & 0xe0) == 0x40;
            proven_quic_hp_mask(&chk, pkt + 5, mask);
            short_bit5 += (mask[0] & 0x20) != 0;
        }
        PROVEN_TEST_ASSERT(kept && long_bit4 > 0 && short_bit5 > 0,
            "a long header's top four bits and a short header's top three are never masked, over masks that would change them", "");
        /* Both suites with AES-256. */
        const proven_tls_suite_t *aes256 = proven_tls_suite_find(PROVEN_TLS_AES_256_GCM_SHA384);
        proven_byte_t secret[48];
        for (int i = 0; i < 48; ++i) secret[i] = (proven_byte_t)(i * 3);
        proven_quic_keys_t k;
        proven_quic_keys_set(&k, aes256, secret);
        static proven_byte_t buf[1300];
        memcpy(buf, "\x41\x00\x07", 3);                 /* a short header, packet number 7 in two bytes */
        memcpy(buf + 3, client_payload, 100);
        proven_size_t n = proven_quic_protect(&k, buf, 3, 2, 7, 100);
        proven_u64 pn;
        proven_size_t at;
        proven_mem_mut_t plain;
        PROVEN_TEST_ASSERT(n == 119 && !k.chacha && k.key_len == 32 && proven_quic_unprotect(&k, buf, 1, n, 5, &pn, &at, &plain) && pn == 7 && plain.size == 100 &&
                           memcmp(plain.ptr, client_payload, 100) == 0, "TLS_AES_256_GCM_SHA384 protects and opens with AES-256 for both keys", "");
    }

    PROVEN_TEST_SECTION("QUIC version 2 (RFC 9369 appendix A; RFC-0036)", "The version 2 salt and labels: the Initial keys, the client's and the server's Initial as printed, the Retry with version 2's key and nonce, the ChaCha20 short header packet with \"quicv2 ku\"; the headers read as the types RFC 9369 section 3.2 gives.", "Check proven_quic_initial_secrets_v, proven_quic_keys_set_v, proven_quic_secret_update_v, proven_quic_retry_tag and the type bits in quic_frame.c.");
    {
        proven_byte_t d2[8], cs2[32], ss2[32];
        unhex(QV2_DCID, d2, sizeof d2);
        proven_quic_initial_secrets_v((proven_mem_view_t){ d2, 8 }, PROVEN_QUIC_VERSION_2, cs2, ss2);
        PROVEN_TEST_ASSERT(equals_hex(cs2, 32, QV2_CLIENT_SECRET) && equals_hex(ss2, 32, QV2_SERVER_SECRET), "client_initial_secret and server_initial_secret from the version 2 salt", "");
        proven_quic_keys_t ck2, sk2;
        proven_quic_keys_set_v(&ck2, aes128, cs2, PROVEN_QUIC_VERSION_2);
        proven_quic_keys_set_v(&sk2, aes128, ss2, PROVEN_QUIC_VERSION_2);
        PROVEN_TEST_ASSERT(equals_hex(ck2.iv, 12, QV2_CLIENT_IV) && same_aes(&ck2.aead, QV2_CLIENT_KEY) && same_hp(&ck2.hp, QV2_CLIENT_HP) &&
                           equals_hex(sk2.iv, 12, QV2_SERVER_IV) && same_aes(&sk2.aead, QV2_SERVER_KEY) && same_hp(&sk2.hp, QV2_SERVER_HP),
            "each side's key, IV and header protection key under \"quicv2 key\", \"quicv2 iv\", \"quicv2 hp\"", "");
        static proven_byte_t cp2[1162];
        memset(cp2, 0, sizeof cp2);
        unhex(QV2_CLIENT_CRYPTO, cp2, sizeof cp2);
        proven_byte_t sample[16], mask[5], want[5];
        unhex(QV2_CLIENT_SAMPLE, sample, 16);
        unhex(QV2_CLIENT_MASK, want, 5);
        proven_quic_hp_mask(&ck2, sample, mask);
        PROVEN_TEST_ASSERT(memcmp(mask, want, 5) == 0 && protect_and_open(&ck2, QV2_CLIENT_HEADER, cp2, sizeof cp2, 2, QV2_CLIENT_PACKET, 0),
            "the client's Initial protected as printed and opened again", "");
        proven_byte_t payload[256];
        proven_size_t n = unhex(QV2_SERVER_PAYLOAD, payload, sizeof payload);
        PROVEN_TEST_ASSERT(protect_and_open(&sk2, QV2_SERVER_HEADER, payload, n, 1, QV2_SERVER_PACKET, 0), "the server's Initial as printed", "");
        static proven_byte_t pk[1300];
        proven_quic_hdr_t h;
        n = unhex(QV2_CLIENT_PACKET, pk, sizeof pk);
        bool types = proven_quic_hdr_parse((proven_mem_view_t){ pk, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_INITIAL && h.version == PROVEN_QUIC_VERSION_2 && h.len == n;
        proven_byte_t rp[128], tag[16];
        n = unhex(QV2_RETRY_PACKET, rp, sizeof rp);
        types = types && proven_quic_hdr_parse((proven_mem_view_t){ rp, n }, 8, &h) && h.type == PROVEN_QUIC_PKT_RETRY;
        proven_quic_retry_tag((proven_mem_view_t){ d2, 8 }, (proven_mem_view_t){ rp, n - 16 }, tag);
        PROVEN_TEST_ASSERT(memcmp(tag, rp + n - 16, 16) == 0 && proven_quic_retry_check((proven_mem_view_t){ d2, 8 }, (proven_mem_view_t){ rp, n }),
            "the Retry's tag under version 2's key and nonce, chosen by the packet's Version field", "");
        /* A header built in each version and read back as the same type. */
        const proven_quic_cid_t a = { .len = 4, .id = { 1, 2, 3, 4 } }, b = { .len = 2, .id = { 9, 9 } };
        const proven_quic_pkt_type_t kinds[3] = { PROVEN_QUIC_PKT_INITIAL, PROVEN_QUIC_PKT_0RTT, PROVEN_QUIC_PKT_HANDSHAKE };
        for (int v = 0; v < 2; ++v) for (int k = 0; k < 3; ++k) {
            proven_byte_t hb[64];
            const proven_u32 ver = v ? PROVEN_QUIC_VERSION_2 : PROVEN_QUIC_VERSION_1;
            const proven_size_t hl = proven_quic_long_header_put_v(ver, kinds[k], &a, &b, (proven_mem_view_t){ NULL, 0 }, 1, 1, 30, (proven_mem_mut_t){ hb, sizeof hb });
            memset(hb + hl, 0, sizeof hb - hl);
            types = types && hl > 0 && proven_quic_hdr_parse((proven_mem_view_t){ hb, hl + 30 + 16 > sizeof hb ? sizeof hb : hl + 30 + 16 }, 8, &h) && h.type == kinds[k] && h.version == ver;
            if (v && k == 0) types = types && ((hb[0] >> 4) & 3) == 1;          /* version 2's Initial is 0b01 */
        }
        PROVEN_TEST_ASSERT(types, "version 2's type bits: Initial 0b01, 0-RTT 0b10, Handshake 0b11, Retry 0b00, written and read", "");
        const proven_tls_suite_t *chacha = proven_tls_suite_find(PROVEN_TLS_CHACHA20_POLY1305_SHA256);
        proven_byte_t secret[32], key[32];
        proven_quic_keys_t chk2;
        unhex(QV2_CHACHA_SECRET, secret, 32);
        proven_quic_keys_set_v(&chk2, chacha, secret, PROVEN_QUIC_VERSION_2);
        unhex(QV2_CHACHA_KEY, key, 32);
        PROVEN_TEST_ASSERT(memcmp(chk2.aead.rk, key, 32) == 0 && equals_hex(chk2.iv, 12, QV2_CHACHA_IV) && equals_hex(chk2.hp.rk, 32, QV2_CHACHA_HP), "ChaCha20's key, IV and header protection key", "");
        proven_quic_secret_update_v(chacha, secret, PROVEN_QUIC_VERSION_2);
        PROVEN_TEST_ASSERT(equals_hex(secret, 32, QV2_CHACHA_KU), "\"quicv2 ku\": the next key phase's secret", "");
        proven_byte_t plain[1];
        unhex(QV2_CHACHA_PLAIN, plain, 1);
        PROVEN_TEST_ASSERT(protect_and_open(&chk2, QV2_CHACHA_HEADER, plain, 1, QV2_CHACHA_PN, QV2_CHACHA_PACKET, QV2_CHACHA_PN - 1000), "the 21-byte short header packet as printed", "");
        unsigned tried = 0;
        PROVEN_TEST_ASSERT(every_change_refused(&ck2, QV2_CLIENT_PACKET, 18, 0, &tried) && every_change_refused(&sk2, QV2_SERVER_PACKET, 18, 0, &tried), "every change of the two Initials refused", "");
        PROVEN_TEST_ASSERT(!protect_and_open(&ck, QV2_CLIENT_HEADER, cp2, sizeof cp2, 2, QV2_CLIENT_PACKET, 0), "version 1's keys do not make version 2's packet", "");
    }

    PROVEN_TEST_SECTION("transport parameters (RFC 9000 section 18)", "The RFC's own ClientHello's, every parameter both ways, and each refusal.", "Check proven_quic_tp_encode and proven_quic_tp_decode.");
    {
        /* The quic_transport_parameters extension (0x0039) inside appendix A.2's ClientHello. */
        proven_size_t at = 0;
        while (at + 4 < sizeof client_payload && !(client_payload[at] == 0x00 && client_payload[at + 1] == 0x39 && client_payload[at + 2] == 0x00)) ++at;
        proven_size_t len = client_payload[at + 3];
        proven_quic_tp_t tp;
        PROVEN_TEST_ASSERT(len == 0x32 && proven_quic_tp_decode((proven_mem_view_t){ client_payload + at + 4, len }, false, &tp) == 0, "the RFC's client parameters decode", "");
        PROVEN_TEST_ASSERT(tp.initial_max_data == PROVEN_QUIC_VARINT_MAX && tp.initial_max_stream_data_bidi_local == 65535 &&
                           tp.initial_max_stream_data_bidi_remote == 65535 && tp.initial_max_stream_data_uni == 65535 &&
                           tp.initial_max_streams_bidi == 16 && tp.initial_max_streams_uni == 16 && tp.max_idle_timeout == 30000 &&
                           tp.has_initial_scid && tp.initial_scid.len == 8 && memcmp(tp.initial_scid.id, dcid, 8) == 0 &&
                           tp.max_udp_payload_size == 65527 && tp.ack_delay_exponent == 3 && tp.max_ack_delay == 25 && tp.active_connection_id_limit == 2 &&
                           !tp.disable_active_migration && !tp.has_original_dcid, "to the values it carries, and the defaults for the rest", "");
        PROVEN_TEST_ASSERT(proven_quic_tp_decode((proven_mem_view_t){ client_payload + at + 4, len }, true, &tp) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR,
            "the same list from a server lacks original_destination_connection_id", "");

        /* Every parameter, from a server. */
        proven_quic_tp_t all, back;
        proven_quic_tp_defaults(&all);
        all.max_idle_timeout = 30000; all.max_udp_payload_size = 1452; all.initial_max_data = 1u << 20;
        all.initial_max_stream_data_bidi_local = 1; all.initial_max_stream_data_bidi_remote = 262144; all.initial_max_stream_data_uni = 16383;
        all.initial_max_streams_bidi = 100; all.initial_max_streams_uni = 3; all.ack_delay_exponent = 20; all.max_ack_delay = 16383;
        all.active_connection_id_limit = 8; all.disable_active_migration = true;
        all.has_original_dcid = all.has_initial_scid = all.has_retry_scid = all.has_reset_token = true;
        all.original_dcid.len = 20; all.initial_scid.len = 0; all.retry_scid.len = 1;
        for (int i = 0; i < 20; ++i) all.original_dcid.id[i] = (proven_byte_t)(0xa0 + i);
        all.retry_scid.id[0] = 0x55;
        for (int i = 0; i < 16; ++i) all.reset_token[i] = (proven_byte_t)(0xe0 + i);
        proven_byte_t enc[256];
        proven_size_t n = proven_quic_tp_encode(&all, true, (proven_mem_mut_t){ enc, sizeof enc });
        PROVEN_TEST_ASSERT(n > 0 && proven_quic_tp_decode((proven_mem_view_t){ enc, n }, true, &back) == 0 && memcmp(&back, &all, sizeof all) == 0,
            "every parameter a server sends comes back as it went, a zero-length connection ID included", "");
        bool small = true;
        for (proven_size_t cap = 0; cap < n; ++cap) small = small && proven_quic_tp_encode(&all, true, (proven_mem_mut_t){ enc, cap }) == 0;
        PROVEN_TEST_ASSERT(small, "every buffer one byte or more too small is refused", "");
        PROVEN_TEST_ASSERT(proven_quic_tp_encode(&all, false, (proven_mem_mut_t){ enc, sizeof enc }) == 0, "a client never sends what only a server may", "");

        /* The defaults are not sent; a client's list is its connection ID alone. */
        proven_quic_tp_t c;
        proven_quic_tp_defaults(&c);
        c.has_initial_scid = true; c.initial_scid.len = 4; memcpy(c.initial_scid.id, "\x01\x02\x03\x04", 4);
        n = proven_quic_tp_encode(&c, false, (proven_mem_mut_t){ enc, sizeof enc });
        PROVEN_TEST_ASSERT(n == 6 && memcmp(enc, "\x0f\x04\x01\x02\x03\x04", 6) == 0, "a parameter at its default is left out", "");
        bool prefixes = true;
        for (proven_size_t k = 0; k < n; ++k) prefixes = prefixes && proven_quic_tp_decode((proven_mem_view_t){ enc, k }, false, &back) != 0;
        PROVEN_TEST_ASSERT(prefixes, "every prefix of it is refused", "");

        static const char *const out_of_range[] = { "0302444b", "03048000fff8", "0a0115", "0b0480004000", "0e0101", "0808d000000000000001", "0908d000000000000001" };
        static const char *const in_range[] = { "030244b0", "03048000fff7", "0a0114", "0b027fff", "0e0102", "0808d000000000000000", "0908d000000000000000" };
        bool ranges = true;
        char hex[128];
        for (proven_size_t i = 0; i < sizeof out_of_range / sizeof out_of_range[0]; ++i) {
            snprintf(hex, sizeof hex, "0f00%s", out_of_range[i]);
            ranges = ranges && tp_decode_hex(hex, false, &back) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR;
        }
        for (proven_size_t i = 0; i < sizeof in_range / sizeof in_range[0]; ++i) {
            snprintf(hex, sizeof hex, "0f00%s", in_range[i]);
            ranges = ranges && tp_decode_hex(hex, false, &back) == 0;
        }
        PROVEN_TEST_ASSERT(ranges, "max_udp_payload_size below 1200 or above 65527, ack_delay_exponent above 20, max_ack_delay of 2^14, "
                                   "active_connection_id_limit below 2, stream counts above 2^60: each refused, and the value next to each accepted", "");
        PROVEN_TEST_ASSERT(proven_quic_tp_decode((proven_mem_view_t){ (const proven_byte_t *)"\x0f\x00", 2 }, false, &back) == 0 &&
                           back.has_initial_scid && back.initial_scid.len == 0, "a zero-length connection ID", "");
        static const char *const refused[] = {
            "",                                                 /* no initial_source_connection_id */
            "0f000f00",                                         /* repeated */
            "0f000101", "0f00010300", "0f0001021e1e",           /* an integer that does not fill its parameter, or is missing */
            "0f000c0100",                                       /* disable_active_migration with a value */
            "0f0000020102", "0f00020f000102030405060708090a0b0c0d0e", "0f001002aabb",   /* server-only, from a client */
            "0f15000102030405060708090a0b0c0d0e0f1011121314",   /* a connection ID of 21 bytes */
            "0f0001", "0f0004", "0f004001",                     /* a length past the end */
        };
        bool r = true;
        for (proven_size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) r = r && tp_decode_hex(refused[i], false, &back) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR;
        PROVEN_TEST_ASSERT(r, "missing, repeated, malformed, server-only and oversized parameters from a client are refused", "");
        PROVEN_TEST_ASSERT(tp_decode_hex("0f00" "000102" "020f" "000000000000000000000000000000", true, &back) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR, "a stateless reset token that is not 16 bytes", "");
        PROVEN_TEST_ASSERT(tp_decode_hex("0f001b00" "404102" "7f7f" "3f06" "1234567890ab", false, &back) == 0 &&
                           tp_decode_hex("0f00" "c000000000000033" "00" "1f04aabbccdd", false, &back) == 0,
            "unknown and reserved parameters (27, 31 * N + 27, ids of every length) are skipped", "");
        /* version_information (RFC 9368 section 3; RFC-0036). */
        bool vi = tp_decode_hex("0f00" "1108" "00000001" "00000001", false, &back) == 0 && back.has_version_info && back.chosen_version == 1 && back.available_n == 1;
        vi = vi && tp_decode_hex("0f00" "110c" "00000001" "000000016b3343cf", false, &back) == 0 && back.available_n == 2 && back.available[1] == PROVEN_QUIC_VERSION_2;
        vi = vi && tp_decode_hex("0f00" "000100" "1104" "6b3343cf", true, &back) == 0 && back.chosen_version == PROVEN_QUIC_VERSION_2 && back.available_n == 0;
        PROVEN_TEST_ASSERT(vi, "version_information read: a client's chosen version among its list; a server's list may be empty", "");
        static const char *const vi_bad[] = {
            "1108" "00000001" "6b3343cf",                   /* a client's chosen version not in its list */
            "1104" "00000001",                              /* nor in an empty one */
            "1105" "0000000100", "1103" "000001", "1100",   /* not a whole number of versions */
            "1108" "00000000" "00000000",                   /* a zero chosen version */
            "1108" "00000001" "00000000",                   /* a zero available version */
            "1128" "00000001" "000000010000000100000001000000010000000100000001000000010000000100000001",   /* nine: more than kept */
        };
        bool vr = true;
        for (proven_size_t i = 0; i < sizeof vi_bad / sizeof vi_bad[0]; ++i) {
            snprintf(hex, sizeof hex, "0f00%s", vi_bad[i]);
            vr = vr && tp_decode_hex(hex, false, &back) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR;
        }
        vr = vr && tp_decode_hex("0f00" "000100" "1108" "00000000" "00000001", true, &back) == PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR;
        PROVEN_TEST_ASSERT(vr, "a malformed version_information, a zero version, or a client's chosen version missing from its list: TRANSPORT_PARAMETER_ERROR", "");
        proven_quic_tp_t vt;
        proven_quic_tp_defaults(&vt);
        vt.has_initial_scid = true;
        vt.has_version_info = true; vt.chosen_version = PROVEN_QUIC_VERSION_2; vt.available_n = 2;
        vt.available[0] = PROVEN_QUIC_VERSION_2; vt.available[1] = PROVEN_QUIC_VERSION_1;
        n = proven_quic_tp_encode(&vt, false, (proven_mem_mut_t){ enc, sizeof enc });
        PROVEN_TEST_ASSERT(n > 0 && proven_quic_tp_decode((proven_mem_view_t){ enc, n }, false, &back) == 0 && memcmp(&back, &vt, sizeof vt) == 0,
            "version_information written and read back", "");

        /* preferred_address: 4 + 2 + 16 + 2 + 1 + cid + 16. */
        const char *pa_ok = "0f00" "000100" "0d2d" "7f000001" "01bb" "00000000000000000000000000000001" "01bb" "04" "a1a2a3a4" "000102030405060708090a0b0c0d0e0f";
        const char *pa_zero = "0f00" "000100" "0d29" "7f000001" "01bb" "00000000000000000000000000000001" "01bb" "00" "000102030405060708090a0b0c0d0e0f";
        const char *pa_short = "0f00" "000100" "0d2c" "7f000001" "01bb" "00000000000000000000000000000001" "01bb" "04" "a1a2a3a4" "000102030405060708090a0b0c0d0e";
        PROVEN_TEST_ASSERT(tp_decode_hex(pa_ok, true, &back) == 0 && back.has_preferred_address && back.preferred_address.ipv4_port == 443 &&
                           back.preferred_address.cid.len == 4 && back.preferred_address.reset_token[15] == 0x0f,
            "a server's preferred_address is read (and never acted on: RFC-0017 B-5)", "");
        PROVEN_TEST_ASSERT(tp_decode_hex(pa_zero, true, &back) != 0 && tp_decode_hex(pa_short, true, &back) != 0 && tp_decode_hex(pa_ok, false, &back) != 0,
            "with a zero-length connection ID, cut short, or from a client, it is refused", "");
        all.has_preferred_address = true;
        PROVEN_TEST_ASSERT(proven_quic_tp_encode(&all, true, (proven_mem_mut_t){ enc, sizeof enc }) == 0, "this library never sends one", "");
        all.has_preferred_address = false;
        all.max_udp_payload_size = 1199;
        PROVEN_TEST_ASSERT(proven_quic_tp_encode(&all, true, (proven_mem_mut_t){ enc, sizeof enc }) == 0, "nor a value out of its range", "");
    }

    PROVEN_TEST_PASS("QUIC packet protection reproduces RFC 9001 appendix A and every change is refused.");
    return 0;
}

#else

int main(void) {
    PROVEN_TEST_SUITE("QUIC packet protection and transport parameters", "Built with PROVEN_NO_HTTP3: nothing to test.", "");
    PROVEN_TEST_PASS("skipped: PROVEN_NO_HTTP3");
    return 0;
}

#endif
