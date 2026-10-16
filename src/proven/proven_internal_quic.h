#ifndef PROVEN_INTERNAL_QUIC_H
#define PROVEN_INTERNAL_QUIC_H

/* QUIC version 1 below the transport (RFC 9000, RFC 9001): variable-length integers, packet
 * numbers, packet protection, the Retry integrity tag, transport parameters, and TLS 1.3 in
 * QUIC mode. Internal: nothing here is public API, and tests reach it by path. Left out of a
 * build compiled with PROVEN_NO_HTTP3. */

#include "proven_internal_tls.h"

#ifndef PROVEN_NO_HTTP3

#define PROVEN_QUIC_VERSION_1 ((proven_u32)0x00000001)
#define PROVEN_QUIC_MAX_CID 20                     /* bytes in a version 1 connection ID */
#define PROVEN_QUIC_VARINT_MAX ((((proven_u64)1) << 62) - 1)

/* Transport error codes (RFC 9000 section 20.1). A TLS alert travels as 0x0100 + alert. */
#define PROVEN_QUIC_NO_ERROR                  0x00
#define PROVEN_QUIC_INTERNAL_ERROR            0x01
#define PROVEN_QUIC_PROTOCOL_VIOLATION        0x0a
#define PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR 0x08
#define PROVEN_QUIC_CRYPTO_BUFFER_EXCEEDED    0x0d
#define PROVEN_QUIC_CRYPTO_ERROR              0x0100

/* The three encryption levels this library uses (no 0-RTT: RFC-0017 B-2). */
typedef enum { PROVEN_QUIC_LEVEL_INITIAL = 0, PROVEN_QUIC_LEVEL_HANDSHAKE = 1, PROVEN_QUIC_LEVEL_APPLICATION = 2 } proven_quic_level_t;
#define PROVEN_QUIC_LEVELS 3

/* ---- Variable-length integers (RFC 9000 section 16) ---- */

/* 1, 2, 4 or 8: the bytes `v` takes; 0 above PROVEN_QUIC_VARINT_MAX. */
proven_size_t proven_quic_varint_len(proven_u64 v);
/* Write `v` in its shortest form; returns the bytes written, 0 above the maximum. */
proven_size_t proven_quic_varint_put(proven_u64 v, proven_byte_t out[8]);

/* A reader over received bytes: every get checks what is left, and the first short read
 * clears `ok` for good. */
typedef struct { const proven_byte_t *p; proven_size_t n; bool ok; } proven_quic_rd_t;
proven_u64 proven_quic_rd_varint(proven_quic_rd_t *r);
proven_u64 proven_quic_rd_uint(proven_quic_rd_t *r, proven_size_t bytes);
/* The next `n` bytes as a reader of their own. */
proven_quic_rd_t proven_quic_rd_take(proven_quic_rd_t *r, proven_size_t n);

/* ---- Packet numbers (RFC 9000 section 17.1 and appendix A.3) ---- */

/* The full packet number from its `bits` low bits, given the largest one received so far in
 * the same number space (`largest_plus_one` 0: none yet). */
proven_u64 proven_quic_pn_decode(proven_u64 largest_plus_one, proven_u64 truncated, unsigned bits);

/* ---- Packet protection (RFC 9001 section 5) ---- */

typedef struct {
    bool chacha;
    proven_size_t key_len;                        /* 16 or 32 */
    proven_crypto_aes_gcm_t aead;                 /* AES: the context. ChaCha20: its key, in the first 32 bytes of aead.rk */
    proven_crypto_aes_gcm_t hp;                   /* the same for the header protection key */
    proven_byte_t iv[12];
} proven_quic_keys_t;

/* The Initial secrets from the client's first Destination Connection ID (RFC 9001 section
 * 5.2): 32 bytes each, for TLS_AES_128_GCM_SHA256. */
void proven_quic_initial_secrets(proven_mem_view_t dcid, proven_byte_t client[32], proven_byte_t server[32]);
/* The suite of the Initial packets. */
const proven_tls_suite_t *proven_quic_initial_suite(void);
/* Key, IV and header protection key from a secret of `suite` ("quic key", "quic iv", "quic hp"). */
void proven_quic_keys_set(proven_quic_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *secret);
/* The secret of the next key phase (RFC 9001 section 6.1, "quic ku"), in place. */
void proven_quic_secret_update(const proven_tls_suite_t *suite, proven_byte_t *secret);
/* Five bytes of header protection mask from a 16-byte sample. */
void proven_quic_hp_mask(const proven_quic_keys_t *keys, const proven_byte_t sample[16], proven_byte_t mask[5]);

/* Protect a packet in place. `packet` holds the header - its packet number field of `pn_len`
 * bytes last, at `header_len - pn_len`, and a Length field already counting the tag - then
 * `payload_len` bytes of plaintext, then room for 16 bytes of tag. The payload is sealed with
 * the header as associated data and the header protected. Returns the packet's length, or 0
 * when pn_len + payload_len < 4 (too short to sample: the caller pads). */
proven_size_t proven_quic_protect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t header_len, proven_size_t pn_len,
                                  proven_u64 pn, proven_size_t payload_len);

/* Remove the protection of one packet in place. `pn_offset` is where its packet number field
 * starts (after the Length field of a long header, after the connection ID of a short one),
 * `packet_len` the packet's whole length, `largest_plus_one` as for proven_quic_pn_decode. On
 * success the first byte and the packet number are unprotected, `*pn` is the full number,
 * `*header_len` where the payload starts, and `*payload` views the plaintext. False when the
 * packet is too short or does not decrypt; its bytes are then not to be used. */
[[nodiscard]] bool proven_quic_unprotect(const proven_quic_keys_t *keys, proven_byte_t *packet, proven_size_t pn_offset, proven_size_t packet_len,
                                         proven_u64 largest_plus_one, proven_u64 *pn, proven_size_t *header_len, proven_mem_mut_t *payload);

/* The Retry integrity tag (RFC 9001 section 5.8): over the Original Destination Connection ID
 * and the Retry packet without its tag. */
void proven_quic_retry_tag(proven_mem_view_t odcid, proven_mem_view_t retry, proven_byte_t tag[16]);
/* True when a whole Retry packet's last 16 bytes are its tag. */
[[nodiscard]] bool proven_quic_retry_check(proven_mem_view_t odcid, proven_mem_view_t retry_packet);

/* ---- Transport parameters (RFC 9000 section 18) ---- */

typedef struct {
    proven_byte_t len;
    proven_byte_t id[PROVEN_QUIC_MAX_CID];
} proven_quic_cid_t;

typedef struct {
    proven_byte_t ipv4[4];
    proven_u16 ipv4_port;
    proven_byte_t ipv6[16];
    proven_u16 ipv6_port;
    proven_quic_cid_t cid;
    proven_byte_t reset_token[16];
} proven_quic_preferred_address_t;

typedef struct {
    proven_u64 max_idle_timeout;                  /* milliseconds; 0: none */
    proven_u64 max_udp_payload_size;              /* 65527 by default */
    proven_u64 initial_max_data;
    proven_u64 initial_max_stream_data_bidi_local;
    proven_u64 initial_max_stream_data_bidi_remote;
    proven_u64 initial_max_stream_data_uni;
    proven_u64 initial_max_streams_bidi;
    proven_u64 initial_max_streams_uni;
    proven_u64 ack_delay_exponent;                /* 3 by default */
    proven_u64 max_ack_delay;                     /* milliseconds, 25 by default */
    proven_u64 active_connection_id_limit;        /* 2 by default */
    bool disable_active_migration;
    bool has_original_dcid, has_initial_scid, has_retry_scid, has_reset_token, has_preferred_address;
    proven_quic_cid_t original_dcid;              /* server only */
    proven_quic_cid_t initial_scid;
    proven_quic_cid_t retry_scid;                 /* server only */
    proven_byte_t reset_token[16];                /* server only */
    proven_quic_preferred_address_t preferred_address;  /* server only; never sent by this library */
} proven_quic_tp_t;

/* Every field at the default RFC 9000 gives it when absent; nothing present. */
void proven_quic_tp_defaults(proven_quic_tp_t *tp);
/* Encode the parameters a `server` (or client) sends: each one that differs from its default,
 * and the connection IDs and token that are present. Returns the bytes written, or 0 when
 * `out` is too small or a value is out of its range. Never sends preferred_address. */
proven_size_t proven_quic_tp_encode(const proven_quic_tp_t *tp, bool server, proven_mem_mut_t out);
/* Decode what the peer sent; `from_server` says which side that is. Returns 0, or
 * PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR for a malformed list, a value out of range, a
 * parameter repeated, one a client may not send, or one that must be there and is not
 * (initial_source_connection_id from either side, original_destination_connection_id from a
 * server). Parameters this library does not know are skipped. */
proven_u64 proven_quic_tp_decode(proven_mem_view_t in, bool from_server, proven_quic_tp_t *tp);

/* ---- TLS 1.3 in QUIC mode (RFC 9001 sections 4 and 8): tls13.c ----
 *
 * The connection is an ordinary proven_tls_conn_t; proven_tls_conn_destroy frees it, and
 * proven_tls_is_established, _alpn, _resumed, _peer_key_sha256 and the like answer as over
 * TCP. What differs: handshake bytes come in and go out per encryption level with no records
 * around them; each level's traffic secrets are handed out instead of being used here; the
 * transport parameters travel in their extension; TLS 1.3 only, no session id, no
 * ChangeCipherSpec, no KeyUpdate, a protocol must be agreed. proven_tls_feed, _pending_output,
 * _read, _write and _close refuse a QUIC-mode connection with PROVEN_ERR_INVALID_STATE, and
 * proven_tls_key_update with PROVEN_ERR_UNSUPPORTED. */

/* A client or server in QUIC mode. `transport_params` is what this side sends, encoded
 * (proven_quic_tp_encode), at most 1,024 bytes. PROVEN_ERR_INVALID_ARG when the configuration
 * cannot speak TLS 1.3 or names no protocol (ALPN), besides what proven_tls_client_create and
 * proven_tls_server_create refuse. The client's first flight is pending at once. */
proven_err_t proven_tls_quic_client_create_(const proven_tls_config_t *config, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                            proven_mem_view_t transport_params, proven_tls_conn_t **out);
proven_err_t proven_tls_quic_server_create_(const proven_tls_config_t *config, proven_mem_view_t transport_params, proven_tls_conn_t **out);

/* Handshake bytes the peer sent at `level`, in order and each once (the QUIC layer puts CRYPTO
 * frames in order). Bytes at a level other than the one being read end the connection with
 * 0x010a. On a failure the connection is dead and proven_tls_quic_error_ says why. */
proven_err_t proven_tls_quic_feed_(proven_tls_conn_t *c, proven_quic_level_t level, proven_mem_view_t in);
/* Handshake bytes waiting to go out at `level`, and how many of them the QUIC layer has taken. */
proven_mem_view_t proven_tls_quic_pending_(const proven_tls_conn_t *c, proven_quic_level_t level);
void proven_tls_quic_sent_(proven_tls_conn_t *c, proven_quic_level_t level, proven_size_t n);
/* The level being read and the level being written: each moves from Initial to Handshake to
 * Application as the handshake does, and the secret of a level is ready when it is reached. */
proven_quic_level_t proven_tls_quic_read_level_(const proven_tls_conn_t *c);
proven_quic_level_t proven_tls_quic_write_level_(const proven_tls_conn_t *c);
/* The traffic secret of `level` (Handshake or Application) for reading or for writing: its
 * length, the suite's hash length, or 0 while there is none. SECRET: out. */
proven_size_t proven_tls_quic_secret_(const proven_tls_conn_t *c, proven_quic_level_t level, bool write, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]);
/* Wipe a level's secrets once the QUIC layer has its keys (and has discarded the level's). */
void proven_tls_quic_secret_drop_(proven_tls_conn_t *c, proven_quic_level_t level);
/* The suite agreed (NULL before the ServerHello), and the peer's transport parameters (NULL
 * until they have arrived and decoded). */
const proven_tls_suite_t *proven_tls_quic_suite_(const proven_tls_conn_t *c);
const proven_quic_tp_t *proven_tls_quic_peer_params_(const proven_tls_conn_t *c);
/* Why the connection failed, as a QUIC error code for CONNECTION_CLOSE: 0x0100 + the TLS alert
 * (RFC 9001 section 4.8), or a transport error such as TRANSPORT_PARAMETER_ERROR. 0: none. */
proven_u64 proven_tls_quic_error_(const proven_tls_conn_t *c);

#endif /* PROVEN_NO_HTTP3 */
#endif
