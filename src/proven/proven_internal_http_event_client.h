#ifndef PROVEN_INTERNAL_HTTP_EVENT_CLIENT_H
#define PROVEN_INTERNAL_HTTP_EVENT_CLIENT_H

#include "proven/h2.h"
#include "proven/http_event_client.h"
#include "proven/url.h"
#include "proven_internal_http_coding.h"

/*
 * Internal: the event-driven client's own types, for the two files that are it -
 * http_event_client.c (HTTP/1.1, and what both protocols share) and http_event_client_h2.c
 * (HTTP/2: connections kept by the client, each carrying many requests). Not public API.
 */

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#define CL_READ_BYTES ((proven_size_t)64 * 1024)
#define CL_PLAIN_BYTES ((proven_size_t)16 * 1024 + 512)
#define CL_MAX_INTERIM 8u

#define CL_CONNECTING 0     /* the connect has not finished */
#define CL_HEAD 1           /* connected; the response head has not arrived */
#define CL_BODY 2           /* reading the response body */

#define CL_T_NONE 0
#define CL_T_CONNECT 1
#define CL_T_WRITE 2
#define CL_T_RESPONSE 3
#define CL_T_BODY 4
#define CL_T_DELAY 5        /* not a limit: the wait before a try of retry_unsent, the request with no connection */

typedef struct { proven_byte_t *ptr; proven_size_t len, cap, off; bool borrowed; } cl_buf_t;

/* A response body being decoded: made when such a response arrives, freed with the request. */
typedef struct {
    proven_http_decoder_t_ d;
    proven_u64 total;                  /* decoded bytes delivered */
    proven_size_t pay_off;             /* a payload being decoded: where it begins, counted from the stash's read position, */
    proven_size_t pay_len;             /* how much of it is left, */
    proven_size_t pay_tail;            /* and the framing bytes after it that were used with it */
    bool have;                         /* there is such a payload */
    bool pay_last;                     /* it ends the body */
    proven_byte_t piece[PROVEN_HTTP_CODING_PIECE];
} cl_dec_t;

/* What every object a caller may hold as a `proven_http_event_request_t *` begins with: this
 * request-and-connection of HTTP/1.1, and a request on an HTTP/2 connection, which is another
 * struct. `kind` says which, and every public function reads it before anything else. */
typedef struct {
    proven_http_event_client_t *client;
    proven_u32 depth;
    proven_u8 kind;                    /* CL_KIND_* */
} cl_head_t;

#define CL_KIND_H1 0
#define CL_KIND_H2 1

/* An HTTP/1.1 connection: what carries one request at a time and, when that request's
 * exchange ended cleanly, is kept for the next one to the same origin. `owner` is the request
 * it carries now; null while it is idle. */
typedef struct cl_conn {
    proven_http_event_client_t *client;
    struct cl_conn *prev, *next;
    proven_http_event_request_t *owner;
    proven_net_conn_t sock;
    proven_loop_io_t io;
    proven_loop_timer_t timer;         /* how long it may sit idle */
    proven_tls_conn_t *tls;
    proven_net_addr_t to;              /* the origin: where it leads, */
    proven_time_t idle_since;          /* when it was last put by */
    proven_size_t unfed;               /* bytes of the read in hand that TLS has not been given yet */
    proven_u32 uses;                   /* exchanges it has carried to their end */
    proven_u16 port;                   /* the URL's port, */
    proven_u8 host_len;                /* the URL's host, */
    bool https;                        /* and the scheme */
    bool connected_at_start;
    bool idle;                         /* counted in the client's idle_count */
    proven_byte_t host[256];
} cl_conn_t;

struct proven_http_event_request {
    cl_head_t h;
    struct proven_http_event_request *prev, *next;
    /* What the caller knows this request as: itself, but for a request that was started
     * while its origin's protocol was not yet known and has turned out to be HTTP/1.1 -
     * the caller holds the object it was given then, and the callbacks name that one. */
    proven_http_event_request_t *face;
    cl_conn_t *k;                      /* its connection; null while it waits for one */
    struct proven_http_event_request *wnext;      /* the next request waiting, of any origin, in the order of starting */
    proven_loop_timer_t timer;
    proven_net_addr_t to;              /* the origin, as in cl_conn_t; the host's bytes lie behind this struct */
    cl_buf_t out;                      /* request bytes the transport has not taken yet */
    cl_buf_t stash;                    /* response bytes not used yet */
    proven_http_body_t body;
    proven_http_event_client_callbacks_t on;
    void *ctx;
    void *user;
    proven_u64 send_left;              /* request-body bytes still promised; LENGTH_UNKNOWN when chunked */
    cl_dec_t *dec;                     /* the response body is decoded on its way to on_body */
    proven_u32 interim;
    proven_u32 tries;                  /* times it was begun again on a new connection, none of it having been sent (retry_unsent) */
    proven_http_method_t method;
    proven_u16 port;
    proven_u8 host_len;
    proven_u8 state;
    proven_u8 timer_kind;
    bool https, streamed, chunked, request_ended, want_writable, paused, peer_eof, processing, dead;
    bool waiting;                      /* in the client's queue, with no connection */
    bool keep;                         /* the response allows the connection to be used again */
    bool stale;                        /* on a connection that carried a request before, with nothing come back on it yet: it may have been dead when it was given */
    bool keep_out;                     /* ... and so `out` is kept after it is sent, for it may have to be sent again */
    bool sent;                         /* some of the request has been given to its connection */
    bool no_resend;                    /* ended by its caller or its client: not to be sent again */
};

struct proven_http_event_client {
    proven_loop_t *loop;
    proven_allocator_t alloc;
    proven_http_event_client_config_t cfg;
    proven_http_event_request_t *requests;
    proven_size_t request_count;
    cl_conn_t *conns;                  /* HTTP/1.1 connections, busy and idle */
    proven_size_t idle_count;
    proven_size_t conn_count;          /* HTTP/1.1 connections there are */
    proven_http_event_request_t *wait_head, *wait_tail;       /* requests waiting for a connection to their origin */
    proven_size_t wait_count;
    bool closing;                      /* being destroyed: nothing new is begun */
    proven_http_header_t *headers;
    proven_byte_t *rbuf, *pbuf, *hbuf;
    void *h2;                          /* http_event_client_h2.c: its connections, its origins */
    void *h2_delayed, *h2_delayed_last;    /* and its requests that wait to be tried again (retry_delay_ms), the first due first */
    proven_loop_timer_t h2_delay_timer;
    proven_byte_t *h2buf;              /* where every HTTP/2 connection reads its header lists */
    proven_size_t h2buf_size;
};

// -----------------------------------------------------------------------------
// Between the two files
// -----------------------------------------------------------------------------

/* Start an HTTP/1.1 request. `face`: null, or the object the caller already holds for it. */
[[nodiscard]]
proven_err_t proven_http_event_client_start_h1_(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                                proven_http_event_request_t *face, proven_http_event_request_t **out);

/* HTTP/2 (http_event_client_h2.c). `start`: PROVEN_ERR_UNSUPPORTED when this request is not
 * one for HTTP/2 - its origin is known to speak HTTP/1.1 - and is to be started as such. */
[[nodiscard]]
proven_err_t proven_http_event_client_h2_start_(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                                bool https, const proven_url_t *url, proven_net_addr_t to,
                                                proven_http_event_request_t **out);
void proven_http_event_client_h2_destroy_(proven_http_event_client_t *client);
/* For max_connections, which counts both protocols. In http_event_client_h2.c: how many
 * HTTP/2 connections there are, and "close one that has no request on it" (false: none).
 * In http_event_client.c: whether the client may have one more connection, an idle one
 * closed for it if need be; and "a place was freed", for the HTTP/1.1 requests that wait. */
proven_size_t proven_http_event_client_h2_conns_(const proven_http_event_client_t *client);
bool proven_http_event_client_h2_close_idle_(proven_http_event_client_t *client);
bool proven_http_event_client_room_(proven_http_event_client_t *client);
void proven_http_event_client_freed_(proven_http_event_client_t *client);
/* The HTTP/1.1 request that `face` became has ended and its on_done has returned. */
void proven_http_event_client_h2_forwarded_done_(proven_http_event_request_t *face);
[[nodiscard]]
proven_result_size_t proven_http_event_client_h2_write_(proven_http_event_request_t *request, proven_mem_view_t data);
[[nodiscard]]
proven_err_t proven_http_event_client_h2_end_(proven_http_event_request_t *request);
void proven_http_event_client_h2_abort_(proven_http_event_request_t *request);
void proven_http_event_client_h2_pause_(proven_http_event_request_t *request);
void proven_http_event_client_h2_resume_(proven_http_event_request_t *request);
void proven_http_event_client_h2_set_user_(proven_http_event_request_t *request, void *user);
[[nodiscard]]
void *proven_http_event_client_h2_user_(const proven_http_event_request_t *request);
[[nodiscard]]
bool proven_http_event_client_h2_is_h1_(const proven_http_event_request_t *request);

/*
 * For tests. A client has 2^30 stream numbers on a connection, and a test cannot use them up
 * by asking: these put the next number where the test says, in an engine and in every
 * HTTP/2 connection a client has. Not public API.
 */
void proven_h2_conn_test_next_id_(proven_h2_conn_t *conn, proven_u32 next_id);
void proven_http_event_client_h2_test_next_id_(proven_http_event_client_t *client, proven_u32 next_id);

#define PROVEN_HTTP_EVENT_REQUEST_IS_H2_(request) (((const cl_head_t *)(const void *)(request))->kind == CL_KIND_H2)

#endif /* hosted, with a network */

#endif /* PROVEN_INTERNAL_HTTP_EVENT_CLIENT_H */
