#ifndef PROVEN_INTERNAL_TLS_H
#define PROVEN_INTERNAL_TLS_H

/* The pieces of TLS 1.3 (RFC 8446) below the state machine: cipher suites, the transcript
 * hash, the key schedule and record protection. Internal; tests reach it by path. */

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/hash.h"
#include "proven/hmac.h"
#include "proven_internal_crypto.h"

#define PROVEN_TLS_AES_128_GCM_SHA256       ((proven_u16)0x1301)
#define PROVEN_TLS_AES_256_GCM_SHA384       ((proven_u16)0x1302)
#define PROVEN_TLS_CHACHA20_POLY1305_SHA256 ((proven_u16)0x1303)

/* Record content types. */
#define PROVEN_TLS_CT_CCS         20
#define PROVEN_TLS_CT_ALERT       21
#define PROVEN_TLS_CT_HANDSHAKE   22
#define PROVEN_TLS_CT_APPLICATION 23

#define PROVEN_TLS_MAX_PLAINTEXT  16384                    /* 2^14, RFC 8446 section 5.1 */
#define PROVEN_TLS_RECORD_HEADER  5
#define PROVEN_TLS_MAX_EXPANSION  (1 + 16)                 /* the inner content type and the tag */
#define PROVEN_TLS_MAX_CIPHERTEXT (PROVEN_TLS_MAX_PLAINTEXT + 256)

typedef struct {
    proven_u16 id;
    proven_hmac_hash_t hash;
    proven_size_t hash_len;
    proven_size_t key_len;
    bool chacha;
} proven_tls_suite_t;

/* NULL for a suite this library does not implement. */
const proven_tls_suite_t *proven_tls_suite_find(proven_u16 id);

/* ---- The transcript: a running hash of the handshake messages ---- */

typedef struct {
    proven_hmac_hash_t hash;
    union { proven_sha256_t s256; proven_sha512_t s512; } ctx;
} proven_tls_transcript_t;

void proven_tls_transcript_init(proven_tls_transcript_t *t, proven_hmac_hash_t hash);
void proven_tls_transcript_update(proven_tls_transcript_t *t, proven_mem_view_t data);
/* The hash of everything so far; the transcript goes on. Returns the hash length. */
proven_size_t proven_tls_transcript_hash(const proven_tls_transcript_t *t, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]);
/* After a HelloRetryRequest the first ClientHello is replaced by a message that carries its
 * hash (RFC 8446 section 4.4.1). */
void proven_tls_transcript_retry(proven_tls_transcript_t *t);

/* ---- The key schedule (RFC 8446 section 7.1). Every secret is hash_len bytes. ---- */

/* HKDF-Expand-Label(secret, label, context, out_len); `label` without the "tls13 " prefix. */
void proven_tls13_expand_label(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                               proven_mem_view_t context, proven_byte_t *out, proven_size_t out_len);
/* Derive-Secret(secret, label, messages), given the transcript hash of the messages. */
void proven_tls13_derive_secret(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                                const proven_byte_t *transcript_hash, proven_byte_t *out);
/* HKDF-Extract. A null salt or a null ikm stands for hash_len zero bytes. */
void proven_tls13_extract(proven_hmac_hash_t hash, const proven_byte_t *salt, proven_mem_view_t ikm, proven_byte_t *out);
/* The Finished verify_data: HMAC(finished_key(base_secret), transcript_hash). */
void proven_tls13_finished(proven_hmac_hash_t hash, const proven_byte_t *base_secret, const proven_byte_t *transcript_hash,
                           proven_byte_t *out);

/* ---- Record protection (RFC 8446 section 5.2) ---- */

typedef struct {
    bool active;                                  /* false: records travel in the clear */
    bool chacha;
    proven_crypto_aes_gcm_t aes;                  /* AES: the context. ChaCha20: its key, in the first 32 bytes of aes.rk */
    proven_byte_t iv[12];
    proven_u64 seq;
} proven_tls_keys_t;

/* Install the key and IV derived from a traffic secret, and start the sequence at zero. */
void proven_tls13_set_keys(proven_tls_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *traffic_secret);

/* One protected record: header, the content with its type appended, and the tag, into `out`,
 * which must hold PROVEN_TLS_RECORD_HEADER + content.size + PROVEN_TLS_MAX_EXPANSION bytes.
 * Returns the number of bytes written. `content` and `out` must not overlap. */
proven_size_t proven_tls13_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out);

/* Open one protected record in place: `record` is the header and `body_len` bytes after it.
 * On success `*type` is the inner content type and `content` views the plaintext inside
 * `record`, padding removed. False when the tag does not verify or the record is malformed. */
[[nodiscard]] bool proven_tls13_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len,
                                     proven_byte_t *type, proven_mem_mut_t *content);

/* ---- TLS 1.2 (RFC 5246) below its state machine: tls12_keys.c ---- */

typedef struct {
    proven_u16 id;
    proven_hmac_hash_t hash;                      /* of the PRF and the handshake hash */
    proven_size_t key_len;
    bool chacha;
    bool rsa_auth;                                /* ECDHE_RSA: the server's key is RSA; otherwise ECDSA or Ed25519 */
} proven_tls12_suite_t;

/* NULL for a suite this library does not implement. */
const proven_tls12_suite_t *proven_tls12_suite_find(proven_u16 id);

#define PROVEN_TLS12_MASTER_SIZE 48
#define PROVEN_TLS12_VERIFY_SIZE 12

/* PRF(secret, label, seed) = P_hash(secret, label | seed), `out_len` bytes. */
void proven_tls12_prf(proven_hmac_hash_t hash, proven_mem_view_t secret, const char *label, proven_mem_view_t seed,
                      proven_byte_t *out, proven_size_t out_len);
/* The extended master secret (RFC 7627): from the premaster secret and the session hash -
 * the hash of the handshake messages through ClientKeyExchange. The only kind made here. */
void proven_tls12_master_secret(proven_hmac_hash_t hash, proven_mem_view_t premaster, proven_mem_view_t session_hash,
                                proven_byte_t master[PROVEN_TLS12_MASTER_SIZE]);
/* A Finished message's verify_data, from the hash of the handshake messages before it. */
void proven_tls12_finished(proven_hmac_hash_t hash, const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], bool server,
                           proven_mem_view_t handshake_hash, proven_byte_t out[PROVEN_TLS12_VERIFY_SIZE]);
/* Expand the master secret into the two directions' keys and start both sequences at zero. */
void proven_tls12_set_keys(proven_tls_keys_t *client_write, proven_tls_keys_t *server_write, const proven_tls12_suite_t *suite,
                           const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], const proven_byte_t client_random[32],
                           const proven_byte_t server_random[32]);
/* One protected record of the given type: header, the explicit nonce (AES-GCM: 8 bytes), the
 * content and the tag, into `out` - PROVEN_TLS_RECORD_HEADER + 8 + content.size + 16 bytes at
 * most. Returns the number written. `content` and `out` must not overlap. */
proven_size_t proven_tls12_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out);
/* Open one in place. The type is the header's, in the clear and authenticated. False when the
 * tag does not verify or the record is malformed. */
[[nodiscard]] bool proven_tls12_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len, proven_mem_mut_t *content);

/* ---- The configuration and what the state machine needs from it ---- */

#include "proven/tls.h"
#include <stdatomic.h>

#define PROVEN_TLS_MAX_CHAIN 8
#define PROVEN_TLS_TICKET_PSK_MAX 48

typedef enum { PROVEN_TLS_KEY_NONE = 0, PROVEN_TLS_KEY_P256, PROVEN_TLS_KEY_ED25519, PROVEN_TLS_KEY_RSA } proven_tls_key_kind_t;

/* An RSA key and its blinding pair. The pair changes with every signature - from inside
 * handshakes, on whichever thread signs - so a flag serialises that, as for the ticket keys.
 * It is held for one squaring of the pair (and, every 65,536 signatures, for making a new
 * one): longer than the ticket keys' few instructions, and still far shorter than the
 * signature that follows outside it. */
typedef struct {
    proven_crypto_rsa_key_t key;                  /* SECRET */
    atomic_flag lock;
    proven_crypto_rsa_blind_t blind;              /* SECRET */
    proven_u32 uses;
} proven_tls_rsa_t;

/* A server's ticket keys: the current one and the one before. They are replaced on a timer
 * from inside handshakes, on whichever thread gets there first; a flag serialises that. */
typedef struct {
    atomic_flag lock;
    proven_i64 rotated_at;                        /* when `cur` was made */
    proven_byte_t cur[32], prev[32];
    proven_u32 cur_id, prev_id;
    bool have_prev;
} proven_tls_ticket_keys_t;

struct proven_tls_config {
    proven_allocator_t alloc;
    const proven_cert_store_t *anchors;
    proven_tls_verify_t verify;
    proven_byte_t (*pins)[32];
    proven_size_t pin_count;
    proven_byte_t *chain_mem;                     /* the certificates, DER, back to back */
    proven_mem_view_t chain[PROVEN_TLS_MAX_CHAIN];
    proven_size_t chain_count;
    proven_tls_key_kind_t key_kind;
    proven_byte_t key[32];                        /* the P-256 scalar or the Ed25519 seed. SECRET */
    proven_byte_t key_public[32];                 /* Ed25519 only */
    proven_tls_rsa_t *rsa;                        /* an RSA key lives here instead: it is two kilobytes */
    proven_tls_client_auth_t client_auth;
    proven_byte_t *alpn_wire;                     /* the names, each behind its length byte */
    proven_size_t alpn_wire_len;
    proven_size_t alpn_count;
    proven_tls_random_fn random; void *random_ctx;
    proven_tls_now_fn now; void *now_ctx;
    bool no_resumption;
    bool keep_peer_certificate;
    proven_u32 ticket_lifetime_s;
    proven_size_t max_handshake_bytes;
    proven_u16 min_version, max_version;          /* never zero: the defaults are filled in */
    proven_tls_ticket_keys_t tickets;             /* the one part that changes: see the lock */
};

/* What a ticket holds, and what a client keeps of a session. */
typedef struct {
    proven_u16 suite;
    proven_size_t psk_len;
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_i64 issued_at;
    bool has_peer_key;
    proven_byte_t peer_key_hash[32];
} proven_tls_ticket_state_t;

#define PROVEN_TLS_TICKET_MAX 160
/* Seal a ticket (server). Returns its length, at most PROVEN_TLS_TICKET_MAX. */
proven_size_t proven_tls_ticket_seal(const proven_tls_config_t *config, const proven_tls_ticket_state_t *state,
                                     proven_byte_t out[PROVEN_TLS_TICKET_MAX]);
/* Open one a client offered. False for anything not sealed by a current key, or too old. */
[[nodiscard]] bool proven_tls_ticket_open(const proven_tls_config_t *config, proven_mem_view_t ticket, proven_tls_ticket_state_t *state);

/* The client's session record, laid out inside proven_tls_session_t's bytes. */
#define PROVEN_TLS_SESSION_TICKET_MAX 640
typedef struct {
    proven_u32 magic;                             /* 0 for an empty session */
    proven_u16 suite;
    proven_u16 psk_len;
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_i64 received_at;
    proven_u32 lifetime_s;
    proven_u32 age_add;
    proven_u16 name_len;
    proven_u16 ticket_len;
    proven_byte_t name[254];
    proven_byte_t has_peer_key;
    proven_byte_t peer_key_hash[32];              /* the server's key, as verified when the session was made */
    proven_byte_t ticket[PROVEN_TLS_SESSION_TICKET_MAX];
} proven_tls_session_data_t;
#define PROVEN_TLS_SESSION_MAGIC ((proven_u32)0x70547331)

void proven_tls_config_random(const proven_tls_config_t *config, proven_byte_t *out, proven_size_t len);
proven_i64 proven_tls_config_now(const proven_tls_config_t *config);
/* The index-th ALPN name of the config. */
proven_u8str_view_t proven_tls_config_alpn(const proven_tls_config_t *config, proven_size_t index);

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
/* For the HTTP server, whose loop must never wait on one connection. */
/* A server-side TLS transport that has not shaken hands yet: the handshake happens inside its
 * reads, as the bytes arrive. */
proven_err_t proven_tls_transport_server_lazy_(proven_transport_t plain, const proven_tls_config_t *config, proven_transport_t *out);
/* The transport underneath has moved. */
void proven_tls_transport_set_plain_(proven_transport_t tls, proven_transport_t plain);
/* True when input is held inside - bytes not yet fed, or plaintext not yet read - so that a
 * read could return data although the socket has nothing new. */
bool proven_tls_transport_buffered_(proven_transport_t tls);
/* close_notify if it goes out without waiting, then close and free. */
void proven_tls_transport_close_now_(proven_transport_t tls);
/* close_notify if it goes out without waiting; the transport stays open for reading. */
void proven_tls_transport_shutdown_now_(proven_transport_t tls);
#endif

/* ---- Issuing certificates: for proven_tls_self_signed, and for tests that need a chain ---- */

typedef struct {
    const char *subject;                          /* the subject's common name */
    const char *issuer;                           /* the issuer's; the same string for a self-signed certificate */
    proven_tls_key_kind_t subject_kind;
    const proven_byte_t *subject_key;             /* 32 bytes: an Ed25519 seed or a P-256 scalar */
    proven_tls_key_kind_t issuer_kind;
    const proven_byte_t *issuer_key;
    const proven_crypto_rsa_key_t *subject_rsa;   /* instead of subject_key, for PROVEN_TLS_KEY_RSA */
    const proven_crypto_rsa_key_t *issuer_rsa;
    bool is_ca;
    proven_u32 eku;                               /* PROVEN_CERT_EKU_SERVER_AUTH | PROVEN_CERT_EKU_CLIENT_AUTH; 0: none */
    const proven_u8str_view_t *names;             /* subjectAltName entries: DNS names and IP literals */
    proven_size_t name_count;
    proven_i64 not_before, not_after;
    proven_byte_t serial[16];
} proven_tls_issue_t;

/* One certificate, DER. PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small (1 KiB is plenty for a few names). */
proven_err_t proven_tls_issue_(const proven_tls_issue_t *what, proven_mem_mut_t out, proven_size_t *len);
/* A private key as PKCS #8 DER; with `sec1`, a P-256 key as an ECPrivateKey instead. At most 80 bytes. */
proven_size_t proven_tls_key_der_(proven_tls_key_kind_t kind, const proven_byte_t key[32], bool sec1, proven_byte_t out[80]);
/* An RSA private key as an RSAPrivateKey (PKCS #1), or with `pkcs8` inside a PrivateKeyInfo.
 * `d` is the private exponent, n_len bytes. Returns the length, or 0 when `out` is too small. */
proven_size_t proven_tls_rsa_key_der_(const proven_crypto_rsa_key_t *key, const proven_byte_t *d, bool pkcs8, proven_mem_mut_t out);
/* A signature of a SHA-256 digest with the config's RSA key, n_len bytes into `sig`:
 * RSASSA-PSS with a salt of 32 bytes, or - `pkcs1`, for TLS 1.2 peers that offer nothing
 * else - PKCS #1 v1.5. False when the config has no RSA key or the signature could not be made. */
[[nodiscard]] bool proven_tls_config_rsa_sign_(const proven_tls_config_t *config, bool pkcs1, proven_mem_view_t digest, proven_byte_t *sig, proven_size_t *len);
/* PEM: the label between the dashes, the bytes in Base64, 64 to a line. */
proven_err_t proven_tls_pem_write_(const char *label, proven_mem_view_t der, proven_mem_mut_t out, proven_size_t *len);
/* 4 or 16 when `text` is an IP literal (cert.c). */
proven_size_t proven_cert_host_as_ip_(proven_u8str_view_t text, proven_byte_t out[16]);

/* Test hooks. Not for any other use. */
void proven_crypto_rsa_test_min_bits(proven_size_t bits);
/* Put one suite first in both roles' order (0: the normal order), and make the server ignore
 * an X25519 key share so that it has to ask for P-256. */
void proven_tls_test_knobs(proven_u16 suite_first, bool server_refuses_x25519);
/* TLS 1.2 test knobs: bit 0, the server answers 1.2 whatever the client offers; bit 1, the
 * client omits the extended master secret; bit 2, the server omits it from its answer; bit 3,
 * the client adds the fallback signal of RFC 7507 to its suites. */
void proven_tls_test_knobs12(unsigned flags);
/* Send an empty handshake message of `type` under the current keys. */
bool proven_tls_test_send_handshake(proven_tls_conn_t *conn, proven_byte_t type);
/* A copy of a connection's sending keys and application secret, for a test that must forge the
 * next record a peer would send. */
void proven_tls_test_peek_write(const proven_tls_conn_t *conn, proven_tls_keys_t *keys, proven_byte_t secret[48]);
/* A client whose first message and X25519 key are given, to replay a published handshake. */
proven_err_t proven_tls_client_create_replay(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                             proven_mem_view_t hello, const proven_byte_t x25519_private[32], proven_tls_conn_t **out);

#endif
