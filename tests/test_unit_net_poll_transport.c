#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Readiness on many sockets at once, and the transport interface with its stream adapters.
 *
 * Readiness is stated as "the next call of that kind will not wait", so each assertion here
 * pairs a poll answer with the call it predicts. The transport half runs the same code over a
 * real connection and over a transport made of memory, because that is the claim: a protocol
 * written against proven_transport_t does not know which it has.
 */

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

/* A transport over memory: reads hand out a fixed script a few bytes at a time, writes are
 * collected, and each can be told to misbehave. */
typedef struct {
    const char *script;
    proven_size_t script_pos;
    proven_size_t read_step;
    proven_byte_t written[64];
    proven_size_t written_len;
    proven_size_t write_step;       /* 0: accept nothing and report no error */
    proven_size_t fail_after;       /* fail with RESET once this many bytes are written */
    int shutdowns;
    int closes;
} mem_transport_t;

static proven_result_size_t mem_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    (void)until;
    mem_transport_t *m = ctx;
    proven_result_size_t r = { PROVEN_OK, 0 };
    proven_size_t left = strlen(m->script) - m->script_pos;
    if (left == 0) { r.err = PROVEN_ERR_EOF; return r; }
    proven_size_t n = left < m->read_step ? left : m->read_step;
    if (n > dest.size) n = dest.size;
    memcpy(dest.ptr, m->script + m->script_pos, n);
    m->script_pos += n;
    r.value = n;
    return r;
}

static proven_result_size_t mem_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    (void)until;
    mem_transport_t *m = ctx;
    proven_result_size_t r = { PROVEN_OK, 0 };
    proven_size_t n = src.size < m->write_step ? src.size : m->write_step;
    if (m->written_len + n > m->fail_after) {
        n = m->fail_after - m->written_len;
        r.err = PROVEN_ERR_RESET;
    }
    memcpy(m->written + m->written_len, src.ptr, n);
    m->written_len += n;
    r.value = n;
    return r;
}

static proven_err_t mem_shutdown(void *ctx) { ((mem_transport_t *)ctx)->shutdowns++; return PROVEN_OK; }
static proven_err_t mem_close(void *ctx) { ((mem_transport_t *)ctx)->closes++; return PROVEN_OK; }

static proven_transport_t mem_transport(mem_transport_t *m) {
    return (proven_transport_t){ .ctx = m, .read_fn = mem_read, .write_fn = mem_write,
                                 .shutdown_fn = mem_shutdown, .close_fn = mem_close };
}

int main(void) {
    PROVEN_TEST_SUITE("net: readiness and the transport interface",
        "proven_net_poll predicts which call will not wait; a transport carries the same code over a socket or over memory.",
        "Inspect proven_net_poll_with and the transport functions at the end of src/proven/net.c.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a transport made of memory",
        "write_all keeps going across short writes, reports the count on failure, and refuses to spin on a sink that takes nothing.",
        "");
    // ---------------------------------------------------------------
    {
        mem_transport_t m = { .script = "line one\nline two\n", .read_step = 3, .write_step = 2, .fail_after = 64 };
        proven_transport_t t = mem_transport(&m);
        PROVEN_TEST_ASSERT(proven_transport_is_valid(t), "a transport with read and write is valid", "");

        proven_result_size_t w = proven_transport_write_all(t, mv("hello"), PROVEN_NET_NO_DEADLINE);
        PROVEN_TEST_ASSERT(w.err == PROVEN_OK && w.value == 5 && m.written_len == 5 && memcmp(m.written, "hello", 5) == 0,
            "five bytes through a sink that takes two at a time all arrive, in order", "");

        m.fail_after = 8;
        w = proven_transport_write_all(t, mv("world!"), PROVEN_NET_NO_DEADLINE);
        PROVEN_TEST_ASSERT(w.err == PROVEN_ERR_RESET && w.value == 3 && m.written_len == 8,
            "a failure part-way reports the error AND the three bytes that went", "");

        m.fail_after = 64;
        m.write_step = 0;
        w = proven_transport_write_all(t, mv("x"), PROVEN_NET_NO_DEADLINE);
        PROVEN_TEST_ASSERT(w.err == PROVEN_ERR_IO && w.value == 0,
            "a sink that accepts nothing and reports no error is PROVEN_ERR_IO, not an endless loop", "");

        /* The stream adapters: a buffered line reader over the transport's reader. */
        proven_transport_stream_t rs;
        proven_reader_t raw = proven_transport_reader(&rs, t, 0);
        proven_byte_t linebuf[32];
        proven_reader_buffered_t lines;
        (void)proven_reader_buffered(&lines, raw, (proven_mem_mut_t){ linebuf, sizeof linebuf });
        /* A line is a view into the reader's buffer, good until the next read: check each
         * before asking for the one after. */
        proven_result_u8str_view_t l1 = proven_reader_read_line(&lines);
        bool first = proven_is_ok(l1.err) && proven_u8str_view_eq(l1.val, PROVEN_LIT("line one"));
        proven_result_u8str_view_t l2 = proven_reader_read_line(&lines);
        bool second = proven_is_ok(l2.err) && proven_u8str_view_eq(l2.val, PROVEN_LIT("line two"));
        proven_result_u8str_view_t l3 = proven_reader_read_line(&lines);
        PROVEN_TEST_ASSERT(first && second && l3.err == PROVEN_ERR_EOF,
            "the line reader reads two lines arriving three bytes at a time, then end of input", "");

        m.write_step = 4;
        m.written_len = 0;
        proven_transport_stream_t ws;
        proven_writer_t writer = proven_transport_writer(&ws, t, 0);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fprint(writer, "{}+{}={}", PROVEN_ARG(2), PROVEN_ARG(3), PROVEN_ARG(5)).err) &&
                           m.written_len == 5 && memcmp(m.written, "2+3=5", 5) == 0,
            "proven_fprint formats straight into the transport's writer", "");

        PROVEN_TEST_ASSERT(proven_transport_shutdown(t) == PROVEN_OK && proven_transport_close(t) == PROVEN_OK &&
                           m.shutdowns == 1 && m.closes == 1,
            "shutdown and close reach the transport once each", "");
        proven_transport_t bare = { .ctx = &m, .read_fn = mem_read, .write_fn = mem_write };
        PROVEN_TEST_ASSERT(proven_transport_shutdown(bare) == PROVEN_OK && proven_transport_close(bare) == PROVEN_OK && m.closes == 1,
            "a transport with no shutdown or close has nothing to do, and that is not an error", "");
        proven_transport_t none = {0};
        proven_byte_t b[4];
        PROVEN_TEST_ASSERT(!proven_transport_is_valid(none) &&
                           proven_transport_read(none, (proven_mem_mut_t){ b, sizeof b }, PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_INVALID_ARG &&
                           proven_transport_write(none, mv("x"), PROVEN_NET_DONT_WAIT).err == PROVEN_ERR_INVALID_ARG &&
                           proven_transport_close(none) == PROVEN_ERR_INVALID_ARG,
            "an empty transport is invalid, and every call on it says so", "");
    }

    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        PROVEN_TEST_INFO("SKIP: this environment refuses to open a listening socket (error {}); the socket half was not run.", PROVEN_ARG((int)err));
        PROVEN_TEST_PASS("the memory transport half passed; no sockets here.");
        return 0;
    }
    PROVEN_TEST_ASSERT(err == PROVEN_OK, "a loopback listener", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("poll predicts the call that will not wait",
        "A listener is readable when a connection is pending; a connection is writable at once and readable when data or the end has arrived.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_poll_item_t items[3] = {0};
        proven_size_t ready = 99;
        items[0].handle = proven_net_listener_handle(&listener);
        items[0].want = PROVEN_NET_READABLE;
        proven_time_t t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(proven_net_poll(items, 1, proven_net_deadline_in(60), &ready) == PROVEN_ERR_TIMEOUT && ready == 0 && items[0].got == 0 &&
                           (proven_time_monotonic_now() - t0) >= 55 * 1000000LL,
            "nobody is connecting: PROVEN_ERR_TIMEOUT after the 60 ms, nothing ready", "");
        PROVEN_TEST_ASSERT(proven_net_poll(items, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_ERR_TIMEOUT,
            "and with no time to wait the answer is the same, at once", "");

        proven_net_conn_t client, server;
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &client) == PROVEN_OK, "a client connects", "");
        PROVEN_TEST_ASSERT(proven_net_poll(items, 1, proven_net_deadline_in(5000), &ready) == PROVEN_OK && ready == 1 &&
                           (items[0].got & PROVEN_NET_READABLE),
            "now the listener is readable", "");
        PROVEN_TEST_ASSERT(proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &server, NULL) == PROVEN_OK,
            "and the accept it predicted does not wait", "");

        items[1].handle = proven_net_conn_handle(&server);
        items[1].want = PROVEN_NET_READABLE | PROVEN_NET_WRITABLE;
        items[2].handle = proven_net_conn_handle(&client);
        items[2].want = PROVEN_NET_READABLE;
        PROVEN_TEST_ASSERT(proven_net_poll(items, 3, proven_net_deadline_in(5000), &ready) == PROVEN_OK && ready == 1 &&
                           items[0].got == 0 && items[1].got == PROVEN_NET_WRITABLE && items[2].got == 0,
            "of three sockets only the server connection is ready, and only for writing", "");

        PROVEN_TEST_ASSERT(proven_net_write_all(&client, mv("data"), proven_net_deadline_in(5000)).err == PROVEN_OK, "the client sends", "");
        items[1].want = PROVEN_NET_READABLE;
        PROVEN_TEST_ASSERT(proven_net_poll(items, 3, proven_net_deadline_in(5000), &ready) == PROVEN_OK && ready == 1 &&
                           items[1].got == PROVEN_NET_READABLE,
            "the server connection becomes readable", "");
        proven_byte_t buf[16];
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 4, "and the read it predicted does not wait", "");

        PROVEN_TEST_ASSERT(proven_net_shutdown_write(&client) == PROVEN_OK, "the client half-closes", "");
        PROVEN_TEST_ASSERT(proven_net_poll(&items[1], 1, proven_net_deadline_in(5000), &ready) == PROVEN_OK && (items[1].got & PROVEN_NET_READABLE),
            "end of input is readable too", "");
        r = proven_net_read(&server, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "and what is read is PROVEN_ERR_EOF: ready means it will not wait, not that it will succeed", "");

        /* More sockets than the inline limit: the caller's memory carries the list. */
        proven_net_poll_item_t many[PROVEN_NET_POLL_INLINE_MAX + 1];
        for (proven_size_t i = 0; i < sizeof many / sizeof many[0]; ++i) {
            many[i].handle = proven_net_conn_handle(&server);
            many[i].want = PROVEN_NET_WRITABLE;
            many[i].got = 0;
        }
        PROVEN_TEST_ASSERT(proven_net_poll(many, PROVEN_NET_POLL_INLINE_MAX + 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_ERR_OUT_OF_BOUNDS,
            "one item more than proven_net_poll takes is PROVEN_ERR_OUT_OF_BOUNDS", "");
        proven_size_t need = proven_net_poll_scratch_size(PROVEN_NET_POLL_INLINE_MAX + 1);
        static proven_byte_t scratch[8192];
        PROVEN_TEST_ASSERT(need != PROVEN_SIZE_MAX && need <= sizeof scratch, "the scratch size for 65 items is a few kilobytes", "");
        PROVEN_TEST_ASSERT(proven_net_poll_with((proven_mem_mut_t){ scratch, sizeof scratch }, many, PROVEN_NET_POLL_INLINE_MAX + 1,
                                                proven_net_deadline_in(5000), &ready) == PROVEN_OK && ready == PROVEN_NET_POLL_INLINE_MAX + 1,
            "proven_net_poll_with takes them all, and all 65 are writable", "");
        PROVEN_TEST_ASSERT(proven_net_poll_with((proven_mem_mut_t){ scratch, need - 1 }, many, PROVEN_NET_POLL_INLINE_MAX + 1,
                                                PROVEN_NET_DONT_WAIT, &ready) == PROVEN_ERR_OUT_OF_BOUNDS,
            "scratch one byte short is PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(proven_net_poll_scratch_size(PROVEN_SIZE_MAX) == PROVEN_SIZE_MAX, "an impossible count has no scratch size", "");

        proven_net_conn_t closed = {0};
        proven_net_poll_item_t bad = { .handle = proven_net_conn_handle(&closed), .want = PROVEN_NET_READABLE };
        PROVEN_TEST_ASSERT(!bad.handle.valid && proven_net_poll(&bad, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_ERR_INVALID_ARG,
            "the handle of a closed socket is not valid, and polling it is PROVEN_ERR_INVALID_ARG", "");

        t0 = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(proven_net_poll(NULL, 0, proven_net_deadline_in(40), &ready) == PROVEN_ERR_TIMEOUT &&
                           (proven_time_monotonic_now() - t0) >= 35 * 1000000LL,
            "a poll of nothing is a wait until the deadline", "");

        (void)proven_net_close(&client);
        (void)proven_net_close(&server);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a connection as a transport, a reader and a writer",
        "The same calls that ran over memory run over a socket; the reader's per-read timeout is a real deadline.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t client, server;
        PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &client) == PROVEN_OK &&
                           proven_net_accept(&listener, proven_net_deadline_in(5000), &server, NULL) == PROVEN_OK, "a connected pair", "");
        proven_transport_t ct = proven_net_conn_transport(&client);
        proven_transport_t st = proven_net_conn_transport(&server);

        proven_transport_stream_t ws;
        proven_writer_t writer = proven_transport_writer(&ws, ct, 5000);
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fprintln(writer, "GET {} HTTP/1.1", PROVEN_ARG((const char *)"/index.html")).err) &&
                           proven_is_ok(proven_fprintln(writer, "Host: {}", PROVEN_ARG((const char *)"example.test")).err),
            "two formatted lines are written to the socket", "");

        proven_transport_stream_t rs;
        proven_reader_t raw = proven_transport_reader(&rs, st, 5000);
        proven_byte_t linebuf[128];
        proven_reader_buffered_t lines;
        (void)proven_reader_buffered(&lines, raw, (proven_mem_mut_t){ linebuf, sizeof linebuf });
        proven_result_u8str_view_t l1 = proven_reader_read_line(&lines);
        PROVEN_TEST_ASSERT(proven_is_ok(l1.err) && proven_u8str_view_eq(l1.val, PROVEN_LIT("GET /index.html HTTP/1.1")), "the first line arrives", "");
        proven_result_u8str_view_t l2 = proven_reader_read_line(&lines);
        PROVEN_TEST_ASSERT(proven_is_ok(l2.err) && proven_u8str_view_eq(l2.val, PROVEN_LIT("Host: example.test")), "and the second", "");

        proven_transport_stream_t quick;
        proven_reader_t impatient = proven_transport_reader(&quick, st, 60);
        proven_byte_t b[8];
        proven_time_t t0 = proven_time_monotonic_now();
        proven_result_size_t r = proven_reader_read(impatient, (proven_mem_mut_t){ b, sizeof b });
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_TIMEOUT && (proven_time_monotonic_now() - t0) >= 55 * 1000000LL,
            "a reader with a 60 ms timeout gives up after 60 ms when nothing comes", "");

        PROVEN_TEST_ASSERT(proven_transport_shutdown(ct) == PROVEN_OK, "shutdown through the transport half-closes the connection", "");
        r = proven_transport_read(st, (proven_mem_mut_t){ b, sizeof b }, proven_net_deadline_in(5000));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "which the other end reads as PROVEN_ERR_EOF", "");
        PROVEN_TEST_ASSERT(proven_transport_close(ct) == PROVEN_OK && !proven_net_conn_is_open(&client), "closing the transport closes the connection", "");
        (void)proven_transport_close(st);

        proven_transport_stream_t s2;
        PROVEN_TEST_ASSERT(!proven_reader_is_valid(proven_transport_reader(&s2, (proven_transport_t){0}, 0)) &&
                           !proven_writer_is_valid(proven_transport_writer(NULL, st, 0)),
            "an invalid transport, or no state, makes no reader and no writer", "");
    }

    (void)proven_net_listener_close(&listener);
    PROVEN_TEST_PASS("readiness and the transport interface behave as specified.");
    return 0;
}
