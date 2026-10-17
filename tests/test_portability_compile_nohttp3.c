#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nohttp3", "POSIX host only: the fixture runs the host compiler through /bin/sh.", "");
    PROVEN_TEST_PASS("Skipped on Windows: the fixture needs a POSIX shell (B-035).");
    return 0;
}
#else

/*
 * PROVEN_NO_HTTP3 is a promise about size (RFC-0017 B-10): QUIC, QPACK and HTTP/3 are left out
 * of the library. That is QUIC's packet protection and transport parameters (quic_crypto.c) and
 * the TLS engine's QUIC mode (v0.61.0), the packet and frame codec (quic_frame.c) and the
 * connection (quic_conn.c) (v0.62.0), QPACK (qpack.c) and the HTTP/3 connection (h3_conn.c)
 * (v0.63.0), and the event server's HTTP/3 (http_event_h3.c, where `http3` is then
 * PROVEN_ERR_UNSUPPORTED) (v0.64.0); the event client's joins it next. TLS over TCP is
 * untouched either way.
 *
 * The suite itself is built without that macro, so nothing else here could notice the promise
 * breaking. This test asks the host compiler directly: every library source must compile with
 * -DPROVEN_NO_HTTP3, a program that includes proven.h and the alias header and makes a TLS
 * client must link against the result and run, and no object may define or reference a QUIC,
 * QPACK or HTTP/3 symbol.
 */

static int run(const char *cmd) {
    int rc = system(cmd);
    PROVEN_TEST_ASSERT(rc != -1, "launch the host compiler", "Check that /bin/sh and a C compiler are available.");
    return rc;
}

int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nohttp3",
        "A hosted build with -DPROVEN_NO_HTTP3 compiles every source, links, runs a TLS client, and contains no QUIC, QPACK or HTTP/3 code.",
        "Inspect the PROVEN_NO_HTTP3 guards in src/proven/quic_*.c, qpack.c, h3_conn.c, http_event_h3.c, http_event.c, http_event_h2.c, proven_internal_quic.h, proven_internal_h3.h, proven_internal_http_event.h and tls13.c.");

    const char *dir = "build/test_portability_compile_nohttp3";
    char cmd[4096];

    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0, "a clean scratch directory", "");

    /* -std=c23 where the compiler has it, the transitional spelling where it does not - the
     * same fallback the build driver makes. */
    snprintf(cmd, sizeof cmd,
        "std=-std=c23; ${CC:-cc} $std -fsyntax-only -x c /dev/null 2>/dev/null || std=-std=c2x; "
        "for f in $(sed -n 's/^PROVEN_BUILD_SOURCE(\"\\([^\"]*\\)\".*/\\1/p' build_sources.inc); do "
        "  ${CC:-cc} $std -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DPROVEN_NO_HTTP3 "
        "    -I./include -I./platform -c \"$f\" -o %s/$(echo \"$f\" | tr '/' '_').o || exit 1; "
        "done; echo $std > %s/std.txt", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "every library source compiles with -DPROVEN_NO_HTTP3",
        "A source that uses a QUIC declaration when the macro is set: guard the use.");

    snprintf(cmd, sizeof cmd,
        "printf '%%s\\n' '#include \"proven.h\"' '#include \"proven/alias_xcv.h\"' "
        "'static const proven_u8str_view_t h3[1] = { { (const proven_byte_t *)\"h3\", 2 } };' "
        "'static void on_req(void *x, proven_http_stream_t *s, const proven_http_request_t *r) { (void)x; (void)s; (void)r; }' "
        "'int main(void) {' "
        "'    proven_tls_options_t o = { .alloc = proven_heap_allocator(), .verify = PROVEN_TLS_VERIFY_PIN_ONLY, .alpn = h3, .alpn_count = 1 };' "
        "'    static const proven_byte_t pin[1][32] = { { 1 } };' "
        "'    o.pins = pin; o.pin_count = 1;' "
        "'    proven_tls_config_t *cfg = NULL; proven_tls_conn_t *c = NULL;' "
        "'    if (proven_tls_config_create(&o, &cfg) != PROVEN_OK) return 1;' "
        "'    if (proven_tls_client_create(cfg, (proven_u8str_view_t){ (const proven_byte_t *)\"example.test\", 12 }, NULL, &c) != PROVEN_OK) return 2;' "
        "'    int ok = proven_tls_pending_output(c).size > 100 ? 0 : 3;' "
        "'    proven_tls_conn_destroy(c); proven_tls_config_destroy(cfg);' "
        "'    proven_loop_t *loop = NULL; proven_http_event_server_t *sv = NULL;' "
        "'    if (proven_loop_create(proven_heap_allocator(), &loop) != PROVEN_OK) return 4;' "
        "'    proven_http_event_server_config_t sc = { .on = { .on_request = on_req }, .http3 = true };' "
        "'    if (proven_http_event_server_create(loop, &sc, &sv) != PROVEN_ERR_UNSUPPORTED || sv) return 5;' "
        "'    proven_loop_destroy(loop);' "
        "'    return ok;' "
        "'}' > %s/main.c && "
        "${CC:-cc} $(cat %s/std.txt) -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DPROVEN_NO_HTTP3 -I./include -I./platform "
        "  %s/main.c %s/*.o -pthread -o %s/nohttp3 && %s/nohttp3",
        dir, dir, dir, dir, dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "a program that includes proven.h links against that build and runs, its TLS client writes a ClientHello, and an event server asked for http3 is PROVEN_ERR_UNSUPPORTED",
        "Something outside the guards references a QUIC function, or the guards in tls13.c broke the engine.");

    snprintf(cmd, sizeof cmd,
        "! (nm %s/*.o 2>/dev/null | grep -E ' [TtUDdRr] (proven_quic_|proven_tls_quic_|proven_qpack_|proven_h3_|proven_http_event_h3_|proven_http_event_alt_svc_)' >/dev/null)", dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "no object of that build defines or references a QUIC, QPACK or HTTP/3 function",
        "List them with: nm build/test_portability_compile_nohttp3/*.o | grep -iE 'quic|qpack|h3_|alt_svc'");

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)run(cmd);

    PROVEN_TEST_PASS("PROVEN_NO_HTTP3 leaves QUIC out of a hosted build, and TLS over TCP works.");
    return 0;
}
#endif
