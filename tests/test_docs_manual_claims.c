#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdlib.h>
#include "test_unit_cert_vectors.h"

/*
 * The manual makes CLAIMS. Each one is a proposition about the library that is either true or
 * false, and the reader is entitled to assume every one of them holds. Prose cannot be
 * test-driven, but a claim can be TESTED - you write the assertion the sentence implies, and the
 * build decides whether the sentence is still true.
 *
 * This is the same thing test_docs_manual_ch08_contracts does for the scanner chapter, done for
 * the modules added in the v26.07.13 line. It exists because prose ages worst of anything in a
 * repository: the README said "`proven` exposes no fsync" for a month after `proven_fs_sync`
 * shipped, and nothing objected, because nobody had written down what that sentence was asserting.
 *
 * The rule for adding to this file: when you write a sentence in the manual that a reader could
 * act on - a value, a boundary, a refusal, a guarantee - write the assertion for it here. If you
 * cannot state the assertion, the sentence is too vague to be in the manual.
 *
 * Each check below quotes the claim it is testing.
 */

int main(void) {
    PROVEN_TEST_SUITE("every factual claim the new chapters make is true",
        "The manual's statements about the hashes, the encoders, the generators and the streams, turned into assertions. A sentence a reader can act on is a proposition the build can check.",
        "A failure names the claim. Either the code changed and the manual did not, or the manual was wrong when it was written - decide which before changing either.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 14, TLS",
        "What the chapter says a configuration refuses, the session's size, and the contract after a failure.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM (s2): "There is no setting that verifies nothing. A client that would check a
         * chain and is given no anchors is refused when the config is made". */
        proven_tls_options_t o = { .alloc = heap };
        proven_tls_config_t *cfg = NULL;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &cfg) == PROVEN_ERR_INVALID_ARG, "a chain-verifying config without anchors is refused", "");
        /* CLAIM (s2 table): PIN_ONLY with no pins is INVALID_ARG; a certificate without a key too. */
        o.verify = PROVEN_TLS_VERIFY_PIN_ONLY;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &cfg) == PROVEN_ERR_INVALID_ARG, "PIN_ONLY without pins is refused", "");
        proven_byte_t pin[1][32] = { { 1 } };
        o.pins = (const proven_byte_t (*)[32])pin; o.pin_count = 1;
        PROVEN_TEST_ASSERT(proven_tls_config_create(&o, &cfg) == PROVEN_OK, "and with a pin it is made", "");
        /* CLAIM (s7): with PIN_ONLY "server_name may be empty"; (s5) "A client has output the
         * moment it is created." */
        proven_tls_conn_t *c = NULL;
        PROVEN_TEST_ASSERT(proven_tls_client_create(cfg, PROVEN_LIT(""), NULL, &c) == PROVEN_OK && proven_tls_pending_output(c).size > 0 &&
                           !proven_tls_is_established(c), "a pinned client needs no name and speaks first", "");
        /* CLAIM (s5): before the handshake completes a write is refused; (s8) the alert numbers
         * are -1 when there was none. */
        PROVEN_TEST_ASSERT(proven_tls_write(c, (proven_mem_view_t){ (const proven_byte_t *)"x", 1 }).err == PROVEN_ERR_INVALID_STATE &&
                           proven_tls_alert_received(c) == -1 && proven_tls_alert_sent(c) == -1 && proven_tls_cipher_suite(c) == 0, "nothing is written before the handshake", "");
        /* CLAIM (s8): "the error is returned once by proven_tls_feed; ... after that every call
         * but the output pair and the questions returns PROVEN_ERR_INVALID_STATE." A record type
         * that does not exist is the quickest failure there is. */
        static const proven_byte_t junk[5] = { 99, 3, 3, 0, 1 };
        proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_tls_feed(c, (proven_mem_view_t){ junk, 5 }, &used) == PROVEN_ERR_PROTOCOL &&
                           proven_tls_feed(c, (proven_mem_view_t){ junk, 5 }, &used) == PROVEN_ERR_INVALID_STATE && proven_tls_alert_sent(c) == 10 &&
                           proven_tls_pending_output(c).size > 0,
            "a failure is reported once, then INVALID_STATE, with the alert left to send", "");
        proven_tls_conn_destroy(c);
        /* CLAIM (s3): a server "speaks TLS when its config has tls set to a config with a
         * certificate" - one without cannot serve. */
        PROVEN_TEST_ASSERT(proven_tls_server_create(cfg, &c) == PROVEN_ERR_INVALID_ARG, "a config with no certificate cannot be a server", "");
        proven_tls_config_destroy(cfg);
        /* CLAIM (s6): the session is "a plain block of bytes", of PROVEN_TLS_SESSION_SIZE. */
        PROVEN_TEST_ASSERT(sizeof(proven_tls_session_t) == PROVEN_TLS_SESSION_SIZE && PROVEN_TLS_SESSION_SIZE == 1024, "a session is 1,024 bytes", "");
        /* CLAIM (ch 1): PROVEN_ERR_PROTOCOL is what the TLS unit returns - shown above. */
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 13, certificates and trust",
        "The name rules, the bounds and the PEM contract as the chapter states them.",
        "");
    // ---------------------------------------------------------------
    {
        static proven_byte_t der[2048];
        proven_size_t n = strlen(CV_LEAF) / 2;
        for (proven_size_t i = 0; i < n; ++i) {
            char c[3] = { CV_LEAF[2 * i], CV_LEAF[2 * i + 1], 0 };
            der[i] = (proven_byte_t)strtoul(c, NULL, 16);
        }
        proven_cert_t leaf;
        PROVEN_TEST_ASSERT(proven_cert_parse((proven_mem_view_t){ der, n }, &leaf) == PROVEN_OK, "the leaf parses", "");

        /* CLAIM (s2): "It copies nothing. Every field of the struct is a view into the bytes you
         * passed". */
        PROVEN_TEST_ASSERT(leaf.der.ptr == der && leaf.subject.ptr > der && leaf.subject.ptr < der + n &&
                           leaf.alt_names.ptr > der && leaf.alt_names.ptr < der + n,
            "the fields point into the caller's buffer", "");

        /* CLAIM (s4): the certificate is for example.test and *.wild.example.test. "A wildcard
         * ... stands for exactly one label"; "Letter case does not matter, and one trailing dot
         * on the host is ignored"; an IP address "matches however the address was spelled". */
        PROVEN_TEST_ASSERT(proven_cert_matches_host(&leaf, PROVEN_LIT("www.wild.example.test")) &&
                           !proven_cert_matches_host(&leaf, PROVEN_LIT("wild.example.test")) &&
                           !proven_cert_matches_host(&leaf, PROVEN_LIT("a.b.wild.example.test")),
            "a wildcard is one label: not none, not two", "");
        PROVEN_TEST_ASSERT(proven_cert_matches_host(&leaf, PROVEN_LIT("EXAMPLE.Test.")) && !proven_cert_matches_host(&leaf, PROVEN_LIT("example.test..")),
            "case is ignored, and one trailing dot - not two", "");
        PROVEN_TEST_ASSERT(proven_cert_matches_host(&leaf, PROVEN_LIT("2001:db8::1")) && proven_cert_matches_host(&leaf, PROVEN_LIT("2001:DB8:0:0:0:0:0:1")),
            "an address matches in either spelling", "");
        /* CLAIM (s4): "Only subjectAltName is consulted." The leaf's common name is "leaf". */
        PROVEN_TEST_ASSERT(!proven_cert_matches_host(&leaf, PROVEN_LIT("leaf")), "the common name is not a host name", "");

        /* CLAIM (s8): "A path is at most PROVEN_CERT_MAX_DEPTH (10) certificates." */
        PROVEN_TEST_ASSERT(PROVEN_CERT_MAX_DEPTH == 10, "the depth bound is 10", "");

        /* CLAIM (s3): OUT_OF_BOUNDS - "*written is the size needed and *pos has not moved". */
        proven_size_t pos = 0, written = 0;
        proven_u8str_view_t label;
        proven_byte_t small[8];
        proven_mem_view_t pem = { (const proven_byte_t *)CV_ROOT_PEM, sizeof CV_ROOT_PEM - 1 };
        PROVEN_TEST_ASSERT(proven_pem_next(pem, &pos, &label, (proven_mem_mut_t){ small, sizeof small }, &written) == PROVEN_ERR_OUT_OF_BOUNDS &&
                           pos == 0 && written == strlen(CV_ROOT) / 2,
            "a short buffer reports the size needed and leaves the position", "");

        /* CLAIM (s5): "It owns copies"; a bundle with no certificate at all is NOT_FOUND. */
        proven_cert_store_t *store = NULL;
        proven_size_t added = 7;
        PROVEN_TEST_ASSERT(proven_cert_store_create(heap, &store) == PROVEN_OK &&
                           proven_cert_store_add_pem(store, (proven_mem_view_t){ (const proven_byte_t *)"nothing", 7 }, &added) == PROVEN_ERR_NOT_FOUND && added == 0,
            "text with no certificate is PROVEN_ERR_NOT_FOUND", "");

        /* CLAIM (s6): "Leave it empty to skip the name check"; and with nothing in the store the
         * answer is UNTRUSTED with the fault NO_ISSUER. */
        proven_mem_view_t chain[1] = { { der, n } };
        proven_cert_verify_options_t opt = { .anchors = store, .now = CV_NOW };
        proven_cert_verify_result_t res;
        PROVEN_TEST_ASSERT(proven_cert_verify(chain, 1, &opt, &res) == PROVEN_ERR_UNTRUSTED && res.fault == PROVEN_CERT_FAULT_NO_ISSUER,
            "an empty store vouches for nothing", "");
        /* CLAIM (s7): a pin "instead of" verification - the leaf itself as the only anchor. */
        PROVEN_TEST_ASSERT(proven_cert_store_add_der(store, chain[0]) == PROVEN_OK && proven_cert_verify(chain, 1, &opt, &res) == PROVEN_OK && res.depth == 1,
            "a certificate that is itself an anchor is a path of one", "");
        proven_cert_store_destroy(store);

        /* CLAIM (ch 1): PROVEN_ERR_LAST is the last code, and the four added here are in order. */
        PROVEN_TEST_ASSERT(PROVEN_ERR_LAST == PROVEN_ERR_PROTOCOL && PROVEN_ERR_EXPIRED == PROVEN_ERR_UNTRUSTED + 1 &&
                           PROVEN_ERR_NOT_YET_VALID == PROVEN_ERR_EXPIRED + 1 && PROVEN_ERR_NAME_MISMATCH == PROVEN_ERR_NOT_YET_VALID + 1 &&
                           PROVEN_ERR_PROTOCOL == PROVEN_ERR_NAME_MISMATCH + 1 && PROVEN_ERR_PROTOCOL < PROVEN_ERR_RESERVED_END,
            "the four codes follow PROVEN_ERR_UNTRUSTED in the order the table lists them", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, HMAC and HKDF",
        "The sizes the table gives, the 255-block limit, and that a truncated SHA-512 is not SHA-384.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: proven_hmac_size is "32, 48 or 64"; a MAC buffer is PROVEN_HMAC_MAX_SIZE (64). */
        PROVEN_TEST_ASSERT(proven_hmac_size(PROVEN_HMAC_SHA256) == 32 && proven_hmac_size(PROVEN_HMAC_SHA384) == 48 &&
                           proven_hmac_size(PROVEN_HMAC_SHA512) == 64 && PROVEN_HMAC_MAX_SIZE == 64, "the three MAC sizes and the buffer size", "");
        /* CLAIM: "The first 48 bytes of a SHA-512 digest are a different number from the SHA-384 of the same input." */
        proven_byte_t d512[PROVEN_SHA512_SIZE], d384[PROVEN_SHA384_SIZE];
        proven_sha512(proven_mem_view_from_u8(PROVEN_LIT("abc")), d512);
        proven_sha384(proven_mem_view_from_u8(PROVEN_LIT("abc")), d384);
        PROVEN_TEST_ASSERT(memcmp(d512, d384, 48) != 0, "a truncated SHA-512 is not SHA-384", "");
        /* CLAIM: HKDF is OUT_OF_BOUNDS "for more than 255 times the hash size"; "Nothing is written on error". */
        static proven_byte_t big[255 * 48 + 1];
        proven_byte_t prk[48] = {1};
        big[0] = 0x5a;
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA384, (proven_mem_view_t){ prk, 48 }, (proven_mem_view_t){0}, (proven_mem_mut_t){ big, 255 * 48 + 1 }) == PROVEN_ERR_OUT_OF_BOUNDS && big[0] == 0x5a &&
                           proven_hkdf_expand(PROVEN_HMAC_SHA384, (proven_mem_view_t){ prk, 48 }, (proven_mem_view_t){0}, (proven_mem_mut_t){ big, 255 * 48 }) == PROVEN_OK,
            "255 blocks is the limit, and past it nothing is written", "");
        /* CLAIM: "Two calls with the same secret and the same info give the same key". */
        proven_byte_t k1[32], k2[32];
        PROVEN_TEST_ASSERT(proven_hkdf(PROVEN_HMAC_SHA256, (proven_mem_view_t){0}, proven_mem_view_from_u8(PROVEN_LIT("secret")), proven_mem_view_from_u8(PROVEN_LIT("a")), (proven_mem_mut_t){ k1, 32 }) == PROVEN_OK &&
                           proven_hkdf(PROVEN_HMAC_SHA256, (proven_mem_view_t){0}, proven_mem_view_from_u8(PROVEN_LIT("secret")), proven_mem_view_from_u8(PROVEN_LIT("a")), (proven_mem_mut_t){ k2, 32 }) == PROVEN_OK &&
                           proven_mem_equal_ct((proven_mem_view_t){ k1, 32 }, (proven_mem_view_t){ k2, 32 }), "the same inputs derive the same key", "");
        /* CLAIM (ch1): proven_mem_equal_ct - "ranges of different sizes are unequal". */
        PROVEN_TEST_ASSERT(!proven_mem_equal_ct((proven_mem_view_t){ k1, 32 }, (proven_mem_view_t){ k1, 31 }), "different sizes are unequal", "");
    }

#if !defined(PROVEN_NO_NET)
    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 9, the selector",
        "Which kind a selector is, and the three refusals its table names.",
        "");
    // ---------------------------------------------------------------
    {
        proven_net_conn_t a, b;
        proven_net_selector_t *sel = NULL, *portable = NULL;
        if (proven_net_pair(&a, &b) == PROVEN_OK) {
            PROVEN_TEST_ASSERT(proven_net_selector_create(heap, &sel) == PROVEN_OK && proven_net_selector_create_poll(heap, &portable) == PROVEN_OK, "a selector of each kind", "");
            /* CLAIM (s9): "On Linux it is epoll"; create_poll makes "one of the portable kind, whatever the system offers". */
#if defined(__linux__)
            PROVEN_TEST_ASSERT(proven_net_selector_kind(sel) == PROVEN_NET_SELECTOR_EPOLL, "on Linux the selector is epoll", "");
#endif
            PROVEN_TEST_ASSERT(proven_net_selector_kind(portable) == PROVEN_NET_SELECTOR_POLL, "the portable kind is the poll kind everywhere", "");
            /* CLAIM (s9): EXISTS when already registered; NOT_FOUND for modify and remove of what is not there. */
            proven_net_selector_t *both[2] = { sel, portable };
            for (int i = 0; i < 2; ++i) {
                proven_net_handle_t h = proven_net_conn_handle(&a);
                PROVEN_TEST_ASSERT(proven_net_selector_modify(both[i], h, PROVEN_NET_READABLE, NULL) == PROVEN_ERR_NOT_FOUND &&
                                   proven_net_selector_remove(both[i], h) == PROVEN_ERR_NOT_FOUND, "not registered: NOT_FOUND", "");
                PROVEN_TEST_ASSERT(proven_net_selector_add(both[i], h, PROVEN_NET_READABLE, NULL) == PROVEN_OK &&
                                   proven_net_selector_add(both[i], h, PROVEN_NET_READABLE, NULL) == PROVEN_ERR_EXISTS, "registered twice: EXISTS", "");
                /* CLAIM (s9): TIMEOUT "when nothing was (count is 0)". */
                proven_net_ready_t ev[2];
                proven_size_t n = 9;
                PROVEN_TEST_ASSERT(proven_net_selector_wait(both[i], ev, 2, PROVEN_NET_DONT_WAIT, &n) == PROVEN_ERR_TIMEOUT && n == 0, "nothing ready: TIMEOUT with a count of 0", "");
                PROVEN_TEST_ASSERT(proven_net_selector_remove(both[i], h) == PROVEN_OK && proven_net_selector_count(both[i]) == 0, "removed before it is closed", "");
            }
            proven_net_selector_destroy(sel);
            proven_net_selector_destroy(portable);
            (void)proven_net_close(&a);
            (void)proven_net_close(&b);
        }
    }
#endif

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 12, WebSocket",
        "The codec's facts as the chapter states them: the handshake values, one spelling for a length, the close-code table, and what the decoder refuses.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM (s6): a key is "24 characters", an accept value "28 characters". */
        proven_byte_t rnd[16] = {0}, key[PROVEN_WS_KEY_SIZE], accept[PROVEN_WS_ACCEPT_SIZE];
        proven_ws_make_key(rnd, key);
        PROVEN_TEST_ASSERT(sizeof key == 24 && sizeof accept == 28 && proven_ws_accept_key((proven_u8str_view_t){ key, sizeof key }, accept) == PROVEN_OK,
            "a made key is 24 characters and has a 28-character answer", "");

        /* CLAIM (s7): "Up to 125 it is in the second byte; up to 65535 in two more; beyond that
         * in eight", and the parser "refuses any other" spelling. */
        static const proven_u64 lens[] = { 125, 126, 65535, 65536 };
        static const proven_size_t hdr[] = { 2, 4, 4, 10 };
        for (int i = 0; i < 4; ++i) {
            proven_byte_t h[PROVEN_WS_MAX_FRAME_HEADER];
            proven_size_t n = 0;
            proven_ws_frame_t f = { .fin = true, .opcode = PROVEN_WS_BINARY, .length = lens[i] };
            PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ h, sizeof h }, &n, &f) == PROVEN_OK && n == hdr[i], "each length takes the header size the chapter gives", "");
        }
        proven_ws_frame_t f;
        proven_size_t hs = 0;
        PROVEN_TEST_ASSERT(proven_ws_frame_parse((proven_mem_view_t){ (const proven_byte_t *)"\x82\x7e\x00\x7d", 4 }, &f, &hs) == PROVEN_ERR_INVALID_FORMAT,
            "125 spelled with the two-byte form is refused", "");
        /* CLAIM (s7): a header is "2 to 14 bytes". */
        proven_byte_t h[PROVEN_WS_MAX_FRAME_HEADER];
        proven_size_t n = 0;
        f = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_BINARY, .masked = true, .length = 65536 };
        PROVEN_TEST_ASSERT(proven_ws_frame_write((proven_mem_mut_t){ h, sizeof h }, &n, &f) == PROVEN_OK && n == 14 && sizeof h == 14, "the largest header is fourteen bytes", "");

        /* CLAIM (s4): the table of close codes - 1005 and 1006 are "never sent"; 3000-4999 are
         * the application's; a reason is "at most 123 bytes". */
        PROVEN_TEST_ASSERT(!proven_ws_close_code_is_valid(1005) && !proven_ws_close_code_is_valid(1006) && proven_ws_close_code_is_valid(1000) &&
                           proven_ws_close_code_is_valid(3000) && proven_ws_close_code_is_valid(4999) && !proven_ws_close_code_is_valid(5000), "the codes that may and may not travel", "");
        proven_byte_t payload[PROVEN_WS_MAX_CONTROL];
        static char reason[125];
        memset(reason, 'r', 124);
        n = 0;
        PROVEN_TEST_ASSERT(proven_ws_close_write((proven_mem_mut_t){ payload, sizeof payload }, &n, 1000, (proven_u8str_view_t){ (const proven_byte_t *)reason, 124 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_ws_close_write((proven_mem_mut_t){ payload, sizeof payload }, &n, 1000, (proven_u8str_view_t){ (const proven_byte_t *)reason, 123 }) == PROVEN_OK,
            "a reason of 123 bytes is the longest", "");
        /* CLAIM (s7): close_code_for gives "1002, 1007, 1009, or 1011". */
        PROVEN_TEST_ASSERT(proven_ws_close_code_for(PROVEN_ERR_INVALID_FORMAT) == 1002 && proven_ws_close_code_for(PROVEN_ERR_INVALID_ENCODING) == 1007 &&
                           proven_ws_close_code_for(PROVEN_ERR_OUT_OF_BOUNDS) == 1009 && proven_ws_close_code_for(PROVEN_ERR_NOMEM) == 1011, "an error's close code", "");

        /* CLAIM (s8): "After any of these the decoder repeats the error"; a frame from a client
         * that is not masked is INVALID_FORMAT; a DATA event's data is "inside `in`". */
        proven_ws_decoder_t d;
        proven_ws_event_t ev;
        proven_size_t used = 0;
        proven_byte_t unmasked[3] = { 0x81, 0x01, 'x' };
        proven_ws_decoder_init(&d, true, 0);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ unmasked, 3 }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_ws_decoder_feed(&d, (proven_mem_mut_t){ unmasked, 3 }, &used, &ev) == PROVEN_ERR_INVALID_FORMAT, "an unmasked client frame is refused, and the refusal repeats", "");
        proven_ws_decoder_init(&d, false, 0);
        PROVEN_TEST_ASSERT(proven_ws_decoder_feed(&d, (proven_mem_mut_t){ unmasked, 3 }, &used, &ev) == PROVEN_OK && ev.kind == PROVEN_WS_EVENT_DATA &&
                           ev.first && ev.last && ev.text && ev.data.ptr == unmasked + 2 && ev.data.size == 1, "the same frame from a server is a message, its data a view into what was fed", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapters 10 and 11, the pieces around a message, and the two drivers",
        "References, forms, ranges, boundaries, challenges, cookies and events as the chapters state them; and what a client and a server refuse before any socket is opened.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t b[512];
        proven_mem_mut_t out = { b, sizeof b };
        proven_size_t n = 0;
        proven_u8str_view_t base = PROVEN_LIT("http://example.com/docs/guide/intro.html?v=2");

        /* CLAIM (ch10 s9): the table of references and what they resolve to. */
        static const char *const refs[][2] = {
            { "chapter2.html", "http://example.com/docs/guide/chapter2.html" },
            { "../img/logo.png", "http://example.com/docs/img/logo.png" },
            { "/login?next=%2F", "http://example.com/login?next=%2F" },
            { "?v=3", "http://example.com/docs/guide/intro.html?v=3" },
            { "//cdn.example.net/x", "http://cdn.example.net/x" },
            { "https://other.example/", "https://other.example/" },
        };
        for (int i = 0; i < 6; ++i) {
            PROVEN_TEST_ASSERT(proven_url_resolve(base, proven_u8str_view_from_cstr(refs[i][0]), out, &n) == PROVEN_OK &&
                               proven_u8str_view_eq((proven_u8str_view_t){ b, n }, proven_u8str_view_from_cstr(refs[i][1])),
                "each row of the chapter's table resolves as printed", "");
        }

        /* CLAIM (ch10 s9): "a space becomes +, and everything but letters, digits and -._~ becomes %XX." */
        n = 0;
        PROVEN_TEST_ASSERT(proven_url_form_append(out, &n, (proven_mem_view_t){ (const proven_byte_t *)"a-._~Z9", 7 }, (proven_mem_view_t){ (const proven_byte_t *)" /=", 3 }) == PROVEN_OK &&
                           n == 15 && memcmp(b, "a-._~Z9=+%2F%3D", 15) == 0, "the unreserved set passes; a space is +; the rest is %XX", "");

        /* CLAIM (ch10 s10): "The three errors of proven_http_range_parse are three different answers." */
        proven_u64 first = 0, last = 0;
        PROVEN_TEST_ASSERT(proven_http_range_parse(PROVEN_LIT("bytes=2000-"), 1000, &first, &last) == PROVEN_ERR_OUT_OF_BOUNDS, "past the end: 416", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse(PROVEN_LIT("bytes=0-1,5-6"), 1000, &first, &last) == PROVEN_ERR_UNSUPPORTED &&
                           proven_http_range_parse(PROVEN_LIT("lines=1-2"), 1000, &first, &last) == PROVEN_ERR_UNSUPPORTED, "several ranges, another unit: ignore and send all", "");
        PROVEN_TEST_ASSERT(proven_http_range_parse(PROVEN_LIT("bytes=x"), 1000, &first, &last) == PROVEN_ERR_INVALID_FORMAT, "unreadable: ignore", "");
        /* CLAIM (ch10 s10): "INVALID_ARG unless first <= last < total". */
        n = 0;
        PROVEN_TEST_ASSERT(proven_http_write_content_range(out, &n, 5, 4, 10) == PROVEN_ERR_INVALID_ARG &&
                           proven_http_write_content_range(out, &n, 0, 10, 10) == PROVEN_ERR_INVALID_ARG && n == 0 &&
                           proven_http_write_content_range(out, &n, 0, 9, 10) == PROVEN_OK, "Content-Range checks its three numbers", "");

        /* CLAIM (ch10 s10): a boundary is "PROVEN_HTTP_BOUNDARY_SIZE (40) characters", safe without quoting. */
        proven_byte_t rnd[16], bd[PROVEN_HTTP_BOUNDARY_SIZE];
        for (int i = 0; i < 16; ++i) rnd[i] = (proven_byte_t)(i * 17);
        proven_http_multipart_boundary(rnd, bd);
        n = 0;
        PROVEN_TEST_ASSERT(sizeof bd == 40 && proven_http_multipart_write_content_type(out, &n, (proven_u8str_view_t){ bd, sizeof bd }) == PROVEN_OK,
            "a made boundary is forty characters and accepted as one", "");
        /* CLAIM (ch10 s10): "a file called a\".png cannot close the quoted string it is written in." */
        n = 0;
        PROVEN_TEST_ASSERT(proven_http_multipart_write_part(out, &n, (proven_u8str_view_t){ bd, sizeof bd }, PROVEN_LIT("f"), PROVEN_LIT("a\".png"), PROVEN_LIT("")) == PROVEN_OK, "a part with that file name is written", "");
        b[n] = '\0';
        PROVEN_TEST_ASSERT(strstr((const char *)b, "filename=\"a%22.png\"") != NULL, "with the quote percent-encoded", "");

        /* CLAIM (ch10 s11): "INVALID_ARG for a colon in user"; "a challenge that offers both is
         * answered with SHA-256"; "SHA-512-256 ... are not" implemented. */
        PROVEN_TEST_ASSERT(proven_http_basic_auth(PROVEN_LIT("a:b"), PROVEN_LIT("c"), out, &n) == PROVEN_ERR_INVALID_ARG, "Basic cannot express a colon in the user name", "");
        proven_http_digest_challenge_t dc;
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(PROVEN_LIT("Digest realm=\"r\", nonce=\"n\", qop=\"auth\", algorithm=MD5, Digest realm=\"r\", nonce=\"n\", qop=\"auth\", algorithm=SHA-256"), &dc) == PROVEN_OK &&
                           dc.algorithm == PROVEN_HTTP_DIGEST_SHA256, "offered MD5 and SHA-256, SHA-256 is chosen", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(PROVEN_LIT("Digest realm=\"r\", nonce=\"n\", qop=\"auth\", algorithm=SHA-512-256"), &dc) == PROVEN_ERR_UNSUPPORTED, "SHA-512-256 alone is unsupported", "");
        PROVEN_TEST_ASSERT(proven_http_digest_challenge_parse(PROVEN_LIT("Basic realm=\"r\""), &dc) == PROVEN_ERR_NOT_FOUND, "no Digest challenge: NOT_FOUND", "");

        /* CLAIM (ch10 s12): "every cookie goes back to the exact host that set it and to no other";
         * a Domain "that does not cover the host that sent it is PROVEN_ERR_PERMISSION";
         * Secure is "refused when set ... over a connection that is not encrypted". */
        proven_http_cookie_jar_t jar;
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(&jar, heap, 8) == PROVEN_OK, "a jar", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_store(&jar, PROVEN_LIT("www.example.com"), PROVEN_LIT("/"), false, PROVEN_LIT("a=1; Domain=example.com"), 0) == PROVEN_OK, "a Domain that covers the host is accepted", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_header(&jar, PROVEN_LIT("example.com"), PROVEN_LIT("/"), false, 0, out, &n) == PROVEN_OK && n == 0, "and the cookie still does not go to the parent domain", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_header(&jar, PROVEN_LIT("www.example.com"), PROVEN_LIT("/"), false, 0, out, &n) == PROVEN_OK && n == 3, "only to the host that set it", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_store(&jar, PROVEN_LIT("www.example.com"), PROVEN_LIT("/"), false, PROVEN_LIT("b=1; Domain=example.net"), 0) == PROVEN_ERR_PERMISSION, "a foreign Domain is PERMISSION", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_store(&jar, PROVEN_LIT("www.example.com"), PROVEN_LIT("/"), false, PROVEN_LIT("c=1; Secure"), 0) == PROVEN_ERR_PERMISSION, "Secure over plain HTTP is refused", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_count(&jar) == 1, "the jar is unchanged by the refusals", "");
        proven_http_cookie_jar_destroy(&jar);

        /* CLAIM (ch10 s13): "At least 64 bytes"; "A stream that ends mid-event has not delivered that event." */
        proven_byte_t work[64];
        proven_sse_t sse;
        proven_sse_event_t ev;
        bool have = true;
        proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ work, 63 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_sse_init(&sse, (proven_mem_mut_t){ work, 64 }) == PROVEN_OK, "63 bytes of work memory are refused, 64 accepted", "");
        PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ (const proven_byte_t *)"data: x\n", 8 }, &used, &ev, &have) == PROVEN_OK && !have && used == 8,
            "data without its blank line is consumed and is not an event", "");

#if !defined(PROVEN_NO_NET)
        /* CLAIM (ch11 s1): the client "refuses an https URL with PROVEN_ERR_UNSUPPORTED".
         * CLAIM (ch11 s7): a header the client writes itself is PROVEN_ERR_INVALID_ARG.
         * Both are decided before a connection is attempted, so no network is needed. */
        proven_http_client_config_t cc = {0};
        cc.alloc = heap;
        proven_http_client_t *client = NULL;
        proven_http_client_response_t resp;
        PROVEN_TEST_ASSERT(proven_http_client_create(&cc, &client) == PROVEN_OK, "a client with only an allocator", "");
        PROVEN_TEST_ASSERT(proven_http_client_get(client, PROVEN_LIT("https://example.invalid/"), &resp) == PROVEN_ERR_UNSUPPORTED, "https without tls_wrap is UNSUPPORTED", "");
        proven_http_client_finish(&resp);
        proven_http_header_t host = { PROVEN_LIT("Host"), PROVEN_LIT("other") };
        proven_http_client_request_t req = { .url = PROVEN_LIT("http://example.invalid/"), .headers = &host, .header_count = 1 };
        PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) == PROVEN_ERR_INVALID_ARG, "a Host header of the caller's is INVALID_ARG", "");
        proven_http_client_finish(&resp);
        proven_http_client_destroy(client);

        /* CLAIM (ch11 s2): create is INVALID_ARG "without an allocator or a handler". */
        proven_http_server_config_t sc = {0};
        proven_http_server_t *server = NULL;
        sc.alloc = heap;
        PROVEN_TEST_ASSERT(proven_http_server_create(&sc, &server) == PROVEN_ERR_INVALID_ARG && server == NULL, "a server without a handler is INVALID_ARG", "");
#endif
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 10, URLs and HTTP messages",
        "The facts the chapter states in prose, each as an assertion.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "In http://example.com@evil.test/ the host is evil.test." */
        proven_url_t u;
        PROVEN_TEST_ASSERT(proven_url_parse(PROVEN_LIT("http://example.com@evil.test/"), &u) == PROVEN_OK &&
                           proven_u8str_view_eq(u.host, PROVEN_LIT("evil.test")),
            "the host must be what follows the last '@', as the chapter warns", "");

        /* CLAIM: "%2e%2e becomes .. and is then treated as .." and "%252e%252e becomes the text
         * %2e%2e, which is a strange file name and nothing more". */
        proven_byte_t out[64];
        proven_size_t n = 0;
        PROVEN_TEST_ASSERT(proven_url_path_resolve(PROVEN_LIT("/%2e%2e/x"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_PERMISSION,
            "an encoded .. must be a climb", "");
        PROVEN_TEST_ASSERT(proven_url_path_resolve(PROVEN_LIT("/%252e%252e/x"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_OK &&
                           n == 9 && memcmp(out, "/%2e%2e/x", 9) == 0,
            "a doubly encoded .. must come out as the literal text %2e%2e", "");
        /* CLAIM: the refusals of section 3, one of each kind. */
        PROVEN_TEST_ASSERT(proven_url_path_resolve(PROVEN_LIT("/a%2Fb"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_path_resolve(PROVEN_LIT("/a%5Cb"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_path_resolve(PROVEN_LIT("/file%00.png"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_url_path_resolve(PROVEN_LIT("/%c0%ae"), (proven_mem_mut_t){ out, sizeof out }, &n) == PROVEN_ERR_INVALID_FORMAT,
            "an encoded slash, an encoded backslash, an encoded NUL and an overlong dot must each be refused", "");

        /* CLAIM: "max_head 0 means PROVEN_HTTP_DEFAULT_MAX_HEAD (16 KiB)" and "always 29 bytes". */
        PROVEN_TEST_ASSERT(PROVEN_HTTP_DEFAULT_MAX_HEAD == 16384 && PROVEN_HTTP_DATE_SIZE == 29,
            "the default head limit and the date size are the numbers the chapter prints", "");

        /* CLAIM: each row of the section 1 table is refused. */
        static const char *const refused_heads[] = {
            "GET / HTTP/1.1\nHost: h\n\n",
            "GET / HTTP/1.1\r\nContent-Length : 5\r\n\r\n",
            "GET / HTTP/1.1\r\nA: b\r\n c\r\n\r\n",
        };
        for (proven_size_t i = 0; i < sizeof refused_heads / sizeof refused_heads[0]; ++i) {
            proven_http_header_t h[8];
            proven_http_request_t r;
            proven_size_t hs = 0;
            proven_mem_view_t d = { (const proven_byte_t *)refused_heads[i], strlen(refused_heads[i]) };
            PROVEN_TEST_ASSERT(proven_http_parse_request(d, h, 8, 0, &r, &hs) == PROVEN_ERR_INVALID_FORMAT,
                "a bare LF, a space before the colon, and a folded line must each be refused", "");
        }
        static const char *const refused_framing[] = {
            "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
            "POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
        };
        for (proven_size_t i = 0; i < sizeof refused_framing / sizeof refused_framing[0]; ++i) {
            proven_http_header_t h[8];
            proven_http_request_t r;
            proven_http_framing_t f;
            proven_size_t hs = 0;
            proven_mem_view_t d = { (const proven_byte_t *)refused_framing[i], strlen(refused_framing[i]) };
            PROVEN_TEST_ASSERT(proven_http_parse_request(d, h, 8, 0, &r, &hs) == PROVEN_OK &&
                               proven_http_request_framing(&r, &f) == PROVEN_ERR_INVALID_FORMAT,
                "two Content-Length fields, and Content-Length with Transfer-Encoding, must each be refused", "");
        }

        /* CLAIM: "Expires in the year 9999 means never ... PROVEN_ERR_OVERFLOW". */
        proven_time_t t = 0;
        PROVEN_TEST_ASSERT(proven_http_date_parse(PROVEN_LIT("Fri, 31 Dec 9999 23:59:59 GMT"), 0, &t) == PROVEN_ERR_OVERFLOW,
            "a date past the range of proven_time_t must be PROVEN_ERR_OVERFLOW, as the chapter says", "");

        /* CLAIM: the response-splitting value of section 7 is refused and nothing is appended. */
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_http_write_header((proven_mem_mut_t){ out, sizeof out }, &len, PROVEN_LIT("Location"),
                                                    PROVEN_LIT("/home\r\nSet-Cookie: session=attacker")) == PROVEN_ERR_INVALID_ARG && len == 0,
            "a header value with a line break must be refused with the length unmoved", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 9, addresses and deadlines",
        "The facts the networking chapter states that need no socket to check.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "`127.1` and `2130706433` are refused although older parsers accept both, and
         * so is `010.0.0.1`, which those parsers read as octal eight." */
        proven_net_addr_t a;
        PROVEN_TEST_ASSERT(proven_net_addr_parse(PROVEN_LIT("127.1"), 1, &a) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_net_addr_parse(PROVEN_LIT("2130706433"), 1, &a) == PROVEN_ERR_INVALID_FORMAT &&
                           proven_net_addr_parse(PROVEN_LIT("010.0.0.1"), 1, &a) == PROVEN_ERR_INVALID_FORMAT,
            "the three IPv4 shorthands the chapter says are refused must be refused", "");

        /* CLAIM: "IPv6 has many spellings going in but one coming out ... so two equal addresses
         * print as equal text", and "PROVEN_NET_ADDR_TEXT_MAX bytes are always enough". */
        proven_net_addr_t b;
        proven_byte_t ta[PROVEN_NET_ADDR_TEXT_MAX], tb[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t na = 0, nb = 0;
        PROVEN_TEST_ASSERT(proven_net_addr_parse(PROVEN_LIT("2001:0DB8:0:0:0:0:0:1"), 80, &a) == PROVEN_OK &&
                           proven_net_addr_parse(PROVEN_LIT("2001:db8::1"), 80, &b) == PROVEN_OK &&
                           proven_net_addr_format(&a, (proven_mem_mut_t){ ta, sizeof ta }, &na) == PROVEN_OK &&
                           proven_net_addr_format(&b, (proven_mem_mut_t){ tb, sizeof tb }, &nb) == PROVEN_OK &&
                           na == nb && memcmp(ta, tb, na) == 0,
            "two spellings of one IPv6 address must print as the same text", "");
        proven_net_addr_t widest;
        memset(&widest, 0, sizeof widest);
        widest.family = PROVEN_NET_FAMILY_IPV6;
        widest.port = 65535;
        widest.scope_id = 0xffffffffu;
        for (int i = 0; i < 16; ++i) widest.ip[i] = (proven_byte_t)(0x11 * ((i % 14) + 1));
        PROVEN_TEST_ASSERT(proven_net_addr_format(&widest, (proven_mem_mut_t){ ta, sizeof ta }, &na) == PROVEN_OK &&
                           na <= PROVEN_NET_ADDR_TEXT_MAX,
            "the widest IPv6 address, zone and port must fit PROVEN_NET_ADDR_TEXT_MAX", "");
        proven_net_addr_t longest;
        char path[104];
        memset(path, 'p', sizeof path);
        PROVEN_TEST_ASSERT(proven_net_addr_unix((proven_u8str_view_t){ (const proven_byte_t *)path, PROVEN_NET_UNIX_PATH_MAX }, &longest) == PROVEN_OK &&
                           proven_net_addr_format(&longest, (proven_mem_mut_t){ ta, sizeof ta }, &na) == PROVEN_OK,
            "and so must the longest Unix-domain path", "");

        /* CLAIM: "proven_net_poll ... Up to PROVEN_NET_POLL_INLINE_MAX (64) items." */
        PROVEN_TEST_ASSERT(PROVEN_NET_POLL_INLINE_MAX == 64, "the inline poll limit is the 64 the chapter prints", "");

        /* CLAIM: "A deadline is a moment ... a reading of the monotonic clock", and
         * "proven_net_deadline_in(ms): the deadline ms milliseconds from now". */
        proven_time_t before = proven_time_monotonic_now();
        proven_net_deadline_t d = proven_net_deadline_in(1000);
        proven_time_t after = proven_time_monotonic_now();
        PROVEN_TEST_ASSERT(d >= before + 1000 * 1000000LL && d <= after + 1000 * 1000000LL,
            "a deadline is the monotonic clock plus the milliseconds asked for", "");
        PROVEN_TEST_ASSERT(PROVEN_NET_DONT_WAIT < before && PROVEN_NET_NO_DEADLINE > d,
            "DONT_WAIT is before every moment and NO_DEADLINE after every moment", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, legacy digests",
        "The sizes and spellings the SHA-1 and MD5 section states as fact.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "proven_sha1(view, out[20])" and "proven_md5(view, out[16])". */
        PROVEN_TEST_ASSERT(PROVEN_SHA1_SIZE == 20 && PROVEN_MD5_SIZE == 16,
            "the digest sizes must be the ones the chapter's reference table gives", "");

        /* CLAIM: "The 40-character lowercase spelling sha1sum and git print. NUL-terminated."
         * and "The 32-character lowercase spelling md5sum prints. NUL-terminated." */
        proven_byte_t s1[PROVEN_SHA1_SIZE];
        char s1hex[41];
        proven_sha1(proven_mem_view_from_u8(PROVEN_LIT("abc")), s1);
        proven_sha1_to_hex(s1, s1hex);
        PROVEN_TEST_ASSERT(strcmp(s1hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
            "proven_sha1_to_hex must be what sha1sum prints for \"abc\", as the chapter states", "");
        proven_byte_t m5[PROVEN_MD5_SIZE];
        char m5hex[33];
        proven_md5(proven_mem_view_from_u8(PROVEN_LIT("abc")), m5);
        proven_md5_to_hex(m5, m5hex);
        PROVEN_TEST_ASSERT(strcmp(m5hex, "900150983cd24fb0d6963f7d28e17f72") == 0,
            "proven_md5_to_hex must be what md5sum prints for \"abc\", as the chapter states", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, hashing",
        "The values and guarantees the hashing section states as fact.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "a checksum; interoperates with gzip/zlib/PNG" - which is only meaningful if it
         * is the standard CRC-32, whose published check value is 0xcbf43926 for "123456789". */
        PROVEN_TEST_ASSERT(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
            "the manual calls proven_crc32 the CRC that gzip/zlib/PNG carry - so it must produce the shared check value",
            "A different value means it is not that CRC, and the interoperability the chapter promises does not exist.");

        /* CLAIM: "proven_crc32_update ... Start from 0; the value you hold between calls is the
         * real CRC, so you can store it, log it, and resume." */
        proven_u32 whole = proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789")));
        proven_u32 c = proven_crc32_update(0, proven_mem_view_from_u8(PROVEN_LIT("1234")));
        c = proven_crc32_update(c, proven_mem_view_from_u8(PROVEN_LIT("56789")));
        PROVEN_TEST_ASSERT(c == whole,
            "chaining chunks through proven_crc32_update must equal the one-shot CRC",
            "The chapter tells the reader they may store the intermediate value and resume. If chaining differs, that instruction corrupts their checksum.");

        /* CLAIM: "PROVEN_SHA256_SIZE 32 - the digest; size your output buffer with this",
         * and: "proven_sha256_to_hex ... 64 lowercase hex characters. NUL-terminated." */
        PROVEN_TEST_ASSERT(PROVEN_SHA256_SIZE == 32,
            "PROVEN_SHA256_SIZE must be the digest size the chapter tells callers to allocate", "");

        proven_byte_t digest[PROVEN_SHA256_SIZE];
        proven_sha256(proven_mem_view_from_u8(PROVEN_LIT("abc")), digest);
        char hex[65];
        proven_sha256_to_hex(digest, hex);
        PROVEN_TEST_ASSERT(strlen(hex) == 64 && hex[64] == '\0',
            "proven_sha256_to_hex must write 64 characters and a NUL, as the chapter states", "");
        /* And it must be the digest the rest of the world computes for "abc". */
        PROVEN_TEST_ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
            "and it must be the standard SHA-256 of \"abc\" - the chapter calls it the spelling sha256sum and git print",
            "If this differs, the fingerprint does not interoperate with anything, which is the only reason to use it.");

        /* CLAIM: "the same input gives the same output on any target" - endianness-independence
         * cannot be tested on one machine, but its consequence can: the digest of the same bytes
         * must not depend on how they were CHUNKED. */
        proven_sha256_t ctx;
        proven_sha256_init(&ctx);
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("a")));
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("b")));
        proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("c")));
        proven_byte_t streamed[PROVEN_SHA256_SIZE];
        proven_sha256_final(&ctx, streamed);
        PROVEN_TEST_ASSERT(memcmp(streamed, digest, PROVEN_SHA256_SIZE) == 0,
            "the streaming digest must equal the one-shot: the chapter says it depends only on the bytes, never on how they were chunked", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 4, encoding",
        "Every promise the encoding section makes about refusal, sizing, and the two alphabets.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t out[64];
        proven_size_t w = 0;

        /* CLAIM: "proven_base64url_encode ... URL-safe alphabet, NO padding". */
        proven_err_t e = proven_base64url_encode(
            proven_mem_view_from_u8(PROVEN_LIT("f")), out, sizeof out, &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w == 2 && memcmp(out, "Zg", 2) == 0,
            "the URL form must emit no '=' padding, as the chapter states",
            "A '=' in a token is the thing the chapter says this form exists to avoid.");

        /* CLAIM: "proven_base64_decode ... Accepts BOTH alphabets, padded or not." */
        proven_size_t dn = 0;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode(
                proven_mem_view_from_u8(PROVEN_LIT("Zg==")), out, sizeof out, &dn)) && dn == 1 && out[0] == 'f',
            "padded standard Base64 must decode", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_base64_decode(
                proven_mem_view_from_u8(PROVEN_LIT("Zg")), out, sizeof out, &dn)) && dn == 1 && out[0] == 'f',
            "and the unpadded URL form must decode to the same byte - the chapter promises both", "");

        /* CLAIM: "proven_base64_decoded_size ... An upper bound for PADDED AND UNPADDED text",
         * and the counter-example warns that `3 * (n/4)` gives 0 for "QQ". */
        PROVEN_TEST_ASSERT(proven_base64_decoded_size(2) >= 1,
            "decoded_size must be an upper bound for unpadded text, as the chapter's caution insists",
            "The chapter tells the reader to size their buffer with this. If it under-reports, that instruction fails on the library's own base64url output.");

        /* CLAIM: "a short buffer is PROVEN_ERR_OUT_OF_BOUNDS, never a truncated prefix" -
         * and the section header calls this the 'Refuse, never truncate' class. */
        proven_byte_t tiny[3];
        memset(tiny, 0xAA, sizeof tiny);
        PROVEN_TEST_ASSERT(proven_hex_encode(proven_mem_view_from_u8(PROVEN_LIT("foobar")),
                                             tiny, sizeof tiny, &w) == PROVEN_ERR_OUT_OF_BOUNDS,
            "a short output buffer must be refused", "");
        PROVEN_TEST_ASSERT(tiny[0] == 0xAA && tiny[2] == 0xAA,
            "and the refused call must write NOTHING - the chapter promises no truncated prefix", "");

        /* CLAIM: "Whitespace is not skipped, on purpose. A pasted, line-wrapped Base64 blob is
         * INVALID_ENCODING, not a silently different result." */
        PROVEN_TEST_ASSERT(proven_base64_decode(proven_mem_view_from_u8(PROVEN_LIT("Zm9v YmFy")),
                                                out, sizeof out, &dn) == PROVEN_ERR_INVALID_ENCODING,
            "embedded whitespace must be INVALID_ENCODING, exactly as the chapter says", "");

        /* CLAIM: "validates the WHOLE input before writing a single byte, so a stray character
         * near the end cannot leave you holding a half-decoded prefix." */
        memset(out, 0xAA, sizeof out);
        PROVEN_TEST_ASSERT(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("6162!!")),
                                             out, sizeof out, &dn) == PROVEN_ERR_INVALID_ENCODING,
            "a stray character is INVALID_ENCODING", "");
        PROVEN_TEST_ASSERT(out[0] == 0xAA && dn == 0,
            "and NOTHING is committed - the valid prefix \"6162\" must not have been written",
            "This is the claim that makes the module worth having over a two-line loop.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 5, randomness",
        "The guarantees the randomness section states, including the ones that are security properties.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "Seed the reproducible generator. Any seed is fine - even 0" and the counter-
         * example's premise: the same seed replays the same run. */
        proven_xoshiro256ss_t a, b;
        proven_xoshiro256ss_seed(&a, 0);
        proven_xoshiro256ss_seed(&b, 0);
        bool replays = true, nonzero = false;
        for (int i = 0; i < 64; ++i) {
            proven_u64 x = proven_xoshiro256ss_next(&a);
            if (x != proven_xoshiro256ss_next(&b)) replays = false;
            if (x != 0) nonzero = true;
        }
        PROVEN_TEST_ASSERT(replays, "the same seed must replay the same run, as the chapter states", "");
        PROVEN_TEST_ASSERT(nonzero,
            "and seed 0 must not be degenerate - the chapter tells the reader any seed is fine",
            "An all-zero xoshiro state emits zeros forever. The chapter's promise rests on the SplitMix64 expansion.");

        /* CLAIM: "proven_chacha_rng ... invalid if the generator was never successfully seeded",
         * and "the generator is left INERT - it yields zeros and an invalid trait". */
        proven_chacha_rng_t g;
        memset(&g, 0, sizeof g);
        PROVEN_TEST_ASSERT(!proven_rng_is_valid(proven_chacha_rng(&g)),
            "an unseeded generator must present an INVALID trait, as the reference table states",
            "If it is valid, every helper draws from a generator with no key - which is the failure the chapter's caution is about.");

        proven_byte_t zeros[128];
        memset(zeros, 0xFF, sizeof zeros);
        proven_chacha_rng_fill(&g, zeros, sizeof zeros);
        bool all_zero = true;
        for (size_t i = 0; i < sizeof zeros; ++i) if (zeros[i] != 0) all_zero = false;
        PROVEN_TEST_ASSERT(all_zero,
            "and it must yield zeros - a visibly dead value, which is what the chapter promises a caller who ignores the seeding bool",
            "Plausible-looking bytes here are exactly the security hole the counter-example warns about.");

        /* CLAIM: "proven_rng_below ... Uniform in [0, bound), UNBIASED", and the counter-example
         * says `% 6` is not. We cannot prove uniformity in a unit test, but we CAN prove the
         * bound is respected and 0 is handled as the table says. */
        proven_xoshiro256ss_t s;
        proven_xoshiro256ss_seed(&s, 7);
        proven_rng_t rng = proven_xoshiro256ss_rng(&s);
        for (int i = 0; i < 5000; ++i) {
            PROVEN_TEST_ASSERT(proven_rng_below(rng, 6) < 6,
                "every draw must be strictly below the bound, as the reference table states", "");
        }
        PROVEN_TEST_ASSERT(proven_rng_below(rng, 0) == 0,
            "a bound of 0 must be 0 - the table says so rather than leaving it undefined", "");

        /* CLAIM: "proven_rng_f64 ... Uniform in [0, 1). 53 bits; never returns 1.0." */
        for (int i = 0; i < 5000; ++i) {
            double d = proven_rng_f64(rng);
            PROVEN_TEST_ASSERT(d >= 0.0 && d < 1.0, "f64 must be in [0,1) and never 1.0", "");
        }

        /* CLAIM: "proven_rng_range ... The full INT64_MIN..INT64_MAX span does not overflow",
         * and "Returns lo if hi < lo". */
        PROVEN_TEST_ASSERT(proven_rng_range(rng, 10, 3) == 10,
            "an inverted range must return lo, as the table states", "");
        (void)proven_rng_range(rng, INT64_MIN, INT64_MAX);   /* must not trap: UBSan is watching */
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 5, creating directories",
        "What the chapter says proven_fs_mkdir and proven_fs_mkdir_all answer when the name is taken.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "proven_fs_mkdir does not look at what holds the name. A file of that name
         * gives the same answer", and: "proven_fs_mkdir_all asks, and returns PROVEN_OK only
         * for a directory." */
        proven_u8str_view_t dir = PROVEN_LIT("claims_mkdir.tmp");
        proven_u8str_view_t file = PROVEN_LIT("claims_mkdir_file.tmp");
        (void)proven_fs_rmdir(heap, dir);
        PROVEN_TEST_ASSERT(proven_fs_mkdir(heap, dir) == PROVEN_OK, "setup: a directory", "");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, file,
            proven_mem_view_from_u8(PROVEN_LIT("x")))), "setup: a file", "");

        PROVEN_TEST_ASSERT(proven_fs_mkdir(heap, dir) == PROVEN_ERR_EXISTS &&
                           proven_fs_mkdir(heap, file) == PROVEN_ERR_EXISTS,
            "proven_fs_mkdir must answer PROVEN_ERR_EXISTS for a directory and for a file alike",
            "The chapter's second counter-example rests on the two answers being the same.");
        PROVEN_TEST_ASSERT(proven_fs_mkdir_all(heap, dir) == PROVEN_OK &&
                           proven_fs_mkdir_all(heap, file) == PROVEN_ERR_EXISTS,
            "proven_fs_mkdir_all must be PROVEN_OK for the directory and PROVEN_ERR_EXISTS for the file",
            "PROVEN_OK for the file would make 'returns PROVEN_OK only for a directory' false.");

        (void)proven_fs_remove(heap, file);
        (void)proven_fs_rmdir(heap, dir);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 5, streams",
        "The refusals and lifetimes the streams sections promise.",
        "");
    // ---------------------------------------------------------------
    {
        /* CLAIM: "A line longer than the buffer is refused, not truncated. PROVEN_ERR_OUT_OF_BOUNDS",
         * and: "A line that exactly FILLS the buffer is fine: the newline does not have to fit
         * too, and neither does a final line with no newline at all." */
        proven_u8str_view_t path = PROVEN_LIT("claims_lines.tmp");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file(heap, path,
            proven_mem_view_from_u8(PROVEN_LIT("abcd\ntoolong\n")))), "setup", "");

        proven_result_file_t rf = proven_fs_open(heap, path, PROVEN_FS_READ);
        PROVEN_TEST_ASSERT(proven_is_ok(rf.err), "setup: open", "");

        proven_byte_t buf[4];   /* exactly the size of the first line */
        proven_sysio_lines_t lines;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_sysio_lines_open(&lines, rf.value,
            (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf })), "setup: line reader", "");

        proven_result_u8str_view_t l1 = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(proven_is_ok(l1.err) && l1.val.size == 4 &&
                           memcmp(l1.val.ptr, "abcd", 4) == 0,
            "a line that exactly fills the buffer must be RETURNED - the chapter says the newline does not have to fit too",
            "OUT_OF_BOUNDS here would make the chapter's parenthesis false, and would lose data.");

        proven_result_u8str_view_t l2 = proven_sysio_read_line(&lines);
        PROVEN_TEST_ASSERT(l2.err == PROVEN_ERR_OUT_OF_BOUNDS,
            "and a line LONGER than the buffer must be refused, not truncated",
            "A truncated line returned as a success is the corruption the chapter says this refusal exists to prevent.");

        (void)proven_fs_close(rf.value);
        (void)proven_fs_remove(heap, path);

        /* CLAIM: "proven_writer_is_valid ... A zeroed handle is invalid, and every constructor
         * returns one on bad arguments - so this is the check, not a NULL test." */
        proven_sysio_out_t out;
        proven_writer_t bad = proven_sysio_stdout_buffered(&out,
            (proven_mem_mut_t){ .ptr = NULL, .size = 0 });
        PROVEN_TEST_ASSERT(!proven_writer_is_valid(bad),
            "a constructor given bad arguments must return an INVALID handle, as the table states", "");
        PROVEN_TEST_ASSERT(!proven_reader_is_valid(proven_sysio_stdin_reader(NULL)),
            "and so must the reader side", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapters 3 and 5, UTF-16 text",
        "The sentences about utf.h and u16 I/O a reader would act on: the refusal writes nothing, pieces are NEED_MORE and wholes are malformed, width counts UTF-8 bytes, an explicit encoding delivers a BOM as U+FEFF, a trailing high surrogate is refused by the writer.",
        "Each assertion quotes its sentence. If one fails, the chapter or the library is wrong - decide which, and fix that one.");
    // ---------------------------------------------------------------
    {
        /* CLAIM (ch3): "the forms that write into your memory write nothing when they refuse." */
        proven_u16 w16[8] = { 0x5A5A };
        proven_size_t n = 99;
        proven_err_t e = proven_utf8_to_utf16((proven_u8str_view_t){ (const proven_byte_t *)"a\xC0\x80", 3 }, w16, 8, &n);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ENCODING && n == 0 && w16[0] == 0x5A5A,
            "an overlong form is refused and nothing is written", "");

        /* CLAIM (ch3): "The partial forms stop there with PROVEN_ERR_NEED_MORE ... for [the whole
         * forms] a text that ends mid-character is malformed." */
        proven_u8str_view_t cut = { (const proven_byte_t *)"x\xED\x95", 3 };
        proven_utf_step_t st = proven_utf8_to_utf16_partial(cut, w16, 8);
        PROVEN_TEST_ASSERT(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 1, "a piece cut mid-character is NEED_MORE", "");
        PROVEN_TEST_ASSERT(proven_utf8_to_utf16_size(cut).err == PROVEN_ERR_INVALID_ENCODING, "a whole text cut there is malformed", "");

        /* CLAIM (ch5): "{:>10} around a u16 argument pads to ten UTF-8 bytes ... a Hangul syllable
         * is three bytes." */
        proven_byte_t fb[32];
        proven_u8str_t fs = proven_u8str_borrow(fb, sizeof fb);
        const proven_u16 han[] = { 0xD55C };
        proven_u16str_view_t hv = { han, 1 };
        proven_fmt_result_t fr = proven_u8str_append_fmt(&fs, "{:>10}", PROVEN_ARG(hv));
        PROVEN_TEST_ASSERT(proven_is_ok(fr.err) && fs.internal.len == 10 && fb[6] == (proven_byte_t)' ' && fb[7] == 0xED,
            "width 10 around one syllable is seven spaces and its three bytes", "");

        /* CLAIM (ch5): "Opened as PROVEN_TEXT_UTF16LE, a file that starts with FF FE delivers the
         * character U+FEFF as the first unit of its first line." */
        proven_reader_view_t rv;
        proven_u16 lb[8];
        proven_u16_reader_t rd;
        (void)proven_u16_reader_init(&rd, proven_reader_from_view(&rv, (proven_u8str_view_t){ (const proven_byte_t *)"\xFF\xFE" "a\0", 4 }),
                                     PROVEN_TEXT_UTF16LE, lb, 8);
        proven_result_u16str_view_t l = proven_u16_reader_read_line(&rd);
        PROVEN_TEST_ASSERT(proven_is_ok(l.err) && l.val.size == 2 && l.val.ptr[0] == 0xFEFF && l.val.ptr[1] == 'a',
            "an explicit encoding does not strip the BOM", "");

        /* CLAIM (ch5): "proven_writer_write_u16 refuses a text that ends with the high half of a
         * pair" and "Validated first: malformed text writes nothing." */
        proven_writer_buf_t wb = { .buf = { fb, sizeof fb } };
        const proven_u16 half[] = { 'o', 'k', 0xD83D };
        e = proven_writer_write_u16(proven_writer_from_buffer(&wb), (proven_u16str_view_t){ half, 3 }, PROVEN_TEXT_UTF8);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ENCODING && wb.len == 0, "a trailing high surrogate writes nothing", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 3, the view vocabulary",
        "n separators yield n + 1 fields; \"\" is one field; an empty separator yields the input once; trim knows six bytes; find_last counts overlaps and answers size for an empty needle; \"\\xFF\" sorts after \"a\".",
        "Each assertion quotes its sentence in chapter 3 section 1.");
    // ---------------------------------------------------------------
    {
        int counts[3] = {0};
        const char *srcs[3] = { "a,b,c", "", "abc" };
        const char *seps[3] = { ",", ",", "" };
        for (int i = 0; i < 3; ++i) {
            proven_u8str_view_split_t it = proven_u8str_view_split(proven_u8str_view_from_cstr(srcs[i]), proven_u8str_view_from_cstr(seps[i]));
            proven_u8str_view_t f;
            while (counts[i] < 100 && proven_u8str_view_split_next(&it, &f)) ++counts[i];
        }
        PROVEN_TEST_ASSERT(counts[0] == 3, "\"a,b,c\" is three fields", "");
        PROVEN_TEST_ASSERT(counts[1] == 1, "\"\" is one empty field, not zero", "");
        PROVEN_TEST_ASSERT(counts[2] == 1, "an empty separator yields the whole input once", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_trim(PROVEN_LIT("\xC2\xA0x")).size == 3, "a no-break space is not whitespace here", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_find_last(PROVEN_LIT("aaa"), PROVEN_LIT("aa")) == 1, "find_last(\"aaa\", \"aa\") is 1", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_find_last(PROVEN_LIT("abc"), PROVEN_LIT("")) == 3, "an empty needle answers size", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_cmp(PROVEN_LIT("\xFF"), PROVEN_LIT("a")) > 0, "\"\\xFF\" sorts after \"a\"", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_cmp(PROVEN_LIT("app"), PROVEN_LIT("apple")) < 0, "\"app\" before \"apple\"", "");
    }

    PROVEN_TEST_PASS("every claim these chapters make, that a reader could act on, is true.");
    return 0;
}
