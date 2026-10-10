#include "proven_internal_tls.h"
#include "proven_internal_der.h"
#ifndef PROVEN_FREESTANDING
#include "proven/random.h"
#include "proven/time.h"
#endif

/* The TLS configuration: this side's identity, how the peer is verified, and a server's
 * ticket keys. Built once; only the ticket keys change afterwards. */

static void cfg_wipe(void *p, proven_size_t n) { proven_mem_wipe((proven_mem_mut_t){ .ptr = p, .size = n }); }

#ifndef PROVEN_FREESTANDING
static void cfg_os_random(void *ctx, proven_byte_t *out, proven_size_t len) {
    (void)ctx;
    /* The OS source failing is not something a handshake can continue from or report: the
     * library's own call panics rather than return weak bytes, and so does this path. */
    (void)proven_random_bytes(out, len);
}

static proven_i64 cfg_os_now(void *ctx) {
    (void)ctx;
    return proven_time_now() / 1000000000;
}
#endif

void proven_tls_config_random(const proven_tls_config_t *config, proven_byte_t *out, proven_size_t len) {
    config->random(config->random_ctx, out, len);
}

proven_i64 proven_tls_config_now(const proven_tls_config_t *config) {
    return config->now(config->now_ctx);
}

proven_u8str_view_t proven_tls_config_alpn(const proven_tls_config_t *config, proven_size_t index) {
    const proven_byte_t *p = config->alpn_wire;
    for (proven_size_t i = 0; i < config->alpn_count; ++i) {
        if (i == index) return (proven_u8str_view_t){ .ptr = p + 1, .size = p[0] };
        p += 1 + p[0];
    }
    return (proven_u8str_view_t){ .ptr = NULL, .size = 0 };
}

#define OID_EC_KEY "\x2a\x86\x48\xce\x3d\x02\x01"
#define OID_P256 "\x2a\x86\x48\xce\x3d\x03\x01\x07"
#define OID_ED25519 "\x2b\x65\x70"

/* ECPrivateKey ::= SEQUENCE { version INTEGER 1, privateKey OCTET STRING, [0] parameters, [1] publicKey } */
static proven_err_t cfg_sec1(der_t in, bool need_curve, proven_byte_t key[32]) {
    der_t seq, d, f, curve;
    proven_u32 version = 0;
    if (!der_expect(&in, DER_SEQUENCE, &seq, NULL) || in.n != 0) return PROVEN_ERR_INVALID_FORMAT;
    if (!der_small_uint(&seq, &version) || version != 1 || !der_expect(&seq, DER_OCTETS, &d, NULL)) return PROVEN_ERR_INVALID_FORMAT;
    bool has_curve = der_expect(&seq, 0xa0, &f, NULL);
    if (has_curve) {
        if (!der_expect(&f, DER_OID, &curve, NULL) || f.n != 0) return PROVEN_ERR_INVALID_FORMAT;
        if (!DER_IS(&curve, OID_P256)) return PROVEN_ERR_UNSUPPORTED;
    } else if (need_curve) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (d.n != 32) return PROVEN_ERR_UNSUPPORTED;
    for (int i = 0; i < 32; ++i) key[i] = d.p[i];
    return PROVEN_OK;
}

/* PrivateKeyInfo ::= SEQUENCE { version INTEGER, algorithm AlgorithmIdentifier, privateKey OCTET STRING, ... } */
static proven_err_t cfg_pkcs8(der_t in, proven_tls_key_kind_t *kind, proven_byte_t key[32]) {
    der_t seq, alg, oid, priv;
    proven_u32 version = 9;
    if (!der_expect(&in, DER_SEQUENCE, &seq, NULL) || in.n != 0) return PROVEN_ERR_INVALID_FORMAT;
    if (!der_small_uint(&seq, &version) || version > 1) return PROVEN_ERR_INVALID_FORMAT;
    if (!der_expect(&seq, DER_SEQUENCE, &alg, NULL) || !der_expect(&alg, DER_OID, &oid, NULL)) return PROVEN_ERR_INVALID_FORMAT;
    if (!der_expect(&seq, DER_OCTETS, &priv, NULL)) return PROVEN_ERR_INVALID_FORMAT;
    if (DER_IS(&oid, OID_ED25519)) {
        der_t seed;
        if (alg.n != 0 || !der_expect(&priv, DER_OCTETS, &seed, NULL) || priv.n != 0 || seed.n != 32) return PROVEN_ERR_INVALID_FORMAT;
        for (int i = 0; i < 32; ++i) key[i] = seed.p[i];
        *kind = PROVEN_TLS_KEY_ED25519;
        return PROVEN_OK;
    }
    if (DER_IS(&oid, OID_EC_KEY)) {
        der_t curve;
        if (!der_expect(&alg, DER_OID, &curve, NULL) || alg.n != 0) return PROVEN_ERR_INVALID_FORMAT;
        if (!DER_IS(&curve, OID_P256)) return PROVEN_ERR_UNSUPPORTED;
        *kind = PROVEN_TLS_KEY_P256;
        return cfg_sec1(priv, false, key);
    }
    return PROVEN_ERR_UNSUPPORTED;                /* RSA and everything else */
}

static bool label_is(proven_u8str_view_t label, const char *want) {
    proven_size_t n = 0;
    while (want[n]) ++n;
    if (label.size != n) return false;
    for (proven_size_t i = 0; i < n; ++i) if (label.ptr[i] != (proven_byte_t)want[i]) return false;
    return true;
}

static proven_err_t cfg_load_key(proven_tls_config_t *c, proven_mem_view_t pem) {
    proven_byte_t der[512];
    proven_size_t pos = 0, len = 0;
    proven_u8str_view_t label;
    proven_err_t result = PROVEN_ERR_INVALID_FORMAT;
    for (;;) {
        proven_err_t e = proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ .ptr = der, .size = sizeof der }, &len);
        if (e == PROVEN_ERR_NOT_FOUND) break;
        if (e == PROVEN_ERR_OUT_OF_BOUNDS) { result = PROVEN_ERR_UNSUPPORTED; break; }     /* far larger than any key read here: RSA */
        if (e != PROVEN_OK) break;
        der_t in = { der, len };
        if (label_is(label, "PRIVATE KEY")) { result = cfg_pkcs8(in, &c->key_kind, c->key); break; }
        if (label_is(label, "EC PRIVATE KEY")) { c->key_kind = PROVEN_TLS_KEY_P256; result = cfg_sec1(in, true, c->key); break; }
        if (label_is(label, "ENCRYPTED PRIVATE KEY") || label_is(label, "RSA PRIVATE KEY")) { result = PROVEN_ERR_UNSUPPORTED; break; }
    }
    cfg_wipe(der, sizeof der);
    return result;
}

static proven_err_t cfg_load_chain(proven_tls_config_t *c, proven_mem_view_t pem) {
    proven_allocator_t a = c->alloc;
    proven_size_t cap = pem.size / 4 * 3 + 3, used = 0, pos = 0;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, cap, 1);
    if (m.err != PROVEN_OK) return m.err;
    c->chain_mem = m.value.ptr;
    for (;;) {
        proven_u8str_view_t label;
        proven_size_t len = 0;
        proven_err_t e = proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ .ptr = c->chain_mem + used, .size = cap - used }, &len);
        if (e == PROVEN_ERR_NOT_FOUND) break;
        if (e != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
        if (!label_is(label, "CERTIFICATE")) continue;
        if (c->chain_count == PROVEN_TLS_MAX_CHAIN) return PROVEN_ERR_INVALID_ARG;
        proven_cert_t probe;
        proven_mem_view_t der = { .ptr = c->chain_mem + used, .size = len };
        if (proven_cert_parse(der, &probe) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
        c->chain[c->chain_count++] = der;
        used += len;
    }
    return c->chain_count > 0 ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

/* The key must be the one in the first certificate. */
static proven_err_t cfg_match_key(proven_tls_config_t *c) {
    proven_cert_t leaf;
    if (proven_cert_parse(c->chain[0], &leaf) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    if (c->key_kind == PROVEN_TLS_KEY_ED25519) {
        proven_crypto_ed25519_public(c->key_public, c->key);
        if (leaf.key_kind != PROVEN_CERT_KEY_ED25519) return PROVEN_ERR_INVALID_STATE;
        for (int i = 0; i < 32; ++i) if (leaf.key.ptr[i] != c->key_public[i]) return PROVEN_ERR_INVALID_STATE;
        return PROVEN_OK;
    }
    proven_byte_t pub[65];
    if (!proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, c->key, pub)) return PROVEN_ERR_INVALID_FORMAT;
    if (leaf.key_kind != PROVEN_CERT_KEY_EC_P256) return PROVEN_ERR_INVALID_STATE;
    for (int i = 0; i < 65; ++i) if (leaf.key.ptr[i] != pub[i]) return PROVEN_ERR_INVALID_STATE;
    return PROVEN_OK;
}

proven_err_t proven_tls_config_create(const proven_tls_options_t *o, proven_tls_config_t **out) {
    if (!out || !o || !proven_alloc_is_valid(o->alloc)) return PROVEN_ERR_INVALID_ARG;
    bool has_cert = o->certificate_pem.ptr && o->certificate_pem.size > 0;
    bool has_key = o->private_key_pem.ptr && o->private_key_pem.size > 0;
    if (has_cert != has_key) return PROVEN_ERR_INVALID_ARG;
    if (o->verify == PROVEN_TLS_VERIFY_PIN_ONLY && (o->pin_count == 0 || !o->pins)) return PROVEN_ERR_INVALID_ARG;
    if (o->verify != PROVEN_TLS_VERIFY_CHAIN && o->verify != PROVEN_TLS_VERIFY_PIN_ONLY) return PROVEN_ERR_INVALID_ARG;
    if (o->pin_count > 0 && !o->pins) return PROVEN_ERR_INVALID_ARG;
    if (o->alpn_count > 0 && !o->alpn) return PROVEN_ERR_INVALID_ARG;
    if (o->client_auth > PROVEN_TLS_CLIENT_AUTH_REQUIRE || o->ticket_lifetime_s > 604800) return PROVEN_ERR_INVALID_ARG;
    /* Anchors are needed by any side that verifies a chain: a client always does; a server
     * does when it asks for client certificates. A config with no certificate can only be a
     * client; one with a certificate may be either, and is held to what it could be used for. */
    bool verifies = !has_cert || o->client_auth != PROVEN_TLS_CLIENT_AUTH_NONE;
    if (o->verify == PROVEN_TLS_VERIFY_CHAIN && verifies && !o->anchors) return PROVEN_ERR_INVALID_ARG;
#ifdef PROVEN_FREESTANDING
    if (!o->random || !o->now) return PROVEN_ERR_INVALID_ARG;
#endif
    proven_size_t alpn_len = 0;
    for (proven_size_t i = 0; i < o->alpn_count; ++i) {
        if (!o->alpn[i].ptr || o->alpn[i].size == 0 || o->alpn[i].size > 255) return PROVEN_ERR_INVALID_ARG;
        alpn_len += 1 + o->alpn[i].size;
    }
    if (alpn_len > 4096) return PROVEN_ERR_INVALID_ARG;

    proven_allocator_t a = o->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(proven_tls_config_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_tls_config_t *c = (proven_tls_config_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    c->alloc = a;
    c->anchors = o->anchors;
    c->verify = o->verify;
    c->client_auth = o->client_auth;
    c->no_resumption = o->no_resumption;
    c->keep_peer_certificate = o->keep_peer_certificate;
    c->ticket_lifetime_s = o->ticket_lifetime_s ? o->ticket_lifetime_s : 7200;
    c->max_handshake_bytes = o->max_handshake_bytes ? o->max_handshake_bytes : 65536;
    c->random = o->random; c->random_ctx = o->random_ctx;
    c->now = o->now; c->now_ctx = o->now_ctx;
#ifndef PROVEN_FREESTANDING
    if (!c->random) c->random = cfg_os_random;
    if (!c->now) c->now = cfg_os_now;
#endif
    atomic_flag_clear(&c->tickets.lock);
    proven_err_t e = PROVEN_OK;
    if (o->pin_count > 0) {
        if (o->pin_count > 64) e = PROVEN_ERR_INVALID_ARG;
        else {
            m = a.alloc_fn(a.ctx, o->pin_count * 32, 1);
            if (m.err != PROVEN_OK) e = m.err;
            else {
                c->pins = (proven_byte_t (*)[32])m.value.ptr;
                c->pin_count = o->pin_count;
                for (proven_size_t i = 0; i < o->pin_count; ++i) for (int k = 0; k < 32; ++k) c->pins[i][k] = o->pins[i][k];
            }
        }
    }
    if (e == PROVEN_OK && alpn_len > 0) {
        m = a.alloc_fn(a.ctx, alpn_len, 1);
        if (m.err != PROVEN_OK) e = m.err;
        else {
            c->alpn_wire = m.value.ptr;
            c->alpn_wire_len = alpn_len;
            c->alpn_count = o->alpn_count;
            proven_size_t at = 0;
            for (proven_size_t i = 0; i < o->alpn_count; ++i) {
                c->alpn_wire[at++] = (proven_byte_t)o->alpn[i].size;
                for (proven_size_t k = 0; k < o->alpn[i].size; ++k) c->alpn_wire[at++] = o->alpn[i].ptr[k];
            }
        }
    }
    if (e == PROVEN_OK && has_cert) {
        e = cfg_load_chain(c, o->certificate_pem);
        if (e == PROVEN_OK) e = cfg_load_key(c, o->private_key_pem);
        if (e == PROVEN_OK) e = cfg_match_key(c);
    }
    if (e == PROVEN_OK) {
        proven_tls_config_random(c, c->tickets.cur, 32);
        proven_tls_config_random(c, (proven_byte_t *)&c->tickets.cur_id, sizeof c->tickets.cur_id);
        c->tickets.rotated_at = proven_tls_config_now(c);
    }
    if (e != PROVEN_OK) { proven_tls_config_destroy(c); return e; }
    *out = c;
    return PROVEN_OK;
}

void proven_tls_config_destroy(proven_tls_config_t *c) {
    if (!c) return;
    proven_allocator_t a = c->alloc;
    if (c->pins) a.free_fn(a.ctx, c->pins);
    if (c->alpn_wire) a.free_fn(a.ctx, c->alpn_wire);
    if (c->chain_mem) a.free_fn(a.ctx, c->chain_mem);
    cfg_wipe(c, sizeof *c);
    a.free_fn(a.ctx, c);
}

/* ---- Tickets ----
 *
 * A ticket is the session's state sealed under a key only this process holds:
 *   key id (4) | nonce (12) | ChaCha20-Poly1305( state ) | tag (16)
 * Nothing is stored per session on the server. The key is replaced every ticket lifetime and
 * the one before it is kept for one more, so a ticket is honoured for its lifetime and no key
 * outlives two. They are never written anywhere. */

static void ticket_lock(proven_tls_ticket_keys_t *t) {
    while (atomic_flag_test_and_set_explicit(&t->lock, memory_order_acquire)) { /* held for a few instructions */ }
}

static void ticket_unlock(proven_tls_ticket_keys_t *t) { atomic_flag_clear_explicit(&t->lock, memory_order_release); }

/* Rotate if it is time, and copy out the keys. The config is const everywhere else; this one
 * field is the exception the lock exists for. */
static void ticket_keys(const proven_tls_config_t *config, proven_byte_t cur[32], proven_u32 *cur_id,
                        proven_byte_t prev[32], proven_u32 *prev_id, bool *have_prev) {
    proven_tls_ticket_keys_t *t = (proven_tls_ticket_keys_t *)&config->tickets;
    proven_i64 now = proven_tls_config_now(config);
    proven_byte_t fresh[32];
    proven_u32 fresh_id;
    /* Drawn outside the lock: the random source may be slow, and an unused draw costs nothing. */
    proven_tls_config_random(config, fresh, 32);
    proven_tls_config_random(config, (proven_byte_t *)&fresh_id, sizeof fresh_id);
    ticket_lock(t);
    if (now - t->rotated_at >= (proven_i64)config->ticket_lifetime_s || now < t->rotated_at) {
        /* Two lifetimes without a handshake: the old key is past use as well. */
        bool stale = now - t->rotated_at >= 2 * (proven_i64)config->ticket_lifetime_s || now < t->rotated_at;
        for (int i = 0; i < 32; ++i) { t->prev[i] = t->cur[i]; t->cur[i] = fresh[i]; }
        t->prev_id = t->cur_id;
        t->cur_id = fresh_id;
        t->have_prev = !stale;
        t->rotated_at = now;
    }
    for (int i = 0; i < 32; ++i) { cur[i] = t->cur[i]; prev[i] = t->prev[i]; }
    *cur_id = t->cur_id; *prev_id = t->prev_id; *have_prev = t->have_prev;
    ticket_unlock(t);
    cfg_wipe(fresh, sizeof fresh);
}

static void put64(proven_byte_t *p, proven_u64 v) { for (int i = 7; i >= 0; --i) { p[i] = (proven_byte_t)v; v >>= 8; } }
static proven_u64 get64(const proven_byte_t *p) { proven_u64 v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }

#define TICKET_PLAIN (1 + 2 + 8 + 1 + PROVEN_TLS_TICKET_PSK_MAX + 1 + 32)

proven_size_t proven_tls_ticket_seal(const proven_tls_config_t *config, const proven_tls_ticket_state_t *state,
                                     proven_byte_t out[PROVEN_TLS_TICKET_MAX]) {
    proven_byte_t cur[32], prev[32], plain[TICKET_PLAIN];
    proven_u32 cur_id, prev_id;
    bool have_prev;
    ticket_keys(config, cur, &cur_id, prev, &prev_id, &have_prev);
    proven_size_t n = 0;
    plain[n++] = 1;                               /* the layout's version */
    plain[n++] = (proven_byte_t)(state->suite >> 8); plain[n++] = (proven_byte_t)state->suite;
    put64(plain + n, (proven_u64)state->issued_at); n += 8;
    plain[n++] = (proven_byte_t)state->psk_len;
    for (proven_size_t i = 0; i < PROVEN_TLS_TICKET_PSK_MAX; ++i) plain[n++] = i < state->psk_len ? state->psk[i] : 0;
    plain[n++] = state->has_peer_key ? 1 : 0;
    for (int i = 0; i < 32; ++i) plain[n++] = state->has_peer_key ? state->peer_key_hash[i] : 0;
    out[0] = (proven_byte_t)(cur_id >> 24); out[1] = (proven_byte_t)(cur_id >> 16); out[2] = (proven_byte_t)(cur_id >> 8); out[3] = (proven_byte_t)cur_id;
    proven_tls_config_random(config, out + 4, 12);
    proven_crypto_chacha20poly1305_seal(cur, out + 4, (proven_mem_view_t){ .ptr = out, .size = 4 },
                                        (proven_mem_view_t){ .ptr = plain, .size = n }, out + 16, out + 16 + n);
    cfg_wipe(cur, sizeof cur); cfg_wipe(prev, sizeof prev); cfg_wipe(plain, sizeof plain);
    return 16 + n + 16;
}

bool proven_tls_ticket_open(const proven_tls_config_t *config, proven_mem_view_t ticket, proven_tls_ticket_state_t *state) {
    if (!ticket.ptr || ticket.size != 16 + TICKET_PLAIN + 16) return false;
    proven_byte_t cur[32], prev[32], plain[TICKET_PLAIN];
    proven_u32 cur_id, prev_id, id = ((proven_u32)ticket.ptr[0] << 24) | ((proven_u32)ticket.ptr[1] << 16) | ((proven_u32)ticket.ptr[2] << 8) | ticket.ptr[3];
    bool have_prev, ok = false;
    ticket_keys(config, cur, &cur_id, prev, &prev_id, &have_prev);
    const proven_byte_t *key = id == cur_id ? cur : (have_prev && id == prev_id) ? prev : NULL;
    if (key) {
        ok = proven_crypto_chacha20poly1305_open(key, ticket.ptr + 4, (proven_mem_view_t){ .ptr = ticket.ptr, .size = 4 },
                                                 (proven_mem_view_t){ .ptr = ticket.ptr + 16, .size = TICKET_PLAIN },
                                                 ticket.ptr + 16 + TICKET_PLAIN, plain);
    }
    if (ok && plain[0] == 1) {
        proven_size_t n = 1;
        state->suite = (proven_u16)(((proven_u16)plain[n] << 8) | plain[n + 1]); n += 2;
        state->issued_at = (proven_i64)get64(plain + n); n += 8;
        state->psk_len = plain[n++];
        for (proven_size_t i = 0; i < PROVEN_TLS_TICKET_PSK_MAX; ++i) state->psk[i] = plain[n++];
        state->has_peer_key = plain[n++] == 1;
        for (int i = 0; i < 32; ++i) state->peer_key_hash[i] = plain[n++];
        proven_i64 age = proven_tls_config_now(config) - state->issued_at;
        ok = state->psk_len <= PROVEN_TLS_TICKET_PSK_MAX && state->psk_len >= 32 && age >= 0 && age <= (proven_i64)config->ticket_lifetime_s;
    } else {
        ok = false;
    }
    cfg_wipe(cur, sizeof cur); cfg_wipe(prev, sizeof prev); cfg_wipe(plain, sizeof plain);
    return ok;
}
