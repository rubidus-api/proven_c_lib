#include "proven/tls.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven_internal_tls.h"

/*
 * The TLS engine over a transport, as a transport. Everything about TLS is in the engine; what
 * is here is the plumbing: read from the plain transport and feed, take what is pending and
 * write it, with every wait bounded by the caller's deadline.
 */

#define TT_MAGIC ((proven_u32)0x544c5354)
#define TT_READ_CHUNK ((proven_size_t)16 * 1024)

typedef struct {
    proven_u32 magic;
    proven_allocator_t alloc;
    proven_transport_t plain;
    proven_tls_conn_t *conn;
    proven_byte_t *rbuf;                          /* bytes read from `plain` and not yet fed */
    proven_size_t rlen, roff;
    proven_err_t failed;
} tls_transport_t;

/* Send everything the engine has pending. */
static proven_err_t tt_flush(tls_transport_t *t, proven_net_deadline_t until) {
    for (;;) {
        proven_mem_view_t out = proven_tls_pending_output(t->conn);
        if (out.size == 0) return PROVEN_OK;
        proven_result_size_t w = proven_transport_write(t->plain, out, until);
        if (w.err != PROVEN_OK) return w.err;
        proven_tls_output_sent(t->conn, w.value);
    }
}

/* Feed what is left over from last time; when nothing is, read more. Returns after one step.
 *
 * Bytes are read into a buffer on the stack and fed at once. Only what the engine would not
 * take yet (it stops when application data is waiting to be read) is kept, in an allocation
 * of exactly that size, so that a connection with nothing in flight holds no buffer here. */
static proven_err_t tt_pull(tls_transport_t *t, proven_net_deadline_t until) {
    proven_byte_t chunk[TT_READ_CHUNK];
    const proven_byte_t *data;
    proven_size_t len;
    if (t->roff < t->rlen) {
        data = t->rbuf + t->roff;
        len = t->rlen - t->roff;
    } else {
        proven_result_size_t r = proven_transport_read(t->plain, (proven_mem_mut_t){ .ptr = chunk, .size = sizeof chunk }, until);
        if (r.err != PROVEN_OK) return r.err;
        data = chunk;
        len = r.value;
    }
    proven_size_t used = 0;
    proven_err_t e = proven_tls_feed(t->conn, (proven_mem_view_t){ .ptr = data, .size = len }, &used);
    if (data != chunk) {
        t->roff += used;
        if (t->roff == t->rlen) { t->alloc.free_fn(t->alloc.ctx, t->rbuf); t->rbuf = NULL; t->rlen = 0; t->roff = 0; }
    } else if (used < len && e == PROVEN_OK) {
        proven_result_mem_mut_t m = t->alloc.alloc_fn(t->alloc.ctx, len - used, 1);
        if (m.err != PROVEN_OK) return m.err;
        t->rbuf = m.value.ptr;
        for (proven_size_t i = 0; i < len - used; ++i) t->rbuf[i] = chunk[used + i];
        t->rlen = len - used;
        t->roff = 0;
    }
    return e;
}

static proven_result_size_t tt_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    tls_transport_t *t = (tls_transport_t *)ctx;
    for (;;) {
        proven_result_size_t r = proven_tls_read(t->conn, dest);
        if (r.err != PROVEN_ERR_NEED_MORE) return r;
        if (t->failed != PROVEN_OK) return (proven_result_size_t){ .err = t->failed, .value = 0 };
        /* The engine may owe the peer something first: an answer to a key update. */
        proven_err_t e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_pull(t, until);
        if (e != PROVEN_OK) {
            /* A peer that goes away without closing the TLS session has cut it short: that is
             * not an end of stream a caller may trust. */
            if (e == PROVEN_ERR_EOF) e = PROVEN_ERR_RESET;
            if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) {
                t->failed = e;
                (void)tt_flush(t, until);         /* the alert, if the engine made one */
            }
            return (proven_result_size_t){ .err = e, .value = 0 };
        }
    }
}

static proven_result_size_t tt_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    tls_transport_t *t = (tls_transport_t *)ctx;
    if (t->failed != PROVEN_OK) return (proven_result_size_t){ .err = t->failed, .value = 0 };
    proven_size_t done = 0;
    while (done < src.size) {
        proven_result_size_t w = proven_tls_write(t->conn, (proven_mem_view_t){ .ptr = src.ptr + done, .size = src.size - done });
        if (w.err != PROVEN_OK) return (proven_result_size_t){ .err = w.err, .value = done };
        done += w.value;
        proven_err_t e = tt_flush(t, until);
        if (e != PROVEN_OK) {
            if (e != PROVEN_ERR_TIMEOUT && e != PROVEN_ERR_AGAIN) t->failed = e;
            return (proven_result_size_t){ .err = done > 0 ? PROVEN_OK : e, .value = done };
        }
    }
    return (proven_result_size_t){ .err = PROVEN_OK, .value = done };
}

static proven_err_t tt_shutdown(void *ctx) {
    tls_transport_t *t = (tls_transport_t *)ctx;
    if (t->failed != PROVEN_OK) return t->failed;
    proven_err_t e = proven_tls_close(t->conn);
    if (e == PROVEN_OK) e = tt_flush(t, proven_net_deadline_in(2000));
    return e;
}

static void tt_free(tls_transport_t *t) {
    proven_allocator_t a = t->alloc;
    if (t->conn) proven_tls_conn_destroy(t->conn);
    if (t->rbuf) a.free_fn(a.ctx, t->rbuf);
    t->magic = 0;
    a.free_fn(a.ctx, t);
}

static proven_err_t tt_close(void *ctx) {
    tls_transport_t *t = (tls_transport_t *)ctx;
    /* Say goodbye if the session is still good; do not wait long for a peer that is not reading. */
    if (t->failed == PROVEN_OK && proven_tls_close(t->conn) == PROVEN_OK) (void)tt_flush(t, proven_net_deadline_in(1000));
    proven_err_t e = proven_transport_close(t->plain);
    tt_free(t);
    return e;
}

static proven_err_t tt_open(proven_transport_t plain, const proven_tls_config_t *config, proven_tls_conn_t *conn,
                            proven_net_deadline_t until, proven_transport_t *out) {
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(tls_transport_t), 16);
    if (m.err != PROVEN_OK) { proven_tls_conn_destroy(conn); return m.err; }
    tls_transport_t *t = (tls_transport_t *)m.value.ptr;
    t->magic = TT_MAGIC; t->alloc = a; t->plain = plain; t->conn = conn; t->rbuf = NULL; t->rlen = 0; t->roff = 0; t->failed = PROVEN_OK;
    proven_err_t e = PROVEN_OK;
    while (e == PROVEN_OK && !proven_tls_is_established(conn)) {
        e = tt_flush(t, until);
        if (e == PROVEN_OK) e = tt_pull(t, until);
    }
    /* The last flight (a client's Finished, a server's ticket), or the alert on failure. */
    proven_err_t fe = tt_flush(t, until);
    if (e == PROVEN_OK) e = fe;
    if (e != PROVEN_OK) { tt_free(t); return e; }               /* `plain` stays open: it is the caller's */
    out->ctx = t;
    out->read_fn = tt_read;
    out->write_fn = tt_write;
    out->shutdown_fn = tt_shutdown;
    out->close_fn = tt_close;
    return PROVEN_OK;
}

proven_err_t proven_tls_transport_client(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_u8str_view_t server_name, proven_tls_session_t *session,
                                         proven_net_deadline_t until, proven_transport_t *out) {
    if (!out || !config || !proven_transport_is_valid(plain)) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_client_create(config, server_name, session, &conn);
    if (e != PROVEN_OK) return e;
    return tt_open(plain, config, conn, until, out);
}

proven_err_t proven_tls_transport_server(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_net_deadline_t until, proven_transport_t *out) {
    if (!out || !config || !proven_transport_is_valid(plain)) return PROVEN_ERR_INVALID_ARG;
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_server_create(config, &conn);
    if (e != PROVEN_OK) return e;
    return tt_open(plain, config, conn, until, out);
}

proven_err_t proven_tls_transport_server_lazy_(proven_transport_t plain, const proven_tls_config_t *config, proven_transport_t *out) {
    proven_tls_conn_t *conn = NULL;
    proven_err_t e = proven_tls_server_create(config, &conn);
    if (e != PROVEN_OK) return e;
    proven_allocator_t a = config->alloc;
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, sizeof(tls_transport_t), 16);
    if (m.err != PROVEN_OK) { proven_tls_conn_destroy(conn); return m.err; }
    tls_transport_t *t = (tls_transport_t *)m.value.ptr;
    t->magic = TT_MAGIC; t->alloc = a; t->plain = plain; t->conn = conn; t->rbuf = NULL; t->rlen = 0; t->roff = 0; t->failed = PROVEN_OK;
    out->ctx = t; out->read_fn = tt_read; out->write_fn = tt_write; out->shutdown_fn = tt_shutdown; out->close_fn = tt_close;
    return PROVEN_OK;
}

void proven_tls_transport_set_plain_(proven_transport_t tls, proven_transport_t plain) {
    ((tls_transport_t *)tls.ctx)->plain = plain;
}

bool proven_tls_transport_buffered_(proven_transport_t tls) {
    tls_transport_t *t = (tls_transport_t *)tls.ctx;
    if (t->failed != PROVEN_OK) return false;
    if (t->roff < t->rlen) return true;
    proven_byte_t probe;
    proven_result_size_t r = proven_tls_read(t->conn, (proven_mem_mut_t){ .ptr = &probe, .size = 0 });
    return r.err == PROVEN_OK;                    /* plaintext is waiting; a zero-length read takes none */
}

void proven_tls_transport_shutdown_now_(proven_transport_t tls) {
    tls_transport_t *t = (tls_transport_t *)tls.ctx;
    if (t->failed == PROVEN_OK && proven_tls_close(t->conn) == PROVEN_OK) (void)tt_flush(t, PROVEN_NET_DONT_WAIT);
}

void proven_tls_transport_close_now_(proven_transport_t tls) {
    tls_transport_t *t = (tls_transport_t *)tls.ctx;
    proven_tls_transport_shutdown_now_(tls);
    (void)proven_transport_close(t->plain);
    tt_free(t);
}

proven_tls_conn_t *proven_tls_transport_conn(proven_transport_t tls) {
    if (tls.read_fn != tt_read || !tls.ctx) return NULL;
    tls_transport_t *t = (tls_transport_t *)tls.ctx;
    return t->magic == TT_MAGIC ? t->conn : NULL;
}

proven_err_t proven_tls_http_wrap(void *config, proven_transport_t plain, proven_u8str_view_t host,
                                  proven_net_deadline_t until, proven_transport_t *out) {
    return proven_tls_transport_client(plain, (const proven_tls_config_t *)config, host, NULL, until, out);
}

#endif
