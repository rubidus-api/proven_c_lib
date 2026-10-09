#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The selector, in both of its kinds: the one the system provides (epoll on Linux, kqueue on
 * the BSDs) and the portable one built on poll, which is the only one Windows has.
 *
 * Every case runs against both where both exist, and the two must answer alike: a program
 * written against one behaves the same on the other, only at a different cost.
 *
 * Sockets come from proven_net_pair, so nothing here depends on a network: what is written
 * into one end makes the other end readable.
 */

enum { PAIRS = 300 };

typedef struct {
    proven_net_conn_t a, b;     /* `a` is registered; `b` is the far end the test writes to */
    bool registered;
    bool ready;                 /* the far end has written and `a` has not been read */
    int seen;
} pair_t;

static pair_t g_pairs[PAIRS];

static void make_ready(pair_t *p) {
    proven_result_size_t w = proven_net_write(&p->b, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 }, proven_net_deadline_in(1000));
    PROVEN_TEST_ASSERT(w.err == PROVEN_OK && w.value == 1, "a byte is written to the far end", "");
    p->ready = true;
}

static void drain(pair_t *p) {
    proven_byte_t buf[64];
    for (;;) {
        proven_result_size_t r = proven_net_read(&p->a, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
        if (r.err != PROVEN_OK) break;
    }
    p->ready = false;
}

/* Wait until nothing more is reported; count, per pair, how often it was. */
static proven_size_t collect(proven_net_selector_t *sel, proven_size_t cap, bool read_them) {
    proven_net_ready_t events[64];
    proven_size_t total = 0;
    for (int round = 0; round < 10000; ++round) {
        proven_size_t n = 0;
        proven_err_t e = proven_net_selector_wait(sel, events, cap, PROVEN_NET_DONT_WAIT, &n);
        if (e == PROVEN_ERR_TIMEOUT) break;
        PROVEN_TEST_ASSERT(e == PROVEN_OK && n > 0 && n <= cap, "a wait that reports something reports between one and cap events", "");
        for (proven_size_t i = 0; i < n; ++i) {
            pair_t *p = events[i].tag;
            PROVEN_TEST_ASSERT(p >= g_pairs && p < g_pairs + PAIRS && (events[i].got & PROVEN_NET_READABLE), "the tag is the one registered, and it is readable", "");
            p->seen++;
            total++;
            if (read_them) drain(p);
        }
        if (!read_them) break;      /* level-triggered: unread sockets would be reported for ever */
    }
    return total;
}

static void run(bool force_poll) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_net_selector_t *sel = NULL;
    proven_err_t e = force_poll ? proven_net_selector_create_poll(heap, &sel) : proven_net_selector_create(heap, &sel);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && sel != NULL, "a selector is created", "");
    proven_net_selector_kind_t kind = proven_net_selector_kind(sel);
    PROVEN_TEST_ASSERT(!force_poll || kind == PROVEN_NET_SELECTOR_POLL, "a selector asked to be the poll kind is", "");
#if defined(__linux__)
    PROVEN_TEST_ASSERT(force_poll || kind == PROVEN_NET_SELECTOR_EPOLL, "on Linux the system's kind is epoll", "");
#endif
    PROVEN_TEST_INFO("kind: {}", PROVEN_ARG(kind == PROVEN_NET_SELECTOR_EPOLL ? "epoll" : kind == PROVEN_NET_SELECTOR_KQUEUE ? "kqueue" : "poll"));
    proven_net_ready_t events[64];
    proven_size_t n = 99;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("register, wait, and what is reported",
        "An empty selector times out; registered sockets are reported when ready and only then; readiness is level-triggered.",
        "Inspect proven_net_selector_wait in src/proven/net.c, and for the system's kind proven_sys_net_selector_wait in platform/proven_sys_net.c.");
    // ---------------------------------------------------------------
    {
        proven_time_t start = proven_time_monotonic_now();
        e = proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(60), &n);
        proven_i64 took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_TIMEOUT && n == 0 && took >= 40 && took < 3000, "an empty selector waits out its deadline: PROVEN_ERR_TIMEOUT", "");

        for (int i = 0; i < PAIRS; ++i) {
            PROVEN_TEST_ASSERT(proven_net_pair(&g_pairs[i].a, &g_pairs[i].b) == PROVEN_OK, "a socket pair", "This test opens 600 sockets; a low descriptor limit fails here.");
            g_pairs[i].registered = false;
            g_pairs[i].ready = false;
            g_pairs[i].seen = 0;
            e = proven_net_selector_add(sel, proven_net_conn_handle(&g_pairs[i].a), PROVEN_NET_READABLE, &g_pairs[i]);
            PROVEN_TEST_ASSERT(e == PROVEN_OK, "a socket is registered", "");
            g_pairs[i].registered = true;
        }
        PROVEN_TEST_ASSERT(proven_net_selector_count(sel) == PAIRS, "the count is the number registered", "");
        PROVEN_TEST_ASSERT(proven_net_selector_add(sel, proven_net_conn_handle(&g_pairs[7].a), PROVEN_NET_READABLE, &g_pairs[7]) == PROVEN_ERR_EXISTS &&
                           proven_net_selector_count(sel) == PAIRS, "registering a socket twice is PROVEN_ERR_EXISTS", "");

        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(30), &n) == PROVEN_ERR_TIMEOUT && n == 0,
            "three hundred idle sockets: nothing to report", "");

        static const int some[] = { 0, 1, 63, 64, 65, 150, 298, 299 };
        for (proven_size_t i = 0; i < 8; ++i) make_ready(&g_pairs[some[i]]);
        e = proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(1000), &n);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && n == 8, "eight of them become readable and exactly eight are reported", "");
        for (int i = 0; i < PAIRS; ++i) g_pairs[i].seen = 0;
        for (proven_size_t i = 0; i < n; ++i) ((pair_t *)events[i].tag)->seen++;
        bool exact = true;
        for (int i = 0; i < PAIRS; ++i) exact = exact && g_pairs[i].seen == (g_pairs[i].ready ? 1 : 0);
        PROVEN_TEST_ASSERT(exact, "each of the eight once, by its own tag, and no other", "");

        e = proven_net_selector_wait(sel, events, 64, PROVEN_NET_DONT_WAIT, &n);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && n == 8, "not read, they are reported again: readiness is level-triggered", "");
        for (int i = 0; i < PAIRS; ++i) if (g_pairs[i].ready) drain(&g_pairs[i]);
        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 64, PROVEN_NET_DONT_WAIT, &n) == PROVEN_ERR_TIMEOUT, "read, they are quiet", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("more ready than fit in one report",
        "With cap smaller than the number of ready sockets, successive waits report all of them and none is starved.",
        "Inspect the rotating start in the poll kind of proven_net_selector_wait.");
    // ---------------------------------------------------------------
    {
        for (int i = 0; i < PAIRS; ++i) { g_pairs[i].seen = 0; if (i % 3 == 0) make_ready(&g_pairs[i]); }
        /* Without reading, ask repeatedly with a small cap: over enough waits every ready
         * socket must have been reported, not only the first seven. */
        for (int round = 0; round < 60; ++round) {
            e = proven_net_selector_wait(sel, events, 7, PROVEN_NET_DONT_WAIT, &n);
            PROVEN_TEST_ASSERT(e == PROVEN_OK && n >= 1 && n <= 7, "a wait with cap 7 reports at most seven", "");
            for (proven_size_t i = 0; i < n; ++i) ((pair_t *)events[i].tag)->seen++;
        }
        int ready = 0, reported = 0;
        for (int i = 0; i < PAIRS; ++i) { if (g_pairs[i].ready) { ready++; if (g_pairs[i].seen > 0) reported++; } else PROVEN_TEST_ASSERT(g_pairs[i].seen == 0, "an idle socket is never reported", ""); }
        if (kind == PROVEN_NET_SELECTOR_POLL) {
            PROVEN_TEST_ASSERT(ready == 100 && reported == 100, "the poll kind rotates: sixty waits of seven reach all hundred", "");
        } else {
            /* The system decides the order of its own queue; what is promised is that reading
             * them clears them, which the next step checks. */
            PROVEN_TEST_ASSERT(ready == 100 && reported >= 7, "the system's kind reports from its own queue", "");
        }
        for (int i = 0; i < PAIRS; ++i) g_pairs[i].seen = 0;
        proven_size_t total = collect(sel, 7, true);
        bool each_once = true;
        for (int i = 0; i < PAIRS; ++i) each_once = each_once && g_pairs[i].seen == (i % 3 == 0 ? 1 : 0);
        PROVEN_TEST_ASSERT(total == 100 && each_once, "reading each as it is reported: all hundred, each exactly once", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("change, remove, hang-up, writability",
        "What is asked of a socket and its tag can be changed; a removed socket is not reported; a peer that closes is reported as readable; a socket asked about writing is reported writable.",
        "Inspect proven_net_selector_modify and _remove. For the poll kind, _remove is where the hash is repaired.");
    // ---------------------------------------------------------------
    {
        pair_t *p = &g_pairs[10], *q = &g_pairs[11];
        make_ready(p);
        PROVEN_TEST_ASSERT(proven_net_selector_modify(sel, proven_net_conn_handle(&p->a), 0, p) == PROVEN_OK, "ask nothing of a socket", "");
        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(30), &n) == PROVEN_ERR_TIMEOUT, "its readable data is then not reported", "");
        PROVEN_TEST_ASSERT(proven_net_selector_modify(sel, proven_net_conn_handle(&p->a), PROVEN_NET_READABLE, q) == PROVEN_OK &&
                           proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(1000), &n) == PROVEN_OK && n == 1 && events[0].tag == q,
            "asked again, with another tag: reported, with the new tag", "");
        PROVEN_TEST_ASSERT(proven_net_selector_modify(sel, proven_net_conn_handle(&p->a), PROVEN_NET_READABLE, p) == PROVEN_OK, "the tag is put back", "");

        PROVEN_TEST_ASSERT(proven_net_selector_remove(sel, proven_net_conn_handle(&p->a)) == PROVEN_OK && proven_net_selector_count(sel) == PAIRS - 1, "a socket is removed", "");
        p->registered = false;
        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(30), &n) == PROVEN_ERR_TIMEOUT, "still readable, no longer reported", "");
        PROVEN_TEST_ASSERT(proven_net_selector_remove(sel, proven_net_conn_handle(&p->a)) == PROVEN_ERR_NOT_FOUND &&
                           proven_net_selector_modify(sel, proven_net_conn_handle(&p->a), PROVEN_NET_READABLE, p) == PROVEN_ERR_NOT_FOUND,
            "removing or changing what is not there is PROVEN_ERR_NOT_FOUND", "");
        drain(p);
        PROVEN_TEST_ASSERT(proven_net_selector_add(sel, proven_net_conn_handle(&p->a), PROVEN_NET_READABLE, p) == PROVEN_OK, "and it can be registered again", "");
        p->registered = true;

        /* The far end goes away. */
        (void)proven_net_close(&q->b);
        e = proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(1000), &n);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && n == 1 && events[0].tag == q && (events[0].got & PROVEN_NET_READABLE), "a peer that closed makes its socket readable", "");
        proven_byte_t b[4];
        PROVEN_TEST_ASSERT(proven_net_read(&q->a, (proven_mem_mut_t){ b, sizeof b }, PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_EOF, "and the read that follows says why: PROVEN_ERR_EOF", "");
        /* Remove before closing - the rule the header states. */
        PROVEN_TEST_ASSERT(proven_net_selector_remove(sel, proven_net_conn_handle(&q->a)) == PROVEN_OK, "it is removed before it is closed", "");
        q->registered = false;
        (void)proven_net_close(&q->a);
        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(30), &n) == PROVEN_ERR_TIMEOUT, "and nothing is left behind to report", "");

        pair_t *w = &g_pairs[12];
        PROVEN_TEST_ASSERT(proven_net_selector_modify(sel, proven_net_conn_handle(&w->a), PROVEN_NET_WRITABLE, w) == PROVEN_OK &&
                           proven_net_selector_wait(sel, events, 64, proven_net_deadline_in(1000), &n) == PROVEN_OK && n == 1 && events[0].tag == w &&
                           (events[0].got & PROVEN_NET_WRITABLE) && !(events[0].got & PROVEN_NET_READABLE),
            "an idle socket asked about writing is writable, and not readable", "");
        PROVEN_TEST_ASSERT(proven_net_selector_modify(sel, proven_net_conn_handle(&w->a), PROVEN_NET_READABLE, w) == PROVEN_OK, "back to reading", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("churn",
        "Twenty thousand random registrations, removals and readiness changes, checked against a model after each wait.",
        "A failure prints the step. For the poll kind this is the test of the hash's deletion; inspect proven_net_selector_remove.");
    // ---------------------------------------------------------------
    {
        proven_xoshiro256ss_t g;
        proven_xoshiro256ss_seed(&g, force_poll ? 1203 : 1204);
        proven_rng_t rng = proven_xoshiro256ss_rng(&g);
        proven_size_t registered = 0;
        for (int i = 0; i < PAIRS; ++i) if (g_pairs[i].registered) registered++;
        for (int step = 0; step < 20000; ++step) {
            int i = (int)proven_rng_below(rng, PAIRS);
            pair_t *p = &g_pairs[i];
            if (i == 11) continue;                      /* that pair was closed above */
            int op = (int)proven_rng_below(rng, 4);
            if (op == 0 && !p->registered) {
                e = proven_net_selector_add(sel, proven_net_conn_handle(&p->a), PROVEN_NET_READABLE, p);
                if (e == PROVEN_OK) { p->registered = true; registered++; }
            } else if (op == 1 && p->registered) {
                e = proven_net_selector_remove(sel, proven_net_conn_handle(&p->a));
                if (e == PROVEN_OK) { p->registered = false; registered--; }
            } else if (op == 2 && !p->ready) {
                make_ready(p);
                e = PROVEN_OK;
            } else if (op == 3 && p->ready) {
                drain(p);
                e = PROVEN_OK;
            } else {
                continue;
            }
            if (e != PROVEN_OK) PROVEN_TEST_INFO("step {} op {} error {}", PROVEN_ARG(step), PROVEN_ARG(op), PROVEN_ARG((int)e));
            PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_net_selector_count(sel) == registered, "each change succeeds and the count follows", "");
            if (step % 97 != 0) continue;
            /* Everything registered and ready is reported; nothing else is. */
            for (int k = 0; k < PAIRS; ++k) g_pairs[k].seen = 0;
            proven_size_t want = 0;
            for (int k = 0; k < PAIRS; ++k) if (g_pairs[k].registered && g_pairs[k].ready) want++;
            proven_size_t got = 0;
            for (int round = 0; round < 40 && got < want; ++round) {
                proven_size_t m = 0;
                proven_err_t we = proven_net_selector_wait(sel, events, 64, PROVEN_NET_DONT_WAIT, &m);
                if (we == PROVEN_ERR_TIMEOUT) break;
                for (proven_size_t k = 0; k < m; ++k) {
                    pair_t *r = events[k].tag;
                    if (r->seen++ == 0) got++;
                    bool ok = r->registered && r->ready;
                    if (!ok) PROVEN_TEST_INFO("step {}: pair {} reported, registered {} ready {}", PROVEN_ARG(step), PROVEN_ARG((int)(r - g_pairs)), PROVEN_ARG((int)r->registered), PROVEN_ARG((int)r->ready));
                    PROVEN_TEST_ASSERT(ok, "a reported socket is one that is registered and ready", "");
                }
                if (m < 64) break;
            }
            if (want <= 64 && got != want) PROVEN_TEST_INFO("step {}: {} ready, {} reported", PROVEN_ARG(step), PROVEN_ARG((proven_u64)want), PROVEN_ARG((proven_u64)got));
            PROVEN_TEST_ASSERT(want > 64 || got == want, "every registered, ready socket is reported", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("arguments",
        "Null pointers, a zero cap and handles that are not open are PROVEN_ERR_INVALID_ARG.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t closed = {0};
        proven_net_selector_t *none = sel;
        proven_allocator_t bad = {0};
        PROVEN_TEST_ASSERT(proven_net_selector_create(bad, &none) == PROVEN_ERR_INVALID_ARG && none == NULL &&
                           proven_net_selector_create_poll(bad, &none) == PROVEN_ERR_INVALID_ARG, "no allocator", "");
        PROVEN_TEST_ASSERT(proven_net_selector_add(sel, proven_net_conn_handle(&closed), PROVEN_NET_READABLE, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_net_selector_remove(sel, proven_net_conn_handle(&closed)) == PROVEN_ERR_INVALID_ARG, "a handle that is not open", "");
        PROVEN_TEST_ASSERT(proven_net_selector_wait(sel, events, 0, PROVEN_NET_DONT_WAIT, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_net_selector_wait(sel, NULL, 4, PROVEN_NET_DONT_WAIT, &n) == PROVEN_ERR_INVALID_ARG &&
                           proven_net_selector_wait(NULL, events, 4, PROVEN_NET_DONT_WAIT, &n) == PROVEN_ERR_INVALID_ARG, "a zero cap and null pointers", "");
        PROVEN_TEST_ASSERT(proven_net_selector_count(NULL) == 0 && proven_net_selector_kind(NULL) == PROVEN_NET_SELECTOR_POLL, "the getters on NULL", "");
        proven_net_selector_destroy(NULL);
    }

    for (int i = 0; i < PAIRS; ++i) {
        if (g_pairs[i].registered) (void)proven_net_selector_remove(sel, proven_net_conn_handle(&g_pairs[i].a));
        (void)proven_net_close(&g_pairs[i].a);
        (void)proven_net_close(&g_pairs[i].b);
    }
    PROVEN_TEST_ASSERT(proven_net_selector_count(sel) == 0, "everything removed: the count is zero", "");
    proven_net_selector_destroy(sel);
}

int main(void) {
    PROVEN_TEST_SUITE("net: the selector",
        "A registered set of sockets and a wait that reports the ready ones, in the system's kind and in the portable one.",
        "Inspect the Selector section of src/proven/net.c and of platform/proven_sys_net.c.");

    {
        proven_net_conn_t a, b;
        proven_err_t err = proven_net_pair(&a, &b);
        if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
            PROVEN_TEST_INFO("SKIP: this environment refuses to open a socket (error {}).", PROVEN_ARG((int)err));
            PROVEN_TEST_PASS("skipped: no sockets here.");
            return 0;
        }
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "a socket pair opens", "");
        (void)proven_net_close(&a);
        (void)proven_net_close(&b);
    }

    run(false);
    run(true);

    PROVEN_TEST_PASS("both kinds of selector keep the same contract.");
    return 0;
}
