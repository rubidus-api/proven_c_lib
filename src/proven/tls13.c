#include "proven_internal_tls.h"

/* The TLS 1.3 state machine (RFC 8446), both roles. Bytes in, bytes out: no I/O, no clock and
 * no random source of its own - those come from the config.
 *
 * Everything that arrives is from a stranger. Every length is checked against what is left
 * before it is used, every message is checked against the state it arrives in, and the first
 * thing that is wrong ends the connection with the alert the RFC names for it. */

/* ---- Alerts (RFC 8446 section 6) ---- */
#define AL_CLOSE_NOTIFY 0
#define AL_UNEXPECTED_MESSAGE 10
#define AL_BAD_RECORD_MAC 20
#define AL_RECORD_OVERFLOW 22
#define AL_HANDSHAKE_FAILURE 40
#define AL_BAD_CERTIFICATE 42
#define AL_UNSUPPORTED_CERTIFICATE 43
#define AL_CERTIFICATE_EXPIRED 45
#define AL_ILLEGAL_PARAMETER 47
#define AL_UNKNOWN_CA 48
#define AL_DECODE_ERROR 50
#define AL_DECRYPT_ERROR 51
#define AL_PROTOCOL_VERSION 70
#define AL_INTERNAL_ERROR 80
#define AL_USER_CANCELED 90
#define AL_MISSING_EXTENSION 109
#define AL_UNSUPPORTED_EXTENSION 110
#define AL_CERTIFICATE_REQUIRED 116
#define AL_NO_APPLICATION_PROTOCOL 120

/* ---- Handshake message types ---- */
#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_NEW_SESSION_TICKET 4
#define HS_ENCRYPTED_EXTENSIONS 8
#define HS_CERTIFICATE 11
#define HS_CERTIFICATE_REQUEST 13
#define HS_CERTIFICATE_VERIFY 15
#define HS_FINISHED 20
#define HS_KEY_UPDATE 24

/* ---- Extensions ---- */
#define EXT_SERVER_NAME 0
#define EXT_SUPPORTED_GROUPS 10
#define EXT_SIGNATURE_ALGORITHMS 13
#define EXT_ALPN 16
#define EXT_RECORD_SIZE_LIMIT 28
#define EXT_PRE_SHARED_KEY 41
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_COOKIE 44
#define EXT_PSK_MODES 45
#define EXT_KEY_SHARE 51

#define GROUP_P256 0x0017
#define GROUP_X25519 0x001d

#define SIG_ECDSA_P256_SHA256 0x0403
#define SIG_ECDSA_P384_SHA384 0x0503
#define SIG_ED25519 0x0807
#define SIG_RSA_PSS_RSAE_SHA256 0x0804
#define SIG_RSA_PSS_RSAE_SHA384 0x0805
#define SIG_RSA_PSS_RSAE_SHA512 0x0806
#define SIG_RSA_PSS_PSS_SHA256 0x0809
#define SIG_RSA_PSS_PSS_SHA384 0x080a
#define SIG_RSA_PSS_PSS_SHA512 0x080b

#define TLS_MAX_UNSENT ((proven_size_t)64 * 1024)
#define TLS_COOKIE_MAX 256
#define TLS_MAX_IGNORED 16                     /* records that carry nothing, tolerated in a row */

static const proven_byte_t HRR_RANDOM[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

typedef enum {
    ST_C_WAIT_SH = 1, ST_C_WAIT_EE, ST_C_WAIT_CERT_CR, ST_C_WAIT_CERT, ST_C_WAIT_CV, ST_C_WAIT_FINISHED,
    ST_S_WAIT_CH, ST_S_WAIT_CERT, ST_S_WAIT_CV, ST_S_WAIT_FINISHED,
    ST_ESTABLISHED
} tls_state_t;

typedef struct { proven_byte_t *ptr; proven_size_t len, cap; } tls_buf_t;

/* What only the handshake needs. Freed, wiped, when it completes. */
typedef struct {
    proven_tls_transcript_t transcript;
    bool transcript_ready;                        /* false until the hash is known (client, before ServerHello) */
    tls_buf_t first_hello;                        /* client: the ClientHello, kept until the hash is known */
    tls_buf_t reassembly;                         /* a handshake message that spans records */
    tls_buf_t scratch;                            /* a message being built */
    proven_byte_t random[32];                     /* client: its random, for the second ClientHello */
    proven_byte_t session_id[32];
    proven_size_t session_id_len;
    proven_byte_t x25519_priv[32];                /* SECRET */
    proven_byte_t p256_priv[32];                  /* SECRET */
    bool have_p256;
    bool retried;                                 /* a HelloRetryRequest has happened */
    bool ccs_sent;
    proven_u16 group;                             /* the group agreed */
    proven_byte_t cookie[TLS_COOKIE_MAX];
    proven_size_t cookie_len;
    proven_byte_t early_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t handshake_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t master_secret[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t client_hs[PROVEN_HMAC_MAX_SIZE], server_hs[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_size_t psk_len;
    bool psk_offered, psk_accepted;
    bool cert_requested;                          /* client: the server asked for a certificate */
    bool client_cert_seen;                        /* server: the client presented one */
    proven_u16 peer_sig_schemes;                  /* a mask: bit 0 ecdsa p256, bit 1 ed25519 */
    tls_buf_t peer_leaf;                          /* the peer's certificate, until CertificateVerify */
    proven_byte_t sni[254];
    proven_size_t sni_len;
    bool client_sent_alpn, client_sent_record_limit, client_sent_sni;
    proven_byte_t test_hello[600];                /* a test's ClientHello, sent instead of ours */
    proven_size_t test_hello_len;
} tls_handshake_t;

struct proven_tls_conn {
    const proven_tls_config_t *config;
    proven_allocator_t alloc;
    bool is_server;
    tls_state_t state;
    bool established, dead, close_sent, close_received;
    proven_err_t error;                           /* the failure, to be returned once */
    bool error_reported;
    int alert_sent, alert_received;
    const proven_tls_suite_t *suite;
    proven_tls_keys_t read_keys, write_keys;
    proven_byte_t read_secret[48], write_secret[48];      /* application traffic secrets, for KeyUpdate */
    proven_byte_t resumption_master[48];
    proven_size_t peer_record_limit;              /* the most plaintext the peer accepts in a record */
    tls_handshake_t *hs;
    tls_buf_t in;                                 /* a record being collected, or decrypted and not yet read */
    proven_size_t plain_off, plain_len;           /* undelivered application data inside `in` */
    tls_buf_t out;
    proven_size_t out_off;
    tls_buf_t post;                               /* a post-handshake message that spans records */
    proven_u32 ignored;                           /* records in a row that carried nothing: see TLS_MAX_IGNORED */
    proven_tls_session_t *session;
    int alpn_index;                               /* into the config's list; -1 none */
    bool resumed;
    bool has_peer_key;
    proven_byte_t peer_key_hash[32];
    proven_cert_fault_t peer_fault;
    tls_buf_t peer_cert;                          /* kept only when the config asks */
    proven_byte_t *server_name;                   /* client: the name it asked for; its own small allocation */
    proven_size_t server_name_len;
};

static void tls_wipe(void *p, proven_size_t n) { proven_mem_wipe((proven_mem_mut_t){ .ptr = p, .size = n }); }

static bool buf_reserve(proven_tls_conn_t *c, tls_buf_t *b, proven_size_t need) {
    if (need <= b->cap) return true;
    proven_size_t cap = b->cap ? b->cap : 256;
    while (cap < need) { if (cap > (proven_size_t)-1 / 2) return false; cap *= 2; }
    proven_result_mem_mut_t m = b->ptr ? c->alloc.realloc_fn(c->alloc.ctx, b->ptr, b->cap, cap, 16) : c->alloc.alloc_fn(c->alloc.ctx, cap, 16);
    if (m.err != PROVEN_OK) return false;
    b->ptr = m.value.ptr;
    b->cap = cap;
    return true;
}

static void buf_free(proven_tls_conn_t *c, tls_buf_t *b) {
    if (b->ptr) { tls_wipe(b->ptr, b->cap); c->alloc.free_fn(c->alloc.ctx, b->ptr); }
    b->ptr = NULL; b->len = 0; b->cap = 0;
}

static bool buf_append(proven_tls_conn_t *c, tls_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n > (proven_size_t)-1 - b->len || !buf_reserve(c, b, b->len + n)) return false;
    for (proven_size_t i = 0; i < n; ++i) b->ptr[b->len + i] = p[i];
    b->len += n;
    return true;
}

/* ---- Reading wire data: every get checks what is left ---- */

typedef struct { const proven_byte_t *p; proven_size_t n; bool ok; } rd_t;

static proven_u32 rd_uint(rd_t *r, int bytes) {
    if (!r->ok || r->n < (proven_size_t)bytes) { r->ok = false; return 0; }
    proven_u32 v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | r->p[i];
    r->p += bytes; r->n -= (proven_size_t)bytes;
    return v;
}

static rd_t rd_take(rd_t *r, proven_size_t n) {
    rd_t sub = { r->p, 0, false };
    if (!r->ok || r->n < n) { r->ok = false; return sub; }
    sub.n = n; sub.ok = true;
    r->p += n; r->n -= n;
    return sub;
}

/* A vector with a length prefix of 1, 2 or 3 bytes. */
static rd_t rd_vec(rd_t *r, int prefix) {
    proven_u32 n = rd_uint(r, prefix);
    return rd_take(r, n);
}

static bool bytes_eq(const proven_byte_t *a, const proven_byte_t *b, proven_size_t n) {
    proven_byte_t d = 0;
    for (proven_size_t i = 0; i < n; ++i) d |= (proven_byte_t)(a[i] ^ b[i]);
    return d == 0;
}

/* ---- Building wire data into the scratch buffer ---- */

typedef struct { proven_tls_conn_t *c; tls_buf_t *b; bool ok; } wr_t;

static void wr_bytes(wr_t *w, const proven_byte_t *p, proven_size_t n) { if (w->ok && !buf_append(w->c, w->b, p, n)) w->ok = false; }
static void wr_uint(wr_t *w, proven_u32 v, int bytes) {
    proven_byte_t t[4];
    for (int i = 0; i < bytes; ++i) t[i] = (proven_byte_t)(v >> (8 * (bytes - 1 - i)));
    wr_bytes(w, t, (proven_size_t)bytes);
}
/* Open a length-prefixed block; wr_close fills the length in. */
static proven_size_t wr_open(wr_t *w, int prefix) { proven_size_t at = w->b->len; wr_uint(w, 0, prefix); return at; }
static void wr_close(wr_t *w, proven_size_t at, int prefix) {
    if (!w->ok) return;
    proven_size_t n = w->b->len - at - (proven_size_t)prefix;
    for (int i = 0; i < prefix; ++i) w->b->ptr[at + (proven_size_t)i] = (proven_byte_t)(n >> (8 * (prefix - 1 - i)));
}

/* ---- Output ---- */

static bool out_reserve(proven_tls_conn_t *c, proven_size_t more) {
    if (c->out_off > 0 && c->out_off == c->out.len) { c->out.len = 0; c->out_off = 0; }
    return buf_reserve(c, &c->out, c->out.len + more);
}

/* One or more records carrying `data` as `type`, under the write keys if there are any. */
static bool emit(proven_tls_conn_t *c, proven_byte_t type, const proven_byte_t *data, proven_size_t len, proven_byte_t clear_minor) {
    proven_size_t limit = c->write_keys.active ? c->peer_record_limit : PROVEN_TLS_MAX_PLAINTEXT;
    do {
        proven_size_t n = len < limit ? len : limit;
        if (!out_reserve(c, PROVEN_TLS_RECORD_HEADER + n + PROVEN_TLS_MAX_EXPANSION)) return false;
        proven_byte_t *o = c->out.ptr + c->out.len;
        if (c->write_keys.active) {
            c->out.len += proven_tls13_seal(&c->write_keys, type, (proven_mem_view_t){ .ptr = data, .size = n }, o);
        } else {
            o[0] = type; o[1] = 0x03; o[2] = clear_minor; o[3] = (proven_byte_t)(n >> 8); o[4] = (proven_byte_t)n;
            for (proven_size_t i = 0; i < n; ++i) o[5 + i] = data[i];
            c->out.len += 5 + n;
        }
        data += n; len -= n;
    } while (len > 0);
    return true;
}

/* End the connection: tell the peer why, remember why. Returns the error for convenience. */
static proven_err_t tls_fail(proven_tls_conn_t *c, int alert, proven_err_t err) {
    if (c->dead) return c->error;
    proven_byte_t body[2] = { 2, (proven_byte_t)alert };
    (void)emit(c, PROVEN_TLS_CT_ALERT, body, 2, 0x03);
    c->alert_sent = alert;
    c->dead = true;
    c->error = err;
    return err;
}

static proven_err_t tls_nomem(proven_tls_conn_t *c) { return tls_fail(c, AL_INTERNAL_ERROR, PROVEN_ERR_NOMEM); }
static proven_err_t tls_decode(proven_tls_conn_t *c) { return tls_fail(c, AL_DECODE_ERROR, PROVEN_ERR_PROTOCOL); }
static proven_err_t tls_illegal(proven_tls_conn_t *c) { return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL); }
static proven_err_t tls_unexpected(proven_tls_conn_t *c) { return tls_fail(c, AL_UNEXPECTED_MESSAGE, PROVEN_ERR_PROTOCOL); }

/* Send the handshake message in the scratch buffer and add it to the transcript. */
static bool hs_send(proven_tls_conn_t *c) {
    tls_handshake_t *h = c->hs;
    if (h->transcript_ready) proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
    return emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, 0x03);
}

/* Begin a handshake message in the scratch buffer; hs_end fills its length in. */
static wr_t hs_begin(proven_tls_conn_t *c, proven_byte_t type) {
    wr_t w = { c, &c->hs->scratch, true };
    c->hs->scratch.len = 0;
    wr_uint(&w, type, 1);
    wr_uint(&w, 0, 3);
    return w;
}

static bool hs_end(wr_t *w) {
    if (!w->ok) return false;
    proven_size_t n = w->b->len - 4;
    w->b->ptr[1] = (proven_byte_t)(n >> 16); w->b->ptr[2] = (proven_byte_t)(n >> 8); w->b->ptr[3] = (proven_byte_t)n;
    return true;
}

static bool send_ccs(proven_tls_conn_t *c) {
    /* The compatibility ChangeCipherSpec travels in the clear even when keys are active. It
     * belongs to a handshake that carries a session id, and to no other. */
    if (c->hs->ccs_sent || c->hs->session_id_len == 0) return true;
    c->hs->ccs_sent = true;
    if (!out_reserve(c, 6)) return false;
    static const proven_byte_t ccs[6] = { PROVEN_TLS_CT_CCS, 0x03, 0x03, 0x00, 0x01, 0x01 };
    for (int i = 0; i < 6; ++i) c->out.ptr[c->out.len + (proven_size_t)i] = ccs[i];
    c->out.len += 6;
    return true;
}

/* ---- Suites, in the order this side prefers them ---- */

static proven_u16 g_test_suite_first;
static bool g_test_server_refuses_x25519;
void proven_tls_test_knobs(proven_u16 suite_first, bool server_refuses_x25519) {
    g_test_suite_first = suite_first;
    g_test_server_refuses_x25519 = server_refuses_x25519;
}

static void suite_order(proven_u16 out[3]) {
    if (g_test_suite_first != 0) {
        static const proven_u16 all[3] = { PROVEN_TLS_AES_128_GCM_SHA256, PROVEN_TLS_AES_256_GCM_SHA384, PROVEN_TLS_CHACHA20_POLY1305_SHA256 };
        int n = 0;
        out[n++] = g_test_suite_first;
        for (int i = 0; i < 3; ++i) if (all[i] != g_test_suite_first && n < 3) out[n++] = all[i];
        return;
    }
    /* Without the processor's AES the bitsliced code is some thirty times slower than
     * ChaCha20: prefer ChaCha20 there. */
    if (proven_crypto_aes_hw_available()) {
        out[0] = PROVEN_TLS_AES_128_GCM_SHA256; out[1] = PROVEN_TLS_AES_256_GCM_SHA384; out[2] = PROVEN_TLS_CHACHA20_POLY1305_SHA256;
    } else {
        out[0] = PROVEN_TLS_CHACHA20_POLY1305_SHA256; out[1] = PROVEN_TLS_AES_128_GCM_SHA256; out[2] = PROVEN_TLS_AES_256_GCM_SHA384;
    }
}

/* ---- Key schedule steps ---- */

static void hs_hash(proven_tls_conn_t *c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) { (void)proven_tls_transcript_hash(&c->hs->transcript, out); }

/* Early secret from the PSK (or from nothing), then the handshake secret and its traffic
 * secrets from the shared key. The transcript must end at the ServerHello. */
static void derive_handshake(proven_tls_conn_t *c, const proven_byte_t *shared, proven_size_t shared_len) {
    tls_handshake_t *h = c->hs;
    proven_hmac_hash_t hash = c->suite->hash;
    proven_byte_t empty_hash[PROVEN_HMAC_MAX_SIZE], derived[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
    proven_tls_transcript_t empty;
    proven_tls_transcript_init(&empty, hash);
    (void)proven_tls_transcript_hash(&empty, empty_hash);
    proven_mem_view_t psk = { .ptr = h->psk_accepted ? h->psk : NULL, .size = h->psk_accepted ? h->psk_len : 0 };
    proven_tls13_extract(hash, NULL, psk, h->early_secret);
    proven_tls13_derive_secret(hash, h->early_secret, "derived", empty_hash, derived);
    proven_tls13_extract(hash, derived, (proven_mem_view_t){ .ptr = shared, .size = shared_len }, h->handshake_secret);
    hs_hash(c, th);
    proven_tls13_derive_secret(hash, h->handshake_secret, "c hs traffic", th, h->client_hs);
    proven_tls13_derive_secret(hash, h->handshake_secret, "s hs traffic", th, h->server_hs);
    proven_tls13_derive_secret(hash, h->handshake_secret, "derived", empty_hash, derived);
    proven_tls13_extract(hash, derived, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, h->master_secret);
    tls_wipe(derived, sizeof derived);
}

/* The application traffic secrets. The transcript must end at the server's Finished. */
static void derive_application(proven_tls_conn_t *c, proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], proven_byte_t server_ap[PROVEN_HMAC_MAX_SIZE]) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, c->hs->master_secret, "c ap traffic", th, client_ap);
    proven_tls13_derive_secret(c->suite->hash, c->hs->master_secret, "s ap traffic", th, server_ap);
}

/* The shared key for the agreed group. False for a peer key that must be refused. */
static bool key_agree(proven_tls_conn_t *c, proven_u16 group, proven_mem_view_t peer, proven_byte_t out[32]) {
    if (group == GROUP_X25519) return peer.size == 32 && proven_crypto_x25519(out, c->hs->x25519_priv, peer.ptr);
    if (group == GROUP_P256) return peer.size == 65 && proven_crypto_ecdh(PROVEN_CRYPTO_EC_P256, c->hs->p256_priv, peer, out);
    return false;
}

static void make_p256_key(proven_tls_conn_t *c, proven_byte_t pub[65]) {
    /* A scalar in range is all but certain on the first draw; the loop is for the rest. */
    for (;;) {
        proven_tls_config_random(c->config, c->hs->p256_priv, 32);
        if (proven_crypto_ec_public(PROVEN_CRYPTO_EC_P256, c->hs->p256_priv, pub)) break;
    }
    c->hs->have_p256 = true;
}

/* ---- The handshake's end ---- */

static void hs_free(proven_tls_conn_t *c) {
    tls_handshake_t *h = c->hs;
    if (!h) return;
    buf_free(c, &h->first_hello); buf_free(c, &h->reassembly); buf_free(c, &h->scratch); buf_free(c, &h->peer_leaf);
    tls_wipe(h, sizeof *h);
    c->alloc.free_fn(c->alloc.ctx, h);
    c->hs = NULL;
}

/* ---- Signatures over the transcript (RFC 8446 section 4.4.3) ---- */

static proven_size_t cv_content(proven_tls_conn_t *c, bool server_side, proven_byte_t out[64 + 34 + PROVEN_HMAC_MAX_SIZE]) {
    static const char server_ctx[] = "TLS 1.3, server CertificateVerify";
    static const char client_ctx[] = "TLS 1.3, client CertificateVerify";
    const char *ctx = server_side ? server_ctx : client_ctx;
    proven_size_t n = 0;
    for (int i = 0; i < 64; ++i) out[n++] = 0x20;
    for (int i = 0; i < 33; ++i) out[n++] = (proven_byte_t)ctx[i];
    out[n++] = 0;
    n += proven_tls_transcript_hash(&c->hs->transcript, out + n);
    return n;
}

static void digest_with(proven_hmac_hash_t hash, proven_mem_view_t data, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    if (hash == PROVEN_HMAC_SHA256) proven_sha256(data, out);
    else if (hash == PROVEN_HMAC_SHA384) proven_sha384(data, out);
    else proven_sha512(data, out);
}

/* Check the peer's CertificateVerify against the certificate it sent. */
static proven_err_t cv_verify(proven_tls_conn_t *c, rd_t body, bool server_side) {
    tls_handshake_t *h = c->hs;
    proven_u16 scheme = (proven_u16)rd_uint(&body, 2);
    rd_t sig = rd_vec(&body, 2);
    if (!body.ok || body.n != 0 || sig.n == 0) return tls_decode(c);
    proven_cert_t leaf;
    if (h->peer_leaf.len == 0 || proven_cert_parse((proven_mem_view_t){ .ptr = h->peer_leaf.ptr, .size = h->peer_leaf.len }, &leaf) != PROVEN_OK) return tls_unexpected(c);
    proven_byte_t content[64 + 34 + PROVEN_HMAC_MAX_SIZE], digest[PROVEN_HMAC_MAX_SIZE];
    proven_mem_view_t msg = { .ptr = content, .size = cv_content(c, server_side, content) };
    proven_mem_view_t s = { .ptr = sig.p, .size = sig.n };
    bool ok = false;
    /* The scheme must be one this side offered, and must be the one for the key's type: a peer
     * does not get to choose a weaker pairing. PKCS #1 v1.5 is not allowed here at all. */
    switch (scheme) {
        case SIG_ECDSA_P256_SHA256:
            if (leaf.key_kind != PROVEN_CERT_KEY_EC_P256) return tls_illegal(c);
            proven_sha256(msg, digest);
            ok = proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P256, leaf.key, (proven_mem_view_t){ .ptr = digest, .size = 32 }, s);
            break;
        case SIG_ECDSA_P384_SHA384:
            if (leaf.key_kind != PROVEN_CERT_KEY_EC_P384) return tls_illegal(c);
            proven_sha384(msg, digest);
            ok = proven_crypto_ecdsa_verify_der(PROVEN_CRYPTO_EC_P384, leaf.key, (proven_mem_view_t){ .ptr = digest, .size = 48 }, s);
            break;
        case SIG_ED25519:
            if (leaf.key_kind != PROVEN_CERT_KEY_ED25519) return tls_illegal(c);
            ok = s.size == 64 && proven_crypto_ed25519_verify(leaf.key.ptr, msg, s.ptr);
            break;
        case SIG_RSA_PSS_RSAE_SHA256: case SIG_RSA_PSS_RSAE_SHA384: case SIG_RSA_PSS_RSAE_SHA512:
        case SIG_RSA_PSS_PSS_SHA256: case SIG_RSA_PSS_PSS_SHA384: case SIG_RSA_PSS_PSS_SHA512: {
            if (leaf.key_kind != PROVEN_CERT_KEY_RSA) return tls_illegal(c);
            int which = (scheme - (scheme >= SIG_RSA_PSS_PSS_SHA256 ? SIG_RSA_PSS_PSS_SHA256 : SIG_RSA_PSS_RSAE_SHA256));
            proven_hmac_hash_t hash = which == 0 ? PROVEN_HMAC_SHA256 : which == 1 ? PROVEN_HMAC_SHA384 : PROVEN_HMAC_SHA512;
            digest_with(hash, msg, digest);
            ok = proven_crypto_rsa_verify_pss(leaf.rsa_n, leaf.rsa_e, hash, proven_hmac_size(hash),
                                              (proven_mem_view_t){ .ptr = digest, .size = proven_hmac_size(hash) }, s);
            break; }
        default:
            return tls_illegal(c);
    }
    if (!ok) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    return PROVEN_OK;
}

/* This side's CertificateVerify, into the scratch buffer. */
static bool cv_build(proven_tls_conn_t *c, bool server_side) {
    const proven_tls_config_t *cfg = c->config;
    proven_byte_t content[64 + 34 + PROVEN_HMAC_MAX_SIZE];
    proven_mem_view_t msg = { .ptr = content, .size = cv_content(c, server_side, content) };
    wr_t w = hs_begin(c, HS_CERTIFICATE_VERIFY);
    if (cfg->key_kind == PROVEN_TLS_KEY_ED25519) {
        proven_byte_t sig[64];
        proven_crypto_ed25519_sign(sig, cfg->key, cfg->key_public, msg);
        wr_uint(&w, SIG_ED25519, 2); wr_uint(&w, 64, 2); wr_bytes(&w, sig, 64);
    } else if (cfg->key_kind == PROVEN_TLS_KEY_RSA) {
        /* rsa_pss_rsae_sha256: the scheme every TLS 1.3 peer must accept (RFC 8446, 9.1). */
        proven_byte_t digest[32], sig[PROVEN_CRYPTO_RSA_SIGN_MAX_BYTES];
        proven_size_t n = 0;
        proven_sha256(msg, digest);
        if (!proven_tls_config_rsa_sign_(cfg, (proven_mem_view_t){ .ptr = digest, .size = 32 }, sig, &n)) return false;
        wr_uint(&w, SIG_RSA_PSS_RSAE_SHA256, 2); wr_uint(&w, (proven_u32)n, 2); wr_bytes(&w, sig, n);
    } else {
        proven_byte_t digest[32], raw[64], der[80];
        proven_sha256(msg, digest);
        if (!proven_crypto_ecdsa_sign(PROVEN_CRYPTO_EC_P256, PROVEN_HMAC_SHA256, cfg->key, (proven_mem_view_t){ .ptr = digest, .size = 32 }, raw)) return false;
        /* SEQUENCE { INTEGER r, INTEGER s }, each minimal and non-negative. */
        proven_size_t n = 2;
        for (int part = 0; part < 2; ++part) {
            const proven_byte_t *v = raw + 32 * part;
            proven_size_t len = 32;
            while (len > 1 && v[0] == 0) { v++; len--; }
            der[n++] = 0x02; der[n++] = (proven_byte_t)(len + (v[0] >> 7));
            if (v[0] & 0x80) der[n++] = 0;
            for (proven_size_t i = 0; i < len; ++i) der[n++] = v[i];
        }
        der[0] = 0x30; der[1] = (proven_byte_t)(n - 2);
        wr_uint(&w, SIG_ECDSA_P256_SHA256, 2); wr_uint(&w, (proven_u32)n, 2); wr_bytes(&w, der, n);
    }
    return hs_end(&w);
}

/* The schemes this side accepts in a CertificateVerify and in certificates. */
static void wr_sig_algs(wr_t *w) {
    static const proven_u16 list[] = {
        SIG_ECDSA_P256_SHA256, SIG_ECDSA_P384_SHA384, SIG_ED25519,
        SIG_RSA_PSS_RSAE_SHA256, SIG_RSA_PSS_RSAE_SHA384, SIG_RSA_PSS_RSAE_SHA512,
        SIG_RSA_PSS_PSS_SHA256, SIG_RSA_PSS_PSS_SHA384, SIG_RSA_PSS_PSS_SHA512,
        0x0401, 0x0501, 0x0601,                   /* RSA PKCS #1 v1.5: in certificates only */
    };
    wr_uint(w, EXT_SIGNATURE_ALGORITHMS, 2);
    proven_size_t ext = wr_open(w, 2), l = wr_open(w, 2);
    for (proven_size_t i = 0; i < sizeof list / sizeof list[0]; ++i) wr_uint(w, list[i], 2);
    wr_close(w, l, 2); wr_close(w, ext, 2);
}

/* Whether the peer's signature_algorithms list has the scheme this side's key signs with. */
static bool peer_accepts_our_key(const proven_tls_config_t *cfg, rd_t list) {
    proven_u16 want = cfg->key_kind == PROVEN_TLS_KEY_ED25519 ? SIG_ED25519 : cfg->key_kind == PROVEN_TLS_KEY_RSA ? SIG_RSA_PSS_RSAE_SHA256 : SIG_ECDSA_P256_SHA256;
    if (list.n % 2 != 0) return false;
    while (list.n >= 2) if ((proven_u16)rd_uint(&list, 2) == want) return true;
    return false;
}

/* This side's Certificate message into the scratch buffer; an empty list when `send` is false. */
static bool cert_build(proven_tls_conn_t *c, bool send) {
    const proven_tls_config_t *cfg = c->config;
    wr_t w = hs_begin(c, HS_CERTIFICATE);
    wr_uint(&w, 0, 1);                            /* certificate_request_context: empty */
    proven_size_t list = wr_open(&w, 3);
    for (proven_size_t i = 0; send && i < cfg->chain_count; ++i) {
        wr_uint(&w, (proven_u32)cfg->chain[i].size, 3);
        wr_bytes(&w, cfg->chain[i].ptr, cfg->chain[i].size);
        wr_uint(&w, 0, 2);                        /* no extensions */
    }
    wr_close(&w, list, 3);
    return hs_end(&w);
}

/* ---- The peer's certificate ---- */

static int alert_for_fault(proven_cert_fault_t f) {
    switch (f) {
        case PROVEN_CERT_FAULT_NO_ISSUER: return AL_UNKNOWN_CA;
        case PROVEN_CERT_FAULT_EXPIRED:
        case PROVEN_CERT_FAULT_NOT_YET_VALID: return AL_CERTIFICATE_EXPIRED;
        case PROVEN_CERT_FAULT_ALGORITHM: return AL_UNSUPPORTED_CERTIFICATE;
        default: return AL_BAD_CERTIFICATE;
    }
}

/* Parse and verify a Certificate message. *empty is set when the peer sent no certificate. */
static proven_err_t cert_process(proven_tls_conn_t *c, rd_t body, bool *empty) {
    const proven_tls_config_t *cfg = c->config;
    tls_handshake_t *h = c->hs;
    proven_mem_view_t chain[17];
    proven_size_t count = 0;
    *empty = false;
    rd_t ctx = rd_vec(&body, 1);
    rd_t list = rd_vec(&body, 3);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (ctx.n != 0) return tls_illegal(c);
    while (list.n > 0) {
        rd_t cert = rd_vec(&list, 3);
        rd_t exts = rd_vec(&list, 2);
        if (!list.ok || cert.n == 0) return tls_decode(c);
        (void)exts;                               /* per-certificate extensions (OCSP, SCT): not used */
        if (count < 17) chain[count++] = (proven_mem_view_t){ .ptr = cert.p, .size = cert.n };
    }
    if (count == 0) { *empty = true; return PROVEN_OK; }
    proven_cert_t leaf;
    if (proven_cert_parse(chain[0], &leaf) != PROVEN_OK) { c->peer_fault = PROVEN_CERT_FAULT_MALFORMED; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    if (leaf.key_kind == PROVEN_CERT_KEY_UNKNOWN) { c->peer_fault = PROVEN_CERT_FAULT_ALGORITHM; return tls_fail(c, AL_UNSUPPORTED_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    /* TLS 1.3 authenticates with a signature: a key restricted to something else cannot. */
    if (leaf.has_key_usage && !(leaf.key_usage & PROVEN_CERT_KU_DIGITAL_SIGNATURE)) { c->peer_fault = PROVEN_CERT_FAULT_USAGE; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    (void)proven_cert_key_sha256(&leaf, c->peer_key_hash);
    c->has_peer_key = true;
    if (cfg->verify == PROVEN_TLS_VERIFY_CHAIN) {
        proven_cert_verify_options_t opt = {
            .anchors = cfg->anchors,
            .host = c->is_server ? (proven_u8str_view_t){ .ptr = NULL, .size = 0 } : (proven_u8str_view_t){ .ptr = c->server_name, .size = c->server_name_len },
            .now = proven_tls_config_now(cfg),
            .use = c->is_server ? PROVEN_CERT_USE_CLIENT : PROVEN_CERT_USE_SERVER,
        };
        proven_cert_verify_result_t res;
        proven_err_t e = cfg->anchors ? proven_cert_verify(chain, count, &opt, &res) : PROVEN_ERR_UNTRUSTED;
        if (e != PROVEN_OK) {
            c->peer_fault = cfg->anchors ? res.fault : PROVEN_CERT_FAULT_NO_ISSUER;
            return tls_fail(c, alert_for_fault(c->peer_fault), e);
        }
    }
    if (cfg->pin_count > 0) {
        bool pinned = false;
        for (proven_size_t i = 0; i < cfg->pin_count; ++i) pinned = pinned || bytes_eq(cfg->pins[i], c->peer_key_hash, 32);
        if (!pinned) { c->peer_fault = PROVEN_CERT_FAULT_PIN_MISMATCH; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
    }
    h->peer_leaf.len = 0;
    if (!buf_append(c, &h->peer_leaf, chain[0].ptr, chain[0].size)) return tls_nomem(c);
    if (cfg->keep_peer_certificate) {
        c->peer_cert.len = 0;
        if (!buf_append(c, &c->peer_cert, chain[0].ptr, chain[0].size)) return tls_nomem(c);
    }
    return PROVEN_OK;
}

static void tr_add(proven_tls_conn_t *c, proven_mem_view_t whole) { proven_tls_transcript_update(&c->hs->transcript, whole); }

/* The Finished message of this side, built and sent. */
static bool finished_send(proven_tls_conn_t *c, const proven_byte_t *base_secret) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE], verify[PROVEN_HMAC_MAX_SIZE];
    hs_hash(c, th);
    proven_tls13_finished(c->suite->hash, base_secret, th, verify);
    wr_t w = hs_begin(c, HS_FINISHED);
    wr_bytes(&w, verify, c->suite->hash_len);
    return hs_end(&w) && hs_send(c);
}

static proven_err_t finished_check(proven_tls_conn_t *c, rd_t body, const proven_byte_t *base_secret) {
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE], want[PROVEN_HMAC_MAX_SIZE];
    if (body.n != c->suite->hash_len) return tls_decode(c);
    hs_hash(c, th);
    proven_tls13_finished(c->suite->hash, base_secret, th, want);
    if (!proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = body.n }, (proven_mem_view_t){ .ptr = body.p, .size = body.n }))
        return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
    return PROVEN_OK;
}

/* The handshake is over: keep the application secrets, drop the rest. */
static void establish(proven_tls_conn_t *c, const proven_byte_t *read_ap, const proven_byte_t *write_ap) {
    proven_size_t n = c->suite->hash_len;
    for (proven_size_t i = 0; i < n; ++i) { c->read_secret[i] = read_ap[i]; c->write_secret[i] = write_ap[i]; }
    c->state = ST_ESTABLISHED;
    c->established = true;
}

/* ================= Client ================= */

static bool host_is_ip(const proven_byte_t *s, proven_size_t n) {
    /* An IP literal is not sent as a server name (RFC 6066). Colons mean IPv6; digits and dots
     * only mean IPv4. */
    bool digits = n > 0;
    for (proven_size_t i = 0; i < n; ++i) {
        if (s[i] == ':') return true;
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) digits = false;
    }
    return digits;
}

/* The session to offer, if there is one that may be offered to this server now. */
static proven_tls_session_data_t *client_session(proven_tls_conn_t *c) {
    if (!c->session || c->config->no_resumption) return NULL;
    _Static_assert(sizeof(proven_tls_session_data_t) <= PROVEN_TLS_SESSION_SIZE, "the session record must fit its public block");
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    if (s->magic != PROVEN_TLS_SESSION_MAGIC) return NULL;
    const proven_tls_suite_t *suite = proven_tls_suite_find(s->suite);
    proven_i64 age = proven_tls_config_now(c->config) - s->received_at;
    if (!suite || s->psk_len != suite->hash_len || s->ticket_len == 0 || s->ticket_len > PROVEN_TLS_SESSION_TICKET_MAX) return NULL;
    if (age < 0 || age >= (proven_i64)s->lifetime_s || age >= 604800) return NULL;
    if (s->name_len != c->server_name_len || !bytes_eq(s->name, c->server_name, s->name_len)) return NULL;
    return s;
}

static proven_err_t client_hello_send(proven_tls_conn_t *c) {
    tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    if (h->test_hello_len > 0 && !h->retried) {
        h->scratch.len = 0;
        if (!buf_append(c, &h->scratch, h->test_hello, h->test_hello_len)) return tls_nomem(c);
        /* The session id is at a fixed place: after the type and length, the version and the random. */
        h->session_id_len = h->test_hello[38];
        for (proven_size_t i = 0; i < h->session_id_len && i < 32; ++i) h->session_id[i] = h->test_hello[39 + i];
        if (!buf_append(c, &h->first_hello, h->scratch.ptr, h->scratch.len)) return tls_nomem(c);
        return emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, 0x01) ? PROVEN_OK : tls_nomem(c);
    }
    proven_u16 suites[3];
    suite_order(suites);
    proven_tls_session_data_t *sess = client_session(c);
    const proven_tls_suite_t *psk_suite = sess ? proven_tls_suite_find(sess->suite) : NULL;
    /* After a retry the server has fixed the suite: a session made under another hash cannot be offered. */
    if (sess && h->retried && psk_suite->hash != c->suite->hash) sess = NULL;

    wr_t w = hs_begin(c, HS_CLIENT_HELLO);
    wr_uint(&w, 0x0303, 2);
    wr_bytes(&w, h->random, 32);
    wr_uint(&w, (proven_u32)h->session_id_len, 1); wr_bytes(&w, h->session_id, h->session_id_len);
    wr_uint(&w, 6, 2);
    for (int i = 0; i < 3; ++i) wr_uint(&w, suites[i], 2);
    wr_uint(&w, 1, 1); wr_uint(&w, 0, 1);         /* compression: none */
    proven_size_t exts = wr_open(&w, 2);

    if (c->server_name_len > 0 && !host_is_ip(c->server_name, c->server_name_len)) {
        wr_uint(&w, EXT_SERVER_NAME, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
        wr_uint(&w, 0, 1); wr_uint(&w, (proven_u32)c->server_name_len, 2); wr_bytes(&w, c->server_name, c->server_name_len);
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2); wr_uint(&w, 3, 2); wr_uint(&w, 2, 1); wr_uint(&w, 0x0304, 2);
    wr_uint(&w, EXT_SUPPORTED_GROUPS, 2); wr_uint(&w, 6, 2); wr_uint(&w, 4, 2); wr_uint(&w, GROUP_X25519, 2); wr_uint(&w, GROUP_P256, 2);
    wr_sig_algs(&w);
    {
        wr_uint(&w, EXT_KEY_SHARE, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
        if (h->retried && h->group == GROUP_P256) {
            proven_byte_t pub[65];
            make_p256_key(c, pub);
            wr_uint(&w, GROUP_P256, 2); wr_uint(&w, 65, 2); wr_bytes(&w, pub, 65);
        } else {
            proven_byte_t pub[32];
            proven_crypto_x25519_public(pub, h->x25519_priv);
            wr_uint(&w, GROUP_X25519, 2); wr_uint(&w, 32, 2); wr_bytes(&w, pub, 32);
        }
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    if (!cfg->no_resumption) { wr_uint(&w, EXT_PSK_MODES, 2); wr_uint(&w, 2, 2); wr_uint(&w, 1, 1); wr_uint(&w, 1, 1); }   /* psk_dhe_ke */
    if (cfg->alpn_count > 0) {
        wr_uint(&w, EXT_ALPN, 2);
        proven_size_t e = wr_open(&w, 2), l = wr_open(&w, 2);
        wr_bytes(&w, cfg->alpn_wire, cfg->alpn_wire_len);
        wr_close(&w, l, 2); wr_close(&w, e, 2);
    }
    wr_uint(&w, EXT_RECORD_SIZE_LIMIT, 2); wr_uint(&w, 2, 2); wr_uint(&w, PROVEN_TLS_MAX_PLAINTEXT + 1, 2);
    if (h->cookie_len > 0) {
        wr_uint(&w, EXT_COOKIE, 2); wr_uint(&w, (proven_u32)h->cookie_len + 2, 2);
        wr_uint(&w, (proven_u32)h->cookie_len, 2); wr_bytes(&w, h->cookie, h->cookie_len);
    }
    proven_size_t binder_at = 0;
    h->psk_offered = false;
    if (sess) {
        /* pre_shared_key is last: the binder covers everything before it. */
        proven_u32 age_ms = (proven_u32)((proven_u64)(proven_tls_config_now(cfg) - sess->received_at) * 1000u) + sess->age_add;
        wr_uint(&w, EXT_PRE_SHARED_KEY, 2);
        proven_size_t e = wr_open(&w, 2), ids = wr_open(&w, 2);
        wr_uint(&w, sess->ticket_len, 2); wr_bytes(&w, sess->ticket, sess->ticket_len); wr_uint(&w, age_ms, 4);
        wr_close(&w, ids, 2);
        binder_at = w.b->len;
        proven_byte_t zeros[PROVEN_HMAC_MAX_SIZE] = { 0 };
        wr_uint(&w, (proven_u32)psk_suite->hash_len + 1, 2); wr_uint(&w, (proven_u32)psk_suite->hash_len, 1); wr_bytes(&w, zeros, psk_suite->hash_len);
        wr_close(&w, e, 2);
        h->psk_offered = true;
        h->psk_len = sess->psk_len;
        for (proven_size_t i = 0; i < sess->psk_len; ++i) h->psk[i] = sess->psk[i];
    }
    wr_close(&w, exts, 2);
    if (!hs_end(&w)) return tls_nomem(c);
    if (h->psk_offered) {
        /* binder = HMAC(finished_key(binder_key), Hash(transcript so far + the hello up to here)) */
        proven_hmac_hash_t hash = psk_suite->hash;
        proven_tls_transcript_t t, empty;
        proven_byte_t early[PROVEN_HMAC_MAX_SIZE], binder_key[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE], binder[PROVEN_HMAC_MAX_SIZE];
        if (h->transcript_ready) t = h->transcript; else proven_tls_transcript_init(&t, hash);
        proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = binder_at });
        (void)proven_tls_transcript_hash(&t, th);
        proven_tls_transcript_init(&empty, hash);
        (void)proven_tls_transcript_hash(&empty, binder);
        proven_tls13_extract(hash, NULL, (proven_mem_view_t){ .ptr = h->psk, .size = h->psk_len }, early);
        proven_tls13_derive_secret(hash, early, "res binder", binder, binder_key);
        proven_tls13_finished(hash, binder_key, th, binder);
        for (proven_size_t i = 0; i < psk_suite->hash_len; ++i) h->scratch.ptr[binder_at + 3 + i] = binder[i];
        tls_wipe(early, sizeof early); tls_wipe(binder_key, sizeof binder_key);
    }
    if (!h->transcript_ready && !buf_append(c, &h->first_hello, h->scratch.ptr, h->scratch.len)) return tls_nomem(c);
    /* The first record of a connection says version 3.1 for the benefit of old middleboxes. */
    if (h->transcript_ready) proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->scratch.ptr, .size = h->scratch.len });
    return emit(c, PROVEN_TLS_CT_HANDSHAKE, h->scratch.ptr, h->scratch.len, h->retried ? 0x03 : 0x01) ? PROVEN_OK : tls_nomem(c);
}

static bool suite_was_offered(proven_u16 id) { return proven_tls_suite_find(id) != NULL; }

static proven_err_t client_server_hello(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    tls_handshake_t *h = c->hs;
    proven_u16 version = (proven_u16)rd_uint(&body, 2);
    rd_t random = rd_take(&body, 32);
    rd_t sid = rd_vec(&body, 1);
    proven_u16 suite_id = (proven_u16)rd_uint(&body, 2);
    proven_u32 compression = rd_uint(&body, 1);
    rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (version != 0x0303 || compression != 0) return tls_illegal(c);
    if (sid.n != h->session_id_len || !bytes_eq(sid.p, h->session_id, sid.n)) return tls_illegal(c);
    if (!suite_was_offered(suite_id)) return tls_illegal(c);
    bool retry = bytes_eq(random.p, HRR_RANDOM, 32);
    bool have_version = false, have_share = false, have_psk = false;
    proven_u16 group = 0;
    rd_t share = { 0 };
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        unsigned bit = type == EXT_SUPPORTED_VERSIONS ? 1u : type == EXT_KEY_SHARE ? 2u : type == EXT_PRE_SHARED_KEY ? 4u : type == EXT_COOKIE ? 8u : 0u;
        if (bit == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
        if (type == EXT_SUPPORTED_VERSIONS) {
            if (data.n != 2 || rd_uint(&data, 2) != 0x0304) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
            have_version = true;
        } else if (type == EXT_KEY_SHARE) {
            group = (proven_u16)rd_uint(&data, 2);
            if (!retry) share = rd_vec(&data, 2);
            if (!data.ok || data.n != 0) return tls_decode(c);
            have_share = true;
        } else if (type == EXT_PRE_SHARED_KEY) {
            if (retry || data.n != 2) return tls_illegal(c);
            if (rd_uint(&data, 2) != 0 || !h->psk_offered) return tls_illegal(c);
            have_psk = true;
        } else {
            if (!retry) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            rd_t cookie = rd_vec(&data, 2);
            if (!data.ok || data.n != 0 || cookie.n == 0) return tls_decode(c);
            if (cookie.n > TLS_COOKIE_MAX) return tls_illegal(c);
            for (proven_size_t i = 0; i < cookie.n; ++i) h->cookie[i] = cookie.p[i];
            h->cookie_len = cookie.n;
        }
    }
    /* No supported_versions means a server that speaks only an older version. */
    if (!have_version) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
    const proven_tls_suite_t *suite = proven_tls_suite_find(suite_id);
    if (retry) {
        if (h->retried) return tls_unexpected(c);                 /* one retry, not two */
        /* It must ask for something else than what was sent, and something this side offered. */
        if (!have_share || group != GROUP_P256) return tls_illegal(c);
        c->suite = suite;
        proven_tls_transcript_init(&h->transcript, suite->hash);
        proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
        proven_tls_transcript_retry(&h->transcript);
        tr_add(c, whole);
        h->transcript_ready = true;
        buf_free(c, &h->first_hello);
        h->retried = true;
        h->group = group;
        if (!send_ccs(c)) return tls_nomem(c);
        return client_hello_send(c);
    }
    if (!have_share) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
    if (h->retried && suite != c->suite) return tls_illegal(c);
    if (group != (h->retried ? h->group : GROUP_X25519)) return tls_illegal(c);
    c->suite = suite;
    h->group = group;
    if (have_psk) {
        /* The server may only select the key under a suite with the hash it was made for. */
        if (h->psk_len != suite->hash_len) return tls_illegal(c);
        h->psk_accepted = true;
        c->resumed = true;
        /* The server's identity is the one verified when the session was made. */
        proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
        c->has_peer_key = s->has_peer_key == 1;
        for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = s->peer_key_hash[i];
        /* A pin is a statement about the key: it holds for a resumed session too. */
        if (c->config->pin_count > 0) {
            bool pinned = false;
            for (proven_size_t i = 0; c->has_peer_key && i < c->config->pin_count; ++i) pinned = pinned || bytes_eq(c->config->pins[i], c->peer_key_hash, 32);
            if (!pinned) { c->peer_fault = PROVEN_CERT_FAULT_PIN_MISMATCH; return tls_fail(c, AL_BAD_CERTIFICATE, PROVEN_ERR_UNTRUSTED); }
        }
    }
    if (!h->transcript_ready) {
        proven_tls_transcript_init(&h->transcript, suite->hash);
        proven_tls_transcript_update(&h->transcript, (proven_mem_view_t){ .ptr = h->first_hello.ptr, .size = h->first_hello.len });
        h->transcript_ready = true;
        buf_free(c, &h->first_hello);
    }
    tr_add(c, whole);
    proven_byte_t shared[32];
    if (!key_agree(c, group, (proven_mem_view_t){ .ptr = share.p, .size = share.n }, shared)) return tls_illegal(c);
    derive_handshake(c, shared, 32);
    tls_wipe(shared, sizeof shared);
    proven_tls13_set_keys(&c->read_keys, c->suite, h->server_hs);
    /* From here on anything this side sends - an alert included - is under its handshake key. */
    proven_tls13_set_keys(&c->write_keys, c->suite, h->client_hs);
    c->state = ST_C_WAIT_EE;
    return PROVEN_OK;
}

static proven_err_t client_encrypted_extensions(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    const proven_tls_config_t *cfg = c->config;
    rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        unsigned bit = type == EXT_ALPN ? 1u : type == EXT_SERVER_NAME ? 2u : type == EXT_RECORD_SIZE_LIMIT ? 4u : type == EXT_SUPPORTED_GROUPS ? 8u : 0u;
        /* A server may only answer what was asked. */
        if (bit == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
        if (type == EXT_ALPN) {
            if (cfg->alpn_count == 0) return tls_fail(c, AL_UNSUPPORTED_EXTENSION, PROVEN_ERR_PROTOCOL);
            rd_t list = rd_vec(&data, 2);
            rd_t name = rd_vec(&list, 1);
            if (!data.ok || data.n != 0 || !list.ok || list.n != 0 || name.n == 0) return tls_decode(c);
            c->alpn_index = -1;
            for (proven_size_t i = 0; i < cfg->alpn_count; ++i) {
                proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
                if (ours.size == name.n && bytes_eq(ours.ptr, name.p, name.n)) c->alpn_index = (int)i;
            }
            if (c->alpn_index < 0) return tls_illegal(c);          /* a protocol that was not offered */
        } else if (type == EXT_RECORD_SIZE_LIMIT) {
            proven_u32 limit = rd_uint(&data, 2);
            if (!data.ok || data.n != 0 || limit < 64 || limit > PROVEN_TLS_MAX_PLAINTEXT + 1) return tls_illegal(c);
            c->peer_record_limit = limit - 1;                      /* the limit counts the content type byte */
        } else if (type == EXT_SERVER_NAME) {
            if (data.n != 0) return tls_decode(c);
        }
    }
    tr_add(c, whole);
    c->state = c->hs->psk_accepted ? ST_C_WAIT_FINISHED : ST_C_WAIT_CERT_CR;
    return PROVEN_OK;
}

static proven_err_t client_certificate_request(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    rd_t ctx = rd_vec(&body, 1);
    rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    if (ctx.n != 0) return tls_illegal(c);
    bool have_algs = false, ours_ok = false;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        if (type == EXT_SIGNATURE_ALGORITHMS) {
            rd_t list = rd_vec(&data, 2);
            if (!data.ok || data.n != 0 || list.n == 0 || list.n % 2 != 0) return tls_decode(c);
            have_algs = true;
            ours_ok = c->config->key_kind != PROVEN_TLS_KEY_NONE && peer_accepts_our_key(c->config, list);
        }
    }
    if (!have_algs) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
    c->hs->cert_requested = true;
    c->hs->peer_sig_schemes = ours_ok ? 1 : 0;
    tr_add(c, whole);
    c->state = ST_C_WAIT_CERT;
    return PROVEN_OK;
}

static void client_store_ticket(proven_tls_conn_t *c, rd_t body);

static proven_err_t client_finished(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    tls_handshake_t *h = c->hs;
    proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], server_ap[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE];
    proven_err_t e = finished_check(c, body, h->server_hs);
    if (e != PROVEN_OK) return e;
    tr_add(c, whole);
    derive_application(c, client_ap, server_ap);
    proven_tls13_set_keys(&c->read_keys, c->suite, server_ap);
    if (!send_ccs(c)) return tls_nomem(c);
    if (h->cert_requested) {
        bool send = h->peer_sig_schemes != 0;
        if (!cert_build(c, send) || !hs_send(c)) return tls_nomem(c);
        if (send && (!cv_build(c, false) || !hs_send(c))) return tls_nomem(c);
    }
    if (!finished_send(c, h->client_hs)) return tls_nomem(c);
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, h->master_secret, "res master", th, c->resumption_master);
    proven_tls13_set_keys(&c->write_keys, c->suite, client_ap);
    establish(c, server_ap, client_ap);
    tls_wipe(client_ap, sizeof client_ap); tls_wipe(server_ap, sizeof server_ap);
    hs_free(c);
    return PROVEN_OK;
}

static proven_err_t client_message(proven_tls_conn_t *c, proven_byte_t type, rd_t body, proven_mem_view_t whole, bool *keys_changed) {
    proven_err_t e;
    bool empty = false;
    switch (c->state) {
        case ST_C_WAIT_SH:
            if (type != HS_SERVER_HELLO) return tls_unexpected(c);
            e = client_server_hello(c, body, whole);
            *keys_changed = c->state == ST_C_WAIT_EE;
            return e;
        case ST_C_WAIT_EE:
            if (type != HS_ENCRYPTED_EXTENSIONS) return tls_unexpected(c);
            return client_encrypted_extensions(c, body, whole);
        case ST_C_WAIT_CERT_CR:
            if (type == HS_CERTIFICATE_REQUEST) return client_certificate_request(c, body, whole);
            /* fall through */
        case ST_C_WAIT_CERT:
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) return tls_decode(c);                      /* a server must present a certificate */
            tr_add(c, whole);
            c->state = ST_C_WAIT_CV;
            return PROVEN_OK;
        case ST_C_WAIT_CV:
            if (type != HS_CERTIFICATE_VERIFY) return tls_unexpected(c);
            e = cv_verify(c, body, true);
            if (e != PROVEN_OK) return e;
            tr_add(c, whole);
            c->state = ST_C_WAIT_FINISHED;
            return PROVEN_OK;
        case ST_C_WAIT_FINISHED:
            if (type != HS_FINISHED) return tls_unexpected(c);
            *keys_changed = true;
            return client_finished(c, body, whole);
        default:
            return tls_unexpected(c);
    }
}

/* ================= Server ================= */

static bool list_has_u16(rd_t list, proven_u16 want) {
    while (list.n >= 2) if ((proven_u16)rd_uint(&list, 2) == want) return true;
    return false;
}

static proven_err_t server_send_ticket(proven_tls_conn_t *c);

static proven_err_t server_client_hello(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    tls_handshake_t *h = c->hs;
    const proven_tls_config_t *cfg = c->config;
    const proven_byte_t *msg_start = whole.ptr;
    proven_u16 version = (proven_u16)rd_uint(&body, 2);
    rd_t random = rd_take(&body, 32);
    rd_t sid = rd_vec(&body, 1);
    rd_t suites = rd_vec(&body, 2);
    rd_t compression = rd_vec(&body, 1);
    rd_t exts = rd_vec(&body, 2);
    if (!body.ok || body.n != 0) return tls_decode(c);
    (void)random;
    if (version != 0x0303 || sid.n > 32 || suites.n < 2 || suites.n % 2 != 0) return tls_illegal(c);
    if (compression.n != 1 || compression.p[0] != 0) return tls_illegal(c);

    rd_t versions = { 0 }, groups = { 0 }, shares = { 0 }, sig_algs = { 0 }, alpn = { 0 }, psk_ids = { 0 }, psk_binders = { 0 }, modes = { 0 };
    proven_size_t binders_at = 0;
    bool have_psk = false, have_sni = false;
    unsigned seen = 0;
    while (exts.n > 0) {
        proven_u16 type = (proven_u16)rd_uint(&exts, 2);
        rd_t data = rd_vec(&exts, 2);
        if (!exts.ok) return tls_decode(c);
        if (have_psk) return tls_illegal(c);                       /* pre_shared_key must be the last extension */
        unsigned bit = 0;
        switch (type) {
            case EXT_SUPPORTED_VERSIONS: bit = 1; versions = rd_vec(&data, 1); break;
            case EXT_SUPPORTED_GROUPS: bit = 2; groups = rd_vec(&data, 2); break;
            case EXT_KEY_SHARE: bit = 4; shares = rd_vec(&data, 2); break;
            case EXT_SIGNATURE_ALGORITHMS: bit = 8; sig_algs = rd_vec(&data, 2); break;
            case EXT_ALPN: bit = 16; alpn = rd_vec(&data, 2); break;
            case EXT_PSK_MODES: bit = 32; modes = rd_vec(&data, 1); break;
            case EXT_SERVER_NAME: {
                bit = 64;
                rd_t list = rd_vec(&data, 2);
                while (list.n > 0) {
                    proven_u32 kind = rd_uint(&list, 1);
                    rd_t name = rd_vec(&list, 2);
                    if (!list.ok) return tls_decode(c);
                    if (kind == 0) {
                        if (have_sni || name.n == 0 || name.n > 253) return tls_illegal(c);
                        for (proven_size_t i = 0; i < name.n; ++i) h->sni[i] = name.p[i];
                        h->sni_len = name.n;
                        have_sni = true;
                    }
                }
                break; }
            case EXT_RECORD_SIZE_LIMIT: {
                bit = 128;
                proven_u32 limit = rd_uint(&data, 2);
                if (!data.ok || limit < 64 || limit > PROVEN_TLS_MAX_PLAINTEXT + 1) return tls_illegal(c);
                c->peer_record_limit = limit - 1;
                h->client_sent_record_limit = true;
                break; }
            case EXT_PRE_SHARED_KEY:
                bit = 256;
                psk_ids = rd_vec(&data, 2);
                binders_at = (proven_size_t)(data.p - msg_start);
                psk_binders = rd_vec(&data, 2);
                have_psk = true;
                break;
            default:
                continue;                                         /* an extension this side does not use */
        }
        if (!data.ok || data.n != 0) return tls_decode(c);
        if (seen & bit) return tls_illegal(c);
        seen |= bit;
    }
    h->client_sent_sni = have_sni;
    h->client_sent_alpn = (seen & 16) != 0;
    if (!(seen & 1) || versions.n % 2 != 0 || !list_has_u16(versions, 0x0304)) return tls_fail(c, AL_PROTOCOL_VERSION, PROVEN_ERR_PROTOCOL);
    if (!(seen & 2) || !(seen & 4) || groups.n % 2 != 0) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);

    /* The suite: the first of this side's order that the client offers. */
    proven_u16 order[3];
    const proven_tls_suite_t *suite = NULL;
    suite_order(order);
    for (int i = 0; i < 3 && !suite; ++i) if (list_has_u16(suites, order[i])) suite = proven_tls_suite_find(order[i]);
    if (!suite) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    if (h->retried && suite != c->suite) return tls_illegal(c);     /* the second hello must still lead to the same choice */
    c->suite = suite;

    /* The key share: X25519 if the client sent one, then P-256. */
    rd_t share_x = { 0 }, share_p = { 0 }, walk = shares;
    while (walk.n > 0) {
        proven_u16 g = (proven_u16)rd_uint(&walk, 2);
        rd_t key = rd_vec(&walk, 2);
        if (!walk.ok) return tls_decode(c);
        if (g == GROUP_X25519 && !share_x.ok && !g_test_server_refuses_x25519) share_x = key;
        if (g == GROUP_P256 && !share_p.ok) share_p = key;
    }
    proven_u16 group = share_x.ok ? GROUP_X25519 : share_p.ok ? GROUP_P256 : 0;
    if (h->retried && group != h->group) return tls_illegal(c);
    if (group == 0) {
        /* Nothing usable was sent. If the client supports a group this side has, ask for it - once. */
        proven_u16 ask = list_has_u16(groups, GROUP_X25519) && !g_test_server_refuses_x25519 ? GROUP_X25519 : list_has_u16(groups, GROUP_P256) ? GROUP_P256 : 0;
        if (ask == 0) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
        if (h->retried) return tls_illegal(c);
        proven_tls_transcript_init(&h->transcript, suite->hash);
        tr_add(c, whole);
        proven_tls_transcript_retry(&h->transcript);
        h->transcript_ready = true;
        h->retried = true;
        h->group = ask;
        h->session_id_len = sid.n;
        for (proven_size_t i = 0; i < sid.n; ++i) h->session_id[i] = sid.p[i];
        wr_t w = hs_begin(c, HS_SERVER_HELLO);
        wr_uint(&w, 0x0303, 2); wr_bytes(&w, HRR_RANDOM, 32);
        wr_uint(&w, (proven_u32)sid.n, 1); wr_bytes(&w, sid.p, sid.n);
        wr_uint(&w, suite->id, 2); wr_uint(&w, 0, 1);
        proven_size_t e = wr_open(&w, 2);
        wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0x0304, 2);
        wr_uint(&w, EXT_KEY_SHARE, 2); wr_uint(&w, 2, 2); wr_uint(&w, ask, 2);
        wr_close(&w, e, 2);
        if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        if (!send_ccs(c)) return tls_nomem(c);
        return PROVEN_OK;                                           /* still waiting for a ClientHello */
    }
    if (h->retried && (sid.n != h->session_id_len || !bytes_eq(sid.p, h->session_id, sid.n))) return tls_illegal(c);
    if (!h->transcript_ready) { proven_tls_transcript_init(&h->transcript, suite->hash); h->transcript_ready = true; }
    h->group = group;
    h->session_id_len = sid.n;
    for (proven_size_t i = 0; i < sid.n; ++i) h->session_id[i] = sid.p[i];

    /* Resumption: the first identity, if it is a ticket of this server, made for a suite with
     * this hash, and its binder proves the client holds the key. The binder is checked before
     * the key is used for anything else. */
    proven_tls_ticket_state_t ticket = { 0 };
    if (have_psk) {
        rd_t ids = psk_ids, binders = psk_binders;
        rd_t identity = rd_vec(&ids, 2);
        (void)rd_uint(&ids, 4);
        rd_t binder = rd_vec(&binders, 1);
        if (!ids.ok || !binders.ok || identity.n == 0 || binder.n < 32) return tls_decode(c);
        bool usable = !cfg->no_resumption && modes.ok && modes.n > 0;
        bool dhe = false;
        for (proven_size_t i = 0; usable && i < modes.n; ++i) dhe = dhe || modes.p[i] == 1;
        if (usable && dhe && proven_tls_ticket_open(cfg, (proven_mem_view_t){ .ptr = identity.p, .size = identity.n }, &ticket)) {
            const proven_tls_suite_t *made = proven_tls_suite_find(ticket.suite);
            if (made && made->hash == suite->hash && ticket.psk_len == suite->hash_len) {
                proven_tls_transcript_t t = h->transcript, empty;
                proven_byte_t early[PROVEN_HMAC_MAX_SIZE], key[PROVEN_HMAC_MAX_SIZE], th[PROVEN_HMAC_MAX_SIZE], want[PROVEN_HMAC_MAX_SIZE];
                proven_tls_transcript_update(&t, (proven_mem_view_t){ .ptr = msg_start, .size = binders_at });
                (void)proven_tls_transcript_hash(&t, th);
                proven_tls_transcript_init(&empty, suite->hash);
                (void)proven_tls_transcript_hash(&empty, want);
                proven_tls13_extract(suite->hash, NULL, (proven_mem_view_t){ .ptr = ticket.psk, .size = ticket.psk_len }, early);
                proven_tls13_derive_secret(suite->hash, early, "res binder", want, key);
                proven_tls13_finished(suite->hash, key, th, want);
                bool match = binder.n == suite->hash_len &&
                             proven_mem_equal_ct((proven_mem_view_t){ .ptr = want, .size = binder.n }, (proven_mem_view_t){ .ptr = binder.p, .size = binder.n });
                tls_wipe(early, sizeof early); tls_wipe(key, sizeof key);
                if (!match) return tls_fail(c, AL_DECRYPT_ERROR, PROVEN_ERR_PROTOCOL);
                h->psk_accepted = true;
                h->psk_len = ticket.psk_len;
                for (proven_size_t i = 0; i < ticket.psk_len; ++i) h->psk[i] = ticket.psk[i];
                c->resumed = true;
                c->has_peer_key = ticket.has_peer_key;
                for (int i = 0; i < 32; ++i) c->peer_key_hash[i] = ticket.peer_key_hash[i];
            }
        }
        tls_wipe(&ticket, sizeof ticket);
    }
    if (!h->psk_accepted) {
        if (!(seen & 8) || sig_algs.n % 2 != 0) return tls_fail(c, AL_MISSING_EXTENSION, PROVEN_ERR_PROTOCOL);
        if (!peer_accepts_our_key(cfg, sig_algs)) return tls_fail(c, AL_HANDSHAKE_FAILURE, PROVEN_ERR_PROTOCOL);
    }
    /* ALPN: this side's first choice among what the client offers; no overlap is a failure. */
    c->alpn_index = -1;
    if (h->client_sent_alpn && cfg->alpn_count > 0) {
        for (proven_size_t i = 0; i < cfg->alpn_count && c->alpn_index < 0; ++i) {
            proven_u8str_view_t ours = proven_tls_config_alpn(cfg, i);
            rd_t list = alpn;
            while (list.n > 0) {
                rd_t name = rd_vec(&list, 1);
                if (!list.ok || name.n == 0) return tls_decode(c);
                if (name.n == ours.size && bytes_eq(name.p, ours.ptr, name.n)) { c->alpn_index = (int)i; break; }
            }
        }
        if (c->alpn_index < 0) return tls_fail(c, AL_NO_APPLICATION_PROTOCOL, PROVEN_ERR_PROTOCOL);
    }
    tr_add(c, whole);

    /* This side's share and the key both arrive at. */
    proven_byte_t shared[32], pub[65];
    proven_size_t pub_len;
    rd_t theirs = group == GROUP_X25519 ? share_x : share_p;
    if (group == GROUP_X25519) {
        proven_tls_config_random(cfg, h->x25519_priv, 32);
        proven_crypto_x25519_public(pub, h->x25519_priv);
        pub_len = 32;
    } else {
        make_p256_key(c, pub);
        pub_len = 65;
    }
    if (!key_agree(c, group, (proven_mem_view_t){ .ptr = theirs.p, .size = theirs.n }, shared)) return tls_illegal(c);

    proven_byte_t server_random[32];
    proven_tls_config_random(cfg, server_random, 32);
    wr_t w = hs_begin(c, HS_SERVER_HELLO);
    wr_uint(&w, 0x0303, 2); wr_bytes(&w, server_random, 32);
    wr_uint(&w, (proven_u32)sid.n, 1); wr_bytes(&w, h->session_id, sid.n);
    wr_uint(&w, suite->id, 2); wr_uint(&w, 0, 1);
    proven_size_t e = wr_open(&w, 2);
    wr_uint(&w, EXT_SUPPORTED_VERSIONS, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0x0304, 2);
    wr_uint(&w, EXT_KEY_SHARE, 2); wr_uint(&w, (proven_u32)pub_len + 4, 2); wr_uint(&w, group, 2); wr_uint(&w, (proven_u32)pub_len, 2); wr_bytes(&w, pub, pub_len);
    if (h->psk_accepted) { wr_uint(&w, EXT_PRE_SHARED_KEY, 2); wr_uint(&w, 2, 2); wr_uint(&w, 0, 2); }
    wr_close(&w, e, 2);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
    if (!send_ccs(c)) return tls_nomem(c);
    derive_handshake(c, shared, 32);
    tls_wipe(shared, sizeof shared);
    proven_tls13_set_keys(&c->write_keys, suite, h->server_hs);
    proven_tls13_set_keys(&c->read_keys, suite, h->client_hs);

    w = hs_begin(c, HS_ENCRYPTED_EXTENSIONS);
    e = wr_open(&w, 2);
    if (c->alpn_index >= 0) {
        proven_u8str_view_t name = proven_tls_config_alpn(cfg, (proven_size_t)c->alpn_index);
        wr_uint(&w, EXT_ALPN, 2); wr_uint(&w, (proven_u32)name.size + 3, 2); wr_uint(&w, (proven_u32)name.size + 1, 2);
        wr_uint(&w, (proven_u32)name.size, 1); wr_bytes(&w, name.ptr, name.size);
    }
    if (h->client_sent_sni) { wr_uint(&w, EXT_SERVER_NAME, 2); wr_uint(&w, 0, 2); }
    if (h->client_sent_record_limit) { wr_uint(&w, EXT_RECORD_SIZE_LIMIT, 2); wr_uint(&w, 2, 2); wr_uint(&w, PROVEN_TLS_MAX_PLAINTEXT + 1, 2); }
    wr_close(&w, e, 2);
    if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);

    bool ask_cert = !h->psk_accepted && cfg->client_auth != PROVEN_TLS_CLIENT_AUTH_NONE;
    if (!h->psk_accepted) {
        if (ask_cert) {
            w = hs_begin(c, HS_CERTIFICATE_REQUEST);
            wr_uint(&w, 0, 1);
            e = wr_open(&w, 2);
            wr_sig_algs(&w);
            wr_close(&w, e, 2);
            if (!hs_end(&w) || !hs_send(c)) return tls_nomem(c);
        }
        if (!cert_build(c, true) || !hs_send(c)) return tls_nomem(c);
        if (!cv_build(c, true) || !hs_send(c)) return tls_nomem(c);
    }
    if (!finished_send(c, h->server_hs)) return tls_nomem(c);
    /* The server may write application data from here; this side's sending keys change now. */
    proven_byte_t client_ap[PROVEN_HMAC_MAX_SIZE], server_ap[PROVEN_HMAC_MAX_SIZE];
    derive_application(c, client_ap, server_ap);
    proven_tls13_set_keys(&c->write_keys, suite, server_ap);
    for (proven_size_t i = 0; i < suite->hash_len; ++i) { c->write_secret[i] = server_ap[i]; c->read_secret[i] = client_ap[i]; }
    tls_wipe(client_ap, sizeof client_ap); tls_wipe(server_ap, sizeof server_ap);
    c->state = ask_cert ? ST_S_WAIT_CERT : ST_S_WAIT_FINISHED;
    return PROVEN_OK;
}

static proven_err_t server_finished(proven_tls_conn_t *c, rd_t body, proven_mem_view_t whole) {
    tls_handshake_t *h = c->hs;
    proven_byte_t th[PROVEN_HMAC_MAX_SIZE];
    proven_err_t e = finished_check(c, body, h->client_hs);
    if (e != PROVEN_OK) return e;
    tr_add(c, whole);
    hs_hash(c, th);
    proven_tls13_derive_secret(c->suite->hash, h->master_secret, "res master", th, c->resumption_master);
    proven_tls13_set_keys(&c->read_keys, c->suite, c->read_secret);
    c->state = ST_ESTABLISHED;
    c->established = true;
    e = c->config->no_resumption ? PROVEN_OK : server_send_ticket(c);
    hs_free(c);
    return e;
}

static proven_err_t server_send_ticket(proven_tls_conn_t *c) {
    const proven_tls_config_t *cfg = c->config;
    proven_tls_ticket_state_t st = { 0 };
    proven_byte_t ticket[PROVEN_TLS_TICKET_MAX], nonce = 0, age_add[4];
    st.suite = c->suite->id;
    st.psk_len = c->suite->hash_len;
    proven_tls13_expand_label(c->suite->hash, c->resumption_master, "resumption", (proven_mem_view_t){ .ptr = &nonce, .size = 1 }, st.psk, st.psk_len);
    st.issued_at = proven_tls_config_now(cfg);
    st.has_peer_key = c->has_peer_key;
    for (int i = 0; i < 32; ++i) st.peer_key_hash[i] = c->peer_key_hash[i];
    proven_size_t ticket_len = proven_tls_ticket_seal(cfg, &st, ticket);
    tls_wipe(&st, sizeof st);
    proven_tls_config_random(cfg, age_add, 4);
    wr_t w = hs_begin(c, HS_NEW_SESSION_TICKET);
    wr_uint(&w, cfg->ticket_lifetime_s, 4);
    wr_bytes(&w, age_add, 4);
    wr_uint(&w, 1, 1); wr_uint(&w, nonce, 1);
    wr_uint(&w, (proven_u32)ticket_len, 2); wr_bytes(&w, ticket, ticket_len);
    wr_uint(&w, 0, 2);
    /* Sent after the handshake: it is not part of the transcript. */
    if (!hs_end(&w) || !emit(c, PROVEN_TLS_CT_HANDSHAKE, w.b->ptr, w.b->len, 0x03)) return tls_nomem(c);
    return PROVEN_OK;
}

static proven_err_t server_message(proven_tls_conn_t *c, proven_byte_t type, rd_t body, proven_mem_view_t whole, bool *keys_changed) {
    proven_err_t e;
    bool empty = false;
    switch (c->state) {
        case ST_S_WAIT_CH:
            if (type != HS_CLIENT_HELLO) return tls_unexpected(c);
            e = server_client_hello(c, body, whole);
            *keys_changed = c->state != ST_S_WAIT_CH;
            return e;
        case ST_S_WAIT_CERT:
            if (type != HS_CERTIFICATE) return tls_unexpected(c);
            e = cert_process(c, body, &empty);
            if (e != PROVEN_OK) return e;
            if (empty) {
                if (c->config->client_auth == PROVEN_TLS_CLIENT_AUTH_REQUIRE) return tls_fail(c, AL_CERTIFICATE_REQUIRED, PROVEN_ERR_UNTRUSTED);
                tr_add(c, whole);
                c->state = ST_S_WAIT_FINISHED;
                return PROVEN_OK;
            }
            tr_add(c, whole);
            c->state = ST_S_WAIT_CV;
            return PROVEN_OK;
        case ST_S_WAIT_CV:
            if (type != HS_CERTIFICATE_VERIFY) return tls_unexpected(c);
            e = cv_verify(c, body, false);
            if (e != PROVEN_OK) return e;
            tr_add(c, whole);
            c->state = ST_S_WAIT_FINISHED;
            return PROVEN_OK;
        case ST_S_WAIT_FINISHED:
            if (type != HS_FINISHED) return tls_unexpected(c);
            *keys_changed = true;
            return server_finished(c, body, whole);
        default:
            return tls_unexpected(c);
    }
}

/* ================= After the handshake ================= */

static void client_store_ticket(proven_tls_conn_t *c, rd_t body) {
    proven_u32 lifetime = rd_uint(&body, 4), age_add = rd_uint(&body, 4);
    rd_t nonce = rd_vec(&body, 1);
    rd_t ticket = rd_vec(&body, 2);
    rd_t exts = rd_vec(&body, 2);
    (void)exts;
    /* A ticket that is malformed or does not fit is simply not kept: resumption is optional. */
    if (!body.ok || !c->session || c->config->no_resumption || ticket.n == 0 || ticket.n > PROVEN_TLS_SESSION_TICKET_MAX || lifetime == 0) return;
    proven_tls_session_data_t *s = (proven_tls_session_data_t *)(void *)c->session->opaque;
    tls_wipe(s, sizeof *s);
    s->suite = c->suite->id;
    s->psk_len = (proven_u16)c->suite->hash_len;
    proven_tls13_expand_label(c->suite->hash, c->resumption_master, "resumption", (proven_mem_view_t){ .ptr = nonce.p, .size = nonce.n }, s->psk, s->psk_len);
    s->received_at = proven_tls_config_now(c->config);
    s->lifetime_s = lifetime > 604800 ? 604800 : lifetime;
    s->age_add = age_add;
    s->name_len = (proven_u16)c->server_name_len;
    for (proven_size_t i = 0; i < c->server_name_len; ++i) s->name[i] = c->server_name[i];
    s->has_peer_key = c->has_peer_key ? 1 : 0;
    for (int i = 0; i < 32; ++i) s->peer_key_hash[i] = c->peer_key_hash[i];
    s->ticket_len = (proven_u16)ticket.n;
    for (proven_size_t i = 0; i < ticket.n; ++i) s->ticket[i] = ticket.p[i];
    s->magic = PROVEN_TLS_SESSION_MAGIC;
}

/* The next traffic secret of a direction (RFC 8446 section 7.2). */
static void rotate(proven_tls_conn_t *c, proven_byte_t *secret, proven_tls_keys_t *keys) {
    proven_byte_t next[PROVEN_HMAC_MAX_SIZE];
    proven_tls13_expand_label(c->suite->hash, secret, "traffic upd", (proven_mem_view_t){ .ptr = next, .size = 0 }, next, c->suite->hash_len);
    for (proven_size_t i = 0; i < c->suite->hash_len; ++i) secret[i] = next[i];
    proven_tls13_set_keys(keys, c->suite, secret);
    tls_wipe(next, sizeof next);
}

static bool key_update_send(proven_tls_conn_t *c, proven_byte_t request) {
    proven_byte_t msg[5] = { HS_KEY_UPDATE, 0, 0, 1, request };
    if (!emit(c, PROVEN_TLS_CT_HANDSHAKE, msg, 5, 0x03)) return false;
    rotate(c, c->write_secret, &c->write_keys);
    return true;
}

static proven_err_t post_message(proven_tls_conn_t *c, proven_byte_t type, rd_t body, bool *keys_changed) {
    if (type == HS_NEW_SESSION_TICKET) {
        if (c->is_server) return tls_unexpected(c);
        client_store_ticket(c, body);
        return PROVEN_OK;
    }
    if (type == HS_KEY_UPDATE) {
        if (body.n != 1 || body.p[0] > 1) return tls_illegal(c);
        rotate(c, c->read_secret, &c->read_keys);
        *keys_changed = true;
        if (body.p[0] == 1 && !c->close_sent) {
            /* Each request is owed an answer, and answers queue until the caller sends them. A
             * peer that asks faster than it reads would make that queue grow without limit. */
            if (c->out.len - c->out_off > TLS_MAX_UNSENT) return tls_unexpected(c);
            if (!key_update_send(c, 0)) return tls_nomem(c);
        }
        return PROVEN_OK;
    }
    return tls_unexpected(c);
}

/* ================= Records in ================= */

/* Handshake bytes from one record. Messages may span records, and several may share one; a
 * message that changes the reading keys must be the last thing in its record. */
static proven_err_t handshake_bytes(proven_tls_conn_t *c, const proven_byte_t *data, proven_size_t len) {
    tls_buf_t *acc = c->established ? &c->post : &c->hs->reassembly;
    proven_size_t limit = c->config->max_handshake_bytes;
    if (len == 0) return tls_unexpected(c);
    if (acc->len + len > limit + 4) return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL);
    if (!buf_append(c, acc, data, len)) return tls_nomem(c);
    proven_size_t at = 0;
    while (acc->len - at >= 4) {
        const proven_byte_t *p = acc->ptr + at;
        proven_size_t mlen = ((proven_size_t)p[1] << 16) | ((proven_size_t)p[2] << 8) | p[3];
        if (mlen > limit) return tls_fail(c, AL_ILLEGAL_PARAMETER, PROVEN_ERR_PROTOCOL);
        if (acc->len - at < 4 + mlen) break;
        rd_t body = { p + 4, mlen, true };
        proven_mem_view_t whole = { .ptr = p, .size = 4 + mlen };
        bool keys_changed = false, was_established = c->established;
        proven_size_t total = acc->len;
        proven_err_t e = c->established ? post_message(c, p[0], body, &keys_changed)
                       : c->is_server ? server_message(c, p[0], body, whole, &keys_changed)
                                      : client_message(c, p[0], body, whole, &keys_changed);
        if (e != PROVEN_OK) return e;
        at += 4 + mlen;
        if (keys_changed && at != total) return tls_unexpected(c);      /* bytes under the old keys after a key change */
        if (c->established != was_established) {
            /* The handshake buffer went away with the handshake; nothing was left in it. */
            return PROVEN_OK;
        }
    }
    /* Keep what is left of an incomplete message. */
    proven_size_t rest = acc->len - at;
    for (proven_size_t i = 0; i < rest; ++i) acc->ptr[i] = acc->ptr[at + i];
    acc->len = rest;
    if (rest == 0 && c->established) buf_free(c, &c->post);
    return PROVEN_OK;
}

static proven_err_t record_process(proven_tls_conn_t *c) {
    proven_byte_t type = c->in.ptr[0];
    proven_byte_t *body = c->in.ptr + PROVEN_TLS_RECORD_HEADER;
    proven_size_t len = c->in.len - PROVEN_TLS_RECORD_HEADER;
    proven_mem_mut_t content = { .ptr = body, .size = len };
    if (type == PROVEN_TLS_CT_CCS) {
        /* The compatibility ChangeCipherSpec: one byte, 1, in the clear, during the handshake
         * only - and never before the first hello of this side's peer has been seen. */
        if (c->established || len != 1 || body[0] != 1) return tls_unexpected(c);
        if (c->is_server && c->state == ST_S_WAIT_CH && !c->hs->retried) return tls_unexpected(c);
        /* One is expected. A peer that sends them without end is not making progress. */
        if (++c->ignored > TLS_MAX_IGNORED) return tls_unexpected(c);
        return PROVEN_OK;
    }
    if (c->read_keys.active) {
        if (type != PROVEN_TLS_CT_APPLICATION) return tls_unexpected(c);
        if (!proven_tls13_open(&c->read_keys, c->in.ptr, len, &type, &content)) return tls_fail(c, AL_BAD_RECORD_MAC, PROVEN_ERR_PROTOCOL);
    } else {
        if (type == PROVEN_TLS_CT_APPLICATION) return tls_unexpected(c);
        if (len > PROVEN_TLS_MAX_PLAINTEXT) return tls_fail(c, AL_RECORD_OVERFLOW, PROVEN_ERR_PROTOCOL);
    }
    switch (type) {
        case PROVEN_TLS_CT_HANDSHAKE:
            c->ignored = 0;
            return handshake_bytes(c, content.ptr, content.size);
        case PROVEN_TLS_CT_ALERT:
            if (content.size != 2) return tls_decode(c);
            if (content.ptr[1] == AL_CLOSE_NOTIFY) { c->close_received = true; return PROVEN_OK; }
            if (content.ptr[1] == AL_USER_CANCELED) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            /* Every other alert is fatal in TLS 1.3 whatever its level says. Nothing is sent back. */
            c->alert_received = content.ptr[1];
            c->dead = true;
            c->error = PROVEN_ERR_PROTOCOL;
            return c->error;
        case PROVEN_TLS_CT_APPLICATION:
            if (!c->established) return tls_unexpected(c);
            /* Empty records are allowed, and are how some peers hide traffic patterns; an
             * endless run of them is a peer keeping this side busy for nothing. */
            if (content.size == 0) return ++c->ignored > TLS_MAX_IGNORED ? tls_unexpected(c) : PROVEN_OK;
            c->ignored = 0;
            c->plain_off = (proven_size_t)(content.ptr - c->in.ptr);
            c->plain_len = c->plain_off + content.size;
            return PROVEN_OK;
        default:
            return tls_unexpected(c);
    }
}

/* ================= The public face ================= */

static proven_err_t conn_new(const proven_tls_config_t *config, bool server, proven_tls_conn_t **out) {
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(proven_tls_conn_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_tls_conn_t *c = (proven_tls_conn_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    m = a.alloc_fn(a.ctx, sizeof(tls_handshake_t), 16);
    if (m.err != PROVEN_OK) { a.free_fn(a.ctx, c); return m.err; }
    c->hs = (tls_handshake_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c->hs; ++i) ((proven_byte_t *)c->hs)[i] = 0;
    c->config = config;
    c->alloc = a;
    c->is_server = server;
    c->alert_sent = -1; c->alert_received = -1; c->alpn_index = -1;
    c->peer_record_limit = PROVEN_TLS_MAX_PLAINTEXT;
    *out = c;
    return PROVEN_OK;
}

static proven_err_t client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                  proven_mem_view_t hello, const proven_byte_t *x25519_private, proven_tls_conn_t **out) {
    if (!out || !config || (server_name.size > 0 && !server_name.ptr) || server_name.size > 253) return PROVEN_ERR_INVALID_ARG;
    if (server_name.size == 0 && config->verify != PROVEN_TLS_VERIFY_PIN_ONLY) return PROVEN_ERR_INVALID_ARG;
    if (config->verify == PROVEN_TLS_VERIFY_CHAIN && !config->anchors) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *c = NULL;
    proven_err_t e = conn_new(config, false, &c);
    if (e != PROVEN_OK) return e;
    tls_handshake_t *h = c->hs;
    c->session = session;
    if (server_name.size > 0) {
        proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, server_name.size, 1);
        if (m.err != PROVEN_OK) { proven_tls_conn_destroy(c); return m.err; }
        c->server_name = m.value.ptr;
    }
    for (proven_size_t i = 0; i < server_name.size; ++i) c->server_name[i] = server_name.ptr[i];
    c->server_name_len = server_name.size;
    if (c->server_name_len > 0 && c->server_name[c->server_name_len - 1] == '.') c->server_name_len--;
    proven_tls_config_random(config, h->random, 32);
    /* A session id of 32 random bytes makes the handshake look like a resumption of TLS 1.2
     * to boxes in the middle that would otherwise drop it. It means nothing else. */
    proven_tls_config_random(config, h->session_id, 32);
    h->session_id_len = 32;
    proven_tls_config_random(config, h->x25519_priv, 32);
    if (hello.size > 0) {
        if (hello.size > sizeof h->test_hello || hello.size < 39 + 32) { proven_tls_conn_destroy(c); return PROVEN_ERR_INVALID_ARG; }
        for (proven_size_t i = 0; i < hello.size; ++i) h->test_hello[i] = hello.ptr[i];
        h->test_hello_len = hello.size;
        for (int i = 0; i < 32; ++i) h->x25519_priv[i] = x25519_private[i];
    }
    c->state = ST_C_WAIT_SH;
    e = client_hello_send(c);
    if (e != PROVEN_OK) { proven_tls_conn_destroy(c); return e; }
    *out = c;
    return PROVEN_OK;
}

void proven_tls_test_peek_write(const proven_tls_conn_t *conn, proven_tls_keys_t *keys, proven_byte_t secret[48]) {
    *keys = conn->write_keys;
    for (int i = 0; i < 48; ++i) secret[i] = conn->write_secret[i];
}

proven_err_t proven_tls_client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                      proven_tls_session_t *session, proven_tls_conn_t **out) {
    return client_create(config, server_name, session, (proven_mem_view_t){ .ptr = NULL, .size = 0 }, NULL, out);
}

proven_err_t proven_tls_client_create_replay(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                             proven_mem_view_t hello, const proven_byte_t x25519_private[32], proven_tls_conn_t **out) {
    if (!hello.ptr || !x25519_private) return PROVEN_ERR_INVALID_ARG;
    return client_create(config, server_name, NULL, hello, x25519_private, out);
}

proven_err_t proven_tls_server_create(const proven_tls_config_t *config, proven_tls_conn_t **out) {
    if (!out || !config || config->chain_count == 0 || config->key_kind == PROVEN_TLS_KEY_NONE) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *c = NULL;
    proven_err_t e = conn_new(config, true, &c);
    if (e != PROVEN_OK) return e;
    c->state = ST_S_WAIT_CH;
    *out = c;
    return PROVEN_OK;
}

void proven_tls_conn_destroy(proven_tls_conn_t *c) {
    if (!c) return;
    proven_allocator_t a = c->alloc;
    hs_free(c);
    buf_free(c, &c->in); buf_free(c, &c->out); buf_free(c, &c->post); buf_free(c, &c->peer_cert);
    if (c->server_name) a.free_fn(a.ctx, c->server_name);
    tls_wipe(c, sizeof *c);
    a.free_fn(a.ctx, c);
}

proven_err_t proven_tls_feed(proven_tls_conn_t *c, proven_mem_view_t in, proven_size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!c || !consumed || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (c->dead) {
        if (!c->error_reported) { c->error_reported = true; return c->error; }
        return PROVEN_ERR_INVALID_STATE;
    }
    proven_size_t used = 0;
    while (!c->dead) {
        if (c->plain_len > c->plain_off) break;                   /* application data is waiting to be read */
        if (c->plain_len > 0) { c->plain_len = 0; c->plain_off = 0; c->in.len = 0; }
        if (c->close_received) { used = in.size; break; }         /* nothing after close_notify means anything */
        if (c->in.len < PROVEN_TLS_RECORD_HEADER) {
            proven_size_t want = PROVEN_TLS_RECORD_HEADER - c->in.len, take = in.size - used < want ? in.size - used : want;
            if (take == 0) break;
            if (!buf_append(c, &c->in, in.ptr + used, take)) { (void)tls_nomem(c); break; }
            used += take;
            if (c->in.len < PROVEN_TLS_RECORD_HEADER) break;
        }
        proven_byte_t type = c->in.ptr[0];
        proven_size_t len = ((proven_size_t)c->in.ptr[3] << 8) | c->in.ptr[4];
        /* The header is checked before a byte of the body is waited for. */
        if (type < PROVEN_TLS_CT_CCS || type > PROVEN_TLS_CT_APPLICATION || c->in.ptr[1] != 0x03) { (void)tls_unexpected(c); break; }
        if (len > PROVEN_TLS_MAX_CIPHERTEXT || (len == 0 && type != PROVEN_TLS_CT_APPLICATION)) { (void)tls_fail(c, AL_RECORD_OVERFLOW, PROVEN_ERR_PROTOCOL); break; }
        proven_size_t need = PROVEN_TLS_RECORD_HEADER + len - c->in.len;
        proven_size_t take = in.size - used < need ? in.size - used : need;
        if (take > 0 && !buf_append(c, &c->in, in.ptr + used, take)) { (void)tls_nomem(c); break; }
        used += take;
        if (c->in.len < PROVEN_TLS_RECORD_HEADER + len) break;
        (void)record_process(c);
        if (c->plain_len == 0) c->in.len = 0;
    }
    *consumed = used;
    if (c->in.len == 0 && c->plain_len == 0) buf_free(c, &c->in);
    if (c->dead) { c->error_reported = true; return c->error; }
    return PROVEN_OK;
}

proven_mem_view_t proven_tls_pending_output(proven_tls_conn_t *c) {
    if (!c || c->out.len <= c->out_off) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    return (proven_mem_view_t){ .ptr = c->out.ptr + c->out_off, .size = c->out.len - c->out_off };
}

void proven_tls_output_sent(proven_tls_conn_t *c, proven_size_t n) {
    if (!c) return;
    proven_size_t left = c->out.len - c->out_off;
    c->out_off += n < left ? n : left;
    if (c->out_off == c->out.len) { buf_free(c, &c->out); c->out_off = 0; }
}

bool proven_tls_is_established(const proven_tls_conn_t *c) { return c && c->established && !c->dead; }

proven_result_size_t proven_tls_read(proven_tls_conn_t *c, proven_mem_mut_t dest) {
    if (!c || (dest.size > 0 && !dest.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->plain_len > c->plain_off) {
        proven_size_t have = c->plain_len - c->plain_off, n = have < dest.size ? have : dest.size;
        for (proven_size_t i = 0; i < n; ++i) dest.ptr[i] = c->in.ptr[c->plain_off + i];
        c->plain_off += n;
        if (c->plain_off == c->plain_len) { c->plain_off = 0; c->plain_len = 0; buf_free(c, &c->in); }
        return (proven_result_size_t){ .err = PROVEN_OK, .value = n };
    }
    if (c->dead) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (c->close_received) return (proven_result_size_t){ .err = PROVEN_ERR_EOF, .value = 0 };
    return (proven_result_size_t){ .err = PROVEN_ERR_NEED_MORE, .value = 0 };
}

proven_result_size_t proven_tls_write(proven_tls_conn_t *c, proven_mem_view_t src) {
    if (!c || (src.size > 0 && !src.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->dead || !c->established || c->close_sent) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    proven_size_t done = 0;
    while (done < src.size) {
        proven_size_t unsent = c->out.len - c->out_off;
        if (unsent >= TLS_MAX_UNSENT) break;
        proven_size_t n = src.size - done < c->peer_record_limit ? src.size - done : c->peer_record_limit;
        /* Long before a key has sealed 2^24 full records it is replaced: far inside every
         * suite's limit on how much one key may protect. */
        if (c->write_keys.seq >= ((proven_u64)1 << 24) && !key_update_send(c, 0)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
        if (!emit(c, PROVEN_TLS_CT_APPLICATION, src.ptr + done, n, 0x03)) return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = done };
        done += n;
    }
    return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
}

proven_err_t proven_tls_close(proven_tls_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->dead) return PROVEN_ERR_INVALID_STATE;
    if (c->close_sent) return PROVEN_OK;
    proven_byte_t body[2] = { 1, AL_CLOSE_NOTIFY };
    c->close_sent = true;
    return emit(c, PROVEN_TLS_CT_ALERT, body, 2, 0x03) ? PROVEN_OK : PROVEN_ERR_NOMEM;
}

proven_err_t proven_tls_key_update(proven_tls_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->dead || !c->established || c->close_sent) return PROVEN_ERR_INVALID_STATE;
    return key_update_send(c, 0) ? PROVEN_OK : PROVEN_ERR_NOMEM;
}

proven_u16 proven_tls_cipher_suite(const proven_tls_conn_t *c) { return c && c->suite ? c->suite->id : 0; }

proven_u8str_view_t proven_tls_alpn(const proven_tls_conn_t *c) {
    if (!c || c->alpn_index < 0) return (proven_u8str_view_t){ .ptr = NULL, .size = 0 };
    return proven_tls_config_alpn(c->config, (proven_size_t)c->alpn_index);
}

bool proven_tls_resumed(const proven_tls_conn_t *c) { return c && c->resumed; }

bool proven_tls_peer_key_sha256(const proven_tls_conn_t *c, proven_byte_t out[32]) {
    if (!c || !out || !c->has_peer_key) return false;
    for (int i = 0; i < 32; ++i) out[i] = c->peer_key_hash[i];
    return true;
}

proven_mem_view_t proven_tls_peer_certificate(const proven_tls_conn_t *c) {
    if (!c) return (proven_mem_view_t){ .ptr = NULL, .size = 0 };
    return (proven_mem_view_t){ .ptr = c->peer_cert.ptr, .size = c->peer_cert.len };
}

proven_cert_fault_t proven_tls_peer_fault(const proven_tls_conn_t *c) { return c ? c->peer_fault : PROVEN_CERT_FAULT_NONE; }

proven_u8str_view_t proven_tls_server_name(const proven_tls_conn_t *c) {
    if (!c || !c->is_server || !c->hs) return (proven_u8str_view_t){ .ptr = NULL, .size = 0 };
    return (proven_u8str_view_t){ .ptr = c->hs->sni, .size = c->hs->sni_len };
}

int proven_tls_alert_received(const proven_tls_conn_t *c) { return c ? c->alert_received : -1; }
int proven_tls_alert_sent(const proven_tls_conn_t *c) { return c ? c->alert_sent : -1; }
