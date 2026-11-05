#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * RFC-0043: the network stack is to become a library of its own, proven_net, that uses nothing of
 * the base library but its public headers. This gate holds the line inside one tree: every
 * network source and every header private to the network code includes only public headers
 * ("proven/...") and the network code's own internal headers - never one of the base library's
 * (proven_internal_crypto.h, proven_internal_der.h, ...) and never the base library's platform
 * layer (proven_sys_mem.h, ...). And every source in the build's list is classified, so a new
 * network file cannot slip in unchecked.
 */

static const char *const NET_SOURCES[] = {
    "tls13", "tls_keys", "tls12_keys", "tls_legacy", "tls_config", "tls_transport",
    "quic_crypto", "quic_frame", "quic_conn", "qpack", "h3_conn", "hpack", "h2", "h2_conn",
    "http", "http_auth", "http_cookie", "http_coding", "http_client", "http_server",
    "http_event", "http_event_h2", "http_event_h3", "http_event_client", "http_event_client_h2", "http_event_client_h3",
    "ws", "ws_conn", "ws_deflate", "ws_event", "ws_event_client", "sse", "net", "loop",
};

/* The network code's own headers: what it may include besides "proven/...". */
static const char *const NET_OWN[] = {
    "proven_internal_tls.h", "proven_internal_quic.h", "proven_internal_quic_net.h", "proven_internal_h3.h",
    "proven_internal_net.h", "proven_internal_loop.h", "proven_internal_http_event.h", "proven_internal_http_event_client.h",
    "proven_internal_http_coding.h", "proven_internal_ws.h", "qpack_static.h", "hpack_tables.h",
    "../../platform/proven_sys_net.h", "../../platform/proven_sys_net_mem.h",
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

static bool in_list(const char *name, size_t len, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; ++i) if (strlen(list[i]) == len && memcmp(list[i], name, len) == 0) return true;
    return false;
}

/* Every quoted #include of `path` that is neither public nor the network code's own: reported,
 * and counted. */
static int stray_includes(const char *path) {
    char *text = slurp(path);
    if (!text) { PROVEN_TEST_INFO("cannot read {}", PROVEN_ARG(path)); return 1; }
    int stray = 0;
    for (char *p = strstr(text, "#include \""); p; p = strstr(p + 1, "#include \"")) {
        const char *name = p + 10;
        const char *end = strchr(name, '"');
        if (!end) break;
        const size_t len = (size_t)(end - name);
        if (len > 7 && memcmp(name, "proven/", 7) == 0) continue;
        if (in_list(name, len, NET_OWN, COUNT(NET_OWN))) continue;
        char shown[160];
        snprintf(shown, sizeof shown, "%s: %.*s", path, (int)len, name);
        PROVEN_TEST_INFO("network code includes the base library's {}", PROVEN_ARG(shown));
        ++stray;
    }
    free(text);
    return stray;
}

int main(void) {
    PROVEN_TEST_SUITE("the network code uses only the base library's public headers",
        "RFC-0043 phase 1: every network source and the network code's private headers include \"proven/...\" headers and their own, nothing else of the base library - no proven_internal_crypto.h, no base platform header - so that the network code can be built as a library of its own.",
        "A failure names the file and the header. Use the public API instead (aead.h, kex.h, sign.h, der.h, ...); if it lacks something, the base library must publish it first.");

    PROVEN_TEST_SECTION("every network file's includes", "Sources and the network code's own headers.", "Named above.");
    {
        int stray = 0;
        char path[128];
        for (size_t i = 0; i < COUNT(NET_SOURCES); ++i) {
            snprintf(path, sizeof path, "src/proven/%s.c", NET_SOURCES[i]);
            stray += stray_includes(path);
        }
        for (size_t i = 0; i < COUNT(NET_OWN); ++i) {
            if (strncmp(NET_OWN[i], "../../platform/", 15) == 0) snprintf(path, sizeof path, "platform/%s", NET_OWN[i] + 15);
            else snprintf(path, sizeof path, "src/proven/%s", NET_OWN[i]);
            stray += stray_includes(path);
        }
        PROVEN_TEST_ASSERT(stray == 0, "no network file includes a header private to the base library", "");
    }

    PROVEN_TEST_SECTION("every source is classified", "A source in build_sources.inc named like a network unit is in this test's list.",
        "A new network source: add it to NET_SOURCES here, and keep it on public headers.");
    {
        char *list = slurp("build_sources.inc");
        PROVEN_TEST_ASSERT(list != NULL, "build_sources.inc is read", "Run from the repository root.");
        static const char *const prefixes[] = { "tls", "quic", "h2", "h3", "qpack", "hpack", "http", "ws", "sse", "net", "loop" };
        int unlisted = 0, seen = 0;
        for (char *p = strstr(list, "\"src/proven/"); p; p = strstr(p + 1, "\"src/proven/")) {
            const char *name = p + 12;
            const char *dot = strstr(name, ".c\"");
            if (!dot) continue;
            const size_t len = (size_t)(dot - name);
            bool netlike = false;
            for (size_t k = 0; k < COUNT(prefixes); ++k) {
                const size_t pl = strlen(prefixes[k]);
                if (len >= pl && memcmp(name, prefixes[k], pl) == 0 && (len == pl || name[pl] == '_' || (name[pl] >= '0' && name[pl] <= '9'))) netlike = true;
            }
            if (!netlike) continue;
            ++seen;
            if (!in_list(name, len, NET_SOURCES, COUNT(NET_SOURCES))) {
                char shown[96];
                snprintf(shown, sizeof shown, "%.*s", (int)len, name);
                PROVEN_TEST_INFO("{}.c looks like a network source and is not in NET_SOURCES", PROVEN_ARG(shown));
                ++unlisted;
            }
        }
        free(list);
        PROVEN_TEST_ASSERT(seen >= (int)COUNT(NET_SOURCES) - 2 && unlisted == 0, "every network-named source is checked", "");
    }

    PROVEN_TEST_PASS("The network code stands on the base library's public headers alone.");
    return 0;
}
