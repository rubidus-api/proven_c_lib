#include "proven_internal_quic.h"

#ifndef PROVEN_NO_HTTP3

/* QUIC version 1 below the transport: the integers of RFC 9000 section 16, packet numbers,
 * packet protection and the Retry tag of RFC 9001 section 5, and the transport parameters of
 * RFC 9000 section 18. Pure computation: no I/O, no allocation.
 *
 * Every packet that reaches this code is from a stranger, unauthenticated until it opens.
 * Header protection is removed with the packet's own bytes as its only input, so nothing here
 * reads past `packet_len`, and a packet that does not open leaves no state behind. */

/* ---- Variable-length integers ---- */

proven_size_t proven_quic_varint_len(proven_u64 v) {
    return v < 64 ? 1 : v < 16384 ? 2 : v < 1073741824 ? 4 : v <= PROVEN_QUIC_VARINT_MAX ? 8 : 0;
}

proven_size_t proven_quic_varint_put(proven_u64 v, proven_byte_t out[8]) {
    proven_size_t n = proven_quic_varint_len(v);
    for (proven_size_t i = 0; i < n; ++i) out[i] = (proven_byte_t)(v >> (8 * (n - 1 - i)));
    if (n > 0) out[0] |= (proven_byte_t)(n == 1 ? 0x00 : n == 2 ? 0x40 : n == 4 ? 0x80 : 0xc0);
    return n;
}

proven_u64 proven_quic_rd_uint(proven_quic_rd_t *r, proven_size_t bytes) {
    if (!r->ok || r->n < bytes) { r->ok = false; return 0; }
    proven_u64 v = 0;
    for (proven_size_t i = 0; i < bytes; ++i) v = (v << 8) | r->p[i];
    r->p += bytes; r->n -= bytes;
    return v;
}

proven_u64 proven_quic_rd_varint(proven_quic_rd_t *r) {
    if (!r->ok || r->n == 0) { r->ok = false; return 0; }
    proven_size_t n = (proven_size_t)1 << (r->p[0] >> 6);
    if (r->n < n) { r->ok = false; return 0; }
    proven_u64 v = r->p[0] & 0x3f;
    for (proven_size_t i = 1; i < n; ++i) v = (v << 8) | r->p[i];
    r->p += n; r->n -= n;
    return v;
}

proven_quic_rd_t proven_quic_rd_take(proven_quic_rd_t *r, proven_size_t n) {
    proven_quic_rd_t sub = { r->p, 0, false };
    if (!r->ok || r->n < n) { r->ok = false; return sub; }
    sub.n = n; sub.ok = true;
    r->p += n; r->n -= n;
    return sub;
}

/* ---- Packet numbers ---- */

proven_u64 proven_quic_pn_decode(proven_u64 largest_plus_one, proven_u64 truncated, unsigned bits) {
    const proven_u64 expected = largest_plus_one, win = (proven_u64)1 << bits, hwin = win / 2, mask = win - 1;
    const proven_u64 candidate = (expected & ~mask) | truncated;
    /* RFC 9000 appendix A.3, with the subtraction moved to the other side where it could wrap. */
    if (candidate + hwin <= expected && candidate < ((proven_u64)1 << 62) - win) return candidate + win;
    if (candidate > expected + hwin && candidate >= win) return candidate - win;
    return candidate;
}

/* ---- Packet protection ---- */

static const proven_byte_t INITIAL_SALT_V1[20] = {
    0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a };

const proven_tls_suite_t *proven_quic_initial_suite(void) { return proven_tls_suite_find(PROVEN_TLS_AES_128_GCM_SHA256); }

void proven_quic_initial_secrets(proven_mem_view_t dcid, proven_byte_t client[32], proven_byte_t server[32]) {
    proven_byte_t initial[PROVEN_HMAC_MAX_SIZE];          /* the extract's output is declared at the largest size */
    (void)proven_hkdf_extract(PROVEN_HMAC_SHA256, (proven_mem_view_t){ .ptr = INITIAL_SALT_V1, .size = sizeof INITIAL_SALT_V1 }, dcid, initial);
    proven_mem_view_t none = { .ptr = initial, .size = 0 };
    proven_tls13_expand_label(PROVEN_HMAC_SHA256, initial, "client in", none, client, 32);
    proven_tls13_expand_label(PROVEN_HMAC_SHA256, initial, "server in", none, server, 32);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = initial, .size = sizeof initial });
}

static void cipher_init(proven_crypto_aes_gcm_t *ctx, bool chacha, const proven_byte_t *key, proven_size_t key_len) {
    if (chacha) for (int i = 0; i < 32; ++i) ctx->rk[i] = key[i];
    else (void)proven_crypto_aes_gcm_init(ctx, key, key_len);                  /* 16 or 32: cannot fail */
}

void proven_quic_keys_set(proven_quic_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *secret) {
    proven_byte_t key[32];
    proven_mem_view_t none = { .ptr = key, .size = 0 };
    keys->chacha = suite->chacha;
    keys->key_len = suite->key_len;
    proven_tls13_expand_label(suite->hash, secret, "quic key", none, key, suite->key_len);
    cipher_init(&keys->aead, suite->chacha, key, suite->key_len);
    proven_tls13_expand_label(suite->hash, secret, "quic iv", none, keys->iv, 12);
    proven_tls13_expand_label(suite->hash, secret, "quic hp", none, key, suite->key_len);
    cipher_init(&keys->hp, suite->chacha, key, suite->key_len);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = key, .size = sizeof key });
}

void proven_quic_secret_update(const proven_tls_suite_t *suite, proven_byte_t *secret) {
    proven_byte_t next[PROVEN_HMAC_MAX_SIZE];
    proven_tls13_expand_label(suite->hash, secret, "quic ku", (proven_mem_view_t){ .ptr = next, .size = 0 }, next, suite->hash_len);
    for (proven_size_t i = 0; i < suite->hash_len; ++i) secret[i] = next[i];
    proven_mem_wipe((proven_mem_mut_t){ .ptr = next, .size = sizeof next });
}

void proven_quic_hp_mask(const proven_quic_keys_t *keys, const proven_byte_t sample[16], proven_byte_t mask[5]) {
    if (keys->chacha) {
        /* RFC 9001 section 5.4.4: the sample's first four bytes are the block counter, little
         * endian, and the other twelve the nonce; the mask is the keystream over five zeros. */
        proven_u32 counter = (proven_u32)sample[0] | (proven_u32)sample[1] << 8 | (proven_u32)sample[2] << 16 | (proven_u32)sample[3] << 24;
        static const proven_byte_t zeros[5] = { 0 };
        proven_crypto_chacha20(keys->hp.rk, counter, sample + 4, zeros, mask, 5);
    } else {
        proven_byte_t block[16];
        proven_crypto_aes_encrypt_block(&keys->hp, sample, block);
        for (int i = 0; i < 5; ++i) mask[i] = block[i];
    }
}

static void packet_nonce(const proven_quic_keys_t *keys, proven_u64 pn, proven_byte_t nonce[12]) {
    for (int i = 0; i < 12; ++i) nonce[i] = keys->iv[i];
    for (int i = 0; i < 8; ++i) nonce[11 - i] ^= (proven_byte_t)(pn >> (8 * i));
}

/* The bits of the first byte that header protection covers: four for a long header, five for a short one. */
static proven_byte_t first_byte_mask(proven_byte_t first) { return (first & 0x80) ? 0x0f : 0x1f; }

proven_size_t proven_quic_protect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t header_len, proven_size_t pn_len,
                                  proven_u64 pn, proven_size_t payload_len) {
    /* The sample starts four bytes after the packet number field begins, whatever its length,
     * and needs sixteen bytes of ciphertext there. */
    if (pn_len < 1 || pn_len > 4 || header_len < pn_len + 1 || pn_len + payload_len < 4) return 0;
    proven_byte_t nonce[12];
    packet_nonce(keys, pn, nonce);
    proven_byte_t *payload = packet + header_len;
    proven_mem_view_t aad = { .ptr = packet, .size = header_len }, plain = { .ptr = payload, .size = payload_len };
    if (keys->chacha) proven_crypto_chacha20poly1305_seal(keys->aead.rk, nonce, aad, plain, payload, payload + payload_len);
    else proven_crypto_aes_gcm_seal(&keys->aead, nonce, aad, plain, payload, payload + payload_len);
    const proven_size_t pn_offset = header_len - pn_len;
    proven_byte_t mask[5];
    proven_quic_hp_mask(keys, packet + pn_offset + 4, mask);
    packet[0] ^= (proven_byte_t)(mask[0] & first_byte_mask(packet[0]));
    for (proven_size_t i = 0; i < pn_len; ++i) packet[pn_offset + i] ^= mask[1 + i];
    return header_len + payload_len + 16;
}

bool proven_quic_unprotect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t pn_offset, proven_size_t packet_len,
                           proven_u64 largest_plus_one, proven_u64 *pn, proven_size_t *header_len, proven_mem_mut_t *payload) {
    /* Enough for the sample, and for the shortest packet number with a tag after it. */
    if (pn_offset < 1 || pn_offset > packet_len || packet_len - pn_offset < 4 + 16) return false;
    proven_byte_t mask[5];
    proven_quic_hp_mask(keys, packet + pn_offset + 4, mask);
    packet[0] ^= (proven_byte_t)(mask[0] & first_byte_mask(packet[0]));
    const proven_size_t pn_len = (proven_size_t)(packet[0] & 0x03) + 1;
    proven_u64 truncated = 0;
    for (proven_size_t i = 0; i < pn_len; ++i) {
        packet[pn_offset + i] ^= mask[1 + i];
        truncated = (truncated << 8) | packet[pn_offset + i];
    }
    const proven_size_t hlen = pn_offset + pn_len;
    if (packet_len - hlen < 16) return false;
    const proven_u64 full = proven_quic_pn_decode(largest_plus_one, truncated, (unsigned)(8 * pn_len));
    proven_byte_t nonce[12];
    packet_nonce(keys, full, nonce);
    proven_byte_t *body = packet + hlen;
    const proven_size_t clen = packet_len - hlen - 16;
    proven_mem_view_t aad = { .ptr = packet, .size = hlen }, cipher = { .ptr = body, .size = clen };
    bool ok = keys->chacha ? proven_crypto_chacha20poly1305_open(keys->aead.rk, nonce, aad, cipher, body + clen, body)
                           : proven_crypto_aes_gcm_open(&keys->aead, nonce, aad, cipher, body + clen, body);
    if (!ok) return false;
    /* What opened is data for the layer above, which parses it as any parser does. */
    PROVEN_CT_PUBLIC(body, clen);
    *pn = full;
    *header_len = hlen;
    *payload = (proven_mem_mut_t){ .ptr = body, .size = clen };
    return true;
}

/* ---- Retry ---- */

static const proven_byte_t RETRY_KEY_V1[16] = {
    0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e };
static const proven_byte_t RETRY_NONCE_V1[12] = { 0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb };

void proven_quic_retry_tag(proven_mem_view_t odcid, proven_mem_view_t retry, proven_byte_t tag[16]) {
    /* The tag is AES-128-GCM under a published key over nothing, with this pseudo-packet as
     * the associated data: the ODCID behind its length byte, then the Retry packet. A Retry
     * longer than a datagram this library accepts gets an all-zero tag, which no check passes. */
    proven_byte_t pseudo[1 + PROVEN_QUIC_MAX_CID + 1500];
    proven_size_t n = 0;
    if (odcid.size > PROVEN_QUIC_MAX_CID || retry.size > 1500) {
        for (int i = 0; i < 16; ++i) tag[i] = 0;
        return;
    }
    pseudo[n++] = (proven_byte_t)odcid.size;
    for (proven_size_t i = 0; i < odcid.size; ++i) pseudo[n++] = odcid.ptr[i];
    for (proven_size_t i = 0; i < retry.size; ++i) pseudo[n++] = retry.ptr[i];
    proven_crypto_aes_gcm_t ctx;
    (void)proven_crypto_aes_gcm_init(&ctx, RETRY_KEY_V1, sizeof RETRY_KEY_V1);
    proven_crypto_aes_gcm_seal(&ctx, RETRY_NONCE_V1, (proven_mem_view_t){ .ptr = pseudo, .size = n },
                               (proven_mem_view_t){ .ptr = pseudo, .size = 0 }, pseudo, tag);
}

bool proven_quic_retry_check(proven_mem_view_t odcid, proven_mem_view_t retry_packet) {
    if (retry_packet.size < 16 || retry_packet.size - 16 > 1500 || odcid.size > PROVEN_QUIC_MAX_CID) return false;
    proven_byte_t want[16];
    proven_quic_retry_tag(odcid, (proven_mem_view_t){ .ptr = retry_packet.ptr, .size = retry_packet.size - 16 }, want);
    return proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = 16 },
                               (proven_mem_view_t){ .ptr = retry_packet.ptr + retry_packet.size - 16, .size = 16 });
}

/* ---- Transport parameters ---- */

enum {
    TP_ORIGINAL_DCID = 0x00, TP_MAX_IDLE_TIMEOUT = 0x01, TP_RESET_TOKEN = 0x02, TP_MAX_UDP_PAYLOAD = 0x03,
    TP_MAX_DATA = 0x04, TP_MAX_STREAM_DATA_BIDI_LOCAL = 0x05, TP_MAX_STREAM_DATA_BIDI_REMOTE = 0x06,
    TP_MAX_STREAM_DATA_UNI = 0x07, TP_MAX_STREAMS_BIDI = 0x08, TP_MAX_STREAMS_UNI = 0x09, TP_ACK_DELAY_EXPONENT = 0x0a,
    TP_MAX_ACK_DELAY = 0x0b, TP_DISABLE_MIGRATION = 0x0c, TP_PREFERRED_ADDRESS = 0x0d, TP_CID_LIMIT = 0x0e,
    TP_INITIAL_SCID = 0x0f, TP_RETRY_SCID = 0x10
};

#define STREAMS_MAX (((proven_u64)1) << 60)

void proven_quic_tp_defaults(proven_quic_tp_t *tp) {
    proven_byte_t *b = (proven_byte_t *)tp;
    for (proven_size_t i = 0; i < sizeof *tp; ++i) b[i] = 0;
    tp->max_udp_payload_size = 65527;
    tp->ack_delay_exponent = 3;
    tp->max_ack_delay = 25;
    tp->active_connection_id_limit = 2;
}

/* The integer parameters, in the order of their ids, and the default each is left out at. */
static const proven_u8 TP_INT_IDS[] = {
    TP_MAX_IDLE_TIMEOUT, TP_MAX_UDP_PAYLOAD, TP_MAX_DATA, TP_MAX_STREAM_DATA_BIDI_LOCAL, TP_MAX_STREAM_DATA_BIDI_REMOTE,
    TP_MAX_STREAM_DATA_UNI, TP_MAX_STREAMS_BIDI, TP_MAX_STREAMS_UNI, TP_ACK_DELAY_EXPONENT, TP_MAX_ACK_DELAY, TP_CID_LIMIT };

static proven_u64 *tp_field(proven_quic_tp_t *tp, proven_u8 id) {
    switch (id) {
        case TP_MAX_IDLE_TIMEOUT: return &tp->max_idle_timeout;
        case TP_MAX_UDP_PAYLOAD: return &tp->max_udp_payload_size;
        case TP_MAX_DATA: return &tp->initial_max_data;
        case TP_MAX_STREAM_DATA_BIDI_LOCAL: return &tp->initial_max_stream_data_bidi_local;
        case TP_MAX_STREAM_DATA_BIDI_REMOTE: return &tp->initial_max_stream_data_bidi_remote;
        case TP_MAX_STREAM_DATA_UNI: return &tp->initial_max_stream_data_uni;
        case TP_MAX_STREAMS_BIDI: return &tp->initial_max_streams_bidi;
        case TP_MAX_STREAMS_UNI: return &tp->initial_max_streams_uni;
        case TP_ACK_DELAY_EXPONENT: return &tp->ack_delay_exponent;
        case TP_MAX_ACK_DELAY: return &tp->max_ack_delay;
        case TP_CID_LIMIT: return &tp->active_connection_id_limit;
        default: return NULL;
    }
}

static proven_u64 tp_default(proven_u8 id) {
    return id == TP_MAX_UDP_PAYLOAD ? 65527 : id == TP_ACK_DELAY_EXPONENT ? 3 : id == TP_MAX_ACK_DELAY ? 25 : id == TP_CID_LIMIT ? 2 : 0;
}

/* A value inside the range RFC 9000 section 18.2 gives its parameter. */
static bool tp_int_ok(proven_u8 id, proven_u64 v) {
    switch (id) {
        case TP_MAX_UDP_PAYLOAD: return v >= 1200 && v <= 65527;
        case TP_ACK_DELAY_EXPONENT: return v <= 20;
        case TP_MAX_ACK_DELAY: return v < 16384;
        case TP_CID_LIMIT: return v >= 2;
        case TP_MAX_STREAMS_BIDI: case TP_MAX_STREAMS_UNI: return v <= STREAMS_MAX;
        default: return v <= PROVEN_QUIC_VARINT_MAX;
    }
}

typedef struct { proven_byte_t *p; proven_size_t n, at; bool ok; } tp_wr_t;

static void wr_raw(tp_wr_t *w, const proven_byte_t *p, proven_size_t n) {
    if (!w->ok || w->n - w->at < n) { w->ok = false; return; }
    for (proven_size_t i = 0; i < n; ++i) w->p[w->at + i] = p[i];
    w->at += n;
}
static void wr_varint(tp_wr_t *w, proven_u64 v) {
    proven_byte_t b[8];
    proven_size_t n = proven_quic_varint_put(v, b);
    if (n == 0) w->ok = false;
    wr_raw(w, b, n);
}
static void wr_param(tp_wr_t *w, proven_u64 id, const proven_byte_t *p, proven_size_t n) { wr_varint(w, id); wr_varint(w, n); wr_raw(w, p, n); }
static void wr_cid(tp_wr_t *w, proven_u64 id, const proven_quic_cid_t *cid) {
    if (cid->len > PROVEN_QUIC_MAX_CID) { w->ok = false; return; }
    wr_param(w, id, cid->id, cid->len);
}

proven_size_t proven_quic_tp_encode(const proven_quic_tp_t *tp, bool server, proven_mem_mut_t out) {
    if (!tp || (out.size > 0 && !out.ptr)) return 0;
    /* What only a server may send, a client never does; and this library never offers a preferred address. */
    if (!server && (tp->has_original_dcid || tp->has_retry_scid || tp->has_reset_token)) return 0;
    if (tp->has_preferred_address) return 0;
    tp_wr_t w = { out.ptr, out.size, 0, true };
    if (tp->has_original_dcid) wr_cid(&w, TP_ORIGINAL_DCID, &tp->original_dcid);
    proven_quic_tp_t copy = *tp;
    for (proven_size_t i = 0; i < sizeof TP_INT_IDS; ++i) {
        const proven_u8 id = TP_INT_IDS[i];
        const proven_u64 v = *tp_field(&copy, id);
        if (!tp_int_ok(id, v)) return 0;
        /* The two that are not integers go where their ids put them. */
        if (id == TP_MAX_UDP_PAYLOAD && tp->has_reset_token) wr_param(&w, TP_RESET_TOKEN, tp->reset_token, 16);
        if (id == TP_CID_LIMIT && tp->disable_active_migration) wr_param(&w, TP_DISABLE_MIGRATION, NULL, 0);
        if (v == tp_default(id)) continue;
        proven_byte_t b[8];
        proven_size_t n = proven_quic_varint_put(v, b);
        wr_param(&w, id, b, n);
    }
    if (tp->has_initial_scid) wr_cid(&w, TP_INITIAL_SCID, &tp->initial_scid);
    if (tp->has_retry_scid) wr_cid(&w, TP_RETRY_SCID, &tp->retry_scid);
    return w.ok ? w.at : 0;
}

static bool rd_cid(proven_quic_rd_t *r, proven_size_t len, proven_quic_cid_t *cid) {
    if (len > PROVEN_QUIC_MAX_CID) return false;
    proven_quic_rd_t v = proven_quic_rd_take(r, len);
    if (!v.ok) return false;
    cid->len = (proven_byte_t)len;
    for (proven_size_t i = 0; i < len; ++i) cid->id[i] = v.p[i];
    return true;
}

proven_u64 proven_quic_tp_decode(proven_mem_view_t in, bool from_server, proven_quic_tp_t *tp) {
    const proven_u64 bad = PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR;
    if (!tp || (in.size > 0 && !in.ptr)) return bad;
    proven_quic_tp_defaults(tp);
    proven_quic_rd_t r = { in.ptr, in.size, true };
    proven_u32 seen = 0;
    while (r.n > 0) {
        proven_u64 id = proven_quic_rd_varint(&r);
        proven_u64 len = proven_quic_rd_varint(&r);
        if (!r.ok || len > r.n) return bad;
        proven_quic_rd_t v = proven_quic_rd_take(&r, (proven_size_t)len);
        if (id > TP_RETRY_SCID) continue;                       /* not known here, reserved or greased: skipped */
        if (seen & ((proven_u32)1 << id)) return bad;
        seen |= (proven_u32)1 << id;
        switch (id) {
            case TP_ORIGINAL_DCID: case TP_RETRY_SCID: case TP_INITIAL_SCID: {
                if (!from_server && id != TP_INITIAL_SCID) return bad;
                proven_quic_cid_t *cid = id == TP_ORIGINAL_DCID ? &tp->original_dcid : id == TP_RETRY_SCID ? &tp->retry_scid : &tp->initial_scid;
                if (!rd_cid(&v, (proven_size_t)len, cid)) return bad;
                if (id == TP_ORIGINAL_DCID) tp->has_original_dcid = true;
                else if (id == TP_RETRY_SCID) tp->has_retry_scid = true;
                else tp->has_initial_scid = true;
                break;
            }
            case TP_RESET_TOKEN:
                if (!from_server || len != 16) return bad;
                for (int i = 0; i < 16; ++i) tp->reset_token[i] = v.p[i];
                tp->has_reset_token = true;
                break;
            case TP_DISABLE_MIGRATION:
                if (len != 0) return bad;
                tp->disable_active_migration = true;
                break;
            case TP_PREFERRED_ADDRESS: {
                if (!from_server) return bad;
                proven_quic_preferred_address_t *pa = &tp->preferred_address;
                proven_quic_rd_t a4 = proven_quic_rd_take(&v, 4);
                pa->ipv4_port = (proven_u16)proven_quic_rd_uint(&v, 2);
                proven_quic_rd_t a6 = proven_quic_rd_take(&v, 16);
                pa->ipv6_port = (proven_u16)proven_quic_rd_uint(&v, 2);
                proven_size_t cid_len = (proven_size_t)proven_quic_rd_uint(&v, 1);
                if (!v.ok || cid_len == 0 || !rd_cid(&v, cid_len, &pa->cid)) return bad;
                proven_quic_rd_t token = proven_quic_rd_take(&v, 16);
                if (!token.ok || v.n != 0) return bad;
                for (int i = 0; i < 4; ++i) pa->ipv4[i] = a4.p[i];
                for (int i = 0; i < 16; ++i) pa->ipv6[i] = a6.p[i];
                for (int i = 0; i < 16; ++i) pa->reset_token[i] = token.p[i];
                tp->has_preferred_address = true;
                break;
            }
            default: {
                proven_u64 *field = tp_field(tp, (proven_u8)id);
                proven_u64 value = proven_quic_rd_varint(&v);
                /* The integer must fill its parameter exactly. */
                if (!field || !v.ok || v.n != 0 || !tp_int_ok((proven_u8)id, value)) return bad;
                *field = value;
                break;
            }
        }
    }
    if (!tp->has_initial_scid || (from_server && !tp->has_original_dcid)) return bad;
    return 0;
}

#endif /* PROVEN_NO_HTTP3 */
