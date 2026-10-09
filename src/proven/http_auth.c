#include "proven/http_auth.h"
#include "proven/encode.h"
#include "proven/hash.h"
#include "proven/hash_legacy.h"

/* RFC 7617 (Basic) and RFC 7616 (Digest), written from those documents. Pure computation. */

static bool auth_is_tchar(proven_byte_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

static proven_byte_t auth_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }

static bool auth_eq_nocase(proven_u8str_view_t a, const char *b) {
    proven_size_t n = 0;
    while (b[n] != '\0') n++;
    if (a.size != n) return false;
    for (proven_size_t i = 0; i < n; ++i) if (auth_lower(a.ptr[i]) != auth_lower((proven_byte_t)b[i])) return false;
    return true;
}

static bool auth_views_eq_nocase(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (auth_lower(a.ptr[i]) != auth_lower(b.ptr[i])) return false;
    return true;
}

static bool auth_has_control(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) if (s.ptr[i] < 0x20 || s.ptr[i] == 0x7f) return true;
    return false;
}

/* Safe to put between double quotes as it is. */
static bool auth_quotable(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) {
        proven_byte_t c = s.ptr[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}

proven_err_t proven_http_basic_auth(proven_u8str_view_t user, proven_u8str_view_t password,
                                    proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!written || (user.size > 0 && !user.ptr) || (password.size > 0 && !password.ptr) || (out.size > 0 && !out.ptr)) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (auth_has_control(user) || auth_has_control(password)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < user.size; ++i) if (user.ptr[i] == ':') return PROVEN_ERR_INVALID_ARG;

    /* "user:password" is encoded three bytes at a time from the two views, so no buffer for
     * the joined text is needed - and a password is not copied anywhere but the output. */
    proven_size_t plain = user.size + 1 + password.size;
    if (plain < user.size) return PROVEN_ERR_OVERFLOW;
    proven_size_t encoded = proven_base64_encoded_size(plain);
    if (encoded == PROVEN_SIZE_MAX || encoded > PROVEN_SIZE_MAX - 6) return PROVEN_ERR_OVERFLOW;
    if (out.size < 6 + encoded) return PROVEN_ERR_OUT_OF_BOUNDS;

    static const char prefix[] = "Basic ";
    for (proven_size_t i = 0; i < 6; ++i) out.ptr[i] = (proven_byte_t)prefix[i];
    proven_size_t w = 6;
    for (proven_size_t i = 0; i < plain; i += 3) {
        proven_byte_t group[3];
        proven_size_t n = 0;
        for (proven_size_t k = i; k < plain && n < 3; ++k) {
            group[n++] = k < user.size ? user.ptr[k] : (k == user.size ? (proven_byte_t)':' : password.ptr[k - user.size - 1]);
        }
        proven_size_t got = 0;
        proven_err_t e = proven_base64_encode((proven_mem_view_t){ .ptr = group, .size = n }, out.ptr + w, out.size - w, &got);
        if (e != PROVEN_OK) return e;
        w += got;
    }
    *written = w;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Reading challenges
// -----------------------------------------------------------------------------

typedef struct {
    proven_u8str_view_t s;
    proven_size_t pos;
} auth_cur_t;

static void auth_skip_ws(auth_cur_t *c) {
    while (c->pos < c->s.size && (c->s.ptr[c->pos] == ' ' || c->s.ptr[c->pos] == '\t')) c->pos++;
}

static proven_u8str_view_t auth_token(auth_cur_t *c) {
    proven_size_t start = c->pos;
    while (c->pos < c->s.size && auth_is_tchar(c->s.ptr[c->pos])) c->pos++;
    return (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
}

/*
 * One step through a challenge list. A header value looks like
 *
 *     Digest realm="a", nonce="b", Basic realm="c"
 *
 * - scheme names and parameter names are both tokens, told apart by what follows: a parameter
 * is followed by '='. Sets `*scheme` when a new challenge begins (param is then empty), or
 * `*name` and `*value` for a parameter of the current one. Returns false at the end, and sets
 * `*bad` when the text cannot be read.
 */
static bool auth_next(auth_cur_t *c, proven_u8str_view_t *scheme, proven_u8str_view_t *name,
                      proven_u8str_view_t *value, bool *escaped, bool *bad) {
    *scheme = (proven_u8str_view_t){0};
    *name = (proven_u8str_view_t){0};
    *value = (proven_u8str_view_t){0};
    *escaped = false;
    for (;;) {
        auth_skip_ws(c);
        if (c->pos < c->s.size && c->s.ptr[c->pos] == ',') { c->pos++; continue; }
        break;
    }
    if (c->pos >= c->s.size) return false;

    proven_u8str_view_t tok = auth_token(c);
    if (tok.size == 0) { *bad = true; return false; }
    proven_size_t after = c->pos;
    auth_skip_ws(c);
    if (c->pos < c->s.size && c->s.ptr[c->pos] == '=') {
        /* A parameter - unless this is a token68 credential ("Negotiate abc=="), which this
         * function does not need to understand: '=' followed by more '=' or the end. */
        c->pos++;
        auth_skip_ws(c);
        *name = tok;
        if (c->pos < c->s.size && c->s.ptr[c->pos] == '"') {
            c->pos++;
            proven_size_t start = c->pos;
            while (c->pos < c->s.size && c->s.ptr[c->pos] != '"') {
                if (c->s.ptr[c->pos] == '\\') {
                    *escaped = true;
                    c->pos++;
                    if (c->pos >= c->s.size) { *bad = true; return false; }
                }
                c->pos++;
            }
            if (c->pos >= c->s.size) { *bad = true; return false; }
            *value = (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
            c->pos++;
        } else {
            proven_size_t start = c->pos;
            while (c->pos < c->s.size && c->s.ptr[c->pos] != ',' && c->s.ptr[c->pos] != ' ' && c->s.ptr[c->pos] != '\t') c->pos++;
            *value = (proven_u8str_view_t){ .ptr = c->s.ptr + start, .size = c->pos - start };
        }
        return true;
    }
    /* A scheme: a token followed by whitespace and its parameters, or by a comma or the end. */
    c->pos = after;
    *scheme = tok;
    return true;
}

bool proven_http_auth_offers(proven_u8str_view_t header_value, proven_u8str_view_t scheme) {
    if (header_value.size > 0 && !header_value.ptr) return false;
    auth_cur_t c = { .s = header_value, .pos = 0 };
    proven_u8str_view_t sch, name, value;
    bool escaped = false, bad = false;
    while (auth_next(&c, &sch, &name, &value, &escaped, &bad)) {
        if (sch.size > 0 && auth_views_eq_nocase(sch, scheme)) return true;
    }
    return false;
}

/* Higher is stronger; -1 is an algorithm this library does not implement. */
static int auth_algorithm_rank(proven_u8str_view_t text, proven_http_digest_algorithm_t *out) {
    if (text.size == 0 || auth_eq_nocase(text, "MD5")) { *out = PROVEN_HTTP_DIGEST_MD5; return 1; }
    if (auth_eq_nocase(text, "MD5-sess")) { *out = PROVEN_HTTP_DIGEST_MD5_SESS; return 1; }
    if (auth_eq_nocase(text, "SHA-256")) { *out = PROVEN_HTTP_DIGEST_SHA256; return 2; }
    if (auth_eq_nocase(text, "SHA-256-sess")) { *out = PROVEN_HTTP_DIGEST_SHA256_SESS; return 2; }
    return -1;
}

proven_err_t proven_http_digest_challenge_parse(proven_u8str_view_t header_value, proven_http_digest_challenge_t *out) {
    if (!out || (header_value.size > 0 && !header_value.ptr)) return PROVEN_ERR_INVALID_ARG;
    auth_cur_t c = { .s = header_value, .pos = 0 };
    proven_u8str_view_t sch, name, value;
    bool escaped = false, bad = false;

    bool in_digest = false, any_digest = false, any_unsupported = false, have_best = false;
    int best_rank = 0;
    proven_http_digest_challenge_t best = {0};
    proven_http_digest_challenge_t cur = {0};
    proven_u8str_view_t cur_algorithm = {0}, cur_qop = {0};
    bool cur_has_qop = false, cur_escaped = false;

    for (;;) {
        bool more = auth_next(&c, &sch, &name, &value, &escaped, &bad);
        bool boundary = !more || sch.size > 0;
        if (boundary && in_digest) {
            /* The challenge that was being read is complete: judge it. */
            if (cur.realm.ptr == (void *)0 || cur.nonce.size == 0) return PROVEN_ERR_INVALID_FORMAT;
            proven_http_digest_algorithm_t alg = PROVEN_HTTP_DIGEST_MD5;
            int rank = auth_algorithm_rank(cur_algorithm, &alg);
            /* qop absent is the RFC 2069 form; present, it must offer "auth". */
            bool qop_ok = !cur_has_qop || cur.qop_auth;
            if (rank < 0 || !qop_ok || cur_escaped) {
                any_unsupported = true;
            } else if (!have_best || rank > best_rank) {
                cur.algorithm = alg;
                best = cur;
                best_rank = rank;
                have_best = true;
            }
            in_digest = false;
        }
        if (!more) break;
        if (sch.size > 0) {
            in_digest = auth_eq_nocase(sch, "Digest");
            if (in_digest) {
                any_digest = true;
                cur = (proven_http_digest_challenge_t){0};
                cur_algorithm = (proven_u8str_view_t){0};
                cur_qop = (proven_u8str_view_t){0};
                cur_has_qop = false;
                cur_escaped = false;
            }
            continue;
        }
        if (!in_digest) continue;
        if (auth_eq_nocase(name, "realm")) { cur.realm = value; if (!cur.realm.ptr) cur.realm.ptr = header_value.ptr; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "nonce")) { cur.nonce = value; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "opaque")) { cur.opaque = value; cur.has_opaque = true; cur_escaped |= escaped; }
        else if (auth_eq_nocase(name, "algorithm")) cur_algorithm = value;
        else if (auth_eq_nocase(name, "stale")) cur.stale = auth_eq_nocase(value, "true");
        else if (auth_eq_nocase(name, "qop")) {
            cur_qop = value;
            cur_has_qop = true;
            /* A list: "auth", "auth-int", or "auth,auth-int". */
            proven_size_t i = 0;
            while (i < cur_qop.size) {
                while (i < cur_qop.size && (cur_qop.ptr[i] == ',' || cur_qop.ptr[i] == ' ' || cur_qop.ptr[i] == '\t')) i++;
                proven_size_t start = i;
                while (i < cur_qop.size && cur_qop.ptr[i] != ',' && cur_qop.ptr[i] != ' ' && cur_qop.ptr[i] != '\t') i++;
                if (auth_eq_nocase((proven_u8str_view_t){ .ptr = cur_qop.ptr + start, .size = i - start }, "auth")) cur.qop_auth = true;
            }
        }
    }
    if (bad) return PROVEN_ERR_INVALID_FORMAT;
    if (have_best) { *out = best; return PROVEN_OK; }
    if (any_unsupported) return PROVEN_ERR_UNSUPPORTED;
    return any_digest ? PROVEN_ERR_INVALID_FORMAT : PROVEN_ERR_NOT_FOUND;
}

// -----------------------------------------------------------------------------
// Answering a Digest challenge
// -----------------------------------------------------------------------------

/* H(data) as lowercase hex, with MD5 or SHA-256; returns the number of hex characters. */
typedef struct {
    bool sha256;
    proven_md5_t md5;
    proven_sha256_t sha;
} auth_hash_t;

static void auth_hash_init(auth_hash_t *h, bool sha256) {
    h->sha256 = sha256;
    if (sha256) proven_sha256_init(&h->sha);
    else proven_md5_init(&h->md5);
}

static void auth_hash_put(auth_hash_t *h, proven_u8str_view_t s) {
    proven_mem_view_t m = { .ptr = s.ptr, .size = s.size };
    if (h->sha256) proven_sha256_update(&h->sha, m);
    else proven_md5_update(&h->md5, m);
}

static void auth_hash_colon(auth_hash_t *h) {
    static const proven_byte_t colon = ':';
    auth_hash_put(h, (proven_u8str_view_t){ .ptr = &colon, .size = 1 });
}

static proven_size_t auth_hash_hex(auth_hash_t *h, char out[65]) {
    if (h->sha256) {
        proven_byte_t d[PROVEN_SHA256_SIZE];
        proven_sha256_final(&h->sha, d);
        proven_sha256_to_hex(d, out);
        return 64;
    }
    proven_byte_t d[PROVEN_MD5_SIZE];
    proven_md5_final(&h->md5, d);
    char hex[33];
    proven_md5_to_hex(d, hex);
    for (int i = 0; i < 33; ++i) out[i] = hex[i];
    return 32;
}

typedef struct {
    proven_mem_mut_t out;
    proven_size_t len;
    bool full;
} auth_w_t;

static void auth_put(auth_w_t *w, proven_u8str_view_t s) {
    if (w->full || s.size > w->out.size - w->len) { w->full = true; return; }
    for (proven_size_t i = 0; i < s.size; ++i) w->out.ptr[w->len + i] = s.ptr[i];
    w->len += s.size;
}

static void auth_put_cstr(auth_w_t *w, const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    auth_put(w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n });
}

proven_err_t proven_http_digest_auth(const proven_http_digest_challenge_t *challenge,
                                     proven_u8str_view_t user, proven_u8str_view_t password,
                                     proven_u8str_view_t method, proven_u8str_view_t uri,
                                     proven_u32 nonce_count, proven_u8str_view_t cnonce,
                                     proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!challenge || !written || (out.size > 0 && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (!auth_quotable(user) || !auth_quotable(challenge->realm) || !auth_quotable(challenge->nonce) ||
        !auth_quotable(challenge->opaque) || !auth_quotable(uri) || !auth_quotable(cnonce) || auth_has_control(password)) {
        return PROVEN_ERR_INVALID_ARG;
    }
    if (method.size == 0 || uri.size == 0 || cnonce.size == 0) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < method.size; ++i) if (!auth_is_tchar(method.ptr[i])) return PROVEN_ERR_INVALID_ARG;

    bool sha256 = challenge->algorithm == PROVEN_HTTP_DIGEST_SHA256 || challenge->algorithm == PROVEN_HTTP_DIGEST_SHA256_SESS;
    bool sess = challenge->algorithm == PROVEN_HTTP_DIGEST_MD5_SESS || challenge->algorithm == PROVEN_HTTP_DIGEST_SHA256_SESS;

    char nc[9];
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) nc[i] = hexd[(nonce_count >> (28 - 4 * i)) & 0xfu];
    nc[8] = '\0';
    proven_u8str_view_t nc_view = { .ptr = (const proven_byte_t *)nc, .size = 8 };

    /* A1 = user:realm:password, and for the -sess variants H(that):nonce:cnonce. */
    auth_hash_t h;
    char ha1[65], ha2[65], response[65];
    auth_hash_init(&h, sha256);
    auth_hash_put(&h, user); auth_hash_colon(&h);
    auth_hash_put(&h, challenge->realm); auth_hash_colon(&h);
    auth_hash_put(&h, password);
    proven_size_t hl = auth_hash_hex(&h, ha1);
    if (sess) {
        auth_hash_init(&h, sha256);
        auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha1, .size = hl }); auth_hash_colon(&h);
        auth_hash_put(&h, challenge->nonce); auth_hash_colon(&h);
        auth_hash_put(&h, cnonce);
        hl = auth_hash_hex(&h, ha1);
    }
    /* A2 = method:uri (qop=auth). */
    auth_hash_init(&h, sha256);
    auth_hash_put(&h, method); auth_hash_colon(&h);
    auth_hash_put(&h, uri);
    (void)auth_hash_hex(&h, ha2);

    /* response = H(HA1:nonce:nc:cnonce:qop:HA2), or H(HA1:nonce:HA2) when the server offered
     * no qop at all (the RFC 2069 form). */
    auth_hash_init(&h, sha256);
    auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha1, .size = hl }); auth_hash_colon(&h);
    auth_hash_put(&h, challenge->nonce); auth_hash_colon(&h);
    if (challenge->qop_auth) {
        auth_hash_put(&h, nc_view); auth_hash_colon(&h);
        auth_hash_put(&h, cnonce); auth_hash_colon(&h);
        auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)"auth", .size = 4 }); auth_hash_colon(&h);
    }
    auth_hash_put(&h, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)ha2, .size = hl });
    (void)auth_hash_hex(&h, response);

    auth_w_t w = { .out = out, .len = 0, .full = false };
    auth_put_cstr(&w, "Digest username=\"");
    auth_put(&w, user);
    auth_put_cstr(&w, "\", realm=\"");
    auth_put(&w, challenge->realm);
    auth_put_cstr(&w, "\", nonce=\"");
    auth_put(&w, challenge->nonce);
    auth_put_cstr(&w, "\", uri=\"");
    auth_put(&w, uri);
    auth_put_cstr(&w, "\", algorithm=");
    auth_put_cstr(&w, challenge->algorithm == PROVEN_HTTP_DIGEST_MD5 ? "MD5" :
                      challenge->algorithm == PROVEN_HTTP_DIGEST_MD5_SESS ? "MD5-sess" :
                      challenge->algorithm == PROVEN_HTTP_DIGEST_SHA256 ? "SHA-256" : "SHA-256-sess");
    auth_put_cstr(&w, ", response=\"");
    auth_put(&w, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)response, .size = hl });
    auth_put_cstr(&w, "\"");
    if (challenge->qop_auth) {
        auth_put_cstr(&w, ", qop=auth, nc=");
        auth_put(&w, nc_view);
        auth_put_cstr(&w, ", cnonce=\"");
        auth_put(&w, cnonce);
        auth_put_cstr(&w, "\"");
    }
    if (challenge->has_opaque) {
        auth_put_cstr(&w, ", opaque=\"");
        auth_put(&w, challenge->opaque);
        auth_put_cstr(&w, "\"");
    }
    if (w.full) return PROVEN_ERR_OUT_OF_BOUNDS;
    *written = w.len;
    return PROVEN_OK;
}
