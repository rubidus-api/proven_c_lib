#include "proven/h2.h"
#include "proven/hpack.h"

/*
 * HTTP/2 (RFC 9113): one connection, either end. Bytes in, events and bytes out; no socket
 * and no clock. What the peer can make this hold or do is bounded by the configuration, and
 * each bound is named where it is enforced.
 */

#define H2C_NO_LENGTH (~(proven_u64)0)
#define H2C_EMPTY_RUN_MAX 16u              /* frames in a row that carry nothing and end nothing */
#define H2C_MAX_LIST ((proven_u32)1 << 20)
#define H2C_MAX_FRAME ((proven_u32)1 << 20)

enum {
    ST_LOCAL_END = 0x01,                   /* this side sent END_STREAM */
    ST_REMOTE_END = 0x02,                  /* the peer sent END_STREAM */
    ST_HEAD_SENT = 0x04,                   /* the request, or the final response, went out */
    ST_HEAD_RECV = 0x08,                   /* a client: the final response head arrived */
    ST_BLOCKED = 0x10,                     /* send_data was cut short */
    ST_HBLOCKED = 0x20,                    /* headers were refused for the output bound */
    ST_NO_BODY = 0x40                      /* a client: the request was HEAD, so content-length describes no body */
};

typedef struct {
    proven_u32 id;
    proven_i32 send_win;                   /* may go below zero when the peer lowers its initial window */
    proven_i32 recv_win;                   /* what the peer may still send */
    proven_u32 unacked;                    /* credit not yet returned in a WINDOW_UPDATE */
    proven_u32 held;                       /* reported in DATA events and not yet consumed */
    proven_u32 flags;
    proven_u64 expect;                     /* content-length, or H2C_NO_LENGTH */
    proven_u64 got;
    void *user;
} h2c_stream_t;

typedef enum { RS_PREFACE = 0, RS_HDR, RS_FIXED, RS_DATA, RS_BLOCK, RS_SKIP } h2c_rs_t;

struct proven_h2_conn {
    proven_allocator_t alloc;
    /* this side's settings */
    proven_u32 cfg_streams, cfg_window, cfg_frame, cfg_list, cfg_table, cfg_debt;
    proven_size_t cfg_out;
    /* the peer's */
    proven_u32 peer_streams, peer_window, peer_frame, peer_list, peer_table;

    proven_hpack_decoder_t *dec;
    proven_hpack_encoder_t *enc;

    h2c_stream_t *streams;
    proven_u32 stream_cap, stream_count;
    proven_u32 *ring;                      /* the streams that ended last, each with how (RING_*) in its low two bits */
    proven_u32 ring_cap, ring_len, ring_pos;

    proven_byte_t *out;
    proven_size_t out_cap, out_len, out_off;
    proven_byte_t *blk;                    /* a header block that came in more than one piece */
    proven_size_t blk_cap, blk_len;
    proven_byte_t *hbuf;                   /* decoded fields, then the scratch they point into */

    proven_i64 send_win, recv_win;
    proven_u32 recv_unacked, recv_target;
    proven_u32 local_window;               /* the initial stream window in force: the default until the peer acknowledges */
    proven_u32 next_id;                    /* a client: the next stream to open */
    proven_u32 last_peer_id;               /* a server: the highest stream the client began */
    proven_u32 last_acted;                 /* a server: the highest stream handed to the application */
    proven_u32 goaway_last, goaway_code, close_code;
    proven_u32 debt, empty_run;
    proven_size_t ctl_queued;

    /* the reader */
    h2c_rs_t rs;
    proven_byte_t small[16];
    proven_u32 small_len, small_need;
    proven_h2_frame_t f;
    proven_u32 content_left, pad_left, settings_left;
    proven_u32 cur, expect_cont;

    bool server, fin, drop, end_headers;
    bool got_settings, settings_acked, peer_table_set;
    bool goaway_recv, goaway_sent, closed, reported, oom;
    bool sweep_goaway, sweep_writable;
};

static proven_u32 h2c_u32(const proven_byte_t *p) {
    return ((proven_u32)p[0] << 24) | ((proven_u32)p[1] << 16) | ((proven_u32)p[2] << 8) | (proven_u32)p[3];
}

static void h2c_copy(proven_byte_t *dst, const proven_byte_t *src, proven_size_t n) {
    for (proven_size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* Overlapping, towards higher addresses or lower. */
static void h2c_move(proven_byte_t *dst, const proven_byte_t *src, proven_size_t n) {
    if (dst < src) for (proven_size_t i = 0; i < n; i++) dst[i] = src[i];
    else if (dst > src) for (proven_size_t i = n; i > 0; i--) dst[i - 1] = src[i - 1];
}

static void h2c_free(proven_h2_conn_t *c, void *p) {
    if (p) c->alloc.free_fn(c->alloc.ctx, p);
}

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------

static void h2c_put_header(proven_byte_t *p, proven_size_t length, proven_u8 type, proven_u8 flags, proven_u32 id) {
    p[0] = (proven_byte_t)(length >> 16); p[1] = (proven_byte_t)(length >> 8); p[2] = (proven_byte_t)length;
    p[3] = type; p[4] = flags;
    p[5] = (proven_byte_t)(id >> 24); p[6] = (proven_byte_t)(id >> 16); p[7] = (proven_byte_t)(id >> 8); p[8] = (proven_byte_t)id;
}

static void h2c_put_u32(proven_byte_t *p, proven_u32 v) {
    p[0] = (proven_byte_t)(v >> 24); p[1] = (proven_byte_t)(v >> 16); p[2] = (proven_byte_t)(v >> 8); p[3] = (proven_byte_t)v;
}

/* Room for `n` more bytes at out[out_len]. False, with the connection over, when there is none
 * to be had. */
static bool h2c_out_reserve(proven_h2_conn_t *c, proven_size_t n) {
    if (c->out_off == c->out_len) c->out_off = c->out_len = 0;
    if (c->out_cap - c->out_len >= n) return true;
    if (c->out_off > 0) {
        h2c_move(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
        if (c->out_cap - c->out_len >= n) return true;
    }
    proven_size_t cap = c->out_cap ? c->out_cap : 128;
    while (cap - c->out_len < n) {
        if (cap > ((proven_size_t)1 << 30)) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return false; }
        cap *= 2;
    }
    proven_result_mem_mut_t m = c->out
        ? c->alloc.realloc_fn(c->alloc.ctx, c->out, c->out_cap, cap, 1)
        : c->alloc.alloc_fn(c->alloc.ctx, cap, 1);
    if (m.err != PROVEN_OK) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return false; }
    c->out = m.value.ptr;
    c->out_cap = cap;
    return true;
}

static proven_size_t h2c_pending(const proven_h2_conn_t *c) {
    return c->out_len - c->out_off;
}

static void h2c_q_goaway(proven_h2_conn_t *c, proven_u32 code) {
    if (!h2c_out_reserve(c, 17)) return;
    proven_byte_t *p = c->out + c->out_len;
    h2c_put_header(p, 8, PROVEN_H2_GOAWAY, 0, 0);
    h2c_put_u32(p + 9, c->last_acted);
    h2c_put_u32(p + 13, code);
    c->out_len += 17;
    c->goaway_sent = true;
}

/* A connection error: say why, and take nothing more. */
static void h2c_fail(proven_h2_conn_t *c, proven_u32 code) {
    if (c->closed) return;
    h2c_q_goaway(c, code);
    c->closed = true;
    c->close_code = code;
}

/* Answers the protocol owes the peer are counted apart from what the application sends: a
 * peer that asks and does not read the answers is ended when they pass the output bound. */
static void h2c_ctl(proven_h2_conn_t *c, proven_size_t n) {
    c->ctl_queued += n;
    if (c->ctl_queued > c->cfg_out) h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM);
}

static void h2c_q_u32_frame(proven_h2_conn_t *c, proven_u8 type, proven_u32 id, proven_u32 value, bool counted) {
    if (c->closed || !h2c_out_reserve(c, 13)) return;
    proven_byte_t *p = c->out + c->out_len;
    h2c_put_header(p, 4, type, 0, id);
    h2c_put_u32(p + 9, value);
    c->out_len += 13;
    if (counted) h2c_ctl(c, 13);
}

static void h2c_q_rst(proven_h2_conn_t *c, proven_u32 id, proven_u32 code, bool counted) {
    h2c_q_u32_frame(c, PROVEN_H2_RST_STREAM, id, code, counted);
}

static void h2c_q_window(proven_h2_conn_t *c, proven_u32 id, proven_u32 increment) {
    h2c_q_u32_frame(c, PROVEN_H2_WINDOW_UPDATE, id, increment, true);
}

static void h2c_q_empty(proven_h2_conn_t *c, proven_u8 type, proven_u8 flags, const proven_byte_t *payload, proven_size_t n) {
    if (c->closed || !h2c_out_reserve(c, 9 + n)) return;
    proven_byte_t *p = c->out + c->out_len;
    h2c_put_header(p, n, type, flags, 0);
    h2c_copy(p + 9, payload, n);
    c->out_len += 9 + n;
    h2c_ctl(c, 9 + n);
}

// -----------------------------------------------------------------------------
// Flow control, receiving
// -----------------------------------------------------------------------------

static void h2c_conn_credit(proven_h2_conn_t *c, proven_size_t n) {
    if (n == 0 || c->closed) return;
    c->recv_unacked += (proven_u32)n;
    if ((proven_u64)c->recv_unacked * 2 >= c->recv_target) {
        proven_u32 inc = c->recv_unacked;
        c->recv_unacked = 0;
        c->recv_win += inc;
        h2c_q_window(c, 0, inc);
    }
}

static void h2c_stream_credit(proven_h2_conn_t *c, h2c_stream_t *s, proven_size_t n) {
    if (n == 0 || c->closed) return;
    s->unacked += (proven_u32)n;
    if ((s->flags & ST_REMOTE_END) != 0) return;          /* the peer will send nothing more here */
    /* Half a window is the batch - and while this side's setting is on its way to the peer
     * there are two windows, the default and the one announced: whichever the peer is
     * holding to, the smaller of the two cannot leave it waiting. */
    const proven_u32 window = c->local_window < c->cfg_window ? c->local_window : c->cfg_window;
    if ((proven_u64)s->unacked * 2 >= window) {
        proven_u32 inc = s->unacked;
        proven_u32 id = s->id;
        s->unacked = 0;
        s->recv_win += (proven_i32)inc;
        h2c_q_window(c, id, inc);
    }
}

// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------

static h2c_stream_t *h2c_find(const proven_h2_conn_t *c, proven_u32 id) {
    for (proven_u32 i = 0; i < c->stream_count; i++)
        if (c->streams[i].id == id) return &c->streams[i];
    return (void *)0;
}

static h2c_stream_t *h2c_add(proven_h2_conn_t *c, proven_u32 id) {
    if (c->stream_count == c->stream_cap) {
        proven_u32 cap = c->stream_cap ? c->stream_cap * 2 : 4;
        if (cap > c->cfg_streams) cap = c->cfg_streams;
        if (cap <= c->stream_cap) return (void *)0;
        proven_result_mem_mut_t m = c->streams
            ? c->alloc.realloc_fn(c->alloc.ctx, c->streams, c->stream_cap * sizeof(h2c_stream_t), cap * sizeof(h2c_stream_t), alignof(h2c_stream_t))
            : c->alloc.alloc_fn(c->alloc.ctx, cap * sizeof(h2c_stream_t), alignof(h2c_stream_t));
        if (m.err != PROVEN_OK) return (void *)0;
        c->streams = (h2c_stream_t *)(void *)m.value.ptr;
        c->stream_cap = cap;
    }
    h2c_stream_t *s = &c->streams[c->stream_count++];
    *s = (h2c_stream_t){ .id = id, .send_win = (proven_i32)c->peer_window, .recv_win = (proven_i32)c->local_window, .expect = H2C_NO_LENGTH };
    return s;
}

/* How a stream ended decides what a late frame for it means (RFC 9113, section 5.1,
 * "closed"), so the last `max_concurrent_streams` endings are remembered. Every stream here
 * has an odd number, which leaves two bits for the manner. */
enum {
    RING_OURS = 1,                         /* this side closed it while the peer could still be sending: what follows is dropped */
    RING_PEER = 2,                         /* the peer reset it: more is a fault of that stream */
    RING_DONE = 3                          /* both sides finished: more is a fault of the connection */
};

static void h2c_ring_add(proven_h2_conn_t *c, proven_u32 id, proven_u32 how) {
    if (!c->ring) {
        proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, (proven_size_t)c->cfg_streams * sizeof(proven_u32), alignof(proven_u32));
        if (m.err != PROVEN_OK) return;                   /* without it stragglers are faults: harsher, never unsafe */
        c->ring = (proven_u32 *)(void *)m.value.ptr;
        c->ring_cap = c->cfg_streams;
    }
    c->ring[c->ring_pos] = (id & ~(proven_u32)1) << 1 | how;
    c->ring_pos = (c->ring_pos + 1) % c->ring_cap;
    if (c->ring_len < c->ring_cap) c->ring_len++;
}

/* 0 when the ending is no longer remembered, or never happened. */
static proven_u32 h2c_ring_find(const proven_h2_conn_t *c, proven_u32 id) {
    for (proven_u32 i = 0; i < c->ring_len; i++)
        if ((c->ring[i] >> 2) == (id >> 1)) return c->ring[i] & 3u;
    return 0;
}

/* Take a stream out, remembering how it ended. What it had not consumed goes back to the
 * connection's window. */
static void h2c_remove(proven_h2_conn_t *c, h2c_stream_t *s, bool properly, proven_u32 how) {
    const proven_u32 held = s->held;
    const proven_u32 id = s->id;
    *s = c->streams[--c->stream_count];
    if (properly && c->debt > 0) c->debt--;
    if (how != 0) h2c_ring_add(c, id, how);
    h2c_conn_credit(c, held);
}

/* Whether nothing was ever sent or received on this stream. */
static bool h2c_is_idle(const proven_h2_conn_t *c, proven_u32 id) {
    if ((id & 1u) == 0) return true;                      /* nobody pushes here */
    return c->server ? id > c->last_peer_id : id >= c->next_id;
}

static void h2c_event_stream(proven_h2_event_t *ev, proven_h2_event_kind_t kind, const h2c_stream_t *s) {
    *ev = (proven_h2_event_t){ .kind = kind, .stream_id = s->id, .user = s->user };
}

/* One more stream ended badly; too many more than end well, and the connection goes. */
static void h2c_debt(proven_h2_conn_t *c) {
    if (++c->debt > c->cfg_debt) h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM);
}

/* A stream error: RST_STREAM, and the application is told if it knew the stream. `forgive`:
 * the peer may not have seen the reset yet, so what follows on the stream is dropped. */
static void h2c_stream_fault(proven_h2_conn_t *c, proven_u32 id, h2c_stream_t *s, proven_u32 code, bool forgive, proven_h2_event_t *ev) {
    h2c_q_rst(c, id, code, true);
    if (s) {
        h2c_event_stream(ev, PROVEN_H2_EVENT_STREAM_RESET, s);
        ev->code = code;
        h2c_remove(c, s, false, forgive ? RING_OURS : RING_DONE);
    } else if (forgive) {
        h2c_ring_add(c, id, RING_OURS);
    }
    h2c_debt(c);
}

/* This side has sent its last on a stream. */
static void h2c_local_end(proven_h2_conn_t *c, h2c_stream_t *s) {
    s->flags |= ST_LOCAL_END;
    if ((s->flags & ST_REMOTE_END) != 0) { h2c_remove(c, s, true, RING_DONE); return; }
    if (!c->server) return;
    /* A response that is complete before its request: the rest is not wanted (section 8.1). */
    h2c_q_rst(c, s->id, PROVEN_H2_NO_ERROR, false);
    h2c_remove(c, s, true, RING_OURS);
}

// -----------------------------------------------------------------------------
// Header blocks, arriving
// -----------------------------------------------------------------------------

static bool h2c_name_is(proven_u8str_view_t name, const char *lit) {
    proven_size_t i = 0;
    for (; i < name.size && lit[i] != '\0'; i++)
        if (name.ptr[i] != (proven_byte_t)lit[i]) return false;
    return i == name.size && lit[i] == '\0';
}

/* The body length a header list announces. False when it announces two, or nonsense. */
static bool h2c_content_length(const proven_http_header_t *fields, proven_size_t count, proven_u64 *out) {
    *out = H2C_NO_LENGTH;
    for (proven_size_t i = 0; i < count; i++) {
        if (!h2c_name_is(fields[i].name, "content-length")) continue;
        proven_u8str_view_t v = fields[i].value;
        if (v.size == 0 || v.size > 19) return false;
        proven_u64 n = 0;
        for (proven_size_t k = 0; k < v.size; k++) {
            if (v.ptr[k] < '0' || v.ptr[k] > '9') return false;
            n = n * 10 + (proven_u64)(v.ptr[k] - '0');
        }
        if (*out != H2C_NO_LENGTH && *out != n) return false;
        *out = n;
    }
    return true;
}

static bool h2c_method_is_head(const proven_http_header_t *headers, proven_size_t count) {
    for (proven_size_t i = 0; i < count; i++)
        if (h2c_name_is(headers[i].name, ":method"))
            return headers[i].value.size == 4 && headers[i].value.ptr[0] == 'H' && headers[i].value.ptr[1] == 'E'
                && headers[i].value.ptr[2] == 'A' && headers[i].value.ptr[3] == 'D';
    return false;
}

/* The peer has sent its last on a stream; `ev` already describes the frame that said so. */
static void h2c_remote_end(proven_h2_conn_t *c, h2c_stream_t *s, proven_h2_event_t *ev) {
    if (s->expect != H2C_NO_LENGTH && s->got != s->expect) {
        /* A body that is not the length it announced is a malformed message (section 8.1.1). */
        h2c_stream_fault(c, s->id, s, PROVEN_H2_PROTOCOL_ERROR, false, ev);
        return;
    }
    s->flags |= ST_REMOTE_END;
    if ((s->flags & ST_LOCAL_END) != 0) h2c_remove(c, s, true, RING_DONE);
}

/* A frame that only an open stream may carry, for one that is neither open nor idle.
 * `unknown`: the code for a stream whose ending is not remembered. */
static void h2c_closed_frame(proven_h2_conn_t *c, proven_u32 id, proven_u32 unknown, proven_h2_event_t *ev) {
    switch (h2c_ring_find(c, id)) {
        case RING_OURS: return;
        case RING_PEER: h2c_stream_fault(c, id, (void *)0, PROVEN_H2_STREAM_CLOSED, false, ev); return;
        case RING_DONE: h2c_fail(c, PROVEN_H2_STREAM_CLOSED); return;
        default: h2c_fail(c, unknown); return;
    }
}

/* A whole header block. It is always decoded - the two ends' tables move together or not at
 * all - and only then is it asked what the list is for. */
static void h2c_on_block(proven_h2_conn_t *c, proven_mem_view_t block, proven_h2_event_t *ev) {
    const proven_u32 id = c->cur;
    const bool fin = c->fin;
    const proven_size_t cap = c->cfg_list / 32 + 1;
    /* A block is at most twice the list limit, and Huffman coding at its densest makes eight
     * bytes of five: scratch of four times the limit holds any one field such a block can
     * carry, so a list that is merely too large is always read to its end and refused as a
     * list - the stream's fault, not the connection's. */
    const proven_size_t scratch_size = (proven_size_t)c->cfg_list * 4;
    if (!c->hbuf) {
        proven_result_mem_mut_t m = c->alloc.alloc_fn(c->alloc.ctx, cap * sizeof(proven_http_header_t) + scratch_size, alignof(proven_http_header_t));
        if (m.err != PROVEN_OK) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return; }
        c->hbuf = m.value.ptr;
    }
    if (!c->dec) {
        if (proven_hpack_decoder_create(c->alloc, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &c->dec) != PROVEN_OK) {
            c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return;
        }
        if (c->settings_acked) proven_hpack_decoder_set_max_table_bytes(c->dec, c->cfg_table);
    }
    proven_http_header_t *fields = (proven_http_header_t *)(void *)c->hbuf;
    proven_mem_mut_t scratch = { .ptr = c->hbuf + cap * sizeof(proven_http_header_t), .size = scratch_size };
    proven_size_t count = 0;
    proven_err_t de = proven_hpack_decode(c->dec, block, fields, cap, scratch, c->cfg_list, &count);
    if (de == PROVEN_ERR_NOMEM) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return; }
    if (de != PROVEN_OK && de != PROVEN_ERR_OUT_OF_BOUNDS) { h2c_fail(c, PROVEN_H2_COMPRESSION_ERROR); return; }
    const bool too_big = de != PROVEN_OK;

    h2c_stream_t *s = h2c_find(c, id);
    if (!s) {
        if (!c->server || (id & 1u) == 0) {
            /* A client is sent a block only on a stream it opened; a server only on odd ones. */
            if (!c->server && (id & 1u) != 0 && id < c->next_id) h2c_closed_frame(c, id, PROVEN_H2_STREAM_CLOSED, ev);
            else h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
            return;
        }
        /* A stream number must be higher than every one before it (section 5.1.1). */
        if (id <= c->last_peer_id) { h2c_closed_frame(c, id, PROVEN_H2_PROTOCOL_ERROR, ev); return; }
        /* A server: a new request. */
        c->last_peer_id = id;
        if (c->goaway_sent) {                              /* said goodbye: not acted on, and safe to send elsewhere */
            h2c_q_rst(c, id, PROVEN_H2_REFUSED_STREAM, true);
            if (!fin) h2c_ring_add(c, id, RING_OURS);
            return;
        }
        if (c->stream_count >= c->cfg_streams) {
            h2c_stream_fault(c, id, (void *)0, PROVEN_H2_REFUSED_STREAM, !fin, ev);
            return;
        }
        proven_h2_request_head_t head;
        proven_u64 length = H2C_NO_LENGTH;
        if (too_big || proven_h2_request_head(fields, count, &head) != PROVEN_OK
            || !h2c_content_length(head.fields, head.field_count, &length) || (fin && length != H2C_NO_LENGTH && length != 0)) {
            h2c_stream_fault(c, id, (void *)0, PROVEN_H2_PROTOCOL_ERROR, !fin, ev);
            return;
        }
        s = h2c_add(c, id);
        if (!s) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return; }
        s->expect = length;
        if (fin) s->flags |= ST_REMOTE_END;
        c->last_acted = id;
        h2c_event_stream(ev, PROVEN_H2_EVENT_HEADERS, s);
        ev->end_stream = fin;
        ev->request = head;
        return;
    }

    if ((s->flags & ST_REMOTE_END) != 0) { h2c_stream_fault(c, id, s, PROVEN_H2_STREAM_CLOSED, false, ev); return; }

    if (!c->server && (s->flags & ST_HEAD_RECV) == 0) {
        proven_u16 status = 0;
        const proven_http_header_t *rest = (void *)0;
        proven_size_t rest_count = 0;
        if (too_big || proven_h2_response_head(fields, count, &status, &rest, &rest_count) != PROVEN_OK || status == 101) {
            h2c_stream_fault(c, id, s, PROVEN_H2_PROTOCOL_ERROR, !fin, ev);
            return;
        }
        if (status < 200) {
            if (fin) { h2c_stream_fault(c, id, s, PROVEN_H2_PROTOCOL_ERROR, false, ev); return; }
        } else {
            proven_u64 length = H2C_NO_LENGTH;
            if (!h2c_content_length(rest, rest_count, &length)) { h2c_stream_fault(c, id, s, PROVEN_H2_PROTOCOL_ERROR, !fin, ev); return; }
            /* Responses that describe a body they do not carry (RFC 9110, section 8.6). */
            if ((s->flags & ST_NO_BODY) != 0 || status == 204 || status == 304) length = H2C_NO_LENGTH;
            s->expect = length;
            s->flags |= ST_HEAD_RECV;
        }
        h2c_event_stream(ev, PROVEN_H2_EVENT_HEADERS, s);
        ev->end_stream = fin;
        ev->status = status;
        ev->fields = rest;
        ev->field_count = rest_count;
        if (fin) h2c_remote_end(c, s, ev);
        return;
    }

    /* Trailers: a second block, and it has to be the end. */
    if (!fin || too_big || proven_h2_trailers_check(fields, count) != PROVEN_OK) {
        h2c_stream_fault(c, id, s, PROVEN_H2_PROTOCOL_ERROR, !fin, ev);
        return;
    }
    h2c_event_stream(ev, PROVEN_H2_EVENT_TRAILERS, s);
    ev->end_stream = true;
    ev->fields = fields;
    ev->field_count = count;
    h2c_remote_end(c, s, ev);
}

// -----------------------------------------------------------------------------
// Frames, arriving
// -----------------------------------------------------------------------------

static void h2c_apply_setting(proven_h2_conn_t *c, proven_h2_setting_t st) {
    switch (st.id) {
        case PROVEN_H2_SETTINGS_HEADER_TABLE_SIZE:
            c->peer_table = st.value;
            c->peer_table_set = true;
            if (c->enc) proven_hpack_encoder_set_peer_table_bytes(c->enc, st.value);
            break;
        case PROVEN_H2_SETTINGS_ENABLE_PUSH:
            if (!c->server && st.value != 0) h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);     /* a server may not ask to be pushed to */
            break;
        case PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS:
            c->peer_streams = st.value;
            break;
        case PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE: {
            /* Every open stream moves by the difference, below zero if need be (section 6.9.2). */
            const proven_i64 delta = (proven_i64)st.value - (proven_i64)c->peer_window;
            for (proven_u32 i = 0; i < c->stream_count; i++) {
                const proven_i64 w = (proven_i64)c->streams[i].send_win + delta;
                if (w > (proven_i64)PROVEN_H2_MAX_WINDOW) { h2c_fail(c, PROVEN_H2_FLOW_CONTROL_ERROR); return; }
                c->streams[i].send_win = (proven_i32)w;
            }
            c->peer_window = st.value;
            if (delta > 0) c->sweep_writable = true;
            break;
        }
        case PROVEN_H2_SETTINGS_MAX_FRAME_SIZE:
            c->peer_frame = st.value;
            break;
        case PROVEN_H2_SETTINGS_MAX_HEADER_LIST_SIZE:
            c->peer_list = st.value;
            break;
        default:
            break;                                         /* a setting this side does not know is ignored */
    }
}

/* The peer has acknowledged this side's settings: they are in force from this frame on. */
static void h2c_settings_acked(proven_h2_conn_t *c) {
    if (c->settings_acked) return;
    c->settings_acked = true;
    if (c->dec) proven_hpack_decoder_set_max_table_bytes(c->dec, c->cfg_table);
    const proven_i64 delta = (proven_i64)c->cfg_window - (proven_i64)c->local_window;
    c->local_window = c->cfg_window;
    for (proven_u32 i = 0; i < c->stream_count; i++)
        c->streams[i].recv_win = (proven_i32)((proven_i64)c->streams[i].recv_win + delta);
}

static void h2c_to_skip(proven_h2_conn_t *c, proven_u32 n) {
    c->rs = RS_SKIP;
    c->pad_left = n;
}

static void h2c_to_fixed(proven_h2_conn_t *c, proven_u32 need) {
    c->rs = RS_FIXED;
    c->small_len = 0;
    c->small_need = need;
}

/* A frame header has been read. */
static void h2c_on_header(proven_h2_conn_t *c, proven_h2_event_t *ev) {
    const proven_h2_frame_t f = c->f;
    /* Nothing may come between the frames of a header block (section 6.10). */
    if (c->expect_cont != 0 ? (f.type != PROVEN_H2_CONTINUATION || f.stream_id != c->expect_cont) : f.type == PROVEN_H2_CONTINUATION) {
        h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
        return;
    }
    proven_h2_error_t code = PROVEN_H2_NO_ERROR;
    bool on_stream = false;
    if (proven_h2_frame_check(&f, c->cfg_frame, &code, &on_stream) != PROVEN_OK) {
        if (!on_stream) { h2c_fail(c, code); return; }
        h2c_to_skip(c, f.length);
        h2c_stream_fault(c, f.stream_id, h2c_find(c, f.stream_id), code, false, ev);
        return;
    }
    /* The first frame is the peer's settings (section 3.4). */
    if (!c->got_settings && (f.type != PROVEN_H2_SETTINGS || (f.flags & PROVEN_H2_FLAG_ACK) != 0)) {
        h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
        return;
    }
    switch (f.type) {
        case PROVEN_H2_DATA: {
            c->cur = f.stream_id;
            c->fin = (f.flags & PROVEN_H2_FLAG_END_STREAM) != 0;
            c->drop = false;
            /* The whole payload counts against the windows, padding and all (section 6.9.1). */
            if ((proven_i64)f.length > c->recv_win) { h2c_fail(c, PROVEN_H2_FLOW_CONTROL_ERROR); return; }
            c->recv_win -= f.length;
            c->rs = RS_DATA;
            c->content_left = f.length;
            c->pad_left = 0;
            h2c_stream_t *s = h2c_find(c, f.stream_id);
            if (!s) {
                if (h2c_is_idle(c, f.stream_id)) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
                c->drop = true;
                h2c_closed_frame(c, f.stream_id, PROVEN_H2_STREAM_CLOSED, ev);
                return;
            }
            if ((s->flags & ST_REMOTE_END) != 0) { c->drop = true; h2c_stream_fault(c, s->id, s, PROVEN_H2_STREAM_CLOSED, false, ev); return; }
            if (!c->server && (s->flags & ST_HEAD_RECV) == 0) { c->drop = true; h2c_stream_fault(c, s->id, s, PROVEN_H2_PROTOCOL_ERROR, true, ev); return; }
            if ((proven_i64)f.length > (proven_i64)s->recv_win) { c->drop = true; h2c_stream_fault(c, s->id, s, PROVEN_H2_FLOW_CONTROL_ERROR, true, ev); return; }
            s->recv_win -= (proven_i32)f.length;
            if ((f.flags & PROVEN_H2_FLAG_PADDED) != 0) {
                if (f.length == 0) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
                h2c_to_fixed(c, 1);
            }
            return;
        }
        case PROVEN_H2_HEADERS: {
            c->cur = f.stream_id;
            c->fin = (f.flags & PROVEN_H2_FLAG_END_STREAM) != 0;
            c->end_headers = (f.flags & PROVEN_H2_FLAG_END_HEADERS) != 0;
            c->expect_cont = f.stream_id;
            c->blk_len = 0;
            const proven_u32 need = ((f.flags & PROVEN_H2_FLAG_PADDED) != 0 ? 1u : 0u) + ((f.flags & PROVEN_H2_FLAG_PRIORITY) != 0 ? 5u : 0u);
            if (f.length < need) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
            if (f.length == need && !c->end_headers && !c->fin && ++c->empty_run > H2C_EMPTY_RUN_MAX) { h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM); return; }
            c->content_left = f.length;
            c->pad_left = 0;
            if (need > 0) h2c_to_fixed(c, need);
            else c->rs = RS_BLOCK;
            return;
        }
        case PROVEN_H2_CONTINUATION:
            c->end_headers = (f.flags & PROVEN_H2_FLAG_END_HEADERS) != 0;
            if (f.length == 0 && !c->end_headers && ++c->empty_run > H2C_EMPTY_RUN_MAX) { h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM); return; }
            c->content_left = f.length;
            c->pad_left = 0;
            c->rs = RS_BLOCK;
            return;
        case PROVEN_H2_SETTINGS:
            if ((f.flags & PROVEN_H2_FLAG_ACK) != 0) { h2c_settings_acked(c); h2c_to_skip(c, 0); return; }
            c->got_settings = true;
            c->settings_left = f.length;
            if (f.length == 0) { h2c_q_empty(c, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, (void *)0, 0); h2c_to_skip(c, 0); }
            else h2c_to_fixed(c, 6);
            return;
        case PROVEN_H2_PUSH_PROMISE:
            /* This side never allows push, as a client by its settings and as a server by the protocol. */
            h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
            return;
        case PROVEN_H2_RST_STREAM:
        case PROVEN_H2_WINDOW_UPDATE:
            h2c_to_fixed(c, 4);
            return;
        case PROVEN_H2_PING:
        case PROVEN_H2_GOAWAY:
            h2c_to_fixed(c, 8);
            return;
        default:
            /* PRIORITY, which is deprecated and ignored, and every type this side does not know. */
            h2c_to_skip(c, f.length);
            return;
    }
}

/* The fixed bytes a frame begins with have been read. */
static void h2c_on_fixed(proven_h2_conn_t *c, proven_h2_event_t *ev) {
    const proven_h2_frame_t f = c->f;
    const proven_byte_t *b = c->small;
    switch (f.type) {
        case PROVEN_H2_DATA: {
            const proven_u32 pad = b[0];
            if (pad >= f.length) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
            c->content_left = f.length - 1 - pad;
            c->pad_left = pad;
            c->rs = RS_DATA;
            /* Padding is nobody's to consume: it goes back to both windows here. */
            h2c_stream_t *s = h2c_find(c, c->cur);
            if (s) h2c_stream_credit(c, s, (proven_size_t)pad + 1);
            h2c_conn_credit(c, (proven_size_t)pad + 1);
            return;
        }
        case PROVEN_H2_HEADERS: {
            const bool padded = (f.flags & PROVEN_H2_FLAG_PADDED) != 0;
            const proven_u32 pad = padded ? b[0] : 0;
            if (c->small_need + pad > f.length) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
            c->content_left = f.length - c->small_need - pad;
            c->pad_left = pad;
            c->rs = RS_BLOCK;
            return;
        }
        case PROVEN_H2_SETTINGS: {
            proven_size_t pos = 0;
            proven_h2_setting_t st = { 0 };
            proven_h2_error_t code = PROVEN_H2_NO_ERROR;
            if (proven_h2_settings_next((proven_mem_view_t){ .ptr = b, .size = 6 }, &pos, &st, &code) != PROVEN_OK) { h2c_fail(c, code); return; }
            h2c_apply_setting(c, st);
            if (c->closed) return;
            c->settings_left -= 6;
            if (c->settings_left > 0) { h2c_to_fixed(c, 6); return; }
            h2c_q_empty(c, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, (void *)0, 0);
            h2c_to_skip(c, 0);
            return;
        }
        case PROVEN_H2_WINDOW_UPDATE: {
            const proven_u32 inc = h2c_u32(b) & 0x7fffffffu;
            h2c_to_skip(c, 0);
            if (f.stream_id == 0) {
                if (inc == 0) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
                if (c->send_win + inc > (proven_i64)PROVEN_H2_MAX_WINDOW) { h2c_fail(c, PROVEN_H2_FLOW_CONTROL_ERROR); return; }
                c->send_win += inc;
                c->sweep_writable = true;
                return;
            }
            h2c_stream_t *s = h2c_find(c, f.stream_id);
            if (!s) {
                if (h2c_is_idle(c, f.stream_id)) h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
                return;                                    /* a stream that has ended: allowed for a while, and nothing to do */
            }
            if (inc == 0) { h2c_stream_fault(c, s->id, s, PROVEN_H2_PROTOCOL_ERROR, true, ev); return; }
            if ((proven_i64)s->send_win + inc > (proven_i64)PROVEN_H2_MAX_WINDOW) { h2c_stream_fault(c, s->id, s, PROVEN_H2_FLOW_CONTROL_ERROR, true, ev); return; }
            s->send_win += (proven_i32)inc;
            c->sweep_writable = true;
            return;
        }
        case PROVEN_H2_RST_STREAM: {
            const proven_u32 code = h2c_u32(b);
            h2c_to_skip(c, 0);
            h2c_stream_t *s = h2c_find(c, f.stream_id);
            if (!s) {
                if (h2c_is_idle(c, f.stream_id)) h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR);
                return;
            }
            h2c_event_stream(ev, PROVEN_H2_EVENT_STREAM_RESET, s);
            ev->code = code;
            /* A server that is done with a response says so this way: that is an end, not a fault. */
            const bool polite = !c->server && code == PROVEN_H2_NO_ERROR && (s->flags & ST_REMOTE_END) != 0;
            h2c_remove(c, s, polite, RING_PEER);
            if (!polite) h2c_debt(c);
            return;
        }
        case PROVEN_H2_PING:
            h2c_to_skip(c, 0);
            if ((f.flags & PROVEN_H2_FLAG_ACK) == 0) { h2c_q_empty(c, PROVEN_H2_PING, PROVEN_H2_FLAG_ACK, b, 8); return; }
            *ev = (proven_h2_event_t){ .kind = PROVEN_H2_EVENT_PONG, .data = { .ptr = c->small, .size = 8 } };
            return;
        case PROVEN_H2_GOAWAY: {
            const proven_u32 last = h2c_u32(b) & 0x7fffffffu;
            if (c->goaway_recv && last > c->goaway_last) { h2c_fail(c, PROVEN_H2_PROTOCOL_ERROR); return; }
            c->goaway_recv = true;
            c->goaway_last = last;
            c->goaway_code = h2c_u32(b + 4);
            c->sweep_goaway = true;
            h2c_to_skip(c, f.length - 8);
            *ev = (proven_h2_event_t){ .kind = PROVEN_H2_EVENT_GOAWAY, .code = c->goaway_code, .last_stream_id = last };
            return;
        }
        default:
            h2c_to_skip(c, 0);
            return;
    }
}

static proven_u32 h2c_min_u32(proven_u32 a, proven_size_t b) {
    return b < a ? (proven_u32)b : a;
}

/* Move the reader on by what `in` allows. `*used` grows by what was taken. Returns when there
 * is an event, when the connection ended, or when the input is used up. */
static proven_err_t h2c_read(proven_h2_conn_t *c, proven_mem_view_t in, proven_size_t *used, proven_h2_event_t *ev) {
    for (;;) {
        if (c->closed || ev->kind != PROVEN_H2_EVENT_NONE) return PROVEN_OK;
        const proven_byte_t *p = in.size > *used ? in.ptr + *used : (void *)0;
        const proven_size_t avail = in.size - *used;
        switch (c->rs) {
            case RS_PREFACE: {
                static const char preface[] = PROVEN_H2_PREFACE;
                if (avail == 0) return PROVEN_OK;
                while (c->small_len < PROVEN_H2_PREFACE_SIZE && *used < in.size) {
                    if (in.ptr[*used] != (proven_byte_t)preface[c->small_len]) {
                        /* Not HTTP/2 at all: nothing is said to it. */
                        c->closed = c->reported = true;
                        c->close_code = PROVEN_H2_PROTOCOL_ERROR;
                        return PROVEN_ERR_INVALID_FORMAT;
                    }
                    c->small_len++;
                    (*used)++;
                }
                if (c->small_len == PROVEN_H2_PREFACE_SIZE) { c->rs = RS_HDR; c->small_len = 0; }
                break;
            }
            case RS_HDR:
            case RS_FIXED: {
                const proven_u32 need = c->rs == RS_HDR ? (proven_u32)PROVEN_H2_FRAME_HEADER_SIZE : c->small_need;
                const proven_u32 n = h2c_min_u32(need - c->small_len, avail);
                if (n == 0 && c->small_len < need) return PROVEN_OK;
                h2c_copy(c->small + c->small_len, p, n);
                c->small_len += n;
                *used += n;
                if (c->small_len < need) return PROVEN_OK;
                if (c->rs == RS_HDR) {
                    (void)proven_h2_frame_parse((proven_mem_view_t){ .ptr = c->small, .size = PROVEN_H2_FRAME_HEADER_SIZE }, &c->f);
                    h2c_on_header(c, ev);
                } else {
                    h2c_on_fixed(c, ev);
                }
                break;
            }
            case RS_DATA: {
                if (c->content_left == 0) {
                    /* A frame with no content: the end of a body, or nothing at all. */
                    h2c_stream_t *s = c->drop ? (void *)0 : h2c_find(c, c->cur);
                    h2c_to_skip(c, c->pad_left);
                    if (!s) break;
                    if (c->fin) {
                        h2c_event_stream(ev, PROVEN_H2_EVENT_DATA, s);
                        ev->end_stream = true;
                        h2c_remote_end(c, s, ev);
                    } else if (++c->empty_run > H2C_EMPTY_RUN_MAX) {
                        h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM);
                    }
                    break;
                }
                const proven_u32 n = h2c_min_u32(c->content_left, avail);
                if (n == 0) return PROVEN_OK;
                c->content_left -= n;
                *used += n;
                const bool last = c->content_left == 0;
                if (last) h2c_to_skip(c, c->pad_left);
                /* The stream is looked up for every piece: it may have been reset between two. */
                h2c_stream_t *s = c->drop ? (void *)0 : h2c_find(c, c->cur);
                if (!s) { h2c_conn_credit(c, n); break; }
                c->empty_run = 0;
                if (s->expect != H2C_NO_LENGTH && s->got + n > s->expect) {
                    c->drop = true;
                    h2c_conn_credit(c, n);
                    h2c_stream_fault(c, s->id, s, PROVEN_H2_PROTOCOL_ERROR, true, ev);
                    break;
                }
                s->got += n;
                s->held += n;
                h2c_event_stream(ev, PROVEN_H2_EVENT_DATA, s);
                ev->data = (proven_mem_view_t){ .ptr = p, .size = n };
                if (last && c->fin) {
                    ev->end_stream = true;
                    h2c_remote_end(c, s, ev);
                }
                break;
            }
            case RS_BLOCK: {
                proven_mem_view_t block = { .ptr = c->blk, .size = 0 };
                if (c->content_left > 0) {
                    if (avail == 0) return PROVEN_OK;
                    /* A block larger than this cannot hold a list that is accepted, and
                     * cannot be skipped either: the tables would part. */
                    if (c->blk_len + (proven_size_t)c->content_left > (proven_size_t)c->cfg_list * 2) { h2c_fail(c, PROVEN_H2_ENHANCE_YOUR_CALM); return PROVEN_OK; }
                    if (c->blk_len == 0 && c->end_headers && avail >= c->content_left) {
                        /* The usual case: the whole block is here, and is read where it lies. */
                        block = (proven_mem_view_t){ .ptr = p, .size = c->content_left };
                        *used += c->content_left;
                        c->content_left = 0;
                        c->empty_run = 0;
                    } else {
                        const proven_u32 n = h2c_min_u32(c->content_left, avail);
                        if (c->blk_len + n > c->blk_cap) {
                            proven_size_t cap = c->blk_cap ? c->blk_cap : 256;
                            while (cap < c->blk_len + n) cap *= 2;
                            proven_result_mem_mut_t m = c->blk
                                ? c->alloc.realloc_fn(c->alloc.ctx, c->blk, c->blk_cap, cap, 1)
                                : c->alloc.alloc_fn(c->alloc.ctx, cap, 1);
                            if (m.err != PROVEN_OK) { c->oom = c->closed = true; c->close_code = PROVEN_H2_INTERNAL_ERROR; return PROVEN_OK; }
                            c->blk = m.value.ptr;
                            c->blk_cap = cap;
                        }
                        h2c_copy(c->blk + c->blk_len, p, n);
                        c->blk_len += n;
                        c->content_left -= n;
                        *used += n;
                        c->empty_run = 0;
                        if (c->content_left > 0) return PROVEN_OK;
                        block = (proven_mem_view_t){ .ptr = c->blk, .size = c->blk_len };
                    }
                } else {
                    block.size = c->blk_len;
                }
                h2c_to_skip(c, c->pad_left);
                if (!c->end_headers) break;                /* CONTINUATION follows */
                c->expect_cont = 0;
                c->blk_len = 0;
                h2c_on_block(c, block, ev);
                break;
            }
            case RS_SKIP: {
                const proven_u32 n = h2c_min_u32(c->pad_left, avail);
                c->pad_left -= n;
                *used += n;
                if (c->pad_left > 0) return PROVEN_OK;
                c->rs = RS_HDR;
                c->small_len = 0;
                if (avail == n) return PROVEN_OK;
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// The object
// -----------------------------------------------------------------------------

proven_err_t proven_h2_conn_create(const proven_h2_config_t *config, proven_h2_conn_t **out) {
    if (out) *out = (void *)0;
    if (!out || !config || !proven_alloc_is_valid(config->alloc)) return PROVEN_ERR_INVALID_ARG;
    const proven_u32 streams = config->max_concurrent_streams ? config->max_concurrent_streams : 100;
    const proven_u32 window = config->initial_window ? config->initial_window : PROVEN_H2_DEFAULT_WINDOW;
    const proven_u32 frame = config->max_frame_size ? config->max_frame_size : PROVEN_H2_MIN_MAX_FRAME_SIZE;
    const proven_u32 list = config->max_header_list_bytes ? config->max_header_list_bytes : 16384;
    const proven_u32 table = config->set_header_table ? config->header_table_bytes : (proven_u32)PROVEN_HPACK_DEFAULT_TABLE_BYTES;
    const proven_size_t bound = config->max_buffered_output ? config->max_buffered_output : 65536;
    if (streams > ((proven_u32)1 << 20) || window > PROVEN_H2_MAX_WINDOW || frame < PROVEN_H2_MIN_MAX_FRAME_SIZE || frame > H2C_MAX_FRAME
        || list < 64 || list > H2C_MAX_LIST || table > ((proven_u32)1 << 30) || bound < 1024 || bound > ((proven_size_t)1 << 30)) return PROVEN_ERR_INVALID_ARG;

    proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof(proven_h2_conn_t), alignof(proven_h2_conn_t));
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_h2_conn_t *c = (proven_h2_conn_t *)(void *)m.value.ptr;
    *c = (proven_h2_conn_t){
        .alloc = config->alloc, .server = config->server,
        .cfg_streams = streams, .cfg_window = window, .cfg_frame = frame, .cfg_list = list, .cfg_table = table,
        .cfg_debt = config->max_reset_debt ? config->max_reset_debt : 100, .cfg_out = bound,
        .peer_streams = 0xffffffffu, .peer_window = PROVEN_H2_DEFAULT_WINDOW, .peer_frame = PROVEN_H2_MIN_MAX_FRAME_SIZE,
        .peer_list = 0xffffffffu, .peer_table = (proven_u32)PROVEN_HPACK_DEFAULT_TABLE_BYTES,
        .send_win = PROVEN_H2_DEFAULT_WINDOW, .recv_win = PROVEN_H2_DEFAULT_WINDOW, .recv_target = PROVEN_H2_DEFAULT_WINDOW,
        .local_window = PROVEN_H2_DEFAULT_WINDOW, .next_id = 1,
        .rs = config->server ? RS_PREFACE : RS_HDR,
    };

    /* The opening: a client's preface, then this side's settings - only those that differ
     * from the protocol's defaults, and for a client the one that forbids push. */
    proven_h2_setting_t st[6];
    proven_size_t n = 0;
    if (table != PROVEN_HPACK_DEFAULT_TABLE_BYTES) st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_HEADER_TABLE_SIZE, table };
    if (!c->server) st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_ENABLE_PUSH, 0 };
    st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, streams };
    if (window != PROVEN_H2_DEFAULT_WINDOW) st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, window };
    if (frame != PROVEN_H2_MIN_MAX_FRAME_SIZE) st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, frame };
    st[n++] = (proven_h2_setting_t){ PROVEN_H2_SETTINGS_MAX_HEADER_LIST_SIZE, list };
    if (!h2c_out_reserve(c, PROVEN_H2_PREFACE_SIZE + 9 + 6 * n + 13)) { proven_h2_conn_destroy(c); return PROVEN_ERR_NOMEM; }
    if (!c->server) {
        h2c_copy(c->out, (const proven_byte_t *)PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE);
        c->out_len = PROVEN_H2_PREFACE_SIZE;
    }
    (void)proven_h2_write_settings((proven_mem_mut_t){ .ptr = c->out, .size = c->out_cap }, &c->out_len, st, n);
    /* The connection's own window is not a setting: it is raised by an update (section 6.9.2). */
    if (window > PROVEN_H2_DEFAULT_WINDOW) {
        h2c_put_header(c->out + c->out_len, 4, PROVEN_H2_WINDOW_UPDATE, 0, 0);
        h2c_put_u32(c->out + c->out_len + 9, window - PROVEN_H2_DEFAULT_WINDOW);
        c->out_len += 13;
        c->recv_win = window;
        c->recv_target = window;
    }
    *out = c;
    return PROVEN_OK;
}

void proven_h2_conn_destroy(proven_h2_conn_t *c) {
    if (!c) return;
    proven_hpack_decoder_destroy(c->dec);
    proven_hpack_encoder_destroy(c->enc);
    h2c_free(c, c->streams);
    h2c_free(c, c->ring);
    h2c_free(c, c->out);
    h2c_free(c, c->blk);
    h2c_free(c, c->hbuf);
    proven_allocator_t a = c->alloc;
    a.free_fn(a.ctx, c);
}

static bool h2c_can_write(const proven_h2_conn_t *c, const h2c_stream_t *s) {
    if (h2c_pending(c) >= c->cfg_out) return false;
    if ((s->flags & ST_HBLOCKED) != 0) return true;
    return (s->flags & ST_BLOCKED) != 0 && s->send_win > 0 && c->send_win > 0;
}

/* An event that needs no input: a stream the peer's goodbye left out, a stream that may send
 * again, the end that follows the last stream after a goodbye. True when `ev` was filled or
 * the connection ended. */
static bool h2c_due(proven_h2_conn_t *c, proven_h2_event_t *ev) {
    if (c->closed) return true;
    if (c->sweep_goaway) {
        /* The peer said which streams it acted on: the others can be sent again elsewhere. */
        for (proven_u32 i = 0; !c->server && i < c->stream_count; i++) {
            h2c_stream_t *s = &c->streams[i];
            if (s->id <= c->goaway_last) continue;
            h2c_event_stream(ev, PROVEN_H2_EVENT_STREAM_RESET, s);
            ev->code = PROVEN_H2_REFUSED_STREAM;
            h2c_remove(c, s, false, RING_PEER);
            return true;
        }
        c->sweep_goaway = false;
    }
    if (c->sweep_writable) {
        for (proven_u32 i = 0; i < c->stream_count; i++) {
            h2c_stream_t *s = &c->streams[i];
            if (!h2c_can_write(c, s)) continue;
            s->flags &= ~(proven_u32)(ST_BLOCKED | ST_HBLOCKED);
            h2c_event_stream(ev, PROVEN_H2_EVENT_STREAM_WRITABLE, s);
            return true;
        }
        c->sweep_writable = false;
    }
    /* After a goodbye from either side, the end of the last stream is the end. */
    if ((c->goaway_recv || c->goaway_sent) && c->stream_count == 0) {
        c->closed = true;
        c->close_code = c->goaway_recv ? c->goaway_code : PROVEN_H2_NO_ERROR;
        return true;
    }
    return false;
}

proven_err_t proven_h2_conn_feed(proven_h2_conn_t *c, proven_mem_view_t in, proven_size_t *consumed, proven_h2_event_t *ev) {
    if (consumed) *consumed = 0;
    if (ev) *ev = (proven_h2_event_t){ .kind = PROVEN_H2_EVENT_NONE };
    if (!c || !consumed || !ev || (in.size > 0 && !in.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (c->reported) return PROVEN_ERR_INVALID_STATE;

    /* What is due comes before what is new, and is looked for again when the input brought
     * no event of its own: a window that just opened is reported in the call that opened it. */
    proven_err_t err = PROVEN_OK;
    if (!h2c_due(c, ev)) {
        err = h2c_read(c, in, consumed, ev);
        if (err == PROVEN_OK && ev->kind == PROVEN_H2_EVENT_NONE && *consumed == in.size) (void)h2c_due(c, ev);
    }
    if (err != PROVEN_OK) return err;
    if (c->closed) {
        *ev = (proven_h2_event_t){ .kind = PROVEN_H2_EVENT_CLOSED, .code = c->close_code };
        *consumed = in.size;
        c->reported = true;
        return c->oom ? PROVEN_ERR_NOMEM : PROVEN_OK;
    }
    if (ev->kind == PROVEN_H2_EVENT_NONE && c->stream_count == 0 && c->rs == RS_HDR && c->small_len == 0 && c->expect_cont == 0) {
        /* Nothing is going on: an idle connection keeps its struct and its tables, no more. */
        h2c_free(c, c->hbuf); c->hbuf = (void *)0;
        h2c_free(c, c->blk); c->blk = (void *)0; c->blk_cap = 0;
        h2c_free(c, c->streams); c->streams = (void *)0; c->stream_cap = 0;
        if (c->out_off == c->out_len) { h2c_free(c, c->out); c->out = (void *)0; c->out_cap = c->out_len = c->out_off = 0; }
    }
    return PROVEN_OK;
}

proven_mem_view_t proven_h2_conn_pending_output(const proven_h2_conn_t *c) {
    if (!c || c->out_len == c->out_off) return (proven_mem_view_t){ .ptr = (void *)0, .size = 0 };
    return (proven_mem_view_t){ .ptr = c->out + c->out_off, .size = c->out_len - c->out_off };
}

void proven_h2_conn_output_sent(proven_h2_conn_t *c, proven_size_t n) {
    if (!c || n == 0) return;
    if (n > h2c_pending(c)) n = h2c_pending(c);
    c->out_off += n;
    c->ctl_queued = c->ctl_queued > n ? c->ctl_queued - n : 0;
    if (c->out_off == c->out_len) {
        c->out_off = c->out_len = 0;
        /* With no stream open there is nothing to say soon: the buffer goes back. */
        if (c->stream_count == 0) { h2c_free(c, c->out); c->out = (void *)0; c->out_cap = 0; }
    }
    c->sweep_writable = true;
}

// -----------------------------------------------------------------------------
// Sending
// -----------------------------------------------------------------------------

/* Encode a header list and frame it: HEADERS, and CONTINUATION frames when the block is
 * larger than the peer's frames. */
static proven_err_t h2c_send_block(proven_h2_conn_t *c, proven_u32 id, const proven_http_header_t *headers, proven_size_t count, bool end_stream) {
    proven_u64 list = 0;
    for (proven_size_t i = 0; i < count; i++) list += (proven_u64)headers[i].name.size + headers[i].value.size + 32;
    if (list > c->peer_list) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (!c->enc) {
        if (proven_hpack_encoder_create(c->alloc, c->cfg_table, &c->enc) != PROVEN_OK) return PROVEN_ERR_NOMEM;
        if (c->peer_table_set) proven_hpack_encoder_set_peer_table_bytes(c->enc, c->peer_table);
    }
    const proven_size_t bound = proven_hpack_encode_bound(headers, count);
    const proven_size_t frame = c->peer_frame;
    if (!h2c_out_reserve(c, bound + 9 * (bound / frame + 1))) { c->oom = false; c->closed = false; return PROVEN_ERR_NOMEM; }
    const proven_size_t start = c->out_len;
    proven_size_t len = start + 9;
    proven_err_t err = proven_hpack_encode(c->enc, headers, count, (void *)0, (proven_mem_mut_t){ .ptr = c->out, .size = c->out_cap }, &len);
    if (err != PROVEN_OK) return err == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_INVALID_ARG;
    const proven_size_t blen = len - start - 9;
    const proven_size_t frames = blen == 0 ? 1 : (blen + frame - 1) / frame;
    const proven_u8 fin = end_stream ? PROVEN_H2_FLAG_END_STREAM : 0;
    /* Make room for the later frames' headers, from the back. */
    for (proven_size_t i = frames - 1; i > 0; i--) {
        const proven_size_t piece = i == frames - 1 ? blen - i * frame : frame;
        proven_byte_t *dst = c->out + start + i * (frame + 9);
        h2c_move(dst + 9, c->out + start + 9 + i * frame, piece);
        h2c_put_header(dst, piece, PROVEN_H2_CONTINUATION, i == frames - 1 ? PROVEN_H2_FLAG_END_HEADERS : 0, id);
    }
    h2c_put_header(c->out + start, frames == 1 ? blen : frame, PROVEN_H2_HEADERS,
                   (proven_u8)(fin | (frames == 1 ? PROVEN_H2_FLAG_END_HEADERS : 0)), id);
    c->out_len = start + blen + 9 * frames;
    return PROVEN_OK;
}

proven_err_t proven_h2_conn_submit_request(proven_h2_conn_t *c, const proven_http_header_t *headers, proven_size_t count,
                                           bool end_stream, proven_u32 *stream_id) {
    if (stream_id) *stream_id = 0;
    proven_h2_request_head_t head;
    if (!c || !stream_id || c->server || (count > 0 && !headers) || proven_h2_request_head(headers, count, &head) != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;
    if (c->closed || c->goaway_recv || c->goaway_sent || c->next_id > PROVEN_H2_MAX_WINDOW) return PROVEN_ERR_INVALID_STATE;
    if (c->stream_count >= c->cfg_streams || c->stream_count >= c->peer_streams || h2c_pending(c) >= c->cfg_out) return PROVEN_ERR_BUSY;
    h2c_stream_t *s = h2c_add(c, c->next_id);
    if (!s) return PROVEN_ERR_NOMEM;
    proven_err_t err = h2c_send_block(c, s->id, headers, count, end_stream);
    if (err != PROVEN_OK) { c->stream_count--; return err; }
    s->flags |= ST_HEAD_SENT | (end_stream ? ST_LOCAL_END : 0u) | (h2c_method_is_head(headers, count) ? ST_NO_BODY : 0u);
    *stream_id = s->id;
    c->next_id += 2;
    return PROVEN_OK;
}

proven_err_t proven_h2_conn_submit_response(proven_h2_conn_t *c, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count,
                                            bool end_stream) {
    proven_u16 status = 0;
    const proven_http_header_t *rest = (void *)0;
    proven_size_t rest_count = 0;
    if (!c || !c->server || (count > 0 && !headers) || proven_h2_response_head(headers, count, &status, &rest, &rest_count) != PROVEN_OK
        || status == 101 || (status < 200 && end_stream)) return PROVEN_ERR_INVALID_ARG;
    if (c->closed) return PROVEN_ERR_INVALID_STATE;
    h2c_stream_t *s = h2c_find(c, stream_id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if ((s->flags & ST_HEAD_SENT) != 0) return PROVEN_ERR_INVALID_STATE;
    if (h2c_pending(c) >= c->cfg_out) { s->flags |= ST_HBLOCKED; return PROVEN_ERR_BUSY; }
    proven_err_t err = h2c_send_block(c, stream_id, headers, count, end_stream);
    if (err != PROVEN_OK) return err;
    if (status >= 200) s->flags |= ST_HEAD_SENT;
    if (end_stream) h2c_local_end(c, s);
    return PROVEN_OK;
}

proven_result_size_t proven_h2_conn_send_data(proven_h2_conn_t *c, proven_u32 stream_id, proven_mem_view_t data, bool end_stream) {
    if (!c || (data.size > 0 && !data.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->closed) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    h2c_stream_t *s = h2c_find(c, stream_id);
    if (!s) return (proven_result_size_t){ .err = PROVEN_ERR_NOT_FOUND, .value = 0 };
    if ((s->flags & ST_HEAD_SENT) == 0 || (s->flags & ST_LOCAL_END) != 0) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };

    proven_size_t take = data.size;
    if (s->send_win <= 0 || c->send_win <= 0) take = 0;
    else {
        if (take > (proven_size_t)s->send_win) take = (proven_size_t)s->send_win;
        if ((proven_i64)take > c->send_win) take = (proven_size_t)c->send_win;
    }
    /* What is already waiting for the peer bounds what more is taken. */
    const proven_size_t pending = h2c_pending(c);
    const proven_size_t room = pending < c->cfg_out ? c->cfg_out - pending : 0;
    if (take > room) take = room;
    const proven_size_t frame = c->peer_frame;
    const bool all = take == data.size;
    if (take > 0 || (all && end_stream)) {
        const proven_size_t frames = take == 0 ? 1 : (take + frame - 1) / frame;
        if (!h2c_out_reserve(c, take + 9 * frames)) { c->oom = false; c->closed = false; return (proven_result_size_t){ .err = PROVEN_ERR_NOMEM, .value = 0 }; }
        proven_size_t done = 0;
        for (proven_size_t i = 0; i < frames; i++) {
            const proven_size_t piece = take - done < frame ? take - done : frame;
            const bool last = i == frames - 1;
            h2c_put_header(c->out + c->out_len, piece, PROVEN_H2_DATA, last && all && end_stream ? PROVEN_H2_FLAG_END_STREAM : 0, stream_id);
            h2c_copy(c->out + c->out_len + 9, data.ptr + done, piece);
            c->out_len += 9 + piece;
            done += piece;
        }
        s->send_win -= (proven_i32)take;
        c->send_win -= (proven_i64)take;
    }
    if (!all) s->flags |= ST_BLOCKED;
    else if (end_stream) h2c_local_end(c, s);
    return (proven_result_size_t){ .err = PROVEN_OK, .value = take };
}

proven_err_t proven_h2_conn_send_trailers(proven_h2_conn_t *c, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count) {
    if (!c || (count > 0 && !headers) || proven_h2_trailers_check(headers, count) != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;
    if (c->closed) return PROVEN_ERR_INVALID_STATE;
    h2c_stream_t *s = h2c_find(c, stream_id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    if ((s->flags & ST_HEAD_SENT) == 0 || (s->flags & ST_LOCAL_END) != 0) return PROVEN_ERR_INVALID_STATE;
    if (h2c_pending(c) >= c->cfg_out) { s->flags |= ST_HBLOCKED; return PROVEN_ERR_BUSY; }
    proven_err_t err = h2c_send_block(c, stream_id, headers, count, true);
    if (err != PROVEN_OK) return err;
    h2c_local_end(c, s);
    return PROVEN_OK;
}

proven_err_t proven_h2_conn_reset(proven_h2_conn_t *c, proven_u32 stream_id, proven_h2_error_t code) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    h2c_stream_t *s = c->closed ? (void *)0 : h2c_find(c, stream_id);
    if (!s) return PROVEN_ERR_NOT_FOUND;
    const bool forgive = (s->flags & ST_REMOTE_END) == 0;
    h2c_q_rst(c, stream_id, (proven_u32)code, false);
    h2c_remove(c, s, false, forgive ? RING_OURS : RING_DONE);
    return PROVEN_OK;
}

void proven_h2_conn_consumed(proven_h2_conn_t *c, proven_u32 stream_id, proven_size_t n) {
    if (!c || c->closed) return;
    h2c_stream_t *s = h2c_find(c, stream_id);
    if (!s) return;
    if (n > s->held) n = s->held;
    s->held -= (proven_u32)n;
    h2c_stream_credit(c, s, n);
    h2c_conn_credit(c, n);
}

proven_err_t proven_h2_conn_ping(proven_h2_conn_t *c, const proven_byte_t opaque[8]) {
    if (!c || !opaque) return PROVEN_ERR_INVALID_ARG;
    if (c->closed) return PROVEN_ERR_INVALID_STATE;
    if (!h2c_out_reserve(c, 17)) { c->oom = false; c->closed = false; return PROVEN_ERR_NOMEM; }
    h2c_put_header(c->out + c->out_len, 8, PROVEN_H2_PING, 0, 0);
    h2c_copy(c->out + c->out_len + 9, opaque, 8);
    c->out_len += 17;
    return PROVEN_OK;
}

void proven_h2_conn_shutdown(proven_h2_conn_t *c, proven_h2_error_t code) {
    if (!c || c->closed) return;
    if (code != PROVEN_H2_NO_ERROR) { h2c_fail(c, (proven_u32)code); return; }
    if (!c->goaway_sent) h2c_q_goaway(c, PROVEN_H2_NO_ERROR);
}

bool proven_h2_conn_is_closed(const proven_h2_conn_t *c) {
    return !c || c->closed;
}

proven_size_t proven_h2_conn_stream_count(const proven_h2_conn_t *c) {
    return c && !c->closed ? c->stream_count : 0;
}

proven_size_t proven_h2_conn_send_room(const proven_h2_conn_t *c, proven_u32 stream_id) {
    const h2c_stream_t *s = c && !c->closed ? h2c_find(c, stream_id) : (void *)0;
    if (!s || (s->flags & ST_HEAD_SENT) == 0 || (s->flags & ST_LOCAL_END) != 0 || s->send_win <= 0 || c->send_win <= 0) return 0;
    proven_size_t room = (proven_size_t)s->send_win;
    if ((proven_i64)room > c->send_win) room = (proven_size_t)c->send_win;
    const proven_size_t pending = h2c_pending(c);
    const proven_size_t out = pending < c->cfg_out ? c->cfg_out - pending : 0;
    return room < out ? room : out;
}

void proven_h2_conn_stream_set_user(proven_h2_conn_t *c, proven_u32 stream_id, void *user) {
    h2c_stream_t *s = c ? h2c_find(c, stream_id) : (void *)0;
    if (s) s->user = user;
}

void *proven_h2_conn_stream_user(const proven_h2_conn_t *c, proven_u32 stream_id) {
    const h2c_stream_t *s = c ? h2c_find(c, stream_id) : (void *)0;
    return s ? s->user : (void *)0;
}
