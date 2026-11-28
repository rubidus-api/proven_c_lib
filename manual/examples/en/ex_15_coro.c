#include "example.h"
#include <string.h>

/*
 * A coroutine per connection, driven by the loop.
 *
 * Each request is a session coroutine - message after message - that drives a child, the
 * reader of chapter 6, which takes a two-byte big-endian length and that many bytes as they
 * arrive. The session answers each message with an echo. The loop knows nothing of coroutines;
 * three things connect them:
 *
 *   - The reason. A step that returns 0 has written why: it used its turn (a yield), or it
 *     wants input (a receive). The dispatcher reads it once after the step.
 *   - The ready queue, in the requests. A request that can run is linked in once (`queued`);
 *     one function posted to the loop steps everything queued when it began, in order. A yield
 *     goes to the back, for the next round, so one busy connection cannot hold the loop.
 *   - The wake. A request that wants input parks on a receive of zero bytes, after trying to
 *     read without one first; the receive's function queues it again.
 *
 * And one rule for the end: only a step closes its request's socket, and nothing queues a
 * request that is closing. So when `closed` comes, nothing of the request is queued or in
 * flight, and its memory can go there.
 */

#define BUDGET 1                        /* messages a step may answer before it gives up its turn */
enum { WAIT_NONE, WAIT_YIELD, WAIT_RECV };

typedef struct ex_app ex_app_t;

typedef struct ex_req {
    ex_app_t *app;
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    proven_coro_t session;              /* the parent: one message after another */
    proven_coro_t reader;               /* the child: one message */
    int high, length, got;              /* the reader's state: a local would not survive a yield */
    proven_byte_t in[64];
    proven_size_t have, pos;            /* bytes in `in`, bytes used */
    proven_byte_t out[2 + 64];
    int wait;                           /* why the last step returned 0 */
    int answered;                       /* messages answered in this step */
    int steps;
    unsigned ticket;                    /* when it was queued: the queue's order */
    bool input_ended, stopping, queued, closing, used;
    struct ex_req *next;                /* the ready queue: a link in the request, no allocation */
    char name;
} ex_req_t;

struct ex_app {
    proven_loop_t *loop;
    ex_req_t reqs[2];
    ex_req_t *head, *tail;              /* the ready queue */
    bool drain_posted;
    char last;                          /* the request stepped last */
    unsigned tickets;
    int replies, closed, unfair, stray;
};

// ---- the child and the parent --------------------------------------------------------

/* The reader: takes bytes from r->in as they are there. Stepped only by the session. */
static proven_i32 reader_step(ex_req_t *r) {
    PROVEN_CORO_BEGIN(&r->reader);
    PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
    r->high = r->in[r->pos++];
    PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
    r->length = (r->high << 8) | r->in[r->pos++];
    r->got = 0;
    if (r->length > 64) { r->input_ended = true; r->length = 0; }     /* longer than this program takes: the session ends */
    while (r->got < r->length) {
        PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
        proven_size_t take = r->have - r->pos;
        if (take > (proven_size_t)(r->length - r->got)) take = (proven_size_t)(r->length - r->got);
        memcpy(r->out + 2 + r->got, r->in + r->pos, take);
        r->pos += take;
        r->got += (int)take;
    }
    PROVEN_CORO_END(&r->reader);
}

/* Read what has arrived, without waiting: 1 bytes came, 0 nothing now, -1 the input has ended. */
static int req_fill(ex_req_t *r) {
    if (r->pos > 0) {                                   /* keep only what is not used yet */
        memmove(r->in, r->in + r->pos, r->have - r->pos);
        r->have -= r->pos;
        r->pos = 0;
    }
    if (r->have == sizeof r->in) return 1;
    proven_result_size_t got = proven_loop_read(r->app->loop, &r->sock, (proven_mem_mut_t){ r->in + r->have, sizeof r->in - r->have });
    if (got.err == PROVEN_ERR_TIMEOUT) return 0;
    if (got.err != PROVEN_OK) return -1;                /* PROVEN_ERR_EOF, PROVEN_ERR_RESET */
    r->have += got.value;
    return 1;
}

/* The session. Every return of 0 writes its reason first. */
static proven_i32 session_step(ex_req_t *r) {
    PROVEN_CORO_BEGIN(&r->session);
    while (!r->stopping && !r->input_ended) {
        PROVEN_CORO_INIT(&r->reader);
        while (!reader_step(r) && !r->input_ended) {
            const int f = req_fill(r);                  /* try without parking first */
            if (f > 0) continue;
            if (f < 0) { r->input_ended = true; break; }
            r->wait = WAIT_RECV;
            PROVEN_CORO_YIELD(&r->session);
            if (r->stopping) break;
        }
        /* the input ended, the message was too long, or the program is stopping: no reply */
        if (!PROVEN_CORO_IS_DONE(&r->reader) || r->input_ended) break;
        r->out[0] = (proven_byte_t)(r->length >> 8);
        r->out[1] = (proven_byte_t)r->length;
        if (proven_loop_send(r->app->loop, &r->sock, (proven_mem_view_t){ r->out, (proven_size_t)r->length + 2 }).err != PROVEN_OK) break;
        r->app->replies++;
        if (++r->answered >= BUDGET) {
            r->wait = WAIT_YIELD;
            PROVEN_CORO_YIELD(&r->session);
        }
    }
    PROVEN_CORO_END(&r->session);
}

// ---- the dispatcher ------------------------------------------------------------------

static void drain(void *ctx);

/* Queue a request once. A request that is closing is never queued again. */
static void wake(ex_req_t *r) {
    if (r->closing) return;
    ex_app_t *app = r->app;
    if (!r->queued) {
        r->queued = true;
        r->ticket = ++app->tickets;
        r->next = NULL;
        if (app->tail) app->tail->next = r; else app->head = r;
        app->tail = r;
    }
    if (!app->drain_posted) {
        app->drain_posted = true;
        /* PROVEN_ERR_NOMEM: nothing is lost - the requests stay queued, and the next wake (even
         * of a request already queued) posts again. */
        if (proven_loop_post(app->loop, drain, app) != PROVEN_OK) app->drain_posted = false;
    }
}

/* One step, and what follows from its reason. */
static void step(ex_req_t *r) {
    ex_app_t *app = r->app;
    if (!r->used) { app->stray++; return; }             /* would be a step on freed memory */
    r->queued = false;
    const ex_req_t *other = &app->reqs[r == &app->reqs[0]];
    /* twice in a row while the other had been queued first, and waited */
    if (app->last == r->name && other->queued && other->ticket < r->ticket) app->unfair++;
    app->last = r->name;
    r->steps++;
    r->answered = 0;
    r->wait = WAIT_NONE;
    if (session_step(r)) {                              /* done: the end of the request */
        r->closing = true;
        proven_loop_close(app->loop, &r->sock);         /* a receive in flight is answered CANCELLED */
        return;
    }
    const int why = r->wait;
    r->wait = WAIT_NONE;
    EXAMPLE_REQUIRE(why != WAIT_NONE, "a step that returns 0 has said why");
    if (why == WAIT_YIELD) { wake(r); return; }         /* the back of the queue: a later round */
    /* Wants input: park on a receive of zero bytes, which holds no buffer while it waits. */
    if (proven_loop_recv(app->loop, &r->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) { r->input_ended = true; wake(r); }
}

/* Step what was queued when this began, in order; what a step queues again waits for the next. */
static void drain(void *ctx) {
    ex_app_t *app = ctx;
    app->drain_posted = false;
    ex_req_t *r = app->head, *last = app->tail;
    app->head = app->tail = NULL;
    while (r) {
        ex_req_t *next = r->next;
        const bool end = r == last;
        step(r);
        if (end) break;
        r = next;
    }
}

// ---- the loop's functions --------------------------------------------------------------

static void req_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_req_t *r = ctx;
    (void)n;                                            /* a receive of zero bytes takes nothing */
    if (err == PROVEN_ERR_CANCELLED) return;            /* it is being closed */
    if (err != PROVEN_OK) r->input_ended = true;        /* PROVEN_ERR_EOF, PROVEN_ERR_RESET: the session ends */
    wake(r);
}

static void req_closed(void *ctx) {
    ex_req_t *r = ctx;
    (void)proven_net_close(&r->conn);
    if (r->queued) r->app->stray++;                     /* cannot be: only a step closes, and wake refuses */
    r->used = false;                                    /* the request's memory may go now */
    r->app->closed++;
}

static const proven_loop_sock_fns_t req_fns = { .received = req_received, .closed = req_closed };

// ---- the other ends, in this thread -------------------------------------------------------

static void say(proven_net_conn_t *c, const char *text) {
    proven_byte_t msg[2 + 64];
    const proven_size_t n = strlen(text);
    msg[0] = (proven_byte_t)(n >> 8);
    msg[1] = (proven_byte_t)n;
    memcpy(msg + 2, text, n);
    proven_result_size_t w = proven_net_write(c, (proven_mem_view_t){ msg, n + 2 }, proven_net_deadline_in(1000));
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == n + 2, "a message written");
}

static bool heard(proven_net_conn_t *c, const char *text) {
    proven_byte_t got[2 + 64];
    const proven_size_t want = strlen(text) + 2;
    for (proven_size_t have = 0; have < want; ) {
        proven_result_size_t r = proven_net_read(c, (proven_mem_mut_t){ got + have, want - have }, proven_net_deadline_in(1000));
        if (r.err != PROVEN_OK) return false;
        have += r.value;
    }
    return got[0] == 0 && got[1] == strlen(text) && memcmp(got + 2, text, strlen(text)) == 0;
}

static void run_until(ex_app_t *app, bool (*done)(const ex_app_t *)) {
    for (int i = 0; i < 200 && !done(app); ++i) {
        EXAMPLE_REQUIRE(proven_loop_poll(app->loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round");
    }
    EXAMPLE_REQUIRE(done(app), "the loop got there");
}
static bool six_replies(const ex_app_t *app) { return app->replies == 6 && !app->head; }
static bool seven_replies(const ex_app_t *app) { return app->replies == 7 && !app->head; }
static bool one_closed(const ex_app_t *app) { return app->closed == 1; }
static bool both_closed(const ex_app_t *app) { return app->closed == 2; }
static bool both_parked(const ex_app_t *app) { return app->reqs[0].steps > 0 && app->reqs[1].steps > 0 && !app->head; }

int main(void) {
    static ex_app_t app;
    proven_net_conn_t peer[2];
    EXAMPLE_REQUIRE(proven_loop_create(proven_heap_allocator(), &app.loop) == PROVEN_OK, "a loop");
    for (int i = 0; i < 2; ++i) {
        ex_req_t *r = &app.reqs[i];
        *r = (ex_req_t){ .app = &app, .used = true, .name = (char)('A' + i) };
        EXAMPLE_REQUIRE(proven_net_pair(&r->conn, &peer[i]) == PROVEN_OK, "a connected pair");
        PROVEN_CORO_INIT(&r->session);
        EXAMPLE_REQUIRE(proven_loop_attach(app.loop, &r->sock, &r->conn, &req_fns, r) == PROVEN_OK, "the request's end given to the loop");
        wake(r);                                        /* a new request starts with a step */
    }
    run_until(&app, both_parked);

    // ---- three messages to each: they take turns, one message a step ----------------------
    say(&peer[0], "one"); say(&peer[0], "two"); say(&peer[0], "three");
    say(&peer[1], "uno"); say(&peer[1], "dos"); say(&peer[1], "tres");
    run_until(&app, six_replies);
    EXAMPLE_REQUIRE(heard(&peer[0], "one") && heard(&peer[0], "two") && heard(&peer[0], "three"), "A's three echoes, in order");
    EXAMPLE_REQUIRE(heard(&peer[1], "uno") && heard(&peer[1], "dos") && heard(&peer[1], "tres"), "B's three echoes, in order");
    EXAMPLE_REQUIRE(app.unfair == 0, "no request was stepped twice in a row while the other was queued");

    // ---- a request that waits for input takes no steps --------------------------------------
    const int b_steps = app.reqs[1].steps;
    say(&peer[0], "four");
    run_until(&app, seven_replies);
    EXAMPLE_REQUIRE(heard(&peer[0], "four"), "A's fourth echo");
    EXAMPLE_REQUIRE(app.reqs[1].steps == b_steps, "B, waiting for input, was not stepped while A was busy");

    // ---- the end of input ends the session -----------------------------------------------------
    (void)proven_net_close(&peer[0]);
    run_until(&app, one_closed);
    EXAMPLE_REQUIRE(!app.reqs[0].used && app.reqs[1].used, "A ended at its EOF, closed by its own step; B goes on");

    // ---- stopping a request that waits on a receive in flight ---------------------------------
    app.reqs[1].stopping = true;
    wake(&app.reqs[1]);                                 /* the session sees `stopping` and ends */
    run_until(&app, both_closed);
    EXAMPLE_REQUIRE(!app.reqs[1].used, "B stopped: its receive in flight cancelled, then closed");
    EXAMPLE_REQUIRE(app.stray == 0, "nothing reached a request after its `closed`");

    (void)proven_net_close(&peer[1]);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
