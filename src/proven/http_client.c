#include "proven/http_client.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/url.h"
#include "proven/http_auth.h"
#include "proven/random.h"
#include "proven/time.h"
#include "proven_internal_http_coding.h"

/*
 * The HTTP/1.1 client driver. The message format is http.c's business and the sockets are
 * net.c's; what is here is the sequence between them: find or make a connection, write a
 * request, read a head, decide whether that head is the answer or a reason to send another
 * request, and hand the body out through a decoder.
 */

#define CL_DEFAULT_TIMEOUT_MS 30000u
#define CL_MAX_HEADERS 64
#define CL_URL_MAX 2048
#define CL_DRAIN_LIMIT 65536u         /* a body larger than this is not read just to keep a connection */
#define CL_MAX_ADDRS 8
#define CL_MAX_INTERIM 8                /* 1xx responses passed over before one is taken for an attack */

/* One connection: the socket, and the transport the exchange runs over (the socket itself, or
 * what tls_wrap built on it). `key` says where it leads, so that it is reused only for that. */
typedef struct cl_conn {
    proven_net_conn_t sock;
    proven_transport_t transport;
    bool wrapped;                      /* transport closes the socket itself */
    bool via_http_proxy;               /* requests on it use the absolute form */
    proven_allocator_t alloc;          /* set when an upgrade hands the connection to the caller */
    proven_u64 last_used;
    proven_size_t key_len;
    proven_byte_t key[300];            /* "scheme://host:port" */
} cl_conn_t;

struct proven_http_client {
    proven_http_client_config_t cfg;   /* string views point into `strings` */
    proven_byte_t *strings;
    proven_url_t proxy;
    bool has_proxy;
    bool proxy_socks;
    cl_conn_t **idle;
    proven_size_t idle_count;
    proven_u64 tick;
};

/* Everything one send needs, in one allocation. */
typedef struct {
    proven_http_client_t *client;
    cl_conn_t *conn;
    proven_byte_t *buf;                /* response head, then the body read window */
    proven_size_t buf_cap;
    proven_size_t buf_len;
    proven_size_t buf_pos;
    proven_byte_t *out;                /* the request head being built */
    proven_size_t out_cap;
    proven_byte_t url[CL_URL_MAX];
    proven_size_t url_len;
    proven_http_header_t headers[CL_MAX_HEADERS];
    proven_http_response_t res;
    proven_http_body_t body;
    bool body_done;
    bool keep_alive;
    bool reused;
    bool got_response_bytes;
    bool decoding;                     /* the body is decoded on its way to the caller */
    proven_mem_view_t pending;         /* decoding: encoded bytes of the window not decoded yet */
    proven_http_decoder_t_ dec;
} cl_call_t;

static proven_u8str_view_t cl_lit(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

static proven_byte_t cl_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }

static bool cl_eq_nocase(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (cl_lower(a.ptr[i]) != cl_lower(b.ptr[i])) return false;
    return true;
}

static bool cl_has_control(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) if (s.ptr[i] < 0x20 || s.ptr[i] == 0x7f) return true;
    return false;
}

static void *cl_alloc(proven_allocator_t a, proven_size_t size) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, size ? size : 1, 16);
    return proven_is_ok(m.err) ? m.value.ptr : (void *)0;
}

static void cl_free(proven_allocator_t a, void *p) {
    if (p) a.free_fn(a.ctx, p);
}

static proven_net_deadline_t cl_io_deadline(const proven_http_client_t *c) {
    return proven_net_deadline_in(c->cfg.io_timeout_ms);
}

// -----------------------------------------------------------------------------
// The client object
// -----------------------------------------------------------------------------

proven_err_t proven_http_client_create(const proven_http_client_config_t *config, proven_http_client_t **out) {
    if (out) *out = (void *)0;
    if (!config || !out || !proven_alloc_is_valid(config->alloc)) return PROVEN_ERR_INVALID_ARG;
    if (cl_has_control(config->username) || cl_has_control(config->password) || cl_has_control(config->user_agent)) return PROVEN_ERR_INVALID_ARG;

    proven_http_client_t *c = cl_alloc(config->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    *c = (proven_http_client_t){0};
    c->cfg = *config;
    if (c->cfg.connect_timeout_ms == 0) c->cfg.connect_timeout_ms = CL_DEFAULT_TIMEOUT_MS;
    if (c->cfg.io_timeout_ms == 0) c->cfg.io_timeout_ms = CL_DEFAULT_TIMEOUT_MS;
    if (c->cfg.max_head_bytes == 0) c->cfg.max_head_bytes = PROVEN_HTTP_DEFAULT_MAX_HEAD;

    /* The four strings are copied, so the caller's need not outlive this call. */
    proven_size_t total = config->proxy.size + config->username.size + config->password.size + config->user_agent.size;
    c->strings = cl_alloc(config->alloc, total);
    if (!c->strings) { cl_free(config->alloc, c); return PROVEN_ERR_NOMEM; }
    proven_size_t at = 0;
    proven_u8str_view_t *dst[4] = { &c->cfg.proxy, &c->cfg.username, &c->cfg.password, &c->cfg.user_agent };
    const proven_u8str_view_t src[4] = { config->proxy, config->username, config->password, config->user_agent };
    for (int i = 0; i < 4; ++i) {
        for (proven_size_t k = 0; k < src[i].size; ++k) c->strings[at + k] = src[i].ptr[k];
        *dst[i] = (proven_u8str_view_t){ .ptr = c->strings + at, .size = src[i].size };
        at += src[i].size;
    }

    if (c->cfg.proxy.size > 0) {
        bool ok = proven_url_parse(c->cfg.proxy, &c->proxy) == PROVEN_OK && c->proxy.host.size > 0;
        bool http = ok && proven_url_scheme_is(&c->proxy, cl_lit("http"));
        bool socks = ok && (proven_url_scheme_is(&c->proxy, cl_lit("socks5")) || proven_url_scheme_is(&c->proxy, cl_lit("socks5h")));
        if (!http && !socks) {
            cl_free(config->alloc, c->strings);
            cl_free(config->alloc, c);
            return PROVEN_ERR_INVALID_ARG;
        }
        c->has_proxy = true;
        c->proxy_socks = socks;
    }
    if (c->cfg.max_idle_connections > 0) {
        if (c->cfg.max_idle_connections > PROVEN_SIZE_MAX / sizeof(cl_conn_t *)) c->cfg.max_idle_connections = 64;
        c->idle = cl_alloc(config->alloc, c->cfg.max_idle_connections * sizeof(cl_conn_t *));
        if (!c->idle) {
            cl_free(config->alloc, c->strings);
            cl_free(config->alloc, c);
            return PROVEN_ERR_NOMEM;
        }
    }
    *out = c;
    return PROVEN_OK;
}

static void cl_conn_close(proven_http_client_t *c, cl_conn_t *conn) {
    if (!conn) return;
    if (conn->wrapped) (void)proven_transport_close(conn->transport);
    (void)proven_net_close(&conn->sock);
    cl_free(c->cfg.alloc, conn);
}

void proven_http_client_destroy(proven_http_client_t *client) {
    if (!client) return;
    for (proven_size_t i = 0; i < client->idle_count; ++i) cl_conn_close(client, client->idle[i]);
    proven_allocator_t a = client->cfg.alloc;
    cl_free(a, client->idle);
    cl_free(a, client->strings);
    cl_free(a, client);
}

// -----------------------------------------------------------------------------
// Connections
// -----------------------------------------------------------------------------

static cl_conn_t *cl_pool_take(proven_http_client_t *c, proven_u8str_view_t key) {
    for (proven_size_t i = 0; i < c->idle_count; ++i) {
        cl_conn_t *k = c->idle[i];
        if (k->key_len == key.size && proven_memcmp(k->key, key.ptr, key.size) == 0) {
            c->idle[i] = c->idle[--c->idle_count];
            return k;
        }
    }
    return (void *)0;
}

static void cl_pool_put(proven_http_client_t *c, cl_conn_t *conn) {
    if (c->cfg.max_idle_connections == 0) { cl_conn_close(c, conn); return; }
    if (c->idle_count == c->cfg.max_idle_connections) {
        proven_size_t oldest = 0;
        for (proven_size_t i = 1; i < c->idle_count; ++i) if (c->idle[i]->last_used < c->idle[oldest]->last_used) oldest = i;
        cl_conn_close(c, c->idle[oldest]);
        c->idle[oldest] = c->idle[--c->idle_count];
    }
    conn->last_used = ++c->tick;
    c->idle[c->idle_count++] = conn;
}

/* Read exactly `n` bytes; a peer that closes first is a reset. */
static proven_err_t cl_read_exact(proven_transport_t t, proven_byte_t *dst, proven_size_t n, proven_net_deadline_t until) {
    proven_size_t got = 0;
    while (got < n) {
        proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ .ptr = dst + got, .size = n - got }, until);
        if (r.err == PROVEN_ERR_EOF) return PROVEN_ERR_RESET;
        if (r.err != PROVEN_OK) return r.err;
        got += r.value;
    }
    return PROVEN_OK;
}

/* RFC 1928 and RFC 1929: ask a SOCKS5 proxy to connect to host:port. The name is sent as a
 * name, so it is the proxy that resolves it. */
static proven_err_t cl_socks5(proven_http_client_t *c, proven_transport_t t, proven_u8str_view_t host, proven_u16 port, proven_net_deadline_t until) {
    if (host.size == 0 || host.size > 255) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t m[600];
    proven_size_t n = 0;
    bool creds = c->proxy.has_userinfo;
    proven_u8str_view_t user = c->proxy.userinfo, pass = { .ptr = c->proxy.userinfo.ptr + c->proxy.userinfo.size, .size = 0 };
    for (proven_size_t i = 0; i < c->proxy.userinfo.size; ++i) {
        if (c->proxy.userinfo.ptr[i] == ':') {
            user = (proven_u8str_view_t){ .ptr = c->proxy.userinfo.ptr, .size = i };
            pass = (proven_u8str_view_t){ .ptr = c->proxy.userinfo.ptr + i + 1, .size = c->proxy.userinfo.size - i - 1 };
            break;
        }
    }
    if (user.size > 255 || pass.size > 255) return PROVEN_ERR_INVALID_ARG;

    m[n++] = 5;
    m[n++] = creds ? 2 : 1;
    m[n++] = 0;                                   /* no authentication */
    if (creds) m[n++] = 2;                        /* user name and password */
    proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
    if (w.err != PROVEN_OK) return w.err;
    proven_err_t e = cl_read_exact(t, m, 2, until);
    if (e != PROVEN_OK) return e;
    if (m[0] != 5) return PROVEN_ERR_INVALID_FORMAT;
    if (m[1] == 2 && creds) {
        n = 0;
        m[n++] = 1;
        m[n++] = (proven_byte_t)user.size;
        for (proven_size_t i = 0; i < user.size; ++i) m[n++] = user.ptr[i];
        m[n++] = (proven_byte_t)pass.size;
        for (proven_size_t i = 0; i < pass.size; ++i) m[n++] = pass.ptr[i];
        w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
        if (w.err != PROVEN_OK) return w.err;
        e = cl_read_exact(t, m, 2, until);
        if (e != PROVEN_OK) return e;
        if (m[1] != 0) return PROVEN_ERR_PERMISSION;
    } else if (m[1] != 0) {
        return PROVEN_ERR_PERMISSION;             /* 0xFF: none of the offered methods is acceptable */
    }

    n = 0;
    m[n++] = 5; m[n++] = 1; m[n++] = 0;           /* CONNECT */
    m[n++] = 3;                                   /* a domain name follows */
    m[n++] = (proven_byte_t)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) m[n++] = host.ptr[i];
    m[n++] = (proven_byte_t)(port >> 8);
    m[n++] = (proven_byte_t)(port & 0xff);
    w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = n }, until);
    if (w.err != PROVEN_OK) return w.err;

    e = cl_read_exact(t, m, 4, until);
    if (e != PROVEN_OK) return e;
    if (m[0] != 5) return PROVEN_ERR_INVALID_FORMAT;
    proven_byte_t rep = m[1], atyp = m[3];
    /* The bound address follows and is of no use here, but it must be read off the stream. */
    proven_size_t rest = 0;
    if (atyp == 1) rest = 4 + 2;
    else if (atyp == 4) rest = 16 + 2;
    else if (atyp == 3) {
        e = cl_read_exact(t, m, 1, until);
        if (e != PROVEN_OK) return e;
        rest = (proven_size_t)m[0] + 2;
    } else return PROVEN_ERR_INVALID_FORMAT;
    e = cl_read_exact(t, m, rest, until);
    if (e != PROVEN_OK) return e;
    switch (rep) {
        case 0: return PROVEN_OK;
        case 2: return PROVEN_ERR_PERMISSION;     /* not allowed by the rule set */
        case 3:
        case 4: return PROVEN_ERR_UNREACHABLE;
        case 5: return PROVEN_ERR_REFUSED;
        case 6: return PROVEN_ERR_TIMEOUT;
        default: return PROVEN_ERR_IO;
    }
}

/* "Proxy-Authorization: Basic ..." from the proxy URL's userinfo, appended to a head. */
static proven_err_t cl_proxy_auth(proven_http_client_t *c, proven_mem_mut_t out, proven_size_t *len) {
    if (!c->proxy.has_userinfo) return PROVEN_OK;
    proven_u8str_view_t ui = c->proxy.userinfo;
    proven_u8str_view_t user = ui, pass = { .ptr = ui.ptr + ui.size, .size = 0 };
    for (proven_size_t i = 0; i < ui.size; ++i) {
        if (ui.ptr[i] == ':') {
            user = (proven_u8str_view_t){ .ptr = ui.ptr, .size = i };
            pass = (proven_u8str_view_t){ .ptr = ui.ptr + i + 1, .size = ui.size - i - 1 };
            break;
        }
    }
    proven_byte_t value[400];
    proven_size_t n = 0;
    proven_err_t e = proven_http_basic_auth(user, pass, (proven_mem_mut_t){ .ptr = value, .size = sizeof value }, &n);
    if (e != PROVEN_OK) return e;
    return proven_http_write_header(out, len, cl_lit("Proxy-Authorization"), (proven_u8str_view_t){ .ptr = value, .size = n });
}

/* Ask an HTTP proxy for a tunnel to host:port (RFC 9110 section 9.3.6). */
static proven_err_t cl_http_tunnel(proven_http_client_t *c, proven_transport_t t, proven_u8str_view_t authority, proven_net_deadline_t until) {
    proven_byte_t m[1024];
    proven_mem_mut_t out = { .ptr = m, .size = sizeof m };
    proven_size_t len = 0;
    proven_err_t e = proven_http_write_request_line(out, &len, cl_lit("CONNECT"), authority);
    if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Host"), authority);
    if (e == PROVEN_OK) e = cl_proxy_auth(c, out, &len);
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e != PROVEN_OK) return e;
    proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = m, .size = len }, until);
    if (w.err != PROVEN_OK) return w.err;

    /* The reply is read a byte at a time: whatever follows its head already belongs to the
     * tunnel, and must not be taken out of it here. A CONNECT reply is a few dozen bytes. */
    proven_size_t have = 0;
    for (;;) {
        if (have == sizeof m) return PROVEN_ERR_OUT_OF_BOUNDS;
        e = cl_read_exact(t, m + have, 1, until);
        if (e != PROVEN_OK) return e;
        have++;
        proven_http_header_t h[16];
        proven_http_response_t res;
        proven_size_t head = 0;
        e = proven_http_parse_response((proven_mem_view_t){ .ptr = m, .size = have }, h, 16, sizeof m, &res, &head);
        if (e == PROVEN_ERR_NEED_MORE) continue;
        if (e != PROVEN_OK) return e;
        if (res.status >= 200 && res.status < 300) return PROVEN_OK;
        return res.status == 407 || res.status == 403 ? PROVEN_ERR_PERMISSION : PROVEN_ERR_REFUSED;
    }
}

/* Connect a socket to host:port, trying each address the name has until one answers. */
static proven_err_t cl_dial(proven_http_client_t *c, proven_u8str_view_t host, proven_u16 port, proven_net_deadline_t until, proven_net_conn_t *out) {
    proven_net_addr_t addrs[CL_MAX_ADDRS];
    proven_size_t count = 0;
    (void)c;
    proven_err_t e = proven_net_resolve(host, port, addrs, CL_MAX_ADDRS, &count);
    if (e != PROVEN_OK) return e;
    proven_err_t last = PROVEN_ERR_NOT_FOUND;
    for (proven_size_t i = 0; i < count; ++i) {
        last = proven_net_connect(addrs[i], until, out);
        if (last == PROVEN_OK) {
            (void)proven_net_conn_set_nodelay(out, true);
            return PROVEN_OK;
        }
    }
    return last;
}

/* Find an idle connection for this URL or make one. */
static proven_err_t cl_connect(proven_http_client_t *c, const proven_url_t *u, bool allow_reuse, cl_conn_t **out, bool *reused) {
    *out = (void *)0;
    *reused = false;
    bool https = proven_url_scheme_is(u, cl_lit("https"));
    proven_u16 port = proven_url_effective_port(u);

    /* The key: where the connection ends up, whatever it goes through. */
    proven_byte_t key[300];
    proven_size_t kl = 0;
    if (u->host.size > 255) return PROVEN_ERR_INVALID_FORMAT;
    const char *scheme = https ? "https://" : "http://";
    for (proven_size_t i = 0; scheme[i] != '\0'; ++i) key[kl++] = (proven_byte_t)scheme[i];
    for (proven_size_t i = 0; i < u->host.size; ++i) key[kl++] = cl_lower(u->host.ptr[i]);
    key[kl++] = ':';
    key[kl++] = (proven_byte_t)('0' + port / 10000 % 10); key[kl++] = (proven_byte_t)('0' + port / 1000 % 10);
    key[kl++] = (proven_byte_t)('0' + port / 100 % 10); key[kl++] = (proven_byte_t)('0' + port / 10 % 10);
    key[kl++] = (proven_byte_t)('0' + port % 10);

    if (allow_reuse) {
        cl_conn_t *k = cl_pool_take(c, (proven_u8str_view_t){ .ptr = key, .size = kl });
        if (k) { *out = k; *reused = true; return PROVEN_OK; }
    }

    cl_conn_t *conn = cl_alloc(c->cfg.alloc, sizeof *conn);
    if (!conn) return PROVEN_ERR_NOMEM;
    *conn = (cl_conn_t){0};
    for (proven_size_t i = 0; i < kl; ++i) conn->key[i] = key[i];
    conn->key_len = kl;

    proven_net_deadline_t until = proven_net_deadline_in(c->cfg.connect_timeout_ms);
    proven_err_t e;
    if (c->has_proxy) {
        proven_u16 pport = c->proxy.has_port ? c->proxy.port : (c->proxy_socks ? 1080 : 8080);
        e = cl_dial(c, c->proxy.host, pport, until, &conn->sock);
    } else {
        e = cl_dial(c, u->host, port, until, &conn->sock);
    }
    if (e != PROVEN_OK) { cl_free(c->cfg.alloc, conn); return e; }
    conn->transport = proven_net_conn_transport(&conn->sock);

    if (c->has_proxy) {
        if (c->proxy_socks) {
            e = cl_socks5(c, conn->transport, u->host, port, until);
        } else if (https) {
            /* "host:port", with an IPv6 literal back in its brackets. */
            proven_byte_t authority[300];
            proven_size_t al = 0;
            if (u->host_is_ipv6) authority[al++] = '[';
            for (proven_size_t i = 0; i < u->host.size; ++i) authority[al++] = u->host.ptr[i];
            if (u->host_is_ipv6) authority[al++] = ']';
            authority[al++] = ':';
            proven_byte_t digits[5];
            proven_size_t dn = 0;
            for (proven_u16 p = port; p > 0 || dn == 0; p = (proven_u16)(p / 10)) digits[dn++] = (proven_byte_t)('0' + p % 10);
            while (dn > 0) authority[al++] = digits[--dn];
            e = cl_http_tunnel(c, conn->transport, (proven_u8str_view_t){ .ptr = authority, .size = al }, until);
        } else {
            conn->via_http_proxy = true;
            e = PROVEN_OK;
        }
        if (e != PROVEN_OK) { cl_conn_close(c, conn); return e; }
    }
    if (https) {
        proven_transport_t secure = {0};
        e = c->cfg.tls_wrap(c->cfg.tls_ctx, conn->transport, u->host, until, &secure);
        if (e != PROVEN_OK || !proven_transport_is_valid(secure)) {
            cl_conn_close(c, conn);
            return e != PROVEN_OK ? e : PROVEN_ERR_INVALID_STATE;
        }
        conn->transport = secure;
        conn->wrapped = true;
    }
    *out = conn;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// One exchange
// -----------------------------------------------------------------------------

/* A connection that owns itself: what an upgrade hands out. */
static proven_result_size_t cl_owned_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    return proven_transport_read(((cl_conn_t *)ctx)->transport, dest, until);
}
static proven_result_size_t cl_owned_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    return proven_transport_write(((cl_conn_t *)ctx)->transport, src, until);
}
static proven_err_t cl_owned_shutdown(void *ctx) {
    return proven_transport_shutdown(((cl_conn_t *)ctx)->transport);
}
static proven_err_t cl_owned_close(void *ctx) {
    cl_conn_t *conn = ctx;
    proven_allocator_t a = conn->alloc;
    proven_err_t e = PROVEN_OK;
    if (conn->wrapped) e = proven_transport_close(conn->transport);
    proven_err_t e2 = proven_net_close(&conn->sock);
    cl_free(a, conn);
    return e != PROVEN_OK ? e : e2;
}

static bool cl_is_reserved_header(proven_u8str_view_t name) {
    return cl_eq_nocase(name, cl_lit("Host")) || cl_eq_nocase(name, cl_lit("Content-Length")) ||
           cl_eq_nocase(name, cl_lit("Transfer-Encoding")) || cl_eq_nocase(name, cl_lit("Connection"));
}

static bool cl_is_credential_header(proven_u8str_view_t name) {
    return cl_eq_nocase(name, cl_lit("Authorization")) || cl_eq_nocase(name, cl_lit("Cookie")) ||
           cl_eq_nocase(name, cl_lit("Proxy-Authorization"));
}

/* What varies between the attempts of one send. */
typedef struct {
    proven_u8str_view_t method;
    bool send_body;
    bool cross_origin;                 /* a redirect left the original host: drop the caller's credentials */
    proven_u8str_view_t authorization; /* the answer to a challenge, when there is one */
} cl_attempt_t;

/* Write the request and read the response head into `call`. */
static proven_err_t cl_exchange(cl_call_t *call, const proven_http_client_request_t *req, const proven_url_t *u, const cl_attempt_t *at) {
    proven_http_client_t *c = call->client;
    cl_conn_t *conn = call->conn;
    proven_mem_mut_t out = { .ptr = call->out, .size = call->out_cap };
    proven_size_t len = 0;
    bool https = proven_url_scheme_is(u, cl_lit("https"));
    bool streaming = at->send_body && proven_reader_is_valid(req->body_stream);

    /* The target: the path and query, or the whole URL when an HTTP proxy is to fetch it. */
    proven_byte_t target[CL_URL_MAX + 8];
    proven_size_t tl = 0;
    if (conn->via_http_proxy) {
        proven_size_t end = (proven_size_t)(u->path.ptr + u->path.size - call->url);
        if (u->has_query) end = (proven_size_t)(u->query.ptr + u->query.size - call->url);
        for (proven_size_t i = 0; i < end; ++i) target[tl++] = call->url[i];
        if (u->path.size == 0 && !u->has_query) target[tl++] = '/';
    } else {
        if (u->path.size == 0) target[tl++] = '/';
        for (proven_size_t i = 0; i < u->path.size; ++i) target[tl++] = u->path.ptr[i];
        if (u->has_query) {
            target[tl++] = '?';
            for (proven_size_t i = 0; i < u->query.size; ++i) target[tl++] = u->query.ptr[i];
        }
    }
    proven_err_t e = proven_http_write_request_line(out, &len, at->method, (proven_u8str_view_t){ .ptr = target, .size = tl });

    /* Host: the port is written only when it is not the scheme's own. */
    proven_byte_t host[300];
    proven_size_t hl = 0;
    if (u->host_is_ipv6) host[hl++] = '[';
    for (proven_size_t i = 0; i < u->host.size; ++i) host[hl++] = u->host.ptr[i];
    if (u->host_is_ipv6) host[hl++] = ']';
    if (u->has_port && u->port != proven_url_default_port(u->scheme)) {
        host[hl++] = ':';
        char digits[5];
        int dn = 0;
        proven_u16 p = u->port;
        do { digits[dn++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (dn > 0) host[hl++] = (proven_byte_t)digits[--dn];
    }
    if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Host"), (proven_u8str_view_t){ .ptr = host, .size = hl });
    if (e == PROVEN_OK && c->cfg.user_agent.size > 0) e = proven_http_write_header(out, &len, cl_lit("User-Agent"), c->cfg.user_agent);

    bool caller_cookie = false;
    for (proven_size_t i = 0; i < req->header_count && e == PROVEN_OK; ++i) {
        proven_u8str_view_t name = req->headers[i].name;
        if (at->cross_origin && cl_is_credential_header(name)) continue;
        if (at->authorization.size > 0 && cl_eq_nocase(name, cl_lit("Authorization"))) continue;
        if (!at->send_body && (cl_eq_nocase(name, cl_lit("Content-Type")))) continue;
        if (cl_eq_nocase(name, cl_lit("Cookie"))) caller_cookie = true;
        e = proven_http_write_header(out, &len, name, req->headers[i].value);
    }
    if (e == PROVEN_OK && at->authorization.size > 0) e = proven_http_write_header(out, &len, cl_lit("Authorization"), at->authorization);
    if (e == PROVEN_OK && conn->via_http_proxy) e = cl_proxy_auth(c, out, &len);

    if (e == PROVEN_OK && c->cfg.cookies && !caller_cookie) {
        /* The jar writes its value straight after "Cookie: "; nothing is written if it is empty. */
        static const char prefix[] = "Cookie: ";
        if (out.size - len > 10) {
            proven_size_t n = 0;
            proven_err_t ce = proven_http_cookie_jar_header(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), https, proven_time_now(),
                                                            (proven_mem_mut_t){ .ptr = out.ptr + len + 8, .size = out.size - len - 10 }, &n);
            if (ce == PROVEN_OK && n > 0) {
                for (int i = 0; i < 8; ++i) out.ptr[len + (proven_size_t)i] = (proven_byte_t)prefix[i];
                len += 8 + n;
                out.ptr[len++] = '\r';
                out.ptr[len++] = '\n';
            } else if (ce != PROVEN_OK) {
                e = ce;
            }
        } else {
            e = PROVEN_ERR_OUT_OF_BOUNDS;
        }
    }

    if (e == PROVEN_OK && c->cfg.decompress && req->upgrade.size == 0 &&
        !proven_http_header_find(req->headers, req->header_count, cl_lit("Accept-Encoding"), &(proven_u8str_view_t){0}) &&
        !proven_http_header_find(req->headers, req->header_count, cl_lit("Range"), &(proven_u8str_view_t){0})) {
        e = proven_http_write_header(out, &len, cl_lit("Accept-Encoding"), cl_lit("gzip"));
    }

    bool wants_body = at->send_body && (streaming || req->body.size > 0);
    bool body_method = cl_eq_nocase(at->method, cl_lit("POST")) || cl_eq_nocase(at->method, cl_lit("PUT")) || cl_eq_nocase(at->method, cl_lit("PATCH"));
    if (e == PROVEN_OK) {
        if (streaming) e = proven_http_write_header(out, &len, cl_lit("Transfer-Encoding"), cl_lit("chunked"));
        else if (wants_body || body_method) e = proven_http_write_header_u64(out, &len, cl_lit("Content-Length"), at->send_body ? req->body.size : 0);
    }
    if (e == PROVEN_OK && req->upgrade.size > 0) {
        /* Asking to change protocol: the two headers that say so, written here because
         * Connection is the client's to write. */
        e = proven_http_write_header(out, &len, cl_lit("Connection"), cl_lit("Upgrade"));
        if (e == PROVEN_OK) e = proven_http_write_header(out, &len, cl_lit("Upgrade"), req->upgrade);
    } else if (e == PROVEN_OK && c->cfg.max_idle_connections == 0) {
        e = proven_http_write_header(out, &len, cl_lit("Connection"), cl_lit("close"));
    }
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e != PROVEN_OK) return e;

    proven_transport_t t = conn->transport;
    proven_result_size_t w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = out.ptr, .size = len }, cl_io_deadline(c));
    if (w.err != PROVEN_OK) return w.err;
    if (wants_body && !streaming) {
        w = proven_transport_write_all(t, req->body, cl_io_deadline(c));
        if (w.err != PROVEN_OK) return w.err;
    } else if (streaming) {
        /* Each piece the reader gives becomes one chunk. The head buffer is free until the
         * response arrives, so it serves as the copy buffer. */
        for (;;) {
            proven_result_size_t r = proven_reader_read(req->body_stream, (proven_mem_mut_t){ .ptr = call->buf, .size = call->buf_cap });
            if (r.err == PROVEN_ERR_EOF) break;
            if (r.err != PROVEN_OK) return r.err;
            proven_byte_t frame[24];
            proven_size_t fl = 0;
            e = proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, r.value);
            if (e != PROVEN_OK) return e;
            w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = frame, .size = fl }, cl_io_deadline(c));
            if (w.err == PROVEN_OK) w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = call->buf, .size = r.value }, cl_io_deadline(c));
            if (w.err == PROVEN_OK) w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\r\n", .size = 2 }, cl_io_deadline(c));
            if (w.err != PROVEN_OK) return w.err;
        }
        w = proven_transport_write_all(t, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"0\r\n\r\n", .size = 5 }, cl_io_deadline(c));
        if (w.err != PROVEN_OK) return w.err;
    }

    /* The head. Interim responses (100 Continue, 103 Early Hints) are read and passed over;
     * 101 is a final answer. */
    call->buf_len = 0;
    call->buf_pos = 0;
    unsigned interim = 0;
    for (;;) {
        proven_size_t head = 0;
        e = proven_http_parse_response((proven_mem_view_t){ .ptr = call->buf, .size = call->buf_len }, call->headers, CL_MAX_HEADERS,
                                       c->cfg.max_head_bytes, &call->res, &head);
        if (e == PROVEN_OK) {
            if (call->res.status >= 100 && call->res.status < 200 && call->res.status != 101) {
                /* A server may send several; one that sends them without end is holding the
                 * caller, and each would restart the read timeout. */
                if (++interim > CL_MAX_INTERIM) return PROVEN_ERR_INVALID_FORMAT;
                proven_size_t rest = call->buf_len - head;
                for (proven_size_t i = 0; i < rest; ++i) call->buf[i] = call->buf[head + i];
                call->buf_len = rest;
                continue;
            }
            call->buf_pos = head;
            break;
        }
        if (e != PROVEN_ERR_NEED_MORE) return e;
        if (call->buf_len == call->buf_cap) return PROVEN_ERR_OUT_OF_BOUNDS;
        proven_result_size_t r = proven_transport_read(t, (proven_mem_mut_t){ .ptr = call->buf + call->buf_len, .size = call->buf_cap - call->buf_len },
                                                       cl_io_deadline(c));
        if (r.err == PROVEN_ERR_EOF) return PROVEN_ERR_RESET;
        if (r.err != PROVEN_OK) return r.err;
        call->got_response_bytes = true;
        call->buf_len += r.value;
    }

    if (c->cfg.cookies) {
        for (proven_size_t i = 0; i < call->res.header_count; ++i) {
            if (!cl_eq_nocase(call->headers[i].name, cl_lit("Set-Cookie"))) continue;
            /* A cookie the jar will not take is the server's problem, not this request's. */
            (void)proven_http_cookie_jar_store(c->cfg.cookies, u->host, u->path.size ? u->path : cl_lit("/"), https,
                                               call->headers[i].value, proven_time_now());
        }
    }

    proven_http_framing_t framing;
    e = proven_http_response_framing(&call->res, proven_http_method_from_text(at->method), &framing);
    if (e != PROVEN_OK) return e;
    e = proven_http_body_init(&call->body, framing, UINT64_MAX);
    if (e != PROVEN_OK) return e;
    call->body_done = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
    call->keep_alive = proven_http_response_keep_alive(&call->res) && framing.kind != PROVEN_HTTP_BODY_UNTIL_CLOSE &&
                       c->cfg.max_idle_connections > 0 && call->res.status != 101;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    proven_http_decoder_free_(&call->dec);
    call->pending = (proven_mem_view_t){0};
    call->decoding = c->cfg.decompress && !call->body_done &&
                     proven_http_decoder_wanted_(call->res.status, call->res.headers, call->res.header_count, &coding);
    if (call->decoding) proven_http_decoder_begin_(&call->dec, c->cfg.alloc, coding);
    return PROVEN_OK;
}

/* The next piece of the body, as a view into the read window of at most `max` bytes. The
 * window is refilled from the transport when it runs dry - so a view is good only until the
 * next call. PROVEN_ERR_EOF at the end of the body. */
static proven_err_t cl_next_payload(cl_call_t *call, proven_size_t max, proven_mem_view_t *payload) {
    if (call->body_done) return PROVEN_ERR_EOF;
    for (;;) {
        if (call->buf_pos < call->buf_len) {
            proven_size_t avail = call->buf_len - call->buf_pos;
            proven_size_t offer = avail < max ? avail : max;
            proven_size_t used = 0;
            bool done = false;
            proven_err_t e = proven_http_body_feed(&call->body, (proven_mem_view_t){ .ptr = call->buf + call->buf_pos, .size = offer }, &used, payload, &done);
            if (e != PROVEN_OK) return e;
            call->buf_pos += used;
            if (done) call->body_done = true;
            if (payload->size > 0) return PROVEN_OK;
            if (done) return PROVEN_ERR_EOF;
            continue;
        }
        call->buf_pos = 0;
        call->buf_len = 0;
        proven_result_size_t r = proven_transport_read(call->conn->transport, (proven_mem_mut_t){ .ptr = call->buf, .size = call->buf_cap },
                                                       cl_io_deadline(call->client));
        if (r.err == PROVEN_ERR_EOF) {
            /* The peer closed. For a body that runs until close that is its end; for any other
             * it is a body cut short, which must not pass for a complete one. */
            call->keep_alive = false;
            if (proven_http_body_end(&call->body) == PROVEN_OK) { call->body_done = true; return PROVEN_ERR_EOF; }
            return PROVEN_ERR_RESET;
        }
        if (r.err != PROVEN_OK) return r.err;
        call->buf_len = r.value;
    }
}

/* Body bytes into `dest`, as they came. The decoder of the framing is fed no more than `dest`
 * can take, so a payload always fits. */
static proven_result_size_t cl_read_body(cl_call_t *call, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (call->body_done) { res.err = PROVEN_ERR_EOF; return res; }
    if (dest.size == 0) return res;
    proven_mem_view_t payload = {0};
    res.err = cl_next_payload(call, dest.size, &payload);
    if (res.err != PROVEN_OK) return res;
    for (proven_size_t i = 0; i < payload.size; ++i) dest.ptr[i] = payload.ptr[i];
    res.value = payload.size;
    return res;
}

/* Body bytes into `dest`, decoded. The end is the end of the HTTP body, not of the compressed
 * stream inside it: the connection is reusable exactly when it would have been. */
static proven_result_size_t cl_read_decoded(cl_call_t *call, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (dest.size == 0) return res;
    for (;;) {
        /* Also with nothing pending: what the decoder still holds comes out first. */
        proven_size_t used = 0, made = 0;
        res.err = proven_http_decoder_step_(&call->dec, call->pending, &used, dest, &made);
        if (res.err != PROVEN_OK) break;
        call->pending.ptr += used;
        call->pending.size -= used;
        if (made > 0) { res.value = made; return res; }
        if (call->pending.size > 0) {
            if (used > 0) continue;
            res.err = PROVEN_ERR_INVALID_FORMAT;       /* input it will not take, and nothing to give */
            break;
        }
        res.err = cl_next_payload(call, call->buf_cap, &call->pending);
        if (res.err == PROVEN_ERR_EOF) {
            res.err = proven_http_decoder_end_(&call->dec);
            if (res.err == PROVEN_OK) { res.err = PROVEN_ERR_EOF; return res; }
        }
        if (res.err != PROVEN_OK) break;
    }
    if (res.err != PROVEN_ERR_TIMEOUT) call->keep_alive = false;
    return res;
}

/* Read away what is left of a body so the connection can carry the next request - up to a
 * limit: past it, closing is cheaper than reading. */
static void cl_drain(cl_call_t *call) {
    proven_byte_t sink[2048];
    proven_size_t total = 0;
    while (!call->body_done && total <= CL_DRAIN_LIMIT) {
        proven_result_size_t r = cl_read_body(call, (proven_mem_mut_t){ .ptr = sink, .size = sizeof sink });
        if (r.err != PROVEN_OK) break;
        total += r.value;
    }
    if (!call->body_done) call->keep_alive = false;
}

/* Give the connection back, or close it. */
static void cl_release_conn(cl_call_t *call) {
    if (!call->conn) return;
    if (call->body_done && call->keep_alive && call->buf_pos == call->buf_len) cl_pool_put(call->client, call->conn);
    else cl_conn_close(call->client, call->conn);
    call->conn = (void *)0;
}

static void cl_random_hex(proven_byte_t out[32]) {
    static const char digits[] = "0123456789abcdef";
    proven_byte_t raw[16] = {0};
    if (!proven_random_bytes(raw, sizeof raw)) {
        /* No entropy source: a cnonce only has to differ between requests, which the clock
         * provides; it is not a secret. */
        proven_u64 t = (proven_u64)proven_time_monotonic_now();
        for (int i = 0; i < 8; ++i) { raw[i] = (proven_byte_t)(t >> (8 * i)); raw[8 + i] = (proven_byte_t)(~t >> (8 * i)); }
    }
    for (int i = 0; i < 16; ++i) { out[i * 2] = (proven_byte_t)digits[raw[i] >> 4]; out[i * 2 + 1] = (proven_byte_t)digits[raw[i] & 0xf]; }
}

proven_err_t proven_http_client_send(proven_http_client_t *client, const proven_http_client_request_t *request,
                                     proven_http_client_response_t *response) {
    if (response) *response = (proven_http_client_response_t){0};
    if (!client || !request || !response) return PROVEN_ERR_INVALID_ARG;
    if (request->header_count > 0 && !request->headers) return PROVEN_ERR_INVALID_ARG;
    if (request->url.size == 0 || request->url.size > CL_URL_MAX) return PROVEN_ERR_INVALID_FORMAT;
    for (proven_size_t i = 0; i < request->header_count; ++i) {
        if (cl_is_reserved_header(request->headers[i].name)) return PROVEN_ERR_INVALID_ARG;
    }
    bool streaming = proven_reader_is_valid(request->body_stream);
    if (streaming && request->body.size > 0) return PROVEN_ERR_INVALID_ARG;

    proven_allocator_t a = client->cfg.alloc;
    proven_size_t cap = client->cfg.max_head_bytes;
    if (cap < 1024) cap = 1024;
    if (cap > PROVEN_SIZE_MAX / 4) return PROVEN_ERR_OVERFLOW;
    cl_call_t *call = cl_alloc(a, sizeof *call + 2 * cap);
    if (!call) return PROVEN_ERR_NOMEM;
    *call = (cl_call_t){0};
    call->client = client;
    call->buf = (proven_byte_t *)(call + 1);
    call->buf_cap = cap;
    call->out = call->buf + cap;
    call->out_cap = cap;
    for (proven_size_t i = 0; i < request->url.size; ++i) call->url[i] = request->url.ptr[i];
    call->url_len = request->url.size;

    cl_attempt_t at = { .method = request->method.size ? request->method : cl_lit("GET"), .send_body = true, .cross_origin = false };
    proven_byte_t auth_value[1024];
    proven_byte_t origin_host[256];
    proven_size_t origin_host_len = 0;
    proven_u16 origin_port = 0;
    bool origin_https = false;
    bool auth_tried = false, stale_retried = false;
    proven_u32 redirects = 0;
    proven_err_t e = PROVEN_OK;

    for (;;) {
        proven_url_t u;
        e = proven_url_parse((proven_u8str_view_t){ .ptr = call->url, .size = call->url_len }, &u);
        if (e != PROVEN_OK || u.host.size == 0 || u.host.size > 255) { e = PROVEN_ERR_INVALID_FORMAT; break; }
        bool https = proven_url_scheme_is(&u, cl_lit("https"));
        if (!https && !proven_url_scheme_is(&u, cl_lit("http"))) { e = PROVEN_ERR_UNSUPPORTED; break; }
        if (https && !client->cfg.tls_wrap) { e = PROVEN_ERR_UNSUPPORTED; break; }
        if (redirects == 0 && origin_host_len == 0) {
            for (proven_size_t i = 0; i < u.host.size; ++i) origin_host[i] = cl_lower(u.host.ptr[i]);
            origin_host_len = u.host.size;
            origin_port = proven_url_effective_port(&u);
            origin_https = https;
        }

        /* A request is replayed on a fresh connection when a reused one turns out to be dead
         * before a single byte of the answer arrived - the server closed it while it sat
         * idle - and the body can be sent again. */
        bool replayable = !(at.send_body && streaming);
        bool allow_reuse = true;
        for (int attempt = 0; attempt < 2; ++attempt) {
            e = cl_connect(client, &u, allow_reuse, &call->conn, &call->reused);
            if (e != PROVEN_OK) break;
            call->got_response_bytes = false;
            e = cl_exchange(call, request, &u, &at);
            if (e == PROVEN_OK) break;
            bool dead_idle = call->reused && !call->got_response_bytes && replayable &&
                             (e == PROVEN_ERR_RESET || e == PROVEN_ERR_EOF || e == PROVEN_ERR_TIMEOUT);
            cl_conn_close(client, call->conn);
            call->conn = (void *)0;
            if (!dead_idle) break;
            allow_reuse = false;
        }
        if (e != PROVEN_OK) break;

        proven_u16 status = call->res.status;

        /* An authentication challenge, answered once. */
        /* Credentials are for the host they were configured for: after a redirect to another
         * origin a challenge is not answered. */
        if (status == 401 && client->cfg.username.size > 0 && replayable && !at.cross_origin && (!auth_tried || !stale_retried)) {
            proven_u8str_view_t challenge;
            proven_size_t n = 0;
            bool answered = false;
            for (proven_size_t i = 0; i < call->res.header_count && !answered; ++i) {
                if (!cl_eq_nocase(call->headers[i].name, cl_lit("WWW-Authenticate"))) continue;
                challenge = call->headers[i].value;
                proven_http_digest_challenge_t dc;
                if (proven_http_digest_challenge_parse(challenge, &dc) == PROVEN_OK) {
                    if (auth_tried && !dc.stale) continue;        /* the password was wrong: do not ask again */
                    if (auth_tried) stale_retried = true;
                    proven_byte_t cnonce[32];
                    cl_random_hex(cnonce);
                    /* The uri in the answer is the target as it was sent. */
                    proven_byte_t uri[CL_URL_MAX + 8];
                    proven_size_t ul = 0;
                    if (u.path.size == 0) uri[ul++] = '/';
                    for (proven_size_t k = 0; k < u.path.size; ++k) uri[ul++] = u.path.ptr[k];
                    if (u.has_query) { uri[ul++] = '?'; for (proven_size_t k = 0; k < u.query.size; ++k) uri[ul++] = u.query.ptr[k]; }
                    if (proven_http_digest_auth(&dc, client->cfg.username, client->cfg.password, at.method,
                                                (proven_u8str_view_t){ .ptr = uri, .size = ul }, 1,
                                                (proven_u8str_view_t){ .ptr = cnonce, .size = sizeof cnonce },
                                                (proven_mem_mut_t){ .ptr = auth_value, .size = sizeof auth_value }, &n) == PROVEN_OK) answered = true;
                }
            }
            for (proven_size_t i = 0; i < call->res.header_count && !answered && !auth_tried; ++i) {
                if (!cl_eq_nocase(call->headers[i].name, cl_lit("WWW-Authenticate"))) continue;
                if (!proven_http_auth_offers(call->headers[i].value, cl_lit("Basic"))) continue;
                if (proven_http_basic_auth(client->cfg.username, client->cfg.password,
                                           (proven_mem_mut_t){ .ptr = auth_value, .size = sizeof auth_value }, &n) == PROVEN_OK) answered = true;
            }
            if (answered) {
                auth_tried = true;
                at.authorization = (proven_u8str_view_t){ .ptr = auth_value, .size = n };
                cl_drain(call);
                cl_release_conn(call);
                continue;
            }
        }

        /* A redirect. */
        bool is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        proven_u8str_view_t location;
        if (is_redirect && redirects < client->cfg.max_redirects &&
            proven_http_header_find(call->headers, call->res.header_count, cl_lit("Location"), &location)) {
            bool to_get = status == 303 || ((status == 301 || status == 302) && cl_eq_nocase(at.method, cl_lit("POST")));
            bool must_repeat_body = !to_get && at.send_body && streaming;
            proven_byte_t next[CL_URL_MAX];
            proven_size_t nl = 0;
            proven_url_t nu;
            bool ok = !must_repeat_body &&
                      proven_url_resolve((proven_u8str_view_t){ .ptr = call->url, .size = call->url_len }, location,
                                         (proven_mem_mut_t){ .ptr = next, .size = sizeof next }, &nl) == PROVEN_OK &&
                      proven_url_parse((proven_u8str_view_t){ .ptr = next, .size = nl }, &nu) == PROVEN_OK && nu.host.size > 0;
            bool next_https = ok && proven_url_scheme_is(&nu, cl_lit("https"));
            if (ok && !next_https && !proven_url_scheme_is(&nu, cl_lit("http"))) ok = false;
            if (ok && https && !next_https) ok = false;               /* never from https down to http */
            if (ok && next_https && !client->cfg.tls_wrap) ok = false;
            if (ok) {
                /* A fragment is the client's own business and is not sent anywhere. */
                if (nu.has_fragment) nl = (proven_size_t)(nu.fragment.ptr - 1 - next);
                bool same = nu.host.size == origin_host_len && proven_url_effective_port(&nu) == origin_port && next_https == origin_https;
                for (proven_size_t i = 0; same && i < nu.host.size; ++i) if (cl_lower(nu.host.ptr[i]) != origin_host[i]) same = false;
                if (!same) at.cross_origin = true;
                /* An answer to a challenge was computed for the old target; the new one may
                 * ask again. */
                at.authorization = (proven_u8str_view_t){0};
                auth_tried = false;
                stale_retried = false;
                if (to_get) { at.method = cl_lit("GET"); at.send_body = false; }
                cl_drain(call);
                cl_release_conn(call);
                for (proven_size_t i = 0; i < nl; ++i) call->url[i] = next[i];
                call->url_len = nl;
                redirects++;
                continue;
            }
        }
        break;
    }

    if (e != PROVEN_OK) {
        if (call->conn) cl_conn_close(client, call->conn);
        proven_http_decoder_free_(&call->dec);
        cl_free(a, call);
        return e;
    }
    response->status = call->res.status;
    response->reason = call->res.reason;
    response->headers = call->headers;
    response->header_count = call->res.header_count;
    response->url = (proven_u8str_view_t){ .ptr = call->url, .size = call->url_len };
    response->redirects = redirects;
    response->internal = call;
    return PROVEN_OK;
}

proven_err_t proven_http_client_get(proven_http_client_t *client, proven_u8str_view_t url,
                                    proven_http_client_response_t *response) {
    proven_http_client_request_t req = {0};
    req.url = url;
    return proven_http_client_send(client, &req, response);
}

proven_result_size_t proven_http_client_read(proven_http_client_response_t *response, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (!response || (dest.size > 0 && !dest.ptr)) return res;
    if (!response->internal) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    cl_call_t *call = response->internal;
    return call->decoding ? cl_read_decoded(call, dest) : cl_read_body(call, dest);
}

proven_err_t proven_http_client_upgrade(proven_http_client_response_t *response, proven_transport_t *out, proven_mem_view_t *early) {
    if (out) *out = (proven_transport_t){0};
    if (early) *early = (proven_mem_view_t){0};
    if (!response || !out || !early) return PROVEN_ERR_INVALID_ARG;
    cl_call_t *call = response->internal;
    if (!call || !call->conn || call->res.status != 101) return PROVEN_ERR_INVALID_STATE;
    cl_conn_t *conn = call->conn;
    conn->alloc = call->client->cfg.alloc;
    call->conn = (void *)0;                     /* finish no longer has a connection to close */
    out->ctx = conn;
    out->read_fn = cl_owned_read;
    out->write_fn = cl_owned_write;
    out->shutdown_fn = cl_owned_shutdown;
    out->close_fn = cl_owned_close;
    *early = (proven_mem_view_t){ .ptr = call->buf + call->buf_pos, .size = call->buf_len - call->buf_pos };
    call->buf_pos = call->buf_len;
    return PROVEN_OK;
}

proven_err_t proven_http_client_read_all(proven_http_client_response_t *response, proven_allocator_t alloc,
                                         proven_u8str_t *out, proven_size_t max_bytes) {
    if (!response || !out) return PROVEN_ERR_INVALID_ARG;
    if (!response->internal) return PROVEN_ERR_INVALID_STATE;
    proven_byte_t chunk[4096];
    proven_size_t total = 0;
    for (;;) {
        proven_size_t room = max_bytes - total;
        /* One byte past the limit is asked for, so that a body of exactly the limit ends in
         * EOF and one that is longer is seen to be longer. */
        proven_size_t want = room < sizeof chunk ? room + 1 : sizeof chunk;
        if (want > sizeof chunk) want = sizeof chunk;
        proven_result_size_t r = proven_http_client_read(response, (proven_mem_mut_t){ .ptr = chunk, .size = want });
        if (r.err == PROVEN_ERR_EOF) return PROVEN_OK;
        if (r.err != PROVEN_OK) return r.err;
        proven_size_t take = r.value > room ? room : r.value;
        proven_err_t e = proven_u8str_append_grow(alloc, out, (proven_u8str_view_t){ .ptr = chunk, .size = take });
        if (e != PROVEN_OK) return e;
        total += take;
        if (r.value > room) return PROVEN_ERR_OUT_OF_BOUNDS;
    }
}

void proven_http_client_finish(proven_http_client_response_t *response) {
    if (!response) return;
    cl_call_t *call = response->internal;
    if (call) {
        proven_allocator_t a = call->client->cfg.alloc;
        cl_release_conn(call);
        proven_http_decoder_free_(&call->dec);
        cl_free(a, call);
    }
    *response = (proven_http_client_response_t){0};
}

#else
/* A translation unit must not be empty. */
typedef int proven_http_client_unused_t;
#endif
