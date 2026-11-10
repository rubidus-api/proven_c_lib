#include "proven.h"
#include "proven_test.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * RFC-0045: beside proven_net (RFC-0043, test_portability_net_boundary) the tree holds two more
 * libraries on the base - proven_crypto (crypto/include/proven, crypto/src, crypto/platform) and
 * proven_codec (codec/...). This gate holds their lines:
 *
 *   - a crypto file includes the base library's public headers, crypto's own public, private and
 *     platform headers, and the umbrellas proven.h and proven_crypto.h - nothing of the codec or
 *     network libraries, nothing private to the base;
 *   - a codec file likewise with codec's own;
 *   - a base file includes nothing of either: no public header, no private one, no platform one.
 *     proven.h's optional includes of proven_crypto.h and proven_codec.h are the one exception.
 */

#define MAX_NAMES 256
typedef struct { char name[MAX_NAMES][300]; int n; } names_t;
static names_t g_base_public, g_crypto_public, g_crypto_own, g_crypto_pal, g_codec_public, g_codec_own, g_codec_pal;

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

static bool is(const char *name, size_t len, const char *lit) { return strlen(lit) == len && memcmp(name, lit, len) == 0; }

static bool listed(const char *name, size_t len, const names_t *l) {
    for (int i = 0; i < l->n; ++i) if (is(name, len, l->name[i])) return true;
    return false;
}

/* The header names of a directory, with a prefix. */
static void names_of(const char *dir, const char *prefix, names_t *l) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && l->n < MAX_NAMES) {
        if (!ends_with(e->d_name, ".h")) continue;
        snprintf(l->name[l->n], 300, "%s%s", prefix, e->d_name);
        ++l->n;
    }
    closedir(d);
}

/* The platform headers of a module as its sources name them ("../platform/x.h") and as its
 * platform files name them ("x.h"). */
static bool pal_of(const char *name, size_t len, const names_t *pal) {
    if (len > 12 && memcmp(name, "../platform/", 12) == 0) return listed(name + 12, len - 12, pal);
    return listed(name, len, pal);
}

typedef bool (*allow_fn)(const char *name, size_t len, const char *file);

static bool module_may_include(const char *name, size_t len, const names_t *pub, const names_t *own, const names_t *pal, const char *umbrella) {
    if (listed(name, len, &g_base_public) || listed(name, len, pub)) return true;
    if (listed(name, len, own) || pal_of(name, len, pal)) return true;
    return is(name, len, "proven.h") || is(name, len, umbrella);
}

static bool crypto_may_include(const char *name, size_t len, const char *file) {
    (void)file;
    return module_may_include(name, len, &g_crypto_public, &g_crypto_own, &g_crypto_pal, "proven_crypto.h");
}

static bool codec_may_include(const char *name, size_t len, const char *file) {
    (void)file;
    return module_may_include(name, len, &g_codec_public, &g_codec_own, &g_codec_pal, "proven_codec.h");
}

static bool base_may_include(const char *name, size_t len, const char *file) {
    if (listed(name, len, &g_crypto_public) || listed(name, len, &g_codec_public)) return false;
    if (listed(name, len, &g_crypto_own) || listed(name, len, &g_codec_own)) return false;
    if (pal_of(name, len, &g_crypto_pal) || pal_of(name, len, &g_codec_pal)) return false;
    if (strstr(name, "crypto/") && (size_t)(strstr(name, "crypto/") - name) < len) return false;
    if (strstr(name, "codec/") && (size_t)(strstr(name, "codec/") - name) < len) return false;
    if (is(name, len, "proven_crypto.h") || is(name, len, "proven_codec.h")) return strcmp(file, "include/proven.h") == 0;
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

/* Files in src/proven named like a unit of another library: a move that was missed. */
static int left_in_base(const char *const *prefixes, size_t count, const char *where) {
    DIR *d = opendir("src/proven");
    int left = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (!ends_with(n, ".c") && !ends_with(n, ".h")) continue;
        const size_t len = strlen(n) - 2;
        for (size_t k = 0; k < count; ++k) {
            const size_t pl = strlen(prefixes[k]);
            if (len >= pl && memcmp(n, prefixes[k], pl) == 0 && (len == pl || n[pl] == '_')) {
                PROVEN_TEST_INFO("src/proven/{} looks like {} code", PROVEN_ARG(n), PROVEN_ARG(where));
                ++left;
                break;
            }
        }
    }
    if (d) closedir(d);
    return left;
}

int main(void) {
    PROVEN_TEST_SUITE("proven_crypto and proven_codec on the base library's public headers; the base on nothing of either",
        "RFC-0045: the cryptography and compression libraries include the base library's public headers and their own, nothing of each other and nothing private to the base; the base includes nothing of either, so that each can be taken without the others and the base alone.",
        "A failure names the file and the header. Crypto or codec code: use the base library's public API; if it lacks something, the base publishes it first. Base code: the dependency runs the other way - move the code, or the base must not need it.");

    names_of("include/proven", "proven/", &g_base_public);
    names_of("crypto/include/proven", "proven/", &g_crypto_public);
    names_of("crypto/src", "", &g_crypto_own);
    names_of("crypto/platform", "", &g_crypto_pal);
    names_of("codec/include/proven", "proven/", &g_codec_public);
    names_of("codec/src", "", &g_codec_own);
    names_of("codec/platform", "", &g_codec_pal);
    PROVEN_TEST_ASSERT(g_base_public.n >= 30 && g_crypto_public.n >= 9 && g_codec_public.n >= 3 && g_crypto_own.n >= 3 && g_codec_own.n >= 4,
        "the three libraries' headers are found", "Run from the repository root.");

    PROVEN_TEST_SECTION("the cryptography library's includes", "crypto/src, crypto/include/proven, crypto/include and crypto/platform.", "Named above.");
    {
        int files = 0;
        const char *what = "crypto code includes what is not its own or the base's public";
        int stray = stray_in("crypto/src", crypto_may_include, what, &files) + stray_in("crypto/include/proven", crypto_may_include, what, &files) +
                    stray_in("crypto/include", crypto_may_include, what, &files) + stray_in("crypto/platform", crypto_may_include, what, &files);
        PROVEN_TEST_ASSERT(files >= 30, "the crypto files are read", "");
        PROVEN_TEST_ASSERT(stray == 0, "no crypto file includes a header of the codec or network libraries or one private to the base", "");
    }

    PROVEN_TEST_SECTION("the compression library's includes", "codec/src, codec/include/proven, codec/include and codec/platform.", "Named above.");
    {
        int files = 0;
        const char *what = "codec code includes what is not its own or the base's public";
        int stray = stray_in("codec/src", codec_may_include, what, &files) + stray_in("codec/include/proven", codec_may_include, what, &files) +
                    stray_in("codec/include", codec_may_include, what, &files) + stray_in("codec/platform", codec_may_include, what, &files);
        PROVEN_TEST_ASSERT(files >= 12, "the codec files are read", "");
        PROVEN_TEST_ASSERT(stray == 0, "no codec file includes a header of the crypto or network libraries or one private to the base", "");
    }

    PROVEN_TEST_SECTION("the base library's includes", "src/proven, include/proven, include and platform.", "Named above.");
    {
        int files = 0;
        const char *what = "the base library includes the crypto or codec library's";
        int stray = stray_in("src/proven", base_may_include, what, &files) + stray_in("include/proven", base_may_include, what, &files) +
                    stray_in("include", base_may_include, what, &files) + stray_in("platform", base_may_include, what, &files);
        PROVEN_TEST_ASSERT(files >= 80, "the base files are read", "");
        PROVEN_TEST_ASSERT(stray == 0, "no base file includes anything of the crypto or codec libraries", "");
    }

    PROVEN_TEST_SECTION("no crypto or codec source left in the base", "A file in src/proven named like one of their units means a move was missed.",
        "Move it under crypto/src or codec/src, or rename it if it belongs to the base.");
    {
        static const char *const crypto[] = { "crypto", "aead", "aes", "kex", "dh", "sign", "der", "cert", "hmac", "hash_legacy", "sha2" };
        static const char *const codec[] = { "deflate", "zstd", "brotli" };
        const int left = left_in_base(crypto, sizeof crypto / sizeof crypto[0], "crypto") + left_in_base(codec, sizeof codec / sizeof codec[0], "codec");
        PROVEN_TEST_ASSERT(left == 0, "every crypto and codec source is under its library", "");
    }

    PROVEN_TEST_PASS("proven_crypto and proven_codec stand on the base library's public headers alone, and the base on nothing of either.");
    return 0;
}
