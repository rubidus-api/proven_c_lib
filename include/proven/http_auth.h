#ifndef PROVEN_HTTP_AUTH_H
#define PROVEN_HTTP_AUTH_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"

/**
 * @file http_auth.h
 * @brief HTTP authentication: the Basic and Digest schemes (RFC 7617, RFC 7616).
 *
 * Both functions here produce the VALUE of an `Authorization` header. Nothing is sent, nothing
 * is stored, and nothing is remembered: the caller decides when to answer a challenge and with
 * which credentials. Pure computation, available in freestanding builds.
 *
 * What each scheme is worth:
 *
 *   - **Basic** sends the password, reversibly encoded, with every request. Over an encrypted
 *     connection that is acceptable; over plain HTTP it is the password in the clear.
 *   - **Digest** never sends the password - it sends a hash that proves knowledge of it, bound
 *     to a nonce the server chose. That protects the password from an eavesdropper on plain
 *     HTTP. It protects nothing else: the request and the response are still readable and
 *     changeable in transit. With MD5 it is also only as strong as MD5; prefer SHA-256 when the
 *     server offers it.
 */

/**
 * @brief Write `Basic <base64 of user:password>`.
 * @return PROVEN_ERR_INVALID_ARG when `user` contains a colon (the format cannot express it)
 *         or either holds a control character; PROVEN_ERR_OUT_OF_BOUNDS when `out` is small.
 */
[[nodiscard]]
proven_err_t proven_http_basic_auth(proven_u8str_view_t user, proven_u8str_view_t password,
                                    proven_mem_mut_t out, proven_size_t *written);

typedef enum {
    PROVEN_HTTP_DIGEST_MD5 = 0,
    PROVEN_HTTP_DIGEST_MD5_SESS,
    PROVEN_HTTP_DIGEST_SHA256,
    PROVEN_HTTP_DIGEST_SHA256_SESS
} proven_http_digest_algorithm_t;

/** @brief A Digest challenge, as views into the header value it was parsed from. */
typedef struct {
    proven_u8str_view_t realm;
    proven_u8str_view_t nonce;
    proven_u8str_view_t opaque;         /**< empty when the server sent none */
    proven_http_digest_algorithm_t algorithm;
    bool has_opaque;
    bool qop_auth;                      /**< the server offers qop=auth */
    bool stale;                         /**< the nonce expired: retry with the same password */
} proven_http_digest_challenge_t;

/**
 * @brief Whether a `WWW-Authenticate` (or `Proxy-Authenticate`) value offers `scheme`.
 *
 * A value may list several challenges. This looks for one whose scheme is `scheme`, ignoring
 * ASCII case: `Basic`, `Digest`, `Bearer`.
 */
[[nodiscard]]
bool proven_http_auth_offers(proven_u8str_view_t header_value, proven_u8str_view_t scheme);

/**
 * @brief Find and parse a Digest challenge in a `WWW-Authenticate` value.
 *
 * When the value holds several Digest challenges - a server may offer the same realm under
 * SHA-256 and under MD5 - the strongest one this library implements is chosen.
 *
 * @return PROVEN_ERR_NOT_FOUND when the value has no Digest challenge;
 *         PROVEN_ERR_UNSUPPORTED when it has one, but only with an algorithm or a `qop` this
 *         library does not implement (SHA-512-256, `auth-int` alone), or with a backslash
 *         escape in a quoted value; PROVEN_ERR_INVALID_FORMAT when the challenge is malformed
 *         or lacks a realm or a nonce.
 */
[[nodiscard]]
proven_err_t proven_http_digest_challenge_parse(proven_u8str_view_t header_value, proven_http_digest_challenge_t *out);

/**
 * @brief Write the `Authorization` value that answers a Digest challenge.
 *
 * @param method the request method, as it will be sent: `GET`.
 * @param uri the request target, as it will be sent: `/dir/index.html`.
 * @param nonce_count how many requests, this one included, have used this nonce: 1 the first
 *        time. A server may refuse a count it has seen.
 * @param cnonce a fresh random string of yours - 16 random bytes in hex is the usual choice.
 *        It must hold only visible ASCII without `"` or `\`.
 *
 * @return PROVEN_ERR_INVALID_ARG when `user`, the realm or `cnonce` holds a `"`, a `\` or a
 *         control character (they are written inside quotes); PROVEN_ERR_OUT_OF_BOUNDS when
 *         `out` is too small.
 */
[[nodiscard]]
proven_err_t proven_http_digest_auth(const proven_http_digest_challenge_t *challenge,
                                     proven_u8str_view_t user, proven_u8str_view_t password,
                                     proven_u8str_view_t method, proven_u8str_view_t uri,
                                     proven_u32 nonce_count, proven_u8str_view_t cnonce,
                                     proven_mem_mut_t out, proven_size_t *written);

#endif /* PROVEN_HTTP_AUTH_H */
