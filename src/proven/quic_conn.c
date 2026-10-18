#include "proven_internal_quic.h"
#include "../../platform/proven_sys_mem.h"

#ifndef PROVEN_NO_HTTP3

/* A QUIC version 1 connection (RFC 9000) with RFC 9002's loss recovery and NewReno, over the
 * TLS engine's QUIC mode. No I/O and no clock: datagrams and the time come in, datagrams and a
 * deadline go out.
 *
 * Everything the peer can make this side hold has a cap (RFC-0017 section 3): stream data
 * received out of order is bounded by flow control (the sum of the highest offsets received
 * may not pass the connection's window), the ranges describing it by a count checked before a
 * packet is applied, CRYPTO data by a window per level, received packet numbers by the ACK
 * range limit, streams by MAX_STREAMS, connection IDs by active_connection_id_limit, datagrams
 * waiting for keys by a count. What this side sends is bounded by the send buffers. */

#define QC_CID_LEN 8
#define QC_MAX_FRAMES 8                 /* frame records per sent packet */
#define QC_PENDING 4                    /* datagrams held per space while its keys are not there */
#define QC_PEER_CIDS 4                  /* our active_connection_id_limit */
#define QC_LOCAL_CIDS 4                 /* the most connection IDs issued to the peer at once */
#define QC_CRYPTO_WINDOW 65536
#define QC_INITIAL_RTT 333000
#define QC_MAX_DGRAM 1452
#define QC_EVENTS 32
#define QC_MAX_SENT 8192                /* packets in flight per space */
#define QC_INF ((proven_u64)-1)

static const proven_u64 Q_STREAM_LIMIT = (((proven_u64)1) << 62) - 1;

/* ---- Small helpers ---- */

/* Block copies through the platform (RFC-0018 S-1: these were byte loops, 45 % of a
 * receiver's time). */
static void qcopy(proven_byte_t *dst, const proven_byte_t *src, proven_size_t n) {
    proven_sys_mem_copy(dst, src, n);
}

static void qmove(proven_byte_t *dst, const proven_byte_t *src, proven_size_t n) {
    proven_sys_mem_move(dst, src, n);
}

static void qzero(void *p, proven_size_t n) {
    proven_sys_mem_zero(p, n);
}

static bool cid_eq(const proven_quic_cid_t *a, const proven_quic_cid_t *b) {
    if (a->len != b->len) return false;
    for (proven_size_t i = 0; i < a->len; ++i) if (a->id[i] != b->id[i]) return false;
    return true;
}

static bool addr_eq(const proven_quic_addr_t *a, const proven_quic_addr_t *b) {
    if (a->len != b->len) return false;
    for (proven_size_t i = 0; i < a->len && i < sizeof a->b; ++i) if (a->b[i] != b->b[i]) return false;
    return true;
}

static proven_u64 qmin(proven_u64 a, proven_u64 b) { return a < b ? a : b; }
static proven_u64 qmax(proven_u64 a, proven_u64 b) { return a > b ? a : b; }

/* ---- Allocation ---- */

typedef struct { proven_allocator_t alloc; proven_size_t held; bool oom; } qmem_t;

static void *qalloc(qmem_t *m, proven_size_t n, proven_size_t align) {
    if (n == 0) n = 1;
    proven_result_mem_mut_t r = m->alloc.alloc_fn(m->alloc.ctx, n, align);
    if (r.err != PROVEN_OK) { m->oom = true; return NULL; }
    m->held += n;
    return r.value.ptr;
}

static void *qrealloc(qmem_t *m, void *p, proven_size_t old, proven_size_t n, proven_size_t align) {
    if (!p) return qalloc(m, n, align);
    proven_result_mem_mut_t r = m->alloc.realloc_fn(m->alloc.ctx, p, old, n, align);
    if (r.err != PROVEN_OK) { m->oom = true; return NULL; }
    m->held = m->held - old + n;
    return r.value.ptr;
}

static void qfree(qmem_t *m, void *p, proven_size_t n) {
    if (!p) return;
    m->alloc.free_fn(m->alloc.ctx, p);
    m->held -= n < 1 ? 1 : n;
}

/* The capacity a buffer grown from 1 KiB by doubling has when it holds `n` bytes. */
static proven_size_t qcap_for(proven_size_t n) {
    proven_size_t cap = 1024;
    while (cap < n) cap *= 2;
    return cap;
}

/* ---- A byte buffer: bytes [head, head + len) of `p` ----
 *
 * `last` is the capacity for the most its last fill needed at once (`peak` while it fills): when
 * it was given back empty, the next fill allocates that at once, not 1 KiB doubled again with
 * its bytes copied at each step (RFC-0018 S-1). A larger fill before it is forgotten by an eighth
 * at each fill after, so one large fill is not kept for the stream's life, and fills that cross a
 * doubling now and then do not regrow every time. */

typedef struct { proven_byte_t *p; proven_size_t cap, head, len, peak, last; } qbuf_t;

/* Room for `n` bytes from the head. */
static bool qbuf_reserve(qmem_t *m, qbuf_t *b, proven_size_t n) {
    if (n > b->peak) b->peak = n;
    if (b->cap - b->head >= n) return true;
    if (b->head > 0) {
        qmove(b->p, b->p + b->head, b->len);
        b->head = 0;
        if (b->cap >= n) return true;
    }
    proven_size_t cap = b->cap ? b->cap : b->last ? b->last : 1024;
    while (cap < n) cap *= 2;
    proven_byte_t *p = qrealloc(m, b->p, b->cap, cap, 1);
    if (!p) return false;
    b->p = p; b->cap = cap;
    return true;
}

/* Bytes taken from the front. An empty buffer is given back: a hundred quiet streams hold
 * nothing, and what is allocated stays within twice what is held. */
static void qbuf_drop(qmem_t *m, qbuf_t *b, proven_size_t n) {
    b->head += n; b->len -= n;
    if (b->len == 0) {
        b->head = 0;
        if (b->cap > 4096) { qfree(m, b->p, b->cap); b->last = (proven_size_t)qmax(qcap_for(b->peak), b->last - b->last / 8); b->p = NULL; b->cap = 0; }
        b->peak = 0;
    }
}

static void qbuf_free(qmem_t *m, qbuf_t *b) {
    qfree(m, b->p, b->cap);
    *b = (qbuf_t){ 0 };
}

/* ---- A ring: bytes [head, head + len) of `p`, wrapping at `cap` ----
 *
 * A send queue's bytes from the acknowledged base to the end: written at the end, dropped from
 * the front, never moved while they wait (RFC-0018 S-1: the linear buffer moved its whole
 * unacknowledged window down whenever the end ran out of room). `peak` and `last` as in qbuf_t. */

typedef struct { proven_byte_t *p; proven_size_t cap, head, len, peak, last; } qring_t;

/* The position of the byte `i` after the head. */
static proven_size_t ring_pos(const qring_t *r, proven_size_t i) {
    const proven_size_t at = r->head + i;
    return at >= r->cap ? at - r->cap : at;
}

/* Room for `n` bytes in all. Growing copies the bytes out unwrapped, once per doubling. */
static bool ring_reserve(qmem_t *m, qring_t *r, proven_size_t n) {
    if (n > r->peak) r->peak = n;
    if (r->cap >= n) return true;
    proven_size_t cap = r->cap ? r->cap * 2 : r->last ? r->last : 1024;
    while (cap < n) cap *= 2;
    proven_byte_t *p = qalloc(m, cap, 1);
    if (!p) return false;
    if (r->len > 0) {
        const proven_size_t first = r->cap - r->head < r->len ? r->cap - r->head : r->len;
        qcopy(p, r->p + r->head, first);
        qcopy(p + first, r->p, r->len - first);
    }
    qfree(m, r->p, r->cap);
    r->p = p; r->cap = cap; r->head = 0;
    return true;
}

/* `n` bytes added at the end, in at most two parts. */
static void ring_put(qring_t *r, const proven_byte_t *src, proven_size_t n) {
    if (n == 0) return;
    const proven_size_t at = ring_pos(r, r->len);
    const proven_size_t first = r->cap - at < n ? r->cap - at : n;
    qcopy(r->p + at, src, first);
    qcopy(r->p, src + first, n - first);
    r->len += n;
}

/* Bytes taken from the front; an empty ring is given back as qbuf_drop gives back a buffer. */
static void ring_drop(qmem_t *m, qring_t *r, proven_size_t n) {
    r->head = ring_pos(r, n);
    r->len -= n;
    if (r->len == 0) {
        r->head = 0;
        if (r->cap > 4096) { qfree(m, r->p, r->cap); r->last = (proven_size_t)qmax(qcap_for(r->peak), r->last - r->last / 8); r->p = NULL; r->cap = 0; }
        r->peak = 0;
    }
}

static void ring_free(qmem_t *m, qring_t *r) {
    qfree(m, r->p, r->cap);
    *r = (qring_t){ 0 };
}

/* ---- A set of ranges [lo, hi), ascending, apart ---- */

typedef struct { proven_u64 lo, hi; } qrange_t;
typedef struct { qrange_t *r; proven_size_t n, cap; } qrset_t;

static bool rset_insert_at(qmem_t *m, qrset_t *s, proven_size_t at, proven_u64 lo, proven_u64 hi) {
    if (s->n == s->cap) {
        proven_size_t cap = s->cap ? s->cap * 2 : 8;
        qrange_t *r = qrealloc(m, s->r, s->cap * sizeof *r, cap * sizeof *r, alignof(qrange_t));
        if (!r) return false;
        s->r = r; s->cap = cap;
    }
    for (proven_size_t i = s->n; i > at; --i) s->r[i] = s->r[i - 1];
    s->r[at] = (qrange_t){ lo, hi };
    ++s->n;
    return true;
}

static void rset_erase(qrset_t *s, proven_size_t at, proven_size_t count) {
    for (proven_size_t i = at; i + count < s->n; ++i) s->r[i] = s->r[i + count];
    s->n -= count;
}

/* Add [lo, hi), merging what it touches. */
static bool rset_add(qmem_t *m, qrset_t *s, proven_u64 lo, proven_u64 hi) {
    if (lo >= hi) return true;
    proven_size_t i = 0;
    while (i < s->n && s->r[i].hi < lo) ++i;
    proven_size_t j = i;
    while (j < s->n && s->r[j].lo <= hi) { lo = qmin(lo, s->r[j].lo); hi = qmax(hi, s->r[j].hi); ++j; }
    if (j > i) { s->r[i] = (qrange_t){ lo, hi }; rset_erase(s, i + 1, j - i - 1); return true; }
    return rset_insert_at(m, s, i, lo, hi);
}

/* Remove [lo, hi). */
static bool rset_remove(qmem_t *m, qrset_t *s, proven_u64 lo, proven_u64 hi) {
    for (proven_size_t i = 0; i < s->n && lo < hi;) {
        qrange_t r = s->r[i];
        if (r.hi <= lo) { ++i; continue; }
        if (r.lo >= hi) break;
        if (r.lo < lo && r.hi > hi) {                          /* split */
            s->r[i].hi = lo;
            return rset_insert_at(m, s, i + 1, hi, r.hi);
        }
        if (r.lo < lo) { s->r[i].hi = lo; ++i; continue; }
        if (r.hi > hi) { s->r[i].lo = hi; break; }
        rset_erase(s, i, 1);
    }
    return true;
}

/* The first part of [lo, hi) not in `s`: false when all of it is. */
static bool rset_first_gap(const qrset_t *s, proven_u64 lo, proven_u64 hi, proven_u64 *a, proven_u64 *b) {
    for (proven_size_t i = 0; i < s->n && lo < hi; ++i) {
        if (s->r[i].hi <= lo) continue;
        if (s->r[i].lo > lo) { *a = lo; *b = qmin(hi, s->r[i].lo); return true; }
        lo = s->r[i].hi;
    }
    if (lo >= hi) return false;
    *a = lo; *b = hi;
    return true;
}

static void rset_free(qmem_t *m, qrset_t *s) {
    qfree(m, s->r, s->cap * sizeof *s->r);
    *s = (qrset_t){ 0 };
}

/* ---- Sending a byte stream (a stream's or a level's CRYPTO data) ----
 *
 * The bytes from `base` (everything below is acknowledged) to `end` (the last written) are
 * kept. `next` is the first never sent; ranges sent and lost wait in `lost`, ranges
 * acknowledged above `base` in `acked`. */

typedef struct {
    qring_t buf;
    proven_u64 base, end, next;
    qrset_t lost, acked;
    bool fin, fin_sent, fin_lost, fin_acked;
} qsendq_t;

static bool sendq_write(qmem_t *m, qsendq_t *q, const proven_byte_t *p, proven_size_t n) {
    if (!ring_reserve(m, &q->buf, q->buf.len + n)) return false;
    ring_put(&q->buf, p, n);
    q->end += n;
    return true;
}

static void sendq_acked(qmem_t *m, qsendq_t *q, proven_u64 off, proven_u64 len, bool fin) {
    if (fin) q->fin_acked = true;
    if (off + len <= q->base) return;
    if (off < q->base) { len -= q->base - off; off = q->base; }
    (void)rset_add(m, &q->acked, off, off + len);
    (void)rset_remove(m, &q->lost, off, off + len);
    if (q->acked.n > 0 && q->acked.r[0].lo == q->base) {
        const proven_u64 to = q->acked.r[0].hi;
        ring_drop(m, &q->buf, (proven_size_t)(to - q->base));
        q->base = to;
        rset_erase(&q->acked, 0, 1);
    }
}

static void sendq_lost(qmem_t *m, qsendq_t *q, proven_u64 off, proven_u64 len, bool fin) {
    if (fin && !q->fin_acked) q->fin_lost = true;
    if (off + len <= q->base) return;
    if (off < q->base) { len -= q->base - off; off = q->base; }
    (void)rset_add(m, &q->lost, off, off + len);
}

/* All of it: sent again from the start (a Retry, a probe). */
static void sendq_resend_all(qmem_t *m, qsendq_t *q) {
    if (q->next > q->base) (void)rset_add(m, &q->lost, q->base, q->next);
    for (proven_size_t i = 0; i < q->acked.n; ++i) (void)rset_remove(m, &q->lost, q->acked.r[i].lo, q->acked.r[i].hi);
    if (q->fin_sent && !q->fin_acked) q->fin_lost = true;
}

/* The bytes from `off` (held, below `end`) to the ring's wrap: a piece ends there, so a frame's
 * data is always one view. */
static proven_u64 sendq_contig(const qsendq_t *q, proven_u64 off) {
    return q->buf.cap - ring_pos(&q->buf, (proven_size_t)(off - q->base));
}

/* The next piece to send, at most `room` bytes and never across the ring's wrap: lost bytes
 * first, then new ones up to `limit` (flow control). False when there is nothing. `*fin` when
 * the piece ends the stream. */
static bool sendq_next(const qsendq_t *q, proven_u64 room, proven_u64 limit, proven_u64 *off, proven_u64 *len, bool *fin, bool *fresh) {
    *fin = false; *fresh = false;
    for (proven_size_t i = 0; i < q->lost.n; ++i) {
        proven_u64 a, b;
        if (!rset_first_gap(&q->acked, qmax(q->lost.r[i].lo, q->base), q->lost.r[i].hi, &a, &b)) continue;
        *off = a; *len = qmin(b - a, room);
        if (*len > 0) *len = qmin(*len, sendq_contig(q, a));
        *fin = q->fin && a + *len == q->end && (q->fin_lost || !q->fin_sent);
        return *len > 0 || *fin;
    }
    const proven_u64 upto = qmin(q->end, limit);
    if (q->next < upto && room > 0) {
        *off = q->next; *len = qmin(qmin(upto - q->next, room), sendq_contig(q, q->next)); *fresh = true;
        *fin = q->fin && *off + *len == q->end;
        return true;
    }
    if (q->fin && q->next == q->end && !q->fin_acked && (!q->fin_sent || q->fin_lost)) {
        *off = q->end; *len = 0; *fin = true;
        return true;
    }
    return false;
}

/* What sendq_next gave was put in a packet. */
static void sendq_sent(qmem_t *m, qsendq_t *q, proven_u64 off, proven_u64 len, bool fin) {
    (void)rset_remove(m, &q->lost, off, off + len);
    if (off + len > q->next) q->next = off + len;
    if (fin) { q->fin_sent = true; q->fin_lost = false; }
}

static const proven_byte_t *sendq_at(const qsendq_t *q, proven_u64 off) {
    return q->buf.p + ring_pos(&q->buf, (proven_size_t)(off - q->base));
}

static bool sendq_done(const qsendq_t *q) {
    return q->fin && q->fin_acked && q->base == q->end;
}

static void sendq_free(qmem_t *m, qsendq_t *q) {
    ring_free(m, &q->buf);
    rset_free(m, &q->lost);
    rset_free(m, &q->acked);
}

/* ---- Receiving a byte stream ----
 *
 * `buf` holds the bytes from `read` (the next the reader takes) up to the highest received,
 * with holes where nothing came yet; `got` says which ranges are there. */

typedef struct {
    qbuf_t buf;
    proven_u64 read, highest, final;
    bool has_final;
    qrset_t got;
} qrecvq_t;

/* Take [off, off + n). Returns false on NOMEM only. */
static bool recvq_insert(qmem_t *m, qrecvq_t *q, proven_u64 off, const proven_byte_t *p, proven_size_t n) {
    if (off + n <= q->read) return true;
    if (off < q->read) { const proven_size_t skip = (proven_size_t)(q->read - off); p += skip; n -= skip; off = q->read; }
    const proven_size_t at = (proven_size_t)(off - q->read), need = at + n;
    if (need > q->buf.len) {
        if (!qbuf_reserve(m, &q->buf, need)) return false;
        /* Only a hole is zeroed: what arrives at the end is copied over at once. Nothing reads
         * outside `got`; the zero is so a hole never holds a freed buffer's bytes. */
        if (at > q->buf.len) qzero(q->buf.p + q->buf.head + q->buf.len, at - q->buf.len);
        q->buf.len = need;
    }
    qcopy(q->buf.p + q->buf.head + at, p, n);
    if (!rset_add(m, &q->got, off, off + n)) return false;
    if (off + n > q->highest) q->highest = off + n;
    return true;
}

/* Bytes ready at `read`. */
static proven_size_t recvq_ready(const qrecvq_t *q) {
    return q->got.n > 0 && q->got.r[0].lo == q->read ? (proven_size_t)(q->got.r[0].hi - q->read) : 0;
}

static proven_size_t recvq_take(qmem_t *m, qrecvq_t *q, proven_byte_t *out, proven_size_t cap) {
    proven_size_t n = recvq_ready(q);
    if (n > cap) n = cap;
    if (n == 0) return 0;
    if (out) qcopy(out, q->buf.p + q->buf.head, n);
    qbuf_drop(m, &q->buf, n);
    (void)rset_remove(m, &q->got, q->read, q->read + n);
    q->read += n;
    return n;
}

static void recvq_free(qmem_t *m, qrecvq_t *q) {
    qbuf_free(m, &q->buf);
    rset_free(m, &q->got);
}

/* ---- Streams ---- */

typedef struct {
    proven_u64 id;
    bool local;                         /* opened by this side */
    bool has_tx, has_rx;                /* the directions it has */
    qsendq_t tx;
    qrecvq_t rx;
    proven_u64 tx_max;                  /* the peer's MAX_STREAM_DATA */
    proven_u64 rx_max;                  /* ours, as last sent */
    proven_u64 rx_window, rx_moved_at;  /* the window as it has grown, and when it last moved (RFC-0019 section 9) */
    bool rx_max_owed;                   /* MAX_STREAM_DATA to send */
    bool tx_reset, tx_reset_owed, tx_reset_acked;
    proven_u64 tx_reset_code, tx_reset_final;
    bool rx_reset;                      /* the peer reset its side */
    proven_u64 rx_reset_code;
    bool rx_stop_owed, rx_stopped;      /* this side asked for no more */
    proven_u64 rx_stop_code;
    bool rx_fin_read;                   /* the reader has had the end */
    bool ev_opened, ev_readable, ev_writable, ev_reset, ev_stop;
    bool tx_blocked;                    /* a write took less than offered: told when there is room again */
    proven_u64 ev_stop_code;
    bool rx_reset_reported;
} qstream_t;

/* ---- A packet number space ---- */

enum { FR_STREAM = 1, FR_CRYPTO, FR_RESET, FR_STOP, FR_MAX_DATA, FR_MAX_STREAM_DATA, FR_MAX_STREAMS_BIDI, FR_MAX_STREAMS_UNI,
       FR_NEW_CID, FR_RETIRE_CID, FR_HANDSHAKE_DONE, FR_PING, FR_PATH_CHALLENGE };

typedef struct { proven_u8 kind; bool fin; proven_u16 len; proven_u64 id, off; } qfrec_t;

typedef struct {
    proven_u64 pn, time;
    proven_u32 size;                    /* bytes in flight; 0 when not counted */
    bool ack_eliciting, in_flight;
    proven_u8 nfr;
    qfrec_t fr[QC_MAX_FRAMES];
} qsent_t;

typedef struct {
    proven_quic_keys_t rx, tx;
    bool rx_ok, tx_ok, discarded;
    proven_u64 next_pn;
    proven_u64 rx_largest_plus_one, rx_largest_time;
    qrset_t rcvd;                       /* packet numbers received (and processed) */
    proven_u64 rx_floor;                /* below it: treated as already seen (ranges dropped for the cap) */
    bool ack_owed;                      /* an ack-eliciting packet not acknowledged yet */
    bool ack_any;                       /* any packet not acknowledged yet */
    unsigned ack_count;
    proven_u64 ack_deadline;
    qsent_t *sent;
    proven_size_t sent_n, sent_cap;
    proven_u64 largest_acked_plus_one;
    proven_u64 loss_time;
    proven_u64 last_ack_eliciting_time;
    proven_size_t ack_eliciting_in_flight;
    unsigned probes;                    /* PTO probe packets owed */
    qsendq_t crypto_tx;
    qrecvq_t crypto_rx;
    proven_byte_t *pending[QC_PENDING];
    proven_size_t pending_len[QC_PENDING];
    proven_quic_addr_t pending_from[QC_PENDING];
    unsigned pending_n;
    proven_u64 lost_pn[32], lost_gap[32];   /* recently declared lost, and how far below the largest acknowledged (QC_INF: by time) */
    unsigned lost_at;
} qspace_t;

typedef struct {
    proven_u64 seq;
    proven_quic_cid_t cid;
    proven_byte_t token[PROVEN_QUIC_RESET_TOKEN];
    bool active, owed, retire_owed, has_token;
} qcid_t;

enum { QS_HANDSHAKE, QS_OPEN, QS_CLOSING, QS_DRAINING, QS_CLOSED };

struct proven_quic_conn {
    qmem_t mem;
    proven_quic_config_t cfg;
    bool server;
    proven_tls_conn_t *tls;
    const proven_tls_suite_t *suite;
    qspace_t sp[PROVEN_QUIC_LEVELS];

    /* 1-RTT secrets, kept for key updates (RFC 9001 section 6). SECRET */
    proven_byte_t rx_secret[PROVEN_HMAC_MAX_SIZE], tx_secret[PROVEN_HMAC_MAX_SIZE];
    proven_size_t secret_len;
    proven_quic_keys_t rx_next, rx_prev;
    bool rx_prev_ok, key_phase;
    proven_u64 rx_prev_until, phase_first_pn, key_updates;
    proven_u64 phase_sent, phase_first_sent;    /* 1-RTT packets sent under the current key, and the first of them */
    bool phase_acked;                   /* one of them has been acknowledged: another update may follow */

    /* Connection IDs. */
    proven_quic_cid_t odcid;            /* the client's first Destination Connection ID */
    proven_quic_cid_t client_dcid;      /* a server: the DCID the client's Initials carry (odcid, or the Retry's SCID) */
    proven_quic_cid_t retry_scid;
    bool retried, got_server_initial;
    proven_byte_t token[256];
    proven_size_t token_len;
    qcid_t local[QC_LOCAL_CIDS];
    proven_u64 local_next_seq;
    proven_u64 peer_cid_limit;          /* the peer's active_connection_id_limit */
    qcid_t peer[QC_PEER_CIDS];
    unsigned peer_cur;
    proven_u64 peer_retire_prior_to;
    proven_quic_cid_t peer_initial_scid;
    proven_byte_t static_key[32];

    /* The path. */
    proven_quic_addr_t peer_addr, old_addr;
    bool has_old, validated;
    proven_u64 amp_rx, amp_tx;
    bool challenge_owed, challenge_wait;
    proven_byte_t challenge[8];
    proven_u64 challenge_deadline;
    struct { proven_byte_t data[8]; proven_quic_addr_t to; bool owed; } resp[2];

    /* Flow control. */
    proven_u64 tx_max_data, tx_data;    /* the peer's limit; new stream bytes sent */
    proven_u64 rx_max_data, rx_data, rx_read;   /* ours as last sent; the sum of highest offsets; bytes read */
    proven_u64 rx_conn_window, rx_conn_moved_at;   /* the connection's window as it has grown, and when it last moved */
    proven_u64 tx_written;                       /* stream bytes ever written: less send_buffered, those acknowledged */
    bool max_data_owed;
    proven_u64 send_buffered;           /* stream bytes held for sending, all streams */
    bool send_room_freed;               /* send_buffered went down: every blocked stream is told */

    /* Streams. */
    qstream_t **streams;
    proven_size_t nstreams, streams_cap, cursor, ev_cursor;
    proven_u64 local_opened[2], peer_max_streams[2];    /* [0] bidi, [1] uni */
    proven_u64 peer_opened[2], peer_closed[2], our_max_streams[2];
    bool max_streams_owed[2];
    proven_u64 rx_ranges;               /* received ranges held by all streams */

    /* RFC 9002. */
    proven_u64 latest_rtt, srtt, rttvar, min_rtt;
    bool have_rtt;
    unsigned pto_count;
    proven_u64 loss_timer;
    proven_u64 cwnd, ssthresh, bif, recovery_start;
    bool in_recovery_set;
    proven_u64 pace_tokens, pace_time;
    proven_u64 peer_max_ack_delay, peer_ack_delay_exp;
    bool hs_acked;                      /* a client: something it sent at Handshake was acknowledged */
    proven_u64 pkt_threshold;           /* RFC 9002's kPacketThreshold, raised when a loss proves spurious */
    proven_u64 time_threshold;          /* its kTimeThreshold in eighths of a round trip: 9, up to 16 the same way */
    proven_u64 spurious;

    /* State. */
    int state;
    bool complete, confirmed, established_told, done_owed;
    proven_u64 idle_timeout, idle_deadline;
    bool ack_eliciting_since_rx;
    proven_u64 close_code, close_frame_type, close_deadline;
    bool close_app, close_remote, close_owed;
    bool close_unsent;                  /* closed by the application, whose call has no time: the closing period starts when the close is sent */
    proven_u32 close_rx, close_answered;
    proven_size_t datagram;             /* bytes a sent datagram may take */
    proven_size_t peer_max_udp;
    /* The path's datagram size, found by probing (RFC-0018 S-2; RFC 9000 section 14.3, RFC
     * 8899): sizes up to `pm_good` passed, `pm_bad` (0: none yet) failed; `pm_size` is the next
     * to probe (0: no search now); one probe at a time, kept by its number, not in the records. */
    int pm_state;                       /* 0 not begun, 1 searching, 2 done */
    proven_size_t pm_good, pm_bad, pm_size;
    unsigned pm_tries;
    unsigned pm_big_lost;               /* packets above the configured size lost since one was acknowledged */
    bool pm_out;
    proven_u64 pm_pn, pm_deadline;

    proven_quic_event_t ev[QC_EVENTS];
    unsigned ev_head, ev_n;

    proven_byte_t *scratch;
    proven_size_t scratch_cap;
    proven_quic_stats_t st;
    proven_u64 now;                     /* the time of the call being served */
    proven_quic_tp_t peer_tp;
    bool have_peer_tp;
    proven_u64 retire_q[16];            /* RETIRE_CONNECTION_ID owed */
    unsigned retire_n;
    bool keys_new;                      /* a level's read keys arrived: datagrams held for them are read again */
    proven_u64 idle_start;              /* the last packet received, or the first ack-eliciting one sent after it */
    bool pace_blocked;                  /* something waited for pacing at the last send */
};

static void push_event(proven_quic_conn_t *c, proven_quic_event_t e) {
    if (c->ev_n == QC_EVENTS) return;   /* bounded: the kinds kept here are few per connection */
    c->ev[(c->ev_head + c->ev_n) % QC_EVENTS] = e;
    ++c->ev_n;
}

static void random_bytes(proven_quic_conn_t *c, proven_byte_t *out, proven_size_t n) {
    proven_tls_config_random(c->cfg.tls, out, n);
}

/* The stateless reset token of one of our connection IDs: HMAC-SHA256 under the static key. */
static void reset_token_for(const proven_byte_t key[32], const proven_quic_cid_t *cid, proven_byte_t token[PROVEN_QUIC_RESET_TOKEN]) {
    proven_byte_t mac[PROVEN_HMAC_MAX_SIZE];
    (void)proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ .ptr = key, .size = 32 }, (proven_mem_view_t){ .ptr = cid->id, .size = cid->len }, mac);
    qcopy(token, mac, PROVEN_QUIC_RESET_TOKEN);
}

static void new_local_cid(proven_quic_conn_t *c, qcid_t *e) {
    *e = (qcid_t){ .seq = c->local_next_seq++, .active = true, .has_token = true };
    e->cid.len = QC_CID_LEN;
    random_bytes(c, e->cid.id, QC_CID_LEN);
    reset_token_for(c->static_key, &e->cid, e->token);
    push_event(c, (proven_quic_event_t){ .kind = PROVEN_QUIC_EV_CID_NEW, .cid = e->cid });
}

static bool is_local_cid(const proven_quic_conn_t *c, const proven_quic_cid_t *cid) {
    for (int i = 0; i < QC_LOCAL_CIDS; ++i) if (c->local[i].active && cid_eq(&c->local[i].cid, cid)) return true;
    return false;
}

static const qcid_t *peer_cid(const proven_quic_conn_t *c) { return &c->peer[c->peer_cur]; }

/* ---- Configuration ---- */

static void cfg_defaults(proven_quic_config_t *o, const proven_quic_config_t *in) {
    *o = *in;
    if (!o->max_streams_bidi) o->max_streams_bidi = 100;
    if (!o->max_streams_uni) o->max_streams_uni = 100;
    if (!o->stream_window) o->stream_window = 256 * 1024;
    if (!o->conn_window) o->conn_window = 1024 * 1024;
    if (!o->stream_send_buffer) o->stream_send_buffer = 256 * 1024;
    if (!o->conn_send_buffer) o->conn_send_buffer = 1024 * 1024;
    if (!o->idle_timeout_ms) o->idle_timeout_ms = 30000;
    if (!o->datagram_size) o->datagram_size = PROVEN_QUIC_MIN_INITIAL;
    if (!o->granularity_us) o->granularity_us = 1000;
    if (!o->max_ack_ranges) o->max_ack_ranges = 64;
    if (!o->key_update_packets) o->key_update_packets = 1u << 22;
}

static bool cfg_valid(const proven_quic_config_t *in) {
    return in && proven_alloc_is_valid(in->alloc) && in->tls
        && (in->datagram_size == 0 || (in->datagram_size >= PROVEN_QUIC_MIN_INITIAL && in->datagram_size <= QC_MAX_DGRAM))
        && in->max_streams_bidi <= 1000000 && in->max_streams_uni <= 1000000
        && (in->max_ack_ranges == 0 || in->max_ack_ranges >= 2);
}

/* The transport parameters this side sends. */
static proven_size_t local_params(proven_quic_conn_t *c, proven_byte_t *out, proven_size_t cap) {
    proven_quic_tp_t tp;
    proven_quic_tp_defaults(&tp);
    tp.max_idle_timeout = c->cfg.idle_timeout_ms;
    tp.max_udp_payload_size = QC_MAX_DGRAM;
    tp.initial_max_data = c->cfg.conn_window;
    tp.initial_max_stream_data_bidi_local = c->cfg.stream_window;
    tp.initial_max_stream_data_bidi_remote = c->cfg.stream_window;
    tp.initial_max_stream_data_uni = c->cfg.stream_window;
    tp.initial_max_streams_bidi = c->cfg.max_streams_bidi;
    tp.initial_max_streams_uni = c->cfg.max_streams_uni;
    tp.active_connection_id_limit = QC_PEER_CIDS;
    tp.disable_active_migration = true;
    tp.has_initial_scid = true;
    tp.initial_scid = c->local[0].cid;
    if (c->server) {
        tp.has_original_dcid = true;
        tp.original_dcid = c->odcid;
        if (c->retried) { tp.has_retry_scid = true; tp.retry_scid = c->retry_scid; }
        tp.has_reset_token = true;
        qcopy(tp.reset_token, c->local[0].token, PROVEN_QUIC_RESET_TOKEN);
    }
    return proven_quic_tp_encode(&tp, c->server, (proven_mem_mut_t){ out, cap });
}

/* ---- Time and the close ---- */

static proven_u64 pto_base(const proven_quic_conn_t *c) {
    const proven_u64 srtt = c->have_rtt ? c->srtt : QC_INITIAL_RTT, var = c->have_rtt ? c->rttvar : QC_INITIAL_RTT / 2;
    return srtt + qmax(4 * var, c->cfg.granularity_us) + (c->confirmed ? c->peer_max_ack_delay : 0);
}

static void enter_closed_event(proven_quic_conn_t *c) {
    push_event(c, (proven_quic_event_t){ .kind = PROVEN_QUIC_EV_CLOSED, .code = c->close_code, .app = c->close_app, .remote = c->close_remote });
}

/* This side ends the connection: CONNECTION_CLOSE, then three PTOs answering at a limited rate. */
static void conn_fail(proven_quic_conn_t *c, proven_u64 code, proven_u64 frame_type) {
    if (c->state >= QS_CLOSING) return;
    c->state = QS_CLOSING;
    c->close_code = code; c->close_frame_type = frame_type; c->close_app = false; c->close_remote = false;
    c->close_owed = true;
    c->close_deadline = c->now + 3 * pto_base(c);
    enter_closed_event(c);
}

/* The peer ended it, or reset it: nothing more is sent. */
static void conn_drain(proven_quic_conn_t *c, proven_u64 code, bool app) {
    if (c->state >= QS_DRAINING) return;
    const bool told = c->state == QS_CLOSING;
    c->state = QS_DRAINING;
    c->close_unsent = false;            /* nothing is sent while draining: the period runs from now */
    c->close_deadline = c->now + 3 * pto_base(c);
    if (!told) { c->close_code = code; c->close_app = app; c->close_remote = true; enter_closed_event(c); }
}

static void check_mem(proven_quic_conn_t *c) {
    if (c->mem.oom && c->state < QS_CLOSING) conn_fail(c, PROVEN_QUIC_INTERNAL_ERROR, 0);
}

/* ---- Streams ---- */

static qstream_t *stream_find(const proven_quic_conn_t *c, proven_u64 id) {
    for (proven_size_t i = 0; i < c->nstreams; ++i) if (c->streams[i]->id == id) return c->streams[i];
    return NULL;
}

static bool id_local(const proven_quic_conn_t *c, proven_u64 id) { return (id & 1) == (c->server ? 1u : 0u); }
static unsigned id_kind(proven_u64 id) { return (id & 2) ? 1u : 0u; }

static qstream_t *stream_new(proven_quic_conn_t *c, proven_u64 id) {
    if (c->nstreams == c->streams_cap) {
        proven_size_t cap = c->streams_cap ? c->streams_cap * 2 : 16;
        qstream_t **a = qrealloc(&c->mem, c->streams, c->streams_cap * sizeof *a, cap * sizeof *a, alignof(qstream_t *));
        if (!a) return NULL;
        c->streams = a; c->streams_cap = cap;
    }
    qstream_t *s = qalloc(&c->mem, sizeof *s, alignof(qstream_t));
    if (!s) return NULL;
    qzero(s, sizeof *s);
    s->id = id;
    s->local = id_local(c, id);
    const bool uni = id_kind(id) == 1;
    s->has_tx = !uni || s->local;
    s->has_rx = !uni || !s->local;
    const proven_quic_tp_t *tp = &c->peer_tp;
    s->tx_max = uni ? tp->initial_max_stream_data_uni : s->local ? tp->initial_max_stream_data_bidi_remote : tp->initial_max_stream_data_bidi_local;
    s->rx_max = c->cfg.stream_window;
    s->rx_window = c->cfg.stream_window;
    s->rx_moved_at = c->now;
    c->streams[c->nstreams++] = s;
    return s;
}

/* Free a stream both of whose sides are over and whose news has been told. */
static void stream_maybe_free(proven_quic_conn_t *c, qstream_t *s) {
    const bool tx_over = !s->has_tx || s->tx_reset_acked || sendq_done(&s->tx);
    const bool rx_over = !s->has_rx || s->rx_fin_read || s->rx_reset_reported;
    if (!tx_over || !rx_over || s->ev_opened || s->ev_readable || s->ev_reset || s->ev_stop || s->rx_max_owed || s->tx_reset_owed || s->rx_stop_owed) return;
    for (proven_size_t i = 0; i < c->nstreams; ++i) {
        if (c->streams[i] != s) continue;
        c->streams[i] = c->streams[--c->nstreams];
        break;
    }
    if (c->cursor >= c->nstreams) c->cursor = 0;
    if (c->ev_cursor >= c->nstreams) c->ev_cursor = 0;
    if (s->tx.end != s->tx.base) { c->send_buffered -= s->tx.end - s->tx.base; c->send_room_freed = true; }
    c->rx_ranges -= s->rx.got.n;
    if (!s->local) {
        const unsigned k = id_kind(s->id);
        ++c->peer_closed[k];
        const proven_u64 lim = k ? c->cfg.max_streams_uni : c->cfg.max_streams_bidi;
        /* A stream over is a stream more the peer may open: the latest limit goes in the next packet. */
        c->our_max_streams[k] = c->peer_closed[k] + lim;
        c->max_streams_owed[k] = true;
    }
    sendq_free(&c->mem, &s->tx);
    recvq_free(&c->mem, &s->rx);
    qfree(&c->mem, s, sizeof *s);
}

/* The stream a peer's frame names: opened by it if it may (and those below it of its kind),
 * NULL with *err 0 when it is already over, or NULL with the error to close with. */
static qstream_t *stream_for_peer(proven_quic_conn_t *c, proven_u64 id, proven_u64 *err) {
    *err = 0;
    qstream_t *s = stream_find(c, id);
    if (s) return s;
    const unsigned k = id_kind(id);
    const proven_u64 idx = id >> 2;
    if (id_local(c, id)) {
        if (idx >= c->local_opened[k]) *err = PROVEN_QUIC_STREAM_STATE_ERROR;
        return NULL;
    }
    if (idx >= c->our_max_streams[k]) { *err = PROVEN_QUIC_STREAM_LIMIT_ERROR; return NULL; }
    if (idx < c->peer_opened[k]) return NULL;
    for (proven_u64 i = c->peer_opened[k]; i <= idx; ++i) {
        qstream_t *n = stream_new(c, (i << 2) | (id & 3));
        if (!n) { *err = PROVEN_QUIC_INTERNAL_ERROR; return NULL; }
        n->ev_opened = true;
        c->peer_opened[k] = i + 1;
        s = n;
    }
    return s;
}

/* A window that was emptied to half within two round trips of its last move is too small for
 * the path: it doubles, up to its cap (RFC-0019 section 9, as TCP's receive window grows). A
 * reader that stops reading stops the growth, so what an unread stream can hold stays bounded
 * by the window it had. */
static proven_u64 rx_grow(const proven_quic_conn_t *c, proven_u64 window, proven_u64 moved_at, proven_u64 cap) {
    if (cap <= window || !c->have_rtt || c->now - moved_at >= 2 * c->srtt) return window;
    return 2 * window < cap ? 2 * window : cap;
}

/* Bytes read: the window moves, and MAX_STREAM_DATA / MAX_DATA are owed at half a window. */
static void rx_credit(proven_quic_conn_t *c, qstream_t *s, proven_u64 n) {
    c->rx_read += n;
    if (s && !s->rx.has_final && s->rx_max - s->rx.read < s->rx_window / 2) {
        s->rx_window = rx_grow(c, s->rx_window, s->rx_moved_at, c->cfg.stream_window_max);
        s->rx_max = s->rx.read + s->rx_window;
        s->rx_moved_at = c->now;
        s->rx_max_owed = true;
        /* The connection's window keeps one and a half of the largest stream window, so one
         * stream that grew is not held back by the connection's. */
        const proven_u64 want = s->rx_window + s->rx_window / 2;
        if (c->cfg.conn_window_max && c->rx_conn_window < want) {
            c->rx_conn_window = want < c->cfg.conn_window_max ? want : c->cfg.conn_window_max;
            c->rx_max_data = c->rx_read + c->rx_conn_window;
            c->rx_conn_moved_at = c->now;
            c->max_data_owed = true;
        }
    }
    if (c->rx_max_data - c->rx_read < c->rx_conn_window / 2) {
        c->rx_conn_window = rx_grow(c, c->rx_conn_window, c->rx_conn_moved_at, c->cfg.conn_window_max);
        c->rx_max_data = c->rx_read + c->rx_conn_window;
        c->rx_conn_moved_at = c->now;
        c->max_data_owed = true;
    }
}

/* What a stream, and the connection, may hold written and unacknowledged: the configured
 * buffer, or - when the windows grow - what the peer has granted beyond what it acknowledged,
 * up to the caps, so the send side keeps up with a receive window that grew. */
static proven_u64 tx_room(const proven_quic_conn_t *c, const qstream_t *s) {
    proven_u64 stream_cap = c->cfg.stream_send_buffer, conn_cap = c->cfg.conn_send_buffer;
    if (c->cfg.stream_window_max) {
        const proven_u64 granted = s->tx_max > s->tx.base ? s->tx_max - s->tx.base : 0;
        stream_cap = qmax(stream_cap, qmin(granted, c->cfg.stream_window_max));
    }
    if (c->cfg.conn_window_max) {
        const proven_u64 acked = c->tx_written - c->send_buffered;
        const proven_u64 granted = c->tx_max_data > acked ? c->tx_max_data - acked : 0;
        conn_cap = qmax(conn_cap, qmin(granted, c->cfg.conn_window_max));
    }
    const proven_u64 held = s->tx.end - s->tx.base;
    const proven_u64 room = qmin(stream_cap - qmin(held, stream_cap), conn_cap - qmin(c->send_buffered, conn_cap));
    return qmin(room, Q_STREAM_LIMIT - s->tx.end);
}

/* This side's sending half ends early (a reset of its own, or the peer's STOP_SENDING). */
static void stream_tx_reset(proven_quic_conn_t *c, qstream_t *s, proven_u64 code) {
    if (!s->has_tx || s->tx_reset || sendq_done(&s->tx)) return;
    s->tx_reset = true; s->tx_reset_owed = true;
    s->tx_reset_code = code;
    s->tx_reset_final = s->tx.next;
    if (s->tx.end != s->tx.base) { c->send_buffered -= s->tx.end - s->tx.base; c->send_room_freed = true; }
    sendq_free(&c->mem, &s->tx);
    s->tx.base = s->tx.end = s->tx.next = s->tx_reset_final;
}

/* ---- The peer's transport parameters ---- */

static void apply_peer_params(proven_quic_conn_t *c, const proven_quic_tp_t *tp) {
    c->peer_tp = *tp;
    c->have_peer_tp = true;
    bool ok = tp->has_initial_scid && cid_eq(&tp->initial_scid, &c->peer_initial_scid);
    if (!c->server) {
        ok = ok && tp->has_original_dcid && cid_eq(&tp->original_dcid, &c->odcid);
        ok = ok && (c->retried ? tp->has_retry_scid && cid_eq(&tp->retry_scid, &c->retry_scid) : !tp->has_retry_scid);
        if (tp->has_reset_token) { qcopy(c->peer[0].token, tp->reset_token, PROVEN_QUIC_RESET_TOKEN); c->peer[0].has_token = true; }
    }
    if (!ok) { conn_fail(c, PROVEN_QUIC_TRANSPORT_PARAMETER_ERROR, 0); return; }
    c->tx_max_data = tp->initial_max_data;
    c->peer_max_streams[0] = tp->initial_max_streams_bidi;
    c->peer_max_streams[1] = tp->initial_max_streams_uni;
    c->peer_max_ack_delay = tp->max_ack_delay * 1000;
    c->peer_ack_delay_exp = tp->ack_delay_exponent;
    c->peer_cid_limit = tp->active_connection_id_limit;
    c->peer_max_udp = tp->max_udp_payload_size > 65527 ? 65527 : (proven_size_t)tp->max_udp_payload_size;
    if (c->datagram > c->peer_max_udp) c->datagram = c->peer_max_udp;
    const proven_u64 ours = (proven_u64)c->cfg.idle_timeout_ms * 1000, theirs = tp->max_idle_timeout * 1000;
    c->idle_timeout = theirs == 0 ? ours : qmin(ours, theirs);
}

/* ---- Keys ---- */

static void space_discard(proven_quic_conn_t *c, int lvl);

static void next_keys(proven_quic_conn_t *c, const proven_byte_t *secret, const proven_quic_keys_t *cur, proven_quic_keys_t *out) {
    proven_byte_t next[PROVEN_HMAC_MAX_SIZE];
    qcopy(next, secret, c->secret_len);
    proven_quic_secret_update(c->suite, next);
    proven_quic_keys_set(out, c->suite, next);
    out->hp = cur->hp;                  /* header protection keys are never updated */
    proven_mem_wipe((proven_mem_mut_t){ .ptr = next, .size = sizeof next });
}

/* After the TLS engine has been fed: its output into CRYPTO, its secrets into keys, the
 * peer's parameters, the handshake's end, or its failure. */
static void tls_after(proven_quic_conn_t *c) {
    for (int lvl = 0; lvl < PROVEN_QUIC_LEVELS; ++lvl) {
        proven_mem_view_t out = proven_tls_quic_pending_(c->tls, (proven_quic_level_t)lvl);
        if (out.size == 0) continue;
        if (!c->sp[lvl].discarded && !sendq_write(&c->mem, &c->sp[lvl].crypto_tx, out.ptr, out.size)) { check_mem(c); return; }
        proven_tls_quic_sent_(c->tls, (proven_quic_level_t)lvl, out.size);
    }
    const proven_u64 err = proven_tls_quic_error_(c->tls);
    if (err) { conn_fail(c, err, PROVEN_QUIC_FT_CRYPTO); return; }
    if (!c->suite) c->suite = proven_tls_quic_suite_(c->tls);
    for (int lvl = 1; lvl < PROVEN_QUIC_LEVELS && c->suite; ++lvl) {
        qspace_t *sp = &c->sp[lvl];
        proven_byte_t secret[PROVEN_HMAC_MAX_SIZE];
        bool got = false;
        if (!sp->tx_ok) {
            const proven_size_t n = proven_tls_quic_secret_(c->tls, (proven_quic_level_t)lvl, true, secret);
            if (n) { proven_quic_keys_set(&sp->tx, c->suite, secret); sp->tx_ok = true; got = true; if (lvl == 2) { qcopy(c->tx_secret, secret, n); c->secret_len = n; } }
        }
        if (!sp->rx_ok) {
            const proven_size_t n = proven_tls_quic_secret_(c->tls, (proven_quic_level_t)lvl, false, secret);
            if (n) {
                proven_quic_keys_set(&sp->rx, c->suite, secret); sp->rx_ok = true; got = true; c->keys_new = true;
                if (lvl == 2) { qcopy(c->rx_secret, secret, n); c->secret_len = n; next_keys(c, c->rx_secret, &sp->rx, &c->rx_next); }
            }
        }
        proven_mem_wipe((proven_mem_mut_t){ .ptr = secret, .size = sizeof secret });
        if (got && sp->tx_ok && sp->rx_ok) proven_tls_quic_secret_drop_(c->tls, (proven_quic_level_t)lvl);
    }
    if (!c->have_peer_tp) {
        const proven_quic_tp_t *tp = proven_tls_quic_peer_params_(c->tls);
        if (tp) apply_peer_params(c, tp);
        if (c->state >= QS_CLOSING) return;
    }
    if (!c->complete && proven_tls_is_established(c->tls)) {
        c->complete = true;
        c->state = QS_OPEN;
        if (c->server) {
            c->confirmed = true;
            c->done_owed = true;
            space_discard(c, PROVEN_QUIC_LEVEL_HANDSHAKE);
        }
        /* More connection IDs for the peer, up to its limit and ours. */
        const proven_u64 want = qmin(c->peer_cid_limit, QC_LOCAL_CIDS);
        for (proven_u64 i = 1; i < want; ++i) {
            for (int j = 0; j < QC_LOCAL_CIDS; ++j) if (!c->local[j].active) { new_local_cid(c, &c->local[j]); c->local[j].owed = true; break; }
        }
        if (!c->established_told) { c->established_told = true; push_event(c, (proven_quic_event_t){ .kind = PROVEN_QUIC_EV_ESTABLISHED }); }
    }
}

/* ---- The path's datagram size (RFC-0018 S-2) ---- */

#define QC_PM_PROBES 3                  /* tries before a size counts as too large (RFC 8899's MAX_PROBES) */
#define QC_PM_STEP 32                   /* the search ends when what passed and what failed are this close */
#define QC_PM_BLACK_HOLE 10             /* larger packets lost in a row, none acknowledged: the path stopped carrying them */

/* The next size to probe: the largest allowed first, then halfway between what passed and
 * what failed. */
static void pm_next(proven_quic_conn_t *c) {
    const proven_size_t top = c->peer_max_udp < QC_MAX_DGRAM ? c->peer_max_udp : QC_MAX_DGRAM;
    proven_size_t want = c->pm_bad ? c->pm_good + (c->pm_bad - c->pm_good) / 2 : top;
    if (want > top) want = top;
    c->pm_tries = 0;
    c->pm_out = false;
    if (want <= c->pm_good || (c->pm_bad && c->pm_bad - c->pm_good < QC_PM_STEP)) { c->pm_size = 0; c->pm_state = 2; return; }
    c->pm_size = want;
    c->pm_state = 1;
}

/* A search from the size in use (the handshake confirmed, a new path, a black hole). */
static void pm_begin(proven_quic_conn_t *c) {
    c->pm_good = c->datagram;
    c->pm_bad = 0;
    c->pm_big_lost = 0;
    pm_next(c);
}

static void pm_passed(proven_quic_conn_t *c) {
    c->pm_good = c->pm_size;
    c->datagram = c->pm_size;
    pm_next(c);
}

/* The probe was not acknowledged in time: tried again, or the size is too large. Never a
 * congestion signal (RFC 9000 section 14.4). While probe timeouts pile up nothing at all is
 * getting through, so the loss says nothing of the size: it is not counted against it (an
 * outage would otherwise end the search at the configured size for good). */
static void pm_lost(proven_quic_conn_t *c) {
    c->pm_out = false;
    ++c->st.pmtu_probes_lost;
    if (c->pto_count >= 2 && c->pm_tries > 0) --c->pm_tries;
    if (c->pm_tries < QC_PM_PROBES) return;
    c->pm_bad = c->pm_size;
    pm_next(c);
}

/* ---- RFC 9002: what was sent, what was acknowledged, what was lost ---- */

static void cc_init(proven_quic_conn_t *c) {
    const proven_u64 mds = c->datagram;
    c->cwnd = qmin(10 * mds, qmax(14720, 2 * mds));
    c->ssthresh = QC_INF;
}

static proven_u64 cc_min_window(const proven_quic_conn_t *c) { return 2 * (proven_u64)c->datagram; }

static bool in_recovery(const proven_quic_conn_t *c, proven_u64 sent_time) {
    return c->in_recovery_set && sent_time <= c->recovery_start;
}

static void cc_congestion_event(proven_quic_conn_t *c, proven_u64 sent_time) {
    if (in_recovery(c, sent_time)) return;
    c->in_recovery_set = true;
    c->recovery_start = c->now;
    c->ssthresh = c->cwnd / 2;
    c->cwnd = qmax(c->ssthresh, cc_min_window(c));
}

static void cc_acked(proven_quic_conn_t *c, const qsent_t *p, proven_u64 bif_before) {
    if (!p->in_flight) return;
    if (in_recovery(c, p->time)) return;
    if (bif_before + c->datagram < c->cwnd / 2) return;      /* application limited: the window was not in use */
    if (c->cwnd < c->ssthresh) c->cwnd += p->size;
    else c->cwnd += (proven_u64)c->datagram * p->size / c->cwnd;
    if (c->cwnd > ((proven_u64)1 << 26)) c->cwnd = (proven_u64)1 << 26;
}

static void update_rtt(proven_quic_conn_t *c, proven_u64 latest, proven_u64 ack_delay) {
    c->latest_rtt = latest;
    if (!c->have_rtt) {
        c->have_rtt = true;
        c->min_rtt = latest; c->srtt = latest; c->rttvar = latest / 2;
        return;
    }
    c->min_rtt = qmin(c->min_rtt, latest);
    if (c->confirmed) ack_delay = qmin(ack_delay, c->peer_max_ack_delay);
    proven_u64 adjusted = latest;
    if (latest >= c->min_rtt + ack_delay) adjusted = latest - ack_delay;
    const proven_u64 diff = c->srtt > adjusted ? c->srtt - adjusted : adjusted - c->srtt;
    c->rttvar = (3 * c->rttvar + diff) / 4;
    c->srtt = (7 * c->srtt + adjusted) / 8;
}

static bool peer_validated_us(const proven_quic_conn_t *c) {
    return c->server || c->confirmed || c->hs_acked;
}

static bool amp_blocked(const proven_quic_conn_t *c) {
    return c->server && !c->validated && c->amp_tx + PROVEN_QUIC_MIN_INITIAL > 3 * c->amp_rx;
}

static bool any_ack_eliciting_in_flight(const proven_quic_conn_t *c) {
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) if (c->sp[i].ack_eliciting_in_flight) return true;
    return false;
}

/* RFC 9002 appendix A.8. Returns the time, and the space in *space. */
static proven_u64 pto_time(const proven_quic_conn_t *c, int *space) {
    proven_u64 duration = (c->have_rtt ? c->srtt : QC_INITIAL_RTT) + qmax(4 * (c->have_rtt ? c->rttvar : QC_INITIAL_RTT / 2), c->cfg.granularity_us);
    duration <<= c->pto_count < 20 ? c->pto_count : 20;
    if (!any_ack_eliciting_in_flight(c)) {
        *space = c->sp[1].tx_ok && !c->sp[1].discarded ? 1 : 0;
        return c->now + duration;
    }
    proven_u64 best = QC_INF;
    *space = -1;
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) {
        const qspace_t *sp = &c->sp[i];
        if (!sp->ack_eliciting_in_flight || sp->discarded) continue;
        proven_u64 d = duration;
        if (i == PROVEN_QUIC_LEVEL_APPLICATION) {
            if (!c->confirmed) break;
            d += c->peer_max_ack_delay << (c->pto_count < 20 ? c->pto_count : 20);
        }
        if (sp->last_ack_eliciting_time + d < best) { best = sp->last_ack_eliciting_time + d; *space = i; }
    }
    return best;
}

static void set_loss_timer(proven_quic_conn_t *c) {
    proven_u64 earliest = QC_INF;
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) if (c->sp[i].loss_time && c->sp[i].loss_time < earliest) earliest = c->sp[i].loss_time;
    if (earliest != QC_INF) { c->loss_timer = earliest; return; }
    if (amp_blocked(c)) { c->loss_timer = QC_INF; return; }
    if (!any_ack_eliciting_in_flight(c) && peer_validated_us(c)) { c->loss_timer = QC_INF; return; }
    int space;
    c->loss_timer = pto_time(c, &space);
}

/* A frame's packet was acknowledged. */
static void frame_acked(proven_quic_conn_t *c, int lvl, const qfrec_t *f) {
    switch (f->kind) {
    case FR_CRYPTO:
        sendq_acked(&c->mem, &c->sp[lvl].crypto_tx, f->off, f->len, false);
        if (!c->server && lvl == PROVEN_QUIC_LEVEL_HANDSHAKE) c->hs_acked = true;
        break;
    case FR_STREAM: {
        qstream_t *s = stream_find(c, f->id);
        if (!s || s->tx_reset) break;
        const proven_u64 before = s->tx.base;
        sendq_acked(&c->mem, &s->tx, f->off, f->len, f->fin);
        c->send_buffered -= s->tx.base - before;
        if (s->tx.base != before) { s->ev_writable = true; c->send_room_freed = true; }
        stream_maybe_free(c, s);
        break;
    }
    case FR_RESET: {
        qstream_t *s = stream_find(c, f->id);
        if (s) { s->tx_reset_acked = true; stream_maybe_free(c, s); }
        break;
    }
    default:
        break;
    }
}

/* A frame's packet was lost: what it carried is owed again, as it is now. */
static void frame_lost(proven_quic_conn_t *c, int lvl, const qfrec_t *f) {
    qstream_t *s;
    switch (f->kind) {
    case FR_CRYPTO: sendq_lost(&c->mem, &c->sp[lvl].crypto_tx, f->off, f->len, false); break;
    case FR_STREAM: if ((s = stream_find(c, f->id)) && !s->tx_reset) sendq_lost(&c->mem, &s->tx, f->off, f->len, f->fin); break;
    case FR_RESET: if ((s = stream_find(c, f->id)) && !s->tx_reset_acked) s->tx_reset_owed = true; break;
    case FR_STOP: if ((s = stream_find(c, f->id)) && s->rx_stopped && !s->rx.has_final) s->rx_stop_owed = true; break;
    case FR_MAX_DATA: c->max_data_owed = true; break;
    case FR_MAX_STREAM_DATA: if ((s = stream_find(c, f->id)) && !s->rx.has_final && !s->rx_reset) s->rx_max_owed = true; break;
    case FR_MAX_STREAMS_BIDI: c->max_streams_owed[0] = true; break;
    case FR_MAX_STREAMS_UNI: c->max_streams_owed[1] = true; break;
    case FR_NEW_CID: for (int i = 0; i < QC_LOCAL_CIDS; ++i) if (c->local[i].active && c->local[i].seq == f->id) c->local[i].owed = true; break;
    case FR_RETIRE_CID: if (c->retire_n < 16) c->retire_q[c->retire_n++] = f->id; break;
    case FR_HANDSHAKE_DONE: if (!c->sp[2].discarded) c->done_owed = true; break;
    case FR_PATH_CHALLENGE: if (c->challenge_wait) c->challenge_owed = true; break;   /* until the validation timer ends it */
    default: break;
    }
    check_mem(c);
}

static void sent_gone(proven_quic_conn_t *c, qspace_t *sp, const qsent_t *p) {
    if (p->in_flight) c->bif -= qmin(c->bif, p->size);
    if (p->ack_eliciting && sp->ack_eliciting_in_flight) --sp->ack_eliciting_in_flight;
}

/* RFC 9002 appendix A.10. */
static void detect_lost(proven_quic_conn_t *c, int lvl) {
    qspace_t *sp = &c->sp[lvl];
    sp->loss_time = 0;
    if (sp->largest_acked_plus_one == 0) return;
    const proven_u64 largest = sp->largest_acked_plus_one - 1;
    const proven_u64 delay = qmax(c->time_threshold * qmax(c->latest_rtt, c->have_rtt ? c->srtt : QC_INITIAL_RTT) / 8, c->cfg.granularity_us);
    const proven_u64 lost_before = c->now > delay ? c->now - delay : 0;
    proven_u64 lost_n = 0, first_time = 0, last_time = 0, first_pn = 0, last_pn = 0;
    proven_size_t keep = 0, i = 0;
    for (; i < sp->sent_n; ++i) {
        qsent_t *p = &sp->sent[i];
        if (p->pn > largest) break;
        if (p->time <= lost_before || largest >= p->pn + c->pkt_threshold) {
            for (unsigned j = 0; j < p->nfr; ++j) frame_lost(c, lvl, &p->fr[j]);
            sp->lost_pn[sp->lost_at % 32] = p->pn;
            sp->lost_gap[sp->lost_at % 32] = largest >= p->pn + c->pkt_threshold ? largest - p->pn : QC_INF;
            ++sp->lost_at;
            if (p->ack_eliciting) {
                if (lost_n == 0) { first_time = p->time; first_pn = p->pn; }
                last_time = p->time; last_pn = p->pn;
                ++lost_n;
            }
            sent_gone(c, sp, p);
            ++c->st.packets_lost;
            if (p->size > c->cfg.datagram_size) ++c->pm_big_lost;
            continue;
        }
        const proven_u64 at = p->time + delay;
        if (sp->loss_time == 0 || at < sp->loss_time) sp->loss_time = at;
        if (keep != i) sp->sent[keep] = *p;
        ++keep;
    }
    if (keep != i) {
        for (proven_size_t j = i; j < sp->sent_n; ++j) sp->sent[keep + j - i] = sp->sent[j];
        sp->sent_n -= i - keep;
    }
    /* With a size raised by probing, the path may have stopped carrying it while smaller packets
     * still pass (a black hole, RFC 8899 section 4.3): ten larger packets lost with none of them
     * acknowledged in between. Back to the configured size, and search again. */
    if (c->pm_big_lost >= QC_PM_BLACK_HOLE && c->datagram > c->cfg.datagram_size) { c->datagram = c->cfg.datagram_size; pm_begin(c); }
    if (lost_n == 0) return;
    cc_congestion_event(c, last_time);
    /* Persistent congestion (RFC 9002 section 7.6): lost packets spanning the period, none of
     * those between them acknowledged (here: their numbers run without a gap). */
    if (c->have_rtt && last_pn - first_pn + 1 == lost_n) {
        const proven_u64 period = (c->srtt + qmax(4 * c->rttvar, c->cfg.granularity_us) + c->peer_max_ack_delay) * 3;
        if (last_time - first_time > period) c->cwnd = cc_min_window(c);
    }
}

/* An ACK frame in space `lvl`. Returns the code to close with, or 0. */
static proven_u64 on_ack(proven_quic_conn_t *c, int lvl, const proven_quic_frame_t *f) {
    qspace_t *sp = &c->sp[lvl];
    if (f->largest >= sp->next_pn) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    if (f->largest + 1 > sp->largest_acked_plus_one) sp->largest_acked_plus_one = f->largest + 1;
    if (lvl == PROVEN_QUIC_LEVEL_APPLICATION && f->largest >= c->phase_first_sent) c->phase_acked = true;
    proven_quic_ack_iter_t it = proven_quic_ack_iter(f);
    proven_u64 lo, hi;
    bool newly = false, eliciting = false;
    proven_u64 largest_time = 0;
    bool largest_new = false;
    const proven_u64 bif_before = c->bif;
    /* The ranges come highest first and the records lowest first: each range is found by a
     * search, and its records taken out. */
    while (proven_quic_ack_next(&it, &lo, &hi)) {
        proven_size_t a = 0, b = sp->sent_n;
        while (a < b) { proven_size_t m = (a + b) / 2; if (sp->sent[m].pn < lo) a = m + 1; else b = m; }
        proven_size_t i = a;
        for (; i < sp->sent_n && sp->sent[i].pn <= hi; ++i) {
            const qsent_t *p = &sp->sent[i];
            newly = true;
            if (p->ack_eliciting) eliciting = true;
            if (p->pn == f->largest) { largest_new = true; largest_time = p->time; }
            sent_gone(c, sp, p);
            if (p->size > c->cfg.datagram_size) c->pm_big_lost = 0;
            for (unsigned j = 0; j < p->nfr; ++j) frame_acked(c, lvl, &p->fr[j]);
            cc_acked(c, p, bif_before);
        }
        if (i > a) {
            for (proven_size_t j = i; j < sp->sent_n; ++j) sp->sent[a + j - i] = sp->sent[j];
            sp->sent_n -= i - a;
        }
    }
    /* A packet declared lost and acknowledged after all was only reordered (RFC 9002 section
     * 6.1.1): the packet threshold grows past the distance it was declared at. */
    if (sp->lost_at) {
        it = proven_quic_ack_iter(f);
        while (proven_quic_ack_next(&it, &lo, &hi)) {
            for (unsigned i = 0; i < 32 && i < sp->lost_at; ++i) {
                if (sp->lost_pn[i] < lo || sp->lost_pn[i] > hi) continue;
                if (sp->lost_gap[i] == QC_INF) c->time_threshold = qmin(c->time_threshold + 2, 16);
                else c->pkt_threshold = qmin(qmax(c->pkt_threshold, sp->lost_gap[i] + 2), 256);
                sp->lost_pn[i] = QC_INF;
                ++c->spurious;
            }
        }
    }
    if (lvl == PROVEN_QUIC_LEVEL_APPLICATION && c->pm_out) {
        it = proven_quic_ack_iter(f);
        while (proven_quic_ack_next(&it, &lo, &hi)) if (c->pm_pn >= lo && c->pm_pn <= hi) { c->pm_out = false; pm_passed(c); break; }
    }
    if (!newly) return 0;
    if (largest_new && eliciting) {
        proven_u64 delay = 0;
        if (lvl == PROVEN_QUIC_LEVEL_APPLICATION) {
            const proven_u64 exp = c->peer_ack_delay_exp <= 20 ? c->peer_ack_delay_exp : 20;
            delay = f->ack_delay > (QC_INF >> 21) ? QC_INF >> 21 : f->ack_delay << exp;
        }
        update_rtt(c, c->now - largest_time, delay);
    }
    detect_lost(c, lvl);
    if (peer_validated_us(c)) c->pto_count = 0;
    set_loss_timer(c);
    check_mem(c);
    return 0;
}

/* A space's keys go: what was in flight there no longer counts (RFC 9002 section 6.4). */
static void space_discard(proven_quic_conn_t *c, int lvl) {
    qspace_t *sp = &c->sp[lvl];
    if (sp->discarded) return;
    for (proven_size_t i = 0; i < sp->sent_n; ++i) sent_gone(c, sp, &sp->sent[i]);
    sp->sent_n = 0;
    sp->ack_eliciting_in_flight = 0;
    sp->loss_time = 0;
    sp->probes = 0;
    sp->discarded = true;
    sp->ack_owed = sp->ack_any = false;
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&sp->rx, .size = sizeof sp->rx });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&sp->tx, .size = sizeof sp->tx });
    sp->rx_ok = sp->tx_ok = false;
    sendq_free(&c->mem, &sp->crypto_tx);
    recvq_free(&c->mem, &sp->crypto_rx);
    for (unsigned i = 0; i < sp->pending_n; ++i) qfree(&c->mem, sp->pending[i], sp->pending_len[i]);
    sp->pending_n = 0;
    if (lvl < 2) c->pto_count = 0;
    set_loss_timer(c);
}

/* RFC 9002 appendix A.9. */
static void on_loss_timeout(proven_quic_conn_t *c) {
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) {
        if (c->sp[i].loss_time && c->sp[i].loss_time <= c->now) {
            detect_lost(c, i);
            set_loss_timer(c);
            return;
        }
    }
    int space;
    (void)pto_time(c, &space);
    if (!any_ack_eliciting_in_flight(c)) {
        /* Anti-deadlock: a client the server has not validated sends something. */
        space = c->sp[1].tx_ok && !c->sp[1].discarded ? 1 : 0;
    }
    if (space >= 0 && !c->sp[space].discarded) {
        qspace_t *sp = &c->sp[space];
        sp->probes = 2;
        /* What the oldest packet in flight carried goes out again in the probes. */
        if (sp->sent_n > 0) for (unsigned j = 0; j < sp->sent[0].nfr; ++j) frame_lost(c, space, &sp->sent[0].fr[j]);
        if (space < 2) sendq_resend_all(&c->mem, &sp->crypto_tx);
    }
    ++c->pto_count;
    /* Two probe timeouts in a row with a size raised by probing: a path that drops every
     * full-sized packet acknowledges nothing, so no loss is ever detected and the probes, full
     * of data, are dropped too. Back to the configured size, and search again (RFC 8899
     * section 4.3). */
    if (c->pto_count >= 2 && c->datagram > c->cfg.datagram_size) { c->datagram = c->cfg.datagram_size; pm_begin(c); }
    set_loss_timer(c);
}

/* ---- Frames received ---- */

static proven_u64 on_crypto(proven_quic_conn_t *c, int lvl, const proven_quic_frame_t *f) {
    qspace_t *sp = &c->sp[lvl];
    if (f->offset + f->data.size > sp->crypto_rx.read + QC_CRYPTO_WINDOW) return PROVEN_QUIC_CRYPTO_BUFFER_EXCEEDED;
    if (!recvq_insert(&c->mem, &sp->crypto_rx, f->offset, f->data.ptr, f->data.size)) return PROVEN_QUIC_INTERNAL_ERROR;
    proven_size_t n;
    while ((n = recvq_ready(&sp->crypto_rx)) > 0) {
        (void)proven_tls_quic_feed_(c->tls, (proven_quic_level_t)lvl, (proven_mem_view_t){ .ptr = sp->crypto_rx.buf.p + sp->crypto_rx.buf.head, .size = n });
        (void)recvq_take(&c->mem, &sp->crypto_rx, NULL, n);
        tls_after(c);
        if (c->state >= QS_CLOSING || sp->discarded) break;
    }
    return 0;
}

static proven_u64 on_stream(proven_quic_conn_t *c, const proven_quic_frame_t *f) {
    if (id_local(c, f->id) && id_kind(f->id) == 1) return PROVEN_QUIC_STREAM_STATE_ERROR;
    proven_u64 err;
    qstream_t *s = stream_for_peer(c, f->id, &err);
    if (!s) return err;
    qrecvq_t *q = &s->rx;
    const proven_u64 end = f->offset + f->data.size;
    if (q->has_final && (end > q->final || (f->fin && end != q->final))) return PROVEN_QUIC_FINAL_SIZE_ERROR;
    if (f->fin && end < q->highest) return PROVEN_QUIC_FINAL_SIZE_ERROR;
    if (end > s->rx_max) return PROVEN_QUIC_FLOW_CONTROL_ERROR;
    if (end > q->highest) {
        c->rx_data += end - q->highest;
        if (c->rx_data > c->rx_max_data) return PROVEN_QUIC_FLOW_CONTROL_ERROR;
    }
    if (f->fin) { q->has_final = true; q->final = end; }
    if (s->rx_reset || s->rx_stopped) {
        /* Nothing is kept for a reader that is gone; the bytes still count against the window. */
        if (end > q->highest) q->highest = end;
        return 0;
    }
    const proven_size_t ranges = q->got.n;
    if (!recvq_insert(&c->mem, q, f->offset, f->data.ptr, f->data.size)) return PROVEN_QUIC_INTERNAL_ERROR;
    c->rx_ranges += q->got.n;           /* never their difference in size_t: it can be negative (ranges merge) */
    c->rx_ranges -= ranges;
    if (end > q->highest) q->highest = end;
    if (recvq_ready(q) > 0 || (q->has_final && q->read == q->final)) s->ev_readable = true;
    return 0;
}

static proven_u64 on_reset_stream(proven_quic_conn_t *c, const proven_quic_frame_t *f) {
    if (id_local(c, f->id) && id_kind(f->id) == 1) return PROVEN_QUIC_STREAM_STATE_ERROR;
    proven_u64 err;
    qstream_t *s = stream_for_peer(c, f->id, &err);
    if (!s) return err;
    qrecvq_t *q = &s->rx;
    if ((q->has_final && f->final_size != q->final) || f->final_size < q->highest) return PROVEN_QUIC_FINAL_SIZE_ERROR;
    if (f->final_size > s->rx_max) return PROVEN_QUIC_FLOW_CONTROL_ERROR;
    c->rx_data += f->final_size - q->highest;
    if (c->rx_data > c->rx_max_data) return PROVEN_QUIC_FLOW_CONTROL_ERROR;
    q->highest = f->final_size;
    q->has_final = true; q->final = f->final_size;
    if (s->rx_reset || s->rx_fin_read) return 0;
    s->rx_reset = true;
    s->rx_reset_code = f->value;
    s->ev_reset = true;
    s->ev_readable = false;
    s->rx_max_owed = false;
    /* What was never read is given back to the connection's window. */
    c->rx_ranges -= q->got.n;
    const proven_u64 unread = q->final - q->read;
    recvq_free(&c->mem, q);
    q->read = q->highest = q->final;
    rx_credit(c, NULL, unread);
    return 0;
}

static proven_u64 on_stop_sending(proven_quic_conn_t *c, const proven_quic_frame_t *f) {
    if (!id_local(c, f->id) && id_kind(f->id) == 1) return PROVEN_QUIC_STREAM_STATE_ERROR;
    proven_u64 err;
    qstream_t *s = stream_for_peer(c, f->id, &err);
    if (!s) return err;
    if (!s->tx_reset && !sendq_done(&s->tx)) {
        stream_tx_reset(c, s, f->value);
        s->ev_stop = true; s->ev_stop_code = f->value;
    }
    return 0;
}

static proven_u64 on_new_cid(proven_quic_conn_t *c, const proven_quic_frame_t *f) {
    if (c->peer[c->peer_cur].cid.len == 0) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    for (int i = 0; i < QC_PEER_CIDS; ++i) {
        const qcid_t *e = &c->peer[i];
        if (!e->active) continue;
        if (e->seq == f->id) return cid_eq(&e->cid, &f->cid) ? 0 : PROVEN_QUIC_PROTOCOL_VIOLATION;
        if (cid_eq(&e->cid, &f->cid)) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    }
    if (f->value > c->peer_retire_prior_to) {
        c->peer_retire_prior_to = f->value;
        for (int i = 0; i < QC_PEER_CIDS; ++i) {
            qcid_t *e = &c->peer[i];
            if (!e->active || e->seq >= f->value) continue;
            e->active = false;
            if (c->retire_n == 16) return PROVEN_QUIC_CONNECTION_ID_LIMIT_ERROR;
            c->retire_q[c->retire_n++] = e->seq;
        }
    }
    if (f->id < c->peer_retire_prior_to) {
        if (c->retire_n == 16) return PROVEN_QUIC_CONNECTION_ID_LIMIT_ERROR;
        c->retire_q[c->retire_n++] = f->id;
    } else {
        int slot = -1;
        for (int i = 0; i < QC_PEER_CIDS; ++i) if (!c->peer[i].active) { slot = i; break; }
        if (slot < 0) return PROVEN_QUIC_CONNECTION_ID_LIMIT_ERROR;
        c->peer[slot] = (qcid_t){ .seq = f->id, .cid = f->cid, .active = true, .has_token = true };
        qcopy(c->peer[slot].token, f->reset_token, PROVEN_QUIC_RESET_TOKEN);
    }
    if (!c->peer[c->peer_cur].active) {
        for (int i = 0; i < QC_PEER_CIDS; ++i) if (c->peer[i].active) { c->peer_cur = (unsigned)i; break; }
    }
    return 0;
}

static proven_u64 on_retire_cid(proven_quic_conn_t *c, const proven_quic_frame_t *f, const proven_quic_cid_t *dcid) {
    if (f->id >= c->local_next_seq) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    for (int i = 0; i < QC_LOCAL_CIDS; ++i) {
        qcid_t *e = &c->local[i];
        if (!e->active || e->seq != f->id) continue;
        if (cid_eq(&e->cid, dcid)) return PROVEN_QUIC_PROTOCOL_VIOLATION;
        e->active = false;
        push_event(c, (proven_quic_event_t){ .kind = PROVEN_QUIC_EV_CID_RETIRED, .cid = e->cid });
        if (c->complete) { new_local_cid(c, e); e->owed = true; }
        break;
    }
    return 0;
}

/* One frame of a packet that opened in space `lvl`. */
static proven_u64 on_frame(proven_quic_conn_t *c, int lvl, const proven_quic_frame_t *f, const proven_quic_cid_t *dcid, const proven_quic_addr_t *from) {
    if (lvl < 2 && !proven_quic_frame_in_handshake(f->type)) return PROVEN_QUIC_PROTOCOL_VIOLATION;
    qstream_t *s;
    proven_u64 err;
    switch (f->type) {
    case PROVEN_QUIC_FT_PADDING: case PROVEN_QUIC_FT_PING: return 0;
    case PROVEN_QUIC_FT_ACK: case PROVEN_QUIC_FT_ACK_ECN: return on_ack(c, lvl, f);
    case PROVEN_QUIC_FT_CRYPTO: return on_crypto(c, lvl, f);
    case PROVEN_QUIC_FT_STREAM: return on_stream(c, f);
    case PROVEN_QUIC_FT_RESET_STREAM: return on_reset_stream(c, f);
    case PROVEN_QUIC_FT_STOP_SENDING: return on_stop_sending(c, f);
    case PROVEN_QUIC_FT_NEW_TOKEN: return c->server ? PROVEN_QUIC_PROTOCOL_VIOLATION : 0;   /* no 0-RTT here: not kept */
    case PROVEN_QUIC_FT_MAX_DATA: if (f->value > c->tx_max_data) c->tx_max_data = f->value; return 0;
    case PROVEN_QUIC_FT_MAX_STREAM_DATA:
        if (!id_local(c, f->id) && id_kind(f->id) == 1) return PROVEN_QUIC_STREAM_STATE_ERROR;
        s = stream_for_peer(c, f->id, &err);
        if (s && f->value > s->tx_max) { s->tx_max = f->value; s->ev_writable = true; }
        return err;
    case PROVEN_QUIC_FT_MAX_STREAMS_BIDI: if (f->value > c->peer_max_streams[0]) c->peer_max_streams[0] = f->value; return 0;
    case PROVEN_QUIC_FT_MAX_STREAMS_UNI: if (f->value > c->peer_max_streams[1]) c->peer_max_streams[1] = f->value; return 0;
    case PROVEN_QUIC_FT_DATA_BLOCKED: case PROVEN_QUIC_FT_STREAMS_BLOCKED_BIDI: case PROVEN_QUIC_FT_STREAMS_BLOCKED_UNI: return 0;
    case PROVEN_QUIC_FT_STREAM_DATA_BLOCKED:
        if (id_local(c, f->id) && id_kind(f->id) == 1) return PROVEN_QUIC_STREAM_STATE_ERROR;
        (void)stream_for_peer(c, f->id, &err);
        return err;
    case PROVEN_QUIC_FT_NEW_CONNECTION_ID: return on_new_cid(c, f);
    case PROVEN_QUIC_FT_RETIRE_CONNECTION_ID: return on_retire_cid(c, f, dcid);
    case PROVEN_QUIC_FT_PATH_CHALLENGE: {
        int slot = c->resp[0].owed ? 1 : 0;
        qcopy(c->resp[slot].data, f->data.ptr, 8);
        c->resp[slot].to = *from;
        c->resp[slot].owed = true;
        return 0;
    }
    case PROVEN_QUIC_FT_PATH_RESPONSE: {
        bool same = c->challenge_wait;
        for (int i = 0; i < 8 && same; ++i) same = f->data.ptr[i] == c->challenge[i];
        if (same) { c->challenge_wait = false; c->validated = true; c->has_old = false; }
        return 0;
    }
    case PROVEN_QUIC_FT_CONNECTION_CLOSE: case PROVEN_QUIC_FT_CONNECTION_CLOSE_APP:
        conn_drain(c, f->value, f->type == PROVEN_QUIC_FT_CONNECTION_CLOSE_APP);
        return 0;
    case PROVEN_QUIC_FT_HANDSHAKE_DONE:
        if (c->server) return PROVEN_QUIC_PROTOCOL_VIOLATION;
        if (!c->confirmed) { c->confirmed = true; space_discard(c, PROVEN_QUIC_LEVEL_HANDSHAKE); }
        return 0;
    default:
        return PROVEN_QUIC_FRAME_ENCODING_ERROR;
    }
}

/* ---- Packets received ---- */

/* This side's 1-RTT write key moves to the next phase (its own update, or following the peer's). */
static void tx_key_next(proven_quic_conn_t *c) {
    qspace_t *sp = &c->sp[2];
    const proven_quic_keys_t tx = sp->tx;
    proven_quic_secret_update(c->suite, c->tx_secret);
    proven_quic_keys_set(&sp->tx, c->suite, c->tx_secret);
    sp->tx.hp = tx.hp;                  /* header protection keys are never updated */
    c->key_phase = !c->key_phase;
    c->phase_sent = 0;
    c->phase_first_sent = sp->next_pn;
    c->phase_acked = false;
    ++c->key_updates;
}

/* This side starts a key update: the read key moves too, the old one kept for late packets. */
static void key_update_start(proven_quic_conn_t *c) {
    qspace_t *sp = &c->sp[2];
    c->rx_prev = sp->rx; c->rx_prev_ok = true;
    c->rx_prev_until = c->now + 3 * pto_base(c);
    sp->rx = c->rx_next;
    proven_quic_secret_update(c->suite, c->rx_secret);
    next_keys(c, c->rx_secret, &sp->rx, &c->rx_next);
    c->phase_first_pn = sp->rx_largest_plus_one;
    tx_key_next(c);
}

/* The 1-RTT packet at `pkt` under the right key phase (RFC 9001 section 6). */
static bool open_short(proven_quic_conn_t *c, proven_byte_t *pkt, proven_size_t pn_offset, proven_size_t len, proven_u64 *pn, proven_size_t *hlen, proven_mem_mut_t *payload) {
    qspace_t *sp = &c->sp[2];
    if (len < pn_offset + 4 + 16) return false;
    proven_byte_t mask[5];
    proven_quic_hp_mask(&sp->rx, pkt + pn_offset + 4, mask);
    const bool phase = ((pkt[0] ^ (mask[0] & 0x1f)) & 0x04) != 0;
    if (phase == c->key_phase) return proven_quic_unprotect(&sp->rx, pkt, pn_offset, len, sp->rx_largest_plus_one, pn, hlen, payload);
    /* The other phase: the previous keys for a late packet, or the next ones for an update. */
    if (c->rx_prev_ok && c->now < c->rx_prev_until) {
        if (c->scratch_cap - QC_MAX_DGRAM >= len) {
            proven_byte_t *copy = c->scratch + QC_MAX_DGRAM;   /* the scratch's second half */
            qcopy(copy, pkt, len);
            if (proven_quic_unprotect(&c->rx_prev, copy, pn_offset, len, sp->rx_largest_plus_one, pn, hlen, payload)) {
                /* Only the previous keys open it: a packet sent before the update. */
                qcopy(pkt, copy, len); payload->ptr = pkt + *hlen;
                return true;
            }
        }
    }
    if (!proven_quic_unprotect(&c->rx_next, pkt, pn_offset, len, sp->rx_largest_plus_one, pn, hlen, payload)) return false;
    if (!c->confirmed) { conn_fail(c, PROVEN_QUIC_KEY_UPDATE_ERROR, 0); return false; }
    /* The peer updated its keys: follow, in both directions. */
    c->rx_prev = sp->rx; c->rx_prev_ok = true;
    c->rx_prev_until = c->now + 3 * pto_base(c);
    sp->rx = c->rx_next;
    proven_quic_secret_update(c->suite, c->rx_secret);
    next_keys(c, c->rx_secret, &sp->rx, &c->rx_next);
    c->phase_first_pn = *pn;
    tx_key_next(c);
    return true;
}

/* Hold the rest of a datagram for a space whose read keys have not arrived. */
static void hold(proven_quic_conn_t *c, int lvl, const proven_byte_t *p, proven_size_t n, const proven_quic_addr_t *from) {
    qspace_t *sp = &c->sp[lvl];
    if (sp->pending_n == QC_PENDING || sp->discarded) { ++c->st.packets_dropped; return; }
    proven_byte_t *copy = qalloc(&c->mem, n, 1);
    if (!copy) { c->mem.oom = false; ++c->st.packets_dropped; return; }   /* not fatal: it is dropped */
    qcopy(copy, p, n);
    sp->pending[sp->pending_n] = copy;
    sp->pending_len[sp->pending_n] = n;
    sp->pending_from[sp->pending_n] = *from;
    ++sp->pending_n;
}

static void ack_note(proven_quic_conn_t *c, int lvl, proven_u64 pn, bool eliciting) {
    qspace_t *sp = &c->sp[lvl];
    const bool in_order = sp->rcvd.n == 0 || pn == sp->rcvd.r[sp->rcvd.n - 1].hi;
    (void)rset_add(&c->mem, &sp->rcvd, pn, pn + 1);
    while (sp->rcvd.n > c->cfg.max_ack_ranges) { sp->rx_floor = sp->rcvd.r[0].hi; rset_erase(&sp->rcvd, 0, 1); }
    if (pn + 1 > sp->rx_largest_plus_one) { sp->rx_largest_plus_one = pn + 1; sp->rx_largest_time = c->now; }
    sp->ack_any = true;
    if (!eliciting) return;
    if (!sp->ack_owed) sp->ack_deadline = c->now + 25000;
    sp->ack_owed = true;
    ++sp->ack_count;
    /* Initial and Handshake at once; 1-RTT every second packet, out of order at once. */
    if (lvl < 2 || sp->ack_count >= 2 || !in_order) sp->ack_deadline = c->now;
}

static bool seen(const qspace_t *sp, proven_u64 pn) {
    if (pn < sp->rx_floor) return true;
    for (proven_size_t i = 0; i < sp->rcvd.n; ++i) if (pn >= sp->rcvd.r[i].lo && pn < sp->rcvd.r[i].hi) return true;
    return false;
}

/* One packet of a datagram. Returns: 1 opened and read, 0 dropped, -1 stop reading the
 * datagram (held for keys, or the connection is over). */
static int read_packet(proven_quic_conn_t *c, proven_byte_t *pkt, const proven_quic_hdr_t *h, const proven_quic_addr_t *from, bool *probing_only) {
    int lvl;
    switch (h->type) {
    case PROVEN_QUIC_PKT_INITIAL: lvl = 0; break;
    case PROVEN_QUIC_PKT_HANDSHAKE: lvl = 1; break;
    case PROVEN_QUIC_PKT_SHORT: lvl = 2; break;
    default: return 0;
    }
    /* Whose packet: one of our connection IDs, or - a server's Initial - the client's own choice. */
    if (!is_local_cid(c, &h->dcid) && !(c->server && lvl == 0 && cid_eq(&h->dcid, &c->client_dcid))) return 0;
    if (h->type != PROVEN_QUIC_PKT_SHORT) {
        if (!c->server && lvl == 0) {
            if (c->got_server_initial ? !cid_eq(&h->scid, &c->peer_initial_scid) : false) return 0;
        }
        if (c->server && !cid_eq(&h->scid, &c->peer[0].cid) && !c->complete) return 0;
    }
    qspace_t *sp = &c->sp[lvl];
    if (sp->discarded) return 0;
    if (!sp->rx_ok) { hold(c, lvl, pkt, h->len, from); return -1; }
    proven_u64 pn;
    proven_size_t hlen;
    proven_mem_mut_t payload;
    const bool ok = lvl == 2 ? open_short(c, pkt, h->pn_offset, h->len, &pn, &hlen, &payload)
                             : proven_quic_unprotect(&sp->rx, pkt, h->pn_offset, h->len, sp->rx_largest_plus_one, &pn, &hlen, &payload);
    if (!ok) { ++c->st.packets_dropped; return c->state >= QS_CLOSING ? -1 : 0; }
    if (seen(sp, pn)) { ++c->st.packets_dropped; return 0; }
    if (c->state == QS_CLOSING) {
        /* Only a close from the peer matters now; the rest is answered with our close. */
        ++c->close_rx;
        proven_quic_rd_t r = { payload.ptr, payload.size, true };
        while (r.ok && r.n > 0) {
            proven_quic_frame_t f;
            if (proven_quic_frame_read(&r, &f) != 0) break;
            if (f.type == PROVEN_QUIC_FT_CONNECTION_CLOSE || f.type == PROVEN_QUIC_FT_CONNECTION_CLOSE_APP) { conn_drain(c, f.value, f.type == PROVEN_QUIC_FT_CONNECTION_CLOSE_APP); return -1; }
        }
        if ((c->close_rx & (c->close_rx - 1)) == 0) c->close_owed = true;   /* at the 1st, 2nd, 4th, 8th... */
        return 1;
    }
    /* RFC 9000 section 17: the reserved bits, now unprotected, must be zero. */
    if ((pkt[0] & (lvl == 2 ? 0x18 : 0x0c)) != 0) { conn_fail(c, PROVEN_QUIC_PROTOCOL_VIOLATION, 0); return -1; }
    if (payload.size == 0) { conn_fail(c, PROVEN_QUIC_PROTOCOL_VIOLATION, 0); return -1; }
    /* Before anything is applied: the stream ranges this packet could add stay within the cap. */
    if (lvl == 2) {
        proven_quic_rd_t r = { payload.ptr, payload.size, true };
        proven_u64 streams = 0;
        while (r.ok && r.n > 0) {
            proven_quic_frame_t f;
            if (proven_quic_frame_read(&r, &f) != 0) break;
            if (f.type == PROVEN_QUIC_FT_STREAM) ++streams;
        }
        const proven_u64 cap = c->rx_conn_window / 512 + 2 * ((proven_u64)c->cfg.max_streams_bidi + c->cfg.max_streams_uni);
        if (c->rx_ranges + streams > cap) { ++c->st.packets_dropped; return 0; }
    }
    if (c->server && lvl == 1) {
        c->validated = true;            /* RFC 9000 section 8.1: a Handshake packet validates the address */
        space_discard(c, PROVEN_QUIC_LEVEL_INITIAL);
    }
    if (!c->server && lvl == 0 && !c->got_server_initial) {
        c->got_server_initial = true;
        c->peer_initial_scid = h->scid;
        c->peer[0].cid = h->scid;       /* from here the client writes to the server's choice */
    }
    bool eliciting = false;
    *probing_only = true;
    proven_quic_rd_t r = { payload.ptr, payload.size, true };
    while (r.n > 0 && c->state < QS_CLOSING) {
        proven_quic_frame_t f;
        proven_u64 err = proven_quic_frame_read(&r, &f);
        if (err == 0) {
            if (proven_quic_frame_ack_eliciting(f.type)) eliciting = true;
            if (!proven_quic_frame_probing(f.type)) *probing_only = false;
            err = on_frame(c, lvl, &f, &h->dcid, from);
        }
        if (err) { conn_fail(c, err, f.type); return -1; }
        if (sp->discarded) break;       /* HANDSHAKE_DONE took this space away */
    }
    if (c->state >= QS_DRAINING) return -1;
    if (!sp->discarded) ack_note(c, lvl, pn, eliciting);
    ++c->st.packets_received;
    c->idle_deadline = 0;               /* recomputed below, from now */
    c->ack_eliciting_since_rx = false;
    /* A server sees its client at a new address: passive migration (RFC 9000 section 9.3). */
    if (c->server && lvl == 2 && !*probing_only && pn + 1 == sp->rx_largest_plus_one && !addr_eq(from, &c->peer_addr) && c->confirmed) {
        c->old_addr = c->peer_addr; c->has_old = c->validated;
        c->peer_addr = *from;
        c->validated = false;
        c->amp_rx = c->amp_tx = 0;
        random_bytes(c, c->challenge, 8);
        c->challenge_owed = true; c->challenge_wait = true;
        c->challenge_deadline = c->now + 3 * pto_base(c);
        c->datagram = c->cfg.datagram_size;     /* a new path: its size is found again */
        c->pm_state = 0; c->pm_out = false;
        cc_init(c);
        c->have_rtt = false;
    }
    check_mem(c);
    return 1;
}

static void read_datagram(proven_quic_conn_t *c, proven_byte_t *d, proven_size_t n, const proven_quic_addr_t *from) {
    bool any = false, first = true, short_first = false;
    proven_size_t at = 0;
    while (at < n && c->state < QS_DRAINING) {
        proven_quic_hdr_t h;
        if (!proven_quic_hdr_parse((proven_mem_view_t){ .ptr = d + at, .size = n - at }, QC_CID_LEN, &h)) break;
        if (first) short_first = h.type == PROVEN_QUIC_PKT_SHORT;
        first = false;
        if (!c->server && h.type == PROVEN_QUIC_PKT_RETRY) {
            /* A Retry: once, before anything from the server, checked by its tag (RFC 9000 section 17.2.5). */
            if (c->retried || c->got_server_initial || !cid_eq(&h.dcid, &c->local[0].cid) || cid_eq(&h.scid, &c->odcid) || h.token.size > sizeof c->token
                || !proven_quic_retry_check((proven_mem_view_t){ .ptr = c->odcid.id, .size = c->odcid.len }, (proven_mem_view_t){ .ptr = d + at, .size = h.len })) {
                ++c->st.packets_dropped;
                break;
            }
            c->retried = true;
            c->retry_scid = h.scid;
            c->peer[0].cid = h.scid;
            qcopy(c->token, h.token.ptr, h.token.size);
            c->token_len = h.token.size;
            proven_byte_t cs[32], ss[32];
            proven_quic_initial_secrets((proven_mem_view_t){ .ptr = h.scid.id, .size = h.scid.len }, cs, ss);
            proven_quic_keys_set(&c->sp[0].tx, proven_quic_initial_suite(), cs);
            proven_quic_keys_set(&c->sp[0].rx, proven_quic_initial_suite(), ss);
            proven_mem_wipe((proven_mem_mut_t){ .ptr = cs, .size = 32 });
            proven_mem_wipe((proven_mem_mut_t){ .ptr = ss, .size = 32 });
            qspace_t *sp = &c->sp[0];
            for (proven_size_t i = 0; i < sp->sent_n; ++i) sent_gone(c, sp, &sp->sent[i]);
            sp->sent_n = 0;
            sendq_resend_all(&c->mem, &sp->crypto_tx);
            set_loss_timer(c);
            any = true;
            break;
        }
        if (!c->server && h.type == PROVEN_QUIC_PKT_VN) {
            /* Version negotiation before anything else from the server, naming no version 1. */
            bool v1 = false;
            for (proven_size_t i = 0; i + 4 <= h.token.size; i += 4) {
                const proven_byte_t *v = h.token.ptr + i;
                if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1) v1 = true;
            }
            if (!c->got_server_initial && !c->retried && !v1 && cid_eq(&h.dcid, &c->local[0].cid) && cid_eq(&h.scid, &c->odcid)) {
                c->close_code = PROVEN_QUIC_CONNECTION_REFUSED;
                c->state = QS_DRAINING;
                c->close_deadline = c->now;
                c->close_remote = true;
                enter_closed_event(c);
            }
            break;
        }
        /* RFC 9000 section 14.1: a server drops an Initial in a datagram under 1,200 bytes. */
        if (c->server && h.type == PROVEN_QUIC_PKT_INITIAL && n < PROVEN_QUIC_MIN_INITIAL) { ++c->st.packets_dropped; at += h.len; continue; }
        bool probing_only = false;
        const int r = read_packet(c, d + at, &h, from, &probing_only);
        if (r > 0) any = true;
        if (r < 0) break;
        at += h.len;
    }
    if (any) {
        c->st.bytes_received += n;
        if (c->server && addr_eq(from, &c->peer_addr)) c->amp_rx += n;
        return;
    }
    /* Nothing opened: a stateless reset from the peer ends the connection (RFC 9000 section 10.3.1). */
    if (short_first && n >= 21 && c->state < QS_DRAINING) {
        for (int i = 0; i < QC_PEER_CIDS; ++i) {
            const qcid_t *e = &c->peer[i];
            if (!e->has_token) continue;
            if (proven_mem_equal_ct((proven_mem_view_t){ .ptr = d + n - 16, .size = 16 }, (proven_mem_view_t){ .ptr = e->token, .size = 16 })) {
                c->close_code = 0;
                c->state = QS_DRAINING;
                c->close_deadline = c->now;
                c->close_remote = true;
                enter_closed_event(c);
                return;
            }
        }
    }
}

/* ---- Sending ---- */

static proven_u64 pace_burst(const proven_quic_conn_t *c) { return 10 * (proven_u64)c->datagram; }

/* RFC 9002 section 7.7: tokens at 5/4 of the window per smoothed round trip. */
static void pace_refill(proven_quic_conn_t *c) {
    if (c->pace_time == 0) { c->pace_tokens = pace_burst(c); c->pace_time = c->now; return; }
    if (c->now <= c->pace_time) return;
    const proven_u64 srtt = c->have_rtt && c->srtt ? c->srtt : QC_INITIAL_RTT;
    const proven_u64 cap = qmax(pace_burst(c), c->cwnd / 4);
    const proven_u64 elapsed = c->now - c->pace_time;
    if (elapsed >= 4 * srtt + 1000000) { c->pace_tokens = cap; c->pace_time = c->now; return; }
    const proven_u64 rate_den = 4 * srtt, rate_num = 5 * c->cwnd;          /* bytes per microsecond, as a fraction */
    const proven_u64 add = elapsed * rate_num / rate_den;
    if (add == 0) return;               /* too soon for a whole byte: the time is not used up */
    c->pace_tokens = qmin(c->pace_tokens + add, cap);
    /* The clock moves on by the time those bytes took, so the fractions are not lost. */
    c->pace_time += c->pace_tokens == cap ? elapsed : add * rate_den / rate_num;
}

static proven_u64 pace_next(const proven_quic_conn_t *c) {
    if (c->pace_tokens >= c->datagram) return c->now;
    const proven_u64 srtt = c->have_rtt && c->srtt ? c->srtt : QC_INITIAL_RTT;
    const proven_u64 need = c->datagram - c->pace_tokens, den = 5 * qmax(c->cwnd, 1);
    return c->pace_time + qmax((need * srtt * 4 + den - 1) / den, 1);
}

static bool rec_add(qsent_t *p, proven_u8 kind, proven_u64 id, proven_u64 off, proven_u64 len, bool fin) {
    if (p->nfr == QC_MAX_FRAMES) return false;
    p->fr[p->nfr++] = (qfrec_t){ .kind = kind, .fin = fin, .len = (proven_u16)len, .id = id, .off = off };
    return true;
}

typedef struct {
    proven_size_t at, hlen, pn_len, payload;
    int lvl;
    proven_u64 pn;
    bool eliciting, padded;
    qsent_t rec;
} qpkt_t;

/* Put a frame if the packet has room for one more record (when it needs one). */
static bool put_rec(proven_quic_wr_t *w, qsent_t *rec, const proven_quic_frame_t *f, proven_u8 kind, proven_u64 id) {
    if (kind && rec->nfr == QC_MAX_FRAMES) return false;
    if (!proven_quic_frame_put(w, f)) return false;
    if (kind) (void)rec_add(rec, kind, id, 0, 0, false);
    return true;
}

static void put_ack(proven_quic_conn_t *c, int lvl, proven_quic_wr_t *w) {
    qspace_t *sp = &c->sp[lvl];
    if (!sp->ack_any || sp->rcvd.n == 0) return;
    proven_u64 ranges[64][2];
    proven_size_t n = 0;
    for (proven_size_t i = sp->rcvd.n; i > 0 && n < 64; --i, ++n) { ranges[n][0] = sp->rcvd.r[i - 1].lo; ranges[n][1] = sp->rcvd.r[i - 1].hi - 1; }
    const proven_u64 delay = lvl == 2 && c->now > sp->rx_largest_time ? (c->now - sp->rx_largest_time) >> 3 : 0;
    if (proven_quic_ack_put(w, (const proven_u64 (*)[2])ranges, n, delay) == 0) return;
    sp->ack_any = sp->ack_owed = false;
    sp->ack_count = 0;
}

static void put_crypto(proven_quic_conn_t *c, int lvl, proven_quic_wr_t *w, qsent_t *rec) {
    qsendq_t *q = &c->sp[lvl].crypto_tx;
    while (rec->nfr < QC_MAX_FRAMES && w->cap - w->n > 8) {
        proven_u64 off, len; bool fin, fresh;
        const proven_size_t over = 1 + 8 + 2;
        if (w->cap - w->n <= over) break;
        if (!sendq_next(q, w->cap - w->n - over, QC_INF, &off, &len, &fin, &fresh) || len == 0) break;
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_CRYPTO, .offset = off, .data = { .ptr = sendq_at(q, off), .size = (proven_size_t)len } };
        if (!proven_quic_frame_put(w, &f)) break;
        sendq_sent(&c->mem, q, off, len, false);
        (void)rec_add(rec, FR_CRYPTO, 0, off, len, false);
    }
}

static void put_streams(proven_quic_conn_t *c, proven_quic_wr_t *w, qsent_t *rec) {
    const proven_size_t n = c->nstreams;
    for (proven_size_t k = 0; k < n && rec->nfr < QC_MAX_FRAMES; ++k) {
        if (w->cap - w->n < 24) break;
        qstream_t *s = c->streams[(c->cursor + k) % n];
        if (!s->has_tx || s->tx_reset) continue;
        const proven_u64 conn_left = c->tx_max_data - c->tx_data;
        const proven_u64 limit = qmin(s->tx_max, s->tx.next + conn_left);
        const proven_size_t room = w->cap - w->n;
        const proven_size_t over = proven_quic_stream_overhead(s->id, s->tx.next + room, room);
        if (room <= over) break;
        proven_u64 off, len; bool fin, fresh;
        if (!sendq_next(&s->tx, room - over, limit, &off, &len, &fin, &fresh)) continue;
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_STREAM, .id = s->id, .offset = off, .fin = fin,
                                  .data = { .ptr = len ? sendq_at(&s->tx, off) : NULL, .size = (proven_size_t)len } };
        if (!proven_quic_frame_put(w, &f)) break;
        if (fresh && off + len > s->tx.next) c->tx_data += off + len - s->tx.next;
        sendq_sent(&c->mem, &s->tx, off, len, fin);
        (void)rec_add(rec, FR_STREAM, s->id, off, len, fin);
        c->cursor = (c->cursor + k + 1) % n;
        k = (proven_size_t)-1;          /* start again from the next stream: one frame each in turn */
        if (w->cap - w->n < 24) break;
    }
}

/* Control frames of the 1-RTT space, each with its latest value. */
static void put_control(proven_quic_conn_t *c, proven_quic_wr_t *w, qsent_t *rec) {
    if (c->done_owed) {
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_HANDSHAKE_DONE };
        if (put_rec(w, rec, &f, FR_HANDSHAKE_DONE, 0)) c->done_owed = false;
    }
    if (c->max_data_owed) {
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_MAX_DATA, .value = c->rx_max_data };
        if (put_rec(w, rec, &f, FR_MAX_DATA, 0)) c->max_data_owed = false;
    }
    for (int k = 0; k < 2; ++k) if (c->max_streams_owed[k]) {
        proven_quic_frame_t f = { .type = k ? PROVEN_QUIC_FT_MAX_STREAMS_UNI : PROVEN_QUIC_FT_MAX_STREAMS_BIDI, .value = c->our_max_streams[k] };
        if (put_rec(w, rec, &f, k ? FR_MAX_STREAMS_UNI : FR_MAX_STREAMS_BIDI, 0)) c->max_streams_owed[k] = false;
    }
    while (c->retire_n > 0) {
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_RETIRE_CONNECTION_ID, .id = c->retire_q[c->retire_n - 1] };
        if (!put_rec(w, rec, &f, FR_RETIRE_CID, f.id)) break;
        --c->retire_n;
    }
    for (int i = 0; i < QC_LOCAL_CIDS; ++i) {
        qcid_t *e = &c->local[i];
        if (!e->active || !e->owed) continue;
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_NEW_CONNECTION_ID, .id = e->seq, .value = 0, .cid = e->cid };
        qcopy(f.reset_token, e->token, PROVEN_QUIC_RESET_TOKEN);
        if (put_rec(w, rec, &f, FR_NEW_CID, e->seq)) e->owed = false;
    }
    if (c->challenge_owed) {
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_PATH_CHALLENGE, .data = { .ptr = c->challenge, .size = 8 } };
        if (put_rec(w, rec, &f, FR_PATH_CHALLENGE, 0)) c->challenge_owed = false;
    }
    for (int i = 0; i < 2; ++i) if (c->resp[i].owed && addr_eq(&c->resp[i].to, &c->peer_addr)) {
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_PATH_RESPONSE, .data = { .ptr = c->resp[i].data, .size = 8 } };
        if (put_rec(w, rec, &f, 0, 0)) c->resp[i].owed = false;
    }
    for (proven_size_t i = 0; i < c->nstreams; ++i) {
        qstream_t *s = c->streams[i];
        if (s->tx_reset_owed) {
            proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_RESET_STREAM, .id = s->id, .value = s->tx_reset_code, .final_size = s->tx_reset_final };
            if (put_rec(w, rec, &f, FR_RESET, s->id)) s->tx_reset_owed = false;
        }
        if (s->rx_stop_owed) {
            proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_STOP_SENDING, .id = s->id, .value = s->rx_stop_code };
            if (put_rec(w, rec, &f, FR_STOP, s->id)) s->rx_stop_owed = false;
        }
        if (s->rx_max_owed) {
            proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_MAX_STREAM_DATA, .id = s->id, .value = s->rx_max };
            if (put_rec(w, rec, &f, FR_MAX_STREAM_DATA, s->id)) s->rx_max_owed = false;
        }
    }
}

static bool space_has_eliciting(const proven_quic_conn_t *c, int lvl) {
    const qspace_t *sp = &c->sp[lvl];
    proven_u64 off, len; bool fin, fresh;
    if (sp->probes) return true;
    if (sendq_next(&sp->crypto_tx, 1000, QC_INF, &off, &len, &fin, &fresh) && len) return true;
    if (lvl < 2 || c->state != QS_OPEN) return false;
    if (c->done_owed || c->max_data_owed || c->max_streams_owed[0] || c->max_streams_owed[1] || c->retire_n || c->challenge_owed) return true;
    for (int i = 0; i < QC_LOCAL_CIDS; ++i) if (c->local[i].active && c->local[i].owed) return true;
    for (int i = 0; i < 2; ++i) if (c->resp[i].owed && addr_eq(&c->resp[i].to, &c->peer_addr)) return true;
    for (proven_size_t i = 0; i < c->nstreams; ++i) {
        const qstream_t *s = c->streams[i];
        if (s->tx_reset_owed || s->rx_stop_owed || s->rx_max_owed) return true;
        if (s->has_tx && !s->tx_reset && sendq_next(&s->tx, 1000, qmin(s->tx_max, s->tx.next + (c->tx_max_data - c->tx_data)), &off, &len, &fin, &fresh)) return true;
    }
    return false;
}

/* One packet of space `lvl` at buf[0..room): its plaintext, header in place. */
static bool build_packet(proven_quic_conn_t *c, int lvl, proven_byte_t *buf, proven_size_t room, bool eliciting_ok, bool close, qpkt_t *pk) {
    qspace_t *sp = &c->sp[lvl];
    *pk = (qpkt_t){ .lvl = lvl, .pn = sp->next_pn };
    pk->pn_len = proven_quic_pn_len(sp->next_pn, sp->largest_acked_plus_one);
    const proven_quic_cid_t *dcid = &peer_cid(c)->cid;
    proven_mem_mut_t out = { buf, room };
    if (lvl == 2) pk->hlen = proven_quic_short_header_put(dcid, c->key_phase, pk->pn, pk->pn_len, out);
    else pk->hlen = proven_quic_long_header_put(lvl == 0 ? PROVEN_QUIC_PKT_INITIAL : PROVEN_QUIC_PKT_HANDSHAKE, dcid, &c->local[0].cid,
                                                (proven_mem_view_t){ .ptr = c->token, .size = c->server ? 0 : c->token_len }, pk->pn, pk->pn_len, 0, out);
    if (pk->hlen == 0 || room < pk->hlen + 16 + 8) return false;
    proven_quic_wr_t w = { buf + pk->hlen, 0, room - pk->hlen - 16, true };
    if (close) {
        const bool app = c->close_app && lvl == 2;
        proven_quic_frame_t f = { .type = app ? PROVEN_QUIC_FT_CONNECTION_CLOSE_APP : PROVEN_QUIC_FT_CONNECTION_CLOSE,
                                  .value = c->close_app && lvl < 2 ? PROVEN_QUIC_APPLICATION_ERROR : c->close_code,
                                  .frame_type = c->close_app ? 0 : c->close_frame_type };
        if (!proven_quic_frame_put(&w, &f)) return false;
    } else {
        /* A packet that holds only acknowledgements is not acknowledged (RFC 9000 section 13.2.1):
         * what came is acknowledged when an ack-eliciting packet is owed it, or with frames that
         * are going anyway - never alone, or two ends would answer each other's ACKs for ever. */
        const bool ack_due = sp->ack_owed && (lvl < 2 || sp->ack_deadline <= c->now);
        if (ack_due || (eliciting_ok && sp->ack_any && space_has_eliciting(c, lvl))) put_ack(c, lvl, &w);
        if (eliciting_ok) {
            put_crypto(c, lvl, &w, &pk->rec);
            if (lvl == 2 && c->state == QS_OPEN) { put_control(c, &w, &pk->rec); put_streams(c, &w, &pk->rec); }
            if (sp->probes && pk->rec.nfr == 0) {
                proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_PING };
                if (proven_quic_frame_put(&w, &f)) (void)rec_add(&pk->rec, FR_PING, 0, 0, 0, false);
            }
            pk->eliciting = pk->rec.nfr > 0;
            for (unsigned i = 0; i < pk->rec.nfr; ++i) if (pk->rec.fr[i].kind == FR_PING) pk->eliciting = true;
        }
    }
    if (w.n == 0) return false;
    /* Room for the header protection sample. */
    while (pk->pn_len + w.n < 4) { w.p[w.n++] = 0; pk->padded = true; }
    pk->payload = w.n;
    return true;
}

/* Header protection and the AEAD over every packet of the datagram; the records kept. */
static proven_size_t seal(proven_quic_conn_t *c, proven_byte_t *buf, qpkt_t *pk, int n) {
    proven_size_t total = 0;
    for (int i = 0; i < n; ++i) {
        qpkt_t *p = &pk[i];
        qspace_t *sp = &c->sp[p->lvl];
        proven_byte_t *at = buf + p->at;
        if (p->lvl < 2) {
            /* The Length field, now that the payload's size is known. */
            const proven_size_t length = p->pn_len + p->payload + 16;
            at[p->hlen - p->pn_len - 2] = (proven_byte_t)(0x40 | (length >> 8));
            at[p->hlen - p->pn_len - 1] = (proven_byte_t)length;
        }
        const proven_size_t len = proven_quic_protect(&sp->tx, at, p->hlen, p->pn_len, p->pn, p->payload);
        total = p->at + len;
        ++sp->next_pn;
        if (p->lvl == 2) ++c->phase_sent;
        ++c->st.packets_sent;
        const bool in_flight = p->eliciting || p->padded;
        if (in_flight && sp->sent_n < QC_MAX_SENT) {
            if (sp->sent_n == sp->sent_cap) {
                const proven_size_t cap = sp->sent_cap ? sp->sent_cap * 2 : 64;
                qsent_t *a = qrealloc(&c->mem, sp->sent, sp->sent_cap * sizeof *a, cap * sizeof *a, alignof(qsent_t));
                if (!a) { check_mem(c); continue; }
                sp->sent = a; sp->sent_cap = cap;
            }
            qsent_t *r = &sp->sent[sp->sent_n++];
            *r = p->rec;
            r->pn = p->pn; r->time = c->now; r->size = (proven_u32)len;
            r->ack_eliciting = p->eliciting; r->in_flight = true;
            c->bif += len;
            if (p->eliciting) {
                ++sp->ack_eliciting_in_flight;
                sp->last_ack_eliciting_time = c->now;
                if (sp->probes) --sp->probes;
                if (!c->ack_eliciting_since_rx) { c->ack_eliciting_since_rx = true; c->idle_start = c->now; }
            }
        }
    }
    return total;
}

static proven_size_t send_close(proven_quic_conn_t *c, proven_byte_t *buf, proven_size_t room) {
    qpkt_t pk[3];
    int n = 0;
    proven_size_t at = 0;
    for (int lvl = 0; lvl < PROVEN_QUIC_LEVELS; ++lvl) {
        if (!c->sp[lvl].tx_ok || c->sp[lvl].discarded) continue;
        if (lvl == 2 && !c->complete && c->sp[1].tx_ok && !c->sp[1].discarded) continue;   /* the peer may not read 1-RTT yet */
        if (!build_packet(c, lvl, buf + at, room - at, false, true, &pk[n])) continue;
        pk[n].at = at;
        at += pk[n].hlen + pk[n].payload + 16;
        ++n;
    }
    if (n == 0) return 0;
    if (!c->server && pk[0].lvl == 0 && at < PROVEN_QUIC_MIN_INITIAL && room >= PROVEN_QUIC_MIN_INITIAL) {
        qpkt_t *last = &pk[n - 1];
        const proven_size_t add = PROVEN_QUIC_MIN_INITIAL - at;
        for (proven_size_t i = 0; i < add; ++i) buf[last->at + last->hlen + last->payload + i] = 0;
        last->payload += add;
    }
    return seal(c, buf, pk, n);
}

proven_err_t proven_quic_conn_send(proven_quic_conn_t *c, proven_u64 now, proven_mem_mut_t out, proven_size_t *len, proven_quic_addr_t *to) {
    *len = 0;
    if (!c || out.size < QC_MAX_DGRAM) return PROVEN_ERR_INVALID_ARG;
    c->now = now;
    *to = c->peer_addr;
    c->pace_blocked = false;
    if (c->state == QS_DRAINING || c->state == QS_CLOSED) return PROVEN_OK;
    proven_size_t budget = c->datagram;
    if (c->server && !c->validated) {
        const proven_u64 allow = 3 * c->amp_rx > c->amp_tx ? 3 * c->amp_rx - c->amp_tx : 0;
        if (allow < budget) budget = (proven_size_t)allow;
    }
    if (c->state == QS_CLOSING) {
        /* The closing period starts at the first chance to send the close, whether or not it can
         * go: to an address that is owed nothing more (the amplification limit) it cannot, and
         * the connection must still end - three PTOs, not the idle timeout. */
        if (c->close_unsent) { c->close_unsent = false; c->close_deadline = now + 3 * pto_base(c); }
        if (!c->close_owed || budget < 64) return PROVEN_OK;
        c->close_owed = false;
        *len = send_close(c, out.ptr, budget);
        c->amp_tx += *len;
        return PROVEN_OK;
    }
    /* A PATH_RESPONSE owed on another path goes there, alone and padded (RFC 9000 section 8.2.2). */
    for (int i = 0; i < 2; ++i) {
        if (!c->resp[i].owed || addr_eq(&c->resp[i].to, &c->peer_addr) || !c->sp[2].tx_ok) continue;
        qpkt_t pk;
        if (!build_packet(c, 2, out.ptr, c->datagram, false, false, &pk)) {
            /* no ACK to carry: a packet of its own */
            pk = (qpkt_t){ .lvl = 2, .pn = c->sp[2].next_pn, .pn_len = proven_quic_pn_len(c->sp[2].next_pn, c->sp[2].largest_acked_plus_one) };
            pk.hlen = proven_quic_short_header_put(&peer_cid(c)->cid, c->key_phase, pk.pn, pk.pn_len, (proven_mem_mut_t){ out.ptr, out.size });
        }
        proven_quic_wr_t w = { out.ptr + pk.hlen, pk.payload, c->datagram - pk.hlen - 16, true };
        proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_PATH_RESPONSE, .data = { .ptr = c->resp[i].data, .size = 8 } };
        (void)proven_quic_frame_put(&w, &f);
        while (w.n < w.cap) w.p[w.n++] = 0;
        pk.payload = w.n; pk.padded = true;
        c->resp[i].owed = false;
        *to = c->resp[i].to;
        *len = seal(c, out.ptr, &pk, 1);
        return PROVEN_OK;
    }
    /* A PMTU probe (RFC-0018 S-2): once the handshake is confirmed and the path validated, a
     * datagram of the size probed, PING and PADDING, alone. Not recorded as in flight: its loss
     * is no congestion, and a probe timeout is not owed for it. */
    if (c->confirmed && c->state == QS_OPEN && c->validated && c->sp[2].tx_ok && !c->challenge_wait) {
        if (c->pm_state == 0) pm_begin(c);
        if (c->pm_state == 1 && !c->pm_out && c->pm_size <= out.size) {
            qpkt_t pk;
            if (!build_packet(c, 2, out.ptr, c->pm_size, false, false, &pk)) {
                pk = (qpkt_t){ .lvl = 2, .pn = c->sp[2].next_pn, .pn_len = proven_quic_pn_len(c->sp[2].next_pn, c->sp[2].largest_acked_plus_one) };
                pk.hlen = proven_quic_short_header_put(&peer_cid(c)->cid, c->key_phase, pk.pn, pk.pn_len, (proven_mem_mut_t){ out.ptr, out.size });
            }
            proven_quic_wr_t w = { out.ptr + pk.hlen, pk.payload, c->pm_size - pk.hlen - 16, true };
            proven_quic_frame_t f = { .type = PROVEN_QUIC_FT_PING };
            (void)proven_quic_frame_put(&w, &f);
            if (w.ok && pk.hlen > 0) {
                while (w.n < w.cap) w.p[w.n++] = 0;
                pk.payload = w.n; pk.eliciting = false; pk.padded = false;
                c->pm_pn = pk.pn;
                *len = seal(c, out.ptr, &pk, 1);
                c->pm_out = true;
                ++c->pm_tries;
                c->pm_deadline = now + (pto_base(c) << (c->pto_count < 6 ? c->pto_count : 6));
                ++c->st.pmtu_probes;
                c->amp_tx += *len;
                c->st.bytes_sent += *len;
                return PROVEN_OK;
            }
        }
    }
    /* RFC 9001 section 6.6: a key is updated well before the AEAD's limit - once the handshake
     * is confirmed and a packet under the current key has been acknowledged. */
    if (c->confirmed && c->phase_sent >= c->cfg.key_update_packets && c->phase_acked && c->sp[2].tx_ok && c->sp[2].rx_ok) key_update_start(c);
    /* What may be ack-eliciting now: the window, pacing, a probe. */
    pace_refill(c);
    const bool window = c->bif + c->datagram <= c->cwnd;
    const bool paced = c->pace_tokens >= c->datagram;
    qpkt_t pk[3];
    int n = 0;
    proven_size_t at = 0;
    bool initial = false, initial_eliciting = false;
    for (int lvl = 0; lvl < PROVEN_QUIC_LEVELS; ++lvl) {
        qspace_t *sp = &c->sp[lvl];
        if (!sp->tx_ok || sp->discarded) continue;
        if (lvl == 2 && !c->complete && !c->server) continue;   /* a client sends no 1-RTT before its Finished */
        bool ok = sp->probes > 0 || (window && paced && sp->sent_n < QC_MAX_SENT);
        /* Initial and Handshake CRYPTO data is not held behind the window: it is a few kilobytes,
         * and no 1-RTT packet can be acknowledged before it arrives. Held, a lost Finished
         * behind 1-RTT data that fills the window would wait for an acknowledgement that cannot
         * come, with no probe timer while the handshake is unconfirmed (RFC 9002 section 6.2.1). */
        if (lvl < 2 && sp->sent_n < QC_MAX_SENT) {
            proven_u64 off, len; bool fin, fresh;
            if (sendq_next(&sp->crypto_tx, 1000, QC_INF, &off, &len, &fin, &fresh) && len) ok = true;
        }
        if (c->server && lvl == 0 && budget < PROVEN_QUIC_MIN_INITIAL) ok = false;   /* an ack-eliciting Initial is padded */
        if (!ok && space_has_eliciting(c, lvl) && window && !paced) c->pace_blocked = true;
        if (budget - at < 64) break;
        if (!build_packet(c, lvl, out.ptr + at, budget - at, ok, false, &pk[n])) continue;
        pk[n].at = at;
        at += pk[n].hlen + pk[n].payload + 16;
        if (lvl == 0) { initial = true; if (pk[n].eliciting) initial_eliciting = true; }
        ++n;
    }
    if (n == 0) return PROVEN_OK;
    /* RFC 9000 section 14.1: a client's datagram with an Initial, and a server's with an
     * ack-eliciting one, is at least 1,200 bytes - padding in its last packet. */
    if (((!c->server && initial) || (c->server && initial_eliciting)) && at < PROVEN_QUIC_MIN_INITIAL) {
        if (budget < PROVEN_QUIC_MIN_INITIAL) return PROVEN_OK;     /* not reached: such a packet was not built */
        qpkt_t *last = &pk[n - 1];
        const proven_size_t add = PROVEN_QUIC_MIN_INITIAL - at;
        for (proven_size_t i = 0; i < add; ++i) out.ptr[last->at + last->hlen + last->payload + i] = 0;
        last->payload += add;
        last->padded = true;
    }
    bool sent_handshake = false;
    for (int i = 0; i < n; ++i) if (pk[i].lvl == 1) sent_handshake = true;
    *len = seal(c, out.ptr, pk, n);
    c->amp_tx += *len;
    c->st.bytes_sent += *len;
    c->pace_tokens -= qmin(c->pace_tokens, *len);
    /* A client drops its Initial keys when it first sends at Handshake (RFC 9001 section 4.9.1). */
    if (!c->server && sent_handshake) space_discard(c, PROVEN_QUIC_LEVEL_INITIAL);
    set_loss_timer(c);
    check_mem(c);
    return PROVEN_OK;
}

static proven_u64 idle_end(const proven_quic_conn_t *c) {
    if (c->idle_timeout == 0) return QC_INF;
    return c->idle_start + qmax(c->idle_timeout, 3 * pto_base(c));
}

proven_u64 proven_quic_conn_deadline(const proven_quic_conn_t *c) {
    if (!c || c->state == QS_CLOSED) return QC_INF;
    if (c->state >= QS_CLOSING) return c->close_unsent ? idle_end(c) : c->close_deadline;
    proven_u64 d = qmin(c->loss_timer, idle_end(c));
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) if (c->sp[i].ack_owed && !c->sp[i].discarded) d = qmin(d, c->sp[i].ack_deadline);
    if (c->pace_blocked) d = qmin(d, pace_next(c));
    if (c->challenge_wait) d = qmin(d, c->challenge_deadline);
    if (c->pm_out) d = qmin(d, c->pm_deadline);
    return d;
}

void proven_quic_conn_timeout(proven_quic_conn_t *c, proven_u64 now) {
    if (!c || c->state == QS_CLOSED) return;
    c->now = now;
    if (c->state >= QS_CLOSING) {
        /* A close not sent yet - none could be, the amplification limit, say - ends with the idle time. */
        if (c->close_unsent ? now >= idle_end(c) : now >= c->close_deadline) c->state = QS_CLOSED;
        return;
    }
    if (now >= idle_end(c)) {
        /* Silence: closed without a word (RFC 9000 section 10.1). */
        c->state = QS_CLOSED;
        c->close_code = PROVEN_QUIC_NO_ERROR; c->close_app = false; c->close_remote = false;
        enter_closed_event(c);
        return;
    }
    if (c->challenge_wait && now >= c->challenge_deadline) {
        c->challenge_wait = false;
        if (c->has_old) { c->peer_addr = c->old_addr; c->validated = true; c->has_old = false; }
    }
    if (c->pm_out && now >= c->pm_deadline) pm_lost(c);
    if (c->loss_timer != QC_INF && now >= c->loss_timer) on_loss_timeout(c);
}

/* Read again what was held for keys that have now arrived. */
static void replay_held(proven_quic_conn_t *c) {
    for (int guard = 0; guard < 4 && c->keys_new; ++guard) {
        c->keys_new = false;
        for (int lvl = 1; lvl < PROVEN_QUIC_LEVELS; ++lvl) {
            qspace_t *sp = &c->sp[lvl];
            if (!sp->rx_ok || sp->pending_n == 0) continue;
            const unsigned n = sp->pending_n;
            proven_byte_t *held[QC_PENDING]; proven_size_t lens[QC_PENDING]; proven_quic_addr_t from[QC_PENDING];
            for (unsigned i = 0; i < n; ++i) { held[i] = sp->pending[i]; lens[i] = sp->pending_len[i]; from[i] = sp->pending_from[i]; }
            sp->pending_n = 0;
            for (unsigned i = 0; i < n; ++i) {
                if (c->state < QS_DRAINING) read_datagram(c, held[i], lens[i], &from[i]);
                qfree(&c->mem, held[i], lens[i]);
            }
        }
    }
}

proven_err_t proven_quic_conn_receive(proven_quic_conn_t *c, proven_mem_view_t d, const proven_quic_addr_t *from, proven_u64 now) {
    if (!c || !from || (d.size && !d.ptr)) return PROVEN_ERR_INVALID_ARG;
    c->now = now;
    if (c->state == QS_DRAINING || c->state == QS_CLOSED || d.size == 0 || d.size > 65527) return PROVEN_OK;
    if (!c->server && !addr_eq(from, &c->peer_addr)) { ++c->st.packets_dropped; return PROVEN_OK; }   /* a server does not move */
    if (c->scratch_cap < 2 * QC_MAX_DGRAM || c->scratch_cap < d.size) {
        const proven_size_t cap = d.size > 2 * QC_MAX_DGRAM ? d.size + QC_MAX_DGRAM : 2 * QC_MAX_DGRAM;
        proven_byte_t *p = qrealloc(&c->mem, c->scratch, c->scratch_cap, cap, 1);
        if (!p) { c->mem.oom = false; return PROVEN_ERR_NOMEM; }
        c->scratch = p; c->scratch_cap = cap;
    }
    qcopy(c->scratch, d.ptr, d.size);
    const proven_u64 before = c->st.packets_received;
    read_datagram(c, c->scratch, d.size, from);
    if (c->st.packets_received != before) c->idle_start = now;
    replay_held(c);
    if (c->server && !addr_eq(from, &c->peer_addr) && c->st.packets_received != before && !c->validated) c->amp_rx += 0;
    set_loss_timer(c);
    check_mem(c);
    return PROVEN_OK;
}

bool proven_quic_conn_poll(proven_quic_conn_t *c, proven_quic_event_t *ev) {
    if (!c || !ev) return false;
    if (c->ev_n > 0) {
        *ev = c->ev[c->ev_head];
        c->ev_head = (c->ev_head + 1) % QC_EVENTS;
        --c->ev_n;
        return true;
    }
    if (c->send_room_freed) {
        /* Room for one stream is room for any that the connection's limit held back. */
        c->send_room_freed = false;
        for (proven_size_t i = 0; i < c->nstreams; ++i) {
            qstream_t *s = c->streams[i];
            if (s->tx_blocked && !s->tx_reset) s->ev_writable = true;
            s->tx_blocked = false;
        }
    }
    for (proven_size_t k = 0; k < c->nstreams; ++k) {
        const proven_size_t i = (c->ev_cursor + k) % c->nstreams;
        qstream_t *s = c->streams[i];
        *ev = (proven_quic_event_t){ .stream_id = s->id };
        if (s->ev_opened) { s->ev_opened = false; ev->kind = PROVEN_QUIC_EV_STREAM_OPENED; }
        else if (s->ev_reset) { s->ev_reset = false; s->rx_reset_reported = true; ev->kind = PROVEN_QUIC_EV_STREAM_RESET; ev->code = s->rx_reset_code; }
        else if (s->ev_stop) { s->ev_stop = false; ev->kind = PROVEN_QUIC_EV_STOP_SENDING; ev->code = s->ev_stop_code; }
        else if (s->ev_readable) { s->ev_readable = false; ev->kind = PROVEN_QUIC_EV_STREAM_READABLE; }
        else if (s->ev_writable) { s->ev_writable = false; ev->kind = PROVEN_QUIC_EV_STREAM_WRITABLE; }
        else continue;
        c->ev_cursor = (i + 1) % (c->nstreams ? c->nstreams : 1);
        stream_maybe_free(c, s);
        return true;
    }
    return false;
}

/* ---- Streams, for the application ---- */

proven_err_t proven_quic_stream_open(proven_quic_conn_t *c, bool bidi, proven_u64 *id) {
    if (!c || !id) return PROVEN_ERR_INVALID_ARG;
    if (c->state != QS_OPEN) return PROVEN_ERR_INVALID_STATE;
    const unsigned k = bidi ? 0 : 1;
    if (c->local_opened[k] >= c->peer_max_streams[k]) return PROVEN_ERR_BUSY;
    const proven_u64 sid = (c->local_opened[k] << 2) | (bidi ? 0u : 2u) | (c->server ? 1u : 0u);
    qstream_t *s = stream_new(c, sid);
    if (!s) { c->mem.oom = false; return PROVEN_ERR_NOMEM; }
    ++c->local_opened[k];
    *id = sid;
    return PROVEN_OK;
}

proven_err_t proven_quic_stream_write(proven_quic_conn_t *c, proven_u64 id, proven_mem_view_t data, bool fin, proven_size_t *taken) {
    if (!c || !taken || (data.size && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    *taken = 0;
    if (c->state != QS_OPEN) return PROVEN_ERR_INVALID_STATE;
    qstream_t *s = stream_find(c, id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if (!s->has_tx || s->tx_reset || s->tx.fin) return PROVEN_ERR_INVALID_STATE;
    const proven_u64 room = tx_room(c, s);
    const proven_size_t n = data.size < room ? data.size : (proven_size_t)room;
    if (n && !sendq_write(&c->mem, &s->tx, data.ptr, n)) { c->mem.oom = false; return PROVEN_ERR_NOMEM; }
    c->send_buffered += n;
    c->tx_written += n;
    *taken = n;
    if (fin && n == data.size) s->tx.fin = true;
    if (n < data.size) s->tx_blocked = true;
    return PROVEN_OK;
}

proven_u64 proven_quic_stream_room(proven_quic_conn_t *c, proven_u64 id) {
    if (!c || c->state != QS_OPEN) return 0;
    qstream_t *s = stream_find(c, id);
    if (!s || !s->has_tx || s->tx_reset || s->tx.fin) return 0;
    s->tx_blocked = true;               /* asked because it may wait: told when room is freed */
    return tx_room(c, s);
}

proven_err_t proven_quic_stream_read(proven_quic_conn_t *c, proven_u64 id, proven_mem_mut_t out, proven_size_t *got, bool *fin) {
    if (!c || !got || !fin || (out.size && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    *got = 0; *fin = false;
    qstream_t *s = stream_find(c, id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if (!s->has_rx || s->rx_reset) return PROVEN_ERR_INVALID_STATE;
    const proven_size_t ranges = s->rx.got.n;
    const proven_size_t n = recvq_take(&c->mem, &s->rx, out.ptr, out.size);
    c->rx_ranges -= ranges - s->rx.got.n;
    *got = n;
    if (n) rx_credit(c, s, n);
    if (s->rx.has_final && s->rx.read == s->rx.final) { *fin = true; s->rx_fin_read = true; s->rx_max_owed = false; }
    else if (recvq_ready(&s->rx) > 0) s->ev_readable = true;
    stream_maybe_free(c, s);
    return PROVEN_OK;
}

proven_err_t proven_quic_stream_reset(proven_quic_conn_t *c, proven_u64 id, proven_u64 code) {
    if (!c || code > PROVEN_QUIC_VARINT_MAX) return PROVEN_ERR_INVALID_ARG;
    if (c->state != QS_OPEN) return PROVEN_ERR_INVALID_STATE;
    qstream_t *s = stream_find(c, id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if (!s->has_tx) return PROVEN_ERR_INVALID_STATE;
    stream_tx_reset(c, s, code);
    return PROVEN_OK;
}

proven_err_t proven_quic_stream_stop(proven_quic_conn_t *c, proven_u64 id, proven_u64 code) {
    if (!c || code > PROVEN_QUIC_VARINT_MAX) return PROVEN_ERR_INVALID_ARG;
    if (c->state != QS_OPEN) return PROVEN_ERR_INVALID_STATE;
    qstream_t *s = stream_find(c, id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if (!s->has_rx) return PROVEN_ERR_INVALID_STATE;
    if (s->rx_stopped || s->rx_reset || s->rx_fin_read) return PROVEN_OK;
    s->rx_stopped = true;
    s->rx_stop_code = code;
    if (!s->rx.has_final) s->rx_stop_owed = true;
    /* What is held and unread is let go, and given back to the connection's window. */
    c->rx_ranges -= s->rx.got.n;
    const proven_u64 unread = s->rx.highest - s->rx.read;
    const proven_u64 highest = s->rx.highest, final = s->rx.final;
    const bool has_final = s->rx.has_final;
    recvq_free(&c->mem, &s->rx);
    s->rx.read = s->rx.highest = highest; s->rx.final = final; s->rx.has_final = has_final;
    s->rx_fin_read = true;              /* the reader is done with it */
    s->ev_readable = false;
    rx_credit(c, NULL, unread);
    stream_maybe_free(c, s);
    return PROVEN_OK;
}

void proven_quic_conn_close(proven_quic_conn_t *c, bool app, proven_u64 code) {
    if (!c || c->state >= QS_CLOSING) return;
    conn_fail(c, code, 0);
    c->close_unsent = true;
    c->close_app = app;
    /* the event already told the code; tell `app` as well */
    if (c->ev_n > 0) c->ev[(c->ev_head + c->ev_n - 1) % QC_EVENTS].app = app;
}

bool proven_quic_conn_sending(const proven_quic_conn_t *c) {
    if (!c) return false;
    for (proven_size_t i = 0; i < c->nstreams; ++i) {
        const qstream_t *s = c->streams[i];
        if (s->has_tx && !s->tx_reset && (s->tx.end != s->tx.base || (s->tx.fin && !s->tx.fin_acked))) return true;
    }
    return false;
}

void proven_quic_conn_stats(const proven_quic_conn_t *c, proven_quic_stats_t *s) {
    *s = c->st;
    s->cwnd = c->cwnd; s->bytes_in_flight = c->bif; s->srtt_us = c->srtt; s->min_rtt_us = c->min_rtt; s->pto_count = c->pto_count;
    s->streams = c->nstreams; s->memory = c->mem.held;
    s->established = c->complete; s->confirmed = c->confirmed; s->closed = c->state == QS_CLOSED;
    s->path_validated = c->validated;
    s->key_updates = c->key_updates;
    s->spurious_losses = c->spurious;
    s->datagram_size = c->datagram;
}

/* ---- For tests: frames as if a packet carried them, and a key update started here ---- */

proven_u64 proven_quic_conn_test_frames_(proven_quic_conn_t *c, int lvl, proven_mem_view_t payload, proven_u64 now) {
    c->now = now;
    if (c->state >= QS_CLOSING) return c->close_code;
    if (c->scratch_cap < payload.size) {
        proven_byte_t *p = qrealloc(&c->mem, c->scratch, c->scratch_cap, payload.size + 2 * QC_MAX_DGRAM, 1);
        if (!p) return PROVEN_QUIC_INTERNAL_ERROR;
        c->scratch = p; c->scratch_cap = payload.size + 2 * QC_MAX_DGRAM;
    }
    qcopy(c->scratch, payload.ptr, payload.size);
    const proven_quic_cid_t dcid = c->local[0].cid;
    proven_quic_rd_t r = { c->scratch, payload.size, true };
    while (r.n > 0 && c->state < QS_CLOSING) {
        proven_quic_frame_t f;
        proven_u64 err = proven_quic_frame_read(&r, &f);
        if (err == 0) err = on_frame(c, lvl, &f, &dcid, &c->peer_addr);
        if (err) { conn_fail(c, err, f.type); break; }
    }
    replay_held(c);
    set_loss_timer(c);
    return c->state >= QS_CLOSING ? c->close_code : 0;
}

bool proven_quic_conn_test_key_update_(proven_quic_conn_t *c) {
    if (!c->confirmed || !c->sp[2].tx_ok || !c->sp[2].rx_ok) return false;
    key_update_start(c);
    return true;
}

/* ---- Making and ending connections ---- */

static proven_quic_conn_t *conn_new(const proven_quic_config_t *cfg, bool server, proven_u64 now) {
    proven_result_mem_mut_t m = cfg->alloc.alloc_fn(cfg->alloc.ctx, sizeof(proven_quic_conn_t), alignof(proven_quic_conn_t));
    if (m.err != PROVEN_OK) return NULL;
    proven_quic_conn_t *c = (proven_quic_conn_t *)m.value.ptr;
    qzero(c, sizeof *c);
    c->mem.alloc = cfg->alloc;
    c->mem.held = sizeof *c;
    cfg_defaults(&c->cfg, cfg);
    c->server = server;
    c->now = now;
    c->idle_start = now;
    c->datagram = c->cfg.datagram_size;
    c->peer_max_udp = 65527;
    c->loss_timer = QC_INF;
    c->pkt_threshold = 3;
    c->time_threshold = 9;
    c->peer_max_ack_delay = 25000;
    c->peer_ack_delay_exp = 3;
    c->peer_cid_limit = 2;
    c->idle_timeout = (proven_u64)c->cfg.idle_timeout_ms * 1000;
    c->rx_max_data = c->cfg.conn_window;
    c->rx_conn_window = c->cfg.conn_window;
    c->rx_conn_moved_at = c->now;
    c->our_max_streams[0] = c->cfg.max_streams_bidi;
    c->our_max_streams[1] = c->cfg.max_streams_uni;
    c->state = QS_HANDSHAKE;
    bool zero = true;
    for (int i = 0; i < 32; ++i) if (cfg->static_key[i]) zero = false;
    if (zero) random_bytes(c, c->static_key, 32); else qcopy(c->static_key, cfg->static_key, 32);
    cc_init(c);
    new_local_cid(c, &c->local[0]);
    return c;
}

static void initial_keys(proven_quic_conn_t *c, const proven_quic_cid_t *dcid) {
    proven_byte_t cs[32], ss[32];
    proven_quic_initial_secrets((proven_mem_view_t){ .ptr = dcid->id, .size = dcid->len }, cs, ss);
    proven_quic_keys_set(&c->sp[0].tx, proven_quic_initial_suite(), c->server ? ss : cs);
    proven_quic_keys_set(&c->sp[0].rx, proven_quic_initial_suite(), c->server ? cs : ss);
    c->sp[0].tx_ok = c->sp[0].rx_ok = true;
    proven_mem_wipe((proven_mem_mut_t){ .ptr = cs, .size = 32 });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = ss, .size = 32 });
}

proven_err_t proven_quic_conn_client_create(const proven_quic_config_t *cfg, proven_u8str_view_t server_name, proven_tls_session_t *session,
                                           const proven_quic_addr_t *server, proven_u64 now, proven_quic_conn_t **out) {
    if (!out || !server || !cfg_valid(cfg)) return PROVEN_ERR_INVALID_ARG;
    *out = NULL;
    proven_quic_conn_t *c = conn_new(cfg, false, now);
    if (!c) return PROVEN_ERR_NOMEM;
    c->peer_addr = *server;
    c->validated = true;
    c->odcid.len = QC_CID_LEN;
    random_bytes(c, c->odcid.id, QC_CID_LEN);
    c->peer[0] = (qcid_t){ .seq = 0, .cid = c->odcid, .active = true };
    initial_keys(c, &c->odcid);
    proven_byte_t tp[256];
    const proven_size_t tpn = local_params(c, tp, sizeof tp);
    proven_err_t e = tpn ? proven_tls_quic_client_create_(cfg->tls, server_name, session, (proven_mem_view_t){ .ptr = tp, .size = tpn }, &c->tls)
                         : PROVEN_ERR_INVALID_ARG;
    if (e != PROVEN_OK) { proven_quic_conn_destroy(c); return e; }
    tls_after(c);
    if (c->mem.oom || c->state >= QS_CLOSING) { proven_quic_conn_destroy(c); return PROVEN_ERR_NOMEM; }
    *out = c;
    return PROVEN_OK;
}

proven_err_t proven_quic_conn_server_create(const proven_quic_config_t *cfg, const proven_quic_accept_t *accept, proven_mem_view_t datagram,
                                           const proven_quic_addr_t *from, proven_u64 now, proven_quic_conn_t **out) {
    if (!out || !accept || !from || !cfg_valid(cfg)) return PROVEN_ERR_INVALID_ARG;
    *out = NULL;
    proven_quic_hdr_t h;
    if (!proven_quic_hdr_parse(datagram, QC_CID_LEN, &h) || h.type != PROVEN_QUIC_PKT_INITIAL) return PROVEN_ERR_INVALID_FORMAT;
    proven_quic_conn_t *c = conn_new(cfg, true, now);
    if (!c) return PROVEN_ERR_NOMEM;
    c->peer_addr = *from;
    c->odcid = accept->odcid;
    c->client_dcid = h.dcid;
    c->retried = accept->retried;
    c->retry_scid = accept->retry_scid;
    c->validated = accept->retried;
    c->peer[0] = (qcid_t){ .seq = 0, .cid = h.scid, .active = true };
    c->peer_initial_scid = h.scid;
    initial_keys(c, &h.dcid);
    proven_byte_t tp[256];
    const proven_size_t tpn = local_params(c, tp, sizeof tp);
    proven_err_t e = tpn ? proven_tls_quic_server_create_(cfg->tls, (proven_mem_view_t){ .ptr = tp, .size = tpn }, &c->tls) : PROVEN_ERR_INVALID_ARG;
    if (e != PROVEN_OK) { proven_quic_conn_destroy(c); return e; }
    e = proven_quic_conn_receive(c, datagram, from, now);
    if (e != PROVEN_OK) { proven_quic_conn_destroy(c); return e; }
    *out = c;
    return PROVEN_OK;
}

void proven_quic_conn_destroy(proven_quic_conn_t *c) {
    if (!c) return;
    for (int i = 0; i < PROVEN_QUIC_LEVELS; ++i) {
        qspace_t *sp = &c->sp[i];
        sendq_free(&c->mem, &sp->crypto_tx);
        recvq_free(&c->mem, &sp->crypto_rx);
        rset_free(&c->mem, &sp->rcvd);
        qfree(&c->mem, sp->sent, sp->sent_cap * sizeof *sp->sent);
        for (unsigned j = 0; j < sp->pending_n; ++j) qfree(&c->mem, sp->pending[j], sp->pending_len[j]);
    }
    for (proven_size_t i = 0; i < c->nstreams; ++i) {
        sendq_free(&c->mem, &c->streams[i]->tx);
        recvq_free(&c->mem, &c->streams[i]->rx);
        qfree(&c->mem, c->streams[i], sizeof *c->streams[i]);
    }
    qfree(&c->mem, c->streams, c->streams_cap * sizeof *c->streams);
    qfree(&c->mem, c->scratch, c->scratch_cap);
    if (c->tls) proven_tls_conn_destroy(c->tls);
    proven_allocator_t a = c->mem.alloc;
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)c, .size = sizeof *c });
    a.free_fn(a.ctx, c);
}

/* ---- Before any connection: a server's stateless answers ---- */

#define QC_TOKEN_AGE 10000000           /* microseconds a Retry token is good for */

static void token_key(const proven_quic_config_t *cfg, proven_crypto_aes_gcm_t *aead) {
    proven_byte_t k[PROVEN_HMAC_MAX_SIZE];
    (void)proven_hmac(PROVEN_HMAC_SHA256, (proven_mem_view_t){ .ptr = cfg->static_key, .size = 32 }, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"proven quic retry", .size = 17 }, k);
    (void)proven_crypto_aes_gcm_init(aead, k, 16);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = k, .size = sizeof k });
}

proven_quic_stateless_t proven_quic_server_stateless(const proven_quic_config_t *cfg, proven_mem_view_t d, const proven_quic_addr_t *from,
                                                      proven_u64 now, bool require_retry, proven_mem_mut_t out, proven_size_t *out_len,
                                                      proven_quic_accept_t *accept) {
    *out_len = 0;
    if (!cfg || !cfg->tls || !from || !accept) return PROVEN_QUIC_STATELESS_DROP;
    *accept = (proven_quic_accept_t){ 0 };
    proven_quic_hdr_t h;
    if (!proven_quic_hdr_parse(d, QC_CID_LEN, &h)) return PROVEN_QUIC_STATELESS_DROP;
    if (h.type == PROVEN_QUIC_PKT_OTHER_VERSION) {
        if (d.size < PROVEN_QUIC_MIN_INITIAL) return PROVEN_QUIC_STATELESS_DROP;
        proven_byte_t r;
        proven_tls_config_random(cfg->tls, &r, 1);
        *out_len = proven_quic_vn_build(&h.scid, &h.dcid, r, out);
        return *out_len ? PROVEN_QUIC_STATELESS_REPLY : PROVEN_QUIC_STATELESS_DROP;
    }
    if (h.type != PROVEN_QUIC_PKT_INITIAL || d.size < PROVEN_QUIC_MIN_INITIAL || h.dcid.len < 8) return PROVEN_QUIC_STATELESS_DROP;
    accept->odcid = h.dcid;
    if (!require_retry) return PROVEN_QUIC_STATELESS_ACCEPT;
    proven_crypto_aes_gcm_t aead;
    token_key(cfg, &aead);
    if (h.token.size == 0) {
        /* A Retry carrying a token: nonce, then sealed {time, odcid, the Retry's scid}, the address as associated data. */
        proven_byte_t tok[12 + 8 + 1 + 20 + 1 + 20 + 16];
        proven_byte_t plain[8 + 1 + 20 + 1 + 20];
        proven_quic_cid_t scid = { .len = QC_CID_LEN };
        proven_tls_config_random(cfg->tls, scid.id, QC_CID_LEN);
        proven_tls_config_random(cfg->tls, tok, 12);
        proven_size_t n = 0;
        for (int i = 7; i >= 0; --i) plain[n++] = (proven_byte_t)(now >> (8 * i));
        plain[n++] = h.dcid.len; qcopy(plain + n, h.dcid.id, h.dcid.len); n += h.dcid.len;
        plain[n++] = scid.len; qcopy(plain + n, scid.id, scid.len); n += scid.len;
        proven_crypto_aes_gcm_seal(&aead, tok, (proven_mem_view_t){ .ptr = from->b, .size = from->len }, (proven_mem_view_t){ .ptr = plain, .size = n }, tok + 12, tok + 12 + n);
        proven_byte_t r;
        proven_tls_config_random(cfg->tls, &r, 1);
        *out_len = proven_quic_retry_build(&h.dcid, &h.scid, &scid, (proven_mem_view_t){ .ptr = tok, .size = 12 + n + 16 }, r, out);
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&aead, .size = sizeof aead });
        return *out_len ? PROVEN_QUIC_STATELESS_REPLY : PROVEN_QUIC_STATELESS_DROP;
    }
    /* A token: it must open under this server's key for this address, be young, and name the
     * connection ID the client now writes to. Otherwise the packet is dropped. */
    proven_byte_t plain[8 + 1 + 20 + 1 + 20];
    const proven_size_t clen = h.token.size >= 28 ? h.token.size - 28 : 0;
    bool ok = h.token.size >= 12 + 16 + 10 && clen <= sizeof plain
              && proven_crypto_aes_gcm_open(&aead, h.token.ptr, (proven_mem_view_t){ .ptr = from->b, .size = from->len },
                                            (proven_mem_view_t){ .ptr = h.token.ptr + 12, .size = clen }, h.token.ptr + 12 + clen, plain);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&aead, .size = sizeof aead });
    if (!ok) return PROVEN_QUIC_STATELESS_DROP;
    proven_u64 t = 0;
    for (int i = 0; i < 8; ++i) t = (t << 8) | plain[i];
    proven_size_t at = 8;
    const proven_size_t ol = plain[at++];
    if (ol > 20 || at + ol + 1 > clen) return PROVEN_QUIC_STATELESS_DROP;
    accept->odcid.len = (proven_byte_t)ol; qcopy(accept->odcid.id, plain + at, ol); at += ol;
    const proven_size_t sl = plain[at++];
    if (sl > 20 || at + sl != clen) return PROVEN_QUIC_STATELESS_DROP;
    accept->retry_scid.len = (proven_byte_t)sl; qcopy(accept->retry_scid.id, plain + at, sl);
    if (t > now || now - t > QC_TOKEN_AGE || !cid_eq(&accept->retry_scid, &h.dcid)) return PROVEN_QUIC_STATELESS_DROP;
    accept->retried = true;
    return PROVEN_QUIC_STATELESS_ACCEPT;
}

proven_size_t proven_quic_stateless_reset(const proven_quic_config_t *cfg, const proven_quic_cid_t *dcid, proven_size_t in_len, proven_mem_mut_t out) {
    /* Shorter than what came (no loop between two such endpoints), at least 21 bytes. */
    if (!cfg || !cfg->tls || in_len < 22 || out.size < 21) return 0;
    proven_size_t n = in_len - 1;
    if (n > 43) n = 43;
    if (n > out.size) n = out.size;
    proven_tls_config_random(cfg->tls, out.ptr, n - 16);
    out.ptr[0] = (proven_byte_t)(0x40 | (out.ptr[0] & 0x3f));
    reset_token_for(cfg->static_key, dcid, out.ptr + n - 16);
    return n;
}

#endif /* PROVEN_NO_HTTP3 */
