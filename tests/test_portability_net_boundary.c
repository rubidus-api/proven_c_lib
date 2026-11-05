#include "proven.h"
#include "proven_test.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * RFC-0043: the tree holds two libraries. proven_base - include/proven, src/proven, platform - and
 * proven_net - net/include/proven, net/src, net/platform - which stands on the base library's
 * public headers alone. This gate holds the line both ways:
 *
 *   - a network file includes "proven/..." public headers (the base library's or its own), the
 *     network code's own private headers (net/src) and its own platform headers (net/platform) -
 *     never a header private to the base library (proven_internal_crypto.h, proven_sys_mem.h, ...);
 *   - a base file includes nothing of the network library: none of its public headers, none of
 *     its private ones. proven.h's optional include of proven_net.h is the one exception.
 */

#define MAX_NAMES 128
static char g_net_public[MAX_NAMES][300];   /* "proven/http.h", ... */
static int g_net_public_n;
static char g_net_own[MAX_NAMES][300];      /* "proven_internal_tls.h", ... (net/src headers) */
static int g_net_own_n;

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

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static bool listed(const char *name, size_t len, char (*list)[300], int n) {
    for (int i = 0; i < n; ++i) if (strlen(list[i]) == len && memcmp(list[i], name, len) == 0) return true;
    return false;
}

/* The header names of a directory, with a prefix, into a list. */
static void names_of(const char *dir, const char *prefix, char (*list)[300], int *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < MAX_NAMES) {
        if (!ends_with(e->d_name, ".h")) continue;
        snprintf(list[*n], 300, "%s%s", prefix, e->d_name);
        ++*n;
    }
    closedir(d);
}

typedef bool (*allow_fn)(const char *name, size_t len, const char *file);

static bool net_may_include(const char *name, size_t len, const char *file) {
    (void)file;
    if (len > 7 && memcmp(name, "proven/", 7) == 0) return true;                  /* public: base or net */
    if (listed(name, len, g_net_own, g_net_own_n)) return true;                   /* net/src's own */
    static const char *const pal[] = { "../platform/proven_sys_net.h", "../platform/proven_sys_net_mem.h", "proven_sys_net.h" };
    for (size_t i = 0; i < sizeof pal / sizeof pal[0]; ++i) if (strlen(pal[i]) == len && memcmp(pal[i], name, len) == 0) return true;
    return (len == 8 && memcmp(name, "proven.h", 8) == 0) || (len == 12 && memcmp(name, "proven_net.h", 12) == 0);   /* the umbrellas */
}

static bool base_may_include(const char *name, size_t len, const char *file) {
    if (listed(name, len, g_net_public, g_net_public_n)) return false;
    if (listed(name, len, g_net_own, g_net_own_n)) return false;
    if (strstr(name, "proven_sys_net") != NULL && (size_t)(strstr(name, "proven_sys_net") - name) < len) return false;
    if (len == 12 && memcmp(name, "proven_net.h", 12) == 0) return strcmp(file, "include/proven.h") == 0;
    return true;
}

/* Every quoted #include of every .c and .h file in `dir` that `allow` refuses: reported, counted. */
static int stray_in(const char *dir, allow_fn allow, const char *what, int *files) {
    DIR *d = opendir(dir);
    if (!d) { PROVEN_TEST_INFO("cannot open {}", PROVEN_ARG(dir)); return 1; }
    int stray = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with(e->d_name, ".c") && !ends_with(e->d_name, ".h")) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        char *text = slurp(path);
        if (!text) continue;
        ++*files;
        for (char *p = strstr(text, "#include \""); p; p = strstr(p + 1, "#include \"")) {
            const char *name = p + 10;
            const char *end = strchr(name, '"');
            if (!end) break;
            const size_t len = (size_t)(end - name);
            if (allow(name, len, path)) continue;
            char shown[600];
            snprintf(shown, sizeof shown, "%s: %.*s", path, (int)len, name);
            PROVEN_TEST_INFO("{} {}", PROVEN_ARG(what), PROVEN_ARG(shown));
            ++stray;
        }
        free(text);
    }
    closedir(d);
    return stray;
}

int main(void) {
    PROVEN_TEST_SUITE("proven_net on the base library's public headers; the base on nothing of proven_net",
        "RFC-0043: the network library includes \"proven/...\" public headers and its own, nothing private to the base library; the base library includes nothing of the network library, so that either can be taken without the other's insides and the base without the network at all.",
        "A failure names the file and the header. Network code: use the public API (aead.h, kex.h, sign.h, der.h, ...); if it lacks something, the base library publishes it first. Base code: the dependency runs the other way - move the code, or the base must not need it.");

    names_of("net/include/proven", "proven/", g_net_public, &g_net_public_n);
    names_of("net/src", "", g_net_own, &g_net_own_n);
    PROVEN_TEST_ASSERT(g_net_public_n >= 16 && g_net_own_n >= 12, "the network library's headers are found", "Run from the repository root.");

    PROVEN_TEST_SECTION("the network library's includes", "net/src, net/include/proven and net/platform.", "Named above.");
    {
        int files = 0;
        int stray = stray_in("net/src", net_may_include, "network code includes the base library's", &files) +
                    stray_in("net/include/proven", net_may_include, "network code includes the base library's", &files) +
                    stray_in("net/platform", net_may_include, "network code includes the base library's", &files);
        PROVEN_TEST_ASSERT(files >= 50, "the network files are read", "");
        PROVEN_TEST_ASSERT(stray == 0, "no network file includes a header private to the base library", "");
    }

    PROVEN_TEST_SECTION("the base library's includes", "src/proven, include/proven, include and platform.", "Named above.");
    {
        int files = 0;
        int stray = stray_in("src/proven", base_may_include, "the base library includes the network library's", &files) +
                    stray_in("include/proven", base_may_include, "the base library includes the network library's", &files) +
                    stray_in("include", base_may_include, "the base library includes the network library's", &files) +
                    stray_in("platform", base_may_include, "the base library includes the network library's", &files);
        PROVEN_TEST_ASSERT(files >= 100, "the base files are read", "");
        PROVEN_TEST_ASSERT(stray == 0, "no base file includes anything of the network library", "");
    }

    PROVEN_TEST_SECTION("no network source left in the base", "A file in src/proven named like a network unit means a move was missed.",
        "Move it under net/src, or rename it if it is not network code.");
    {
        static const char *const prefixes[] = { "tls", "quic", "h2", "h3", "qpack", "hpack", "http", "ws", "sse", "net", "loop" };
        DIR *d = opendir("src/proven");
        int left = 0;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            if (!ends_with(n, ".c")) continue;
            const size_t len = strlen(n) - 2;
            for (size_t k = 0; k < sizeof prefixes / sizeof prefixes[0]; ++k) {
                const size_t pl = strlen(prefixes[k]);
                if (len >= pl && memcmp(n, prefixes[k], pl) == 0 && (len == pl || n[pl] == '_')) {
                    PROVEN_TEST_INFO("src/proven/{} looks like network code", PROVEN_ARG(n));
                    ++left;
                    break;
                }
            }
        }
        if (d) closedir(d);
        PROVEN_TEST_ASSERT(left == 0, "every network source is under net/", "");
    }

    PROVEN_TEST_PASS("proven_net stands on the base library's public headers alone, and the base on nothing of it.");
    return 0;
}
