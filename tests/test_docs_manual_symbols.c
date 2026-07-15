#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/*
 * The manual and the headers must name the same set of functions - in BOTH directions, because
 * each direction fails differently and each has already happened here:
 *
 *   Headers -> manual: a public function nothing documents is a feature nobody can find.
 *       proven_fs_dir_open/_next/_close - a whole streaming-directory API, and the answer to
 *       proven_fs_list reading a 50,000-entry directory into 4.2 MB before you see any of it -
 *       went undocumented for months. A reader who never learns it exists reaches for the wrong
 *       call, and no test noticed.
 *
 *   Manual -> headers: a function the manual documents that does not EXIST is worse. The reader
 *       writes the call and the LINKER tells them, which is the point at which they stop
 *       trusting the manual. Both of these were live in this repository:
 *         - `proven_sysio_flush` was deleted, and the manual went on declaring it as public API
 *           in a signature listing, in the present tense.
 *         - `proven_pool_free` never existed at all. The real symbol is a static
 *           `proven_pool_free_trait`; freeing a pool slot goes through the allocator trait. The
 *           manual described it as a callable function for as long as the manual has existed.
 *
 * So this test parses the public headers, parses manual/, and requires the two to agree. It is
 * the mechanical half of the documentation process in docs/DOCUMENTING.md; the half a human has
 * to do is judgement about depth, and no test can check that.
 *
 * What counts as "the manual names it": the symbol appears inside a `backtick span` or a fenced
 * code block. Prose that merely mentions a family (`proven_fs_*`) is not a claim that a symbol
 * exists, so a trailing `_` or `*` is treated as a wildcard and ignored.
 */

#define MAX_SYMS 1200
#define MAX_NAME 96

static char g_real[MAX_SYMS][MAX_NAME];   /* every real function: public API + PAL */
static bool g_real_public[MAX_SYMS];      /* true if declared in include/proven (not the PAL) */
static int  g_real_n = 0;

/* Two sets, because the two directions are not the same question.
 *
 *   g_call      - names the manual writes as a CALL: `proven_x(...)`. Writing a call is a claim
 *                 that the function EXISTS, so this is what the ghost check tests. Strict on
 *                 purpose: prose that merely names a function is not a promise you can link it.
 *
 *   g_mention   - any name the manual presents as code at all, with or without parentheses. A
 *                 reference-table row often writes `proven_rng_is_valid` bare. That still counts
 *                 as documented, so this is what the coverage check tests. */
static char g_call[MAX_SYMS][MAX_NAME];
static char g_call_where[MAX_SYMS][64];
static int  g_call_n = 0;

static char g_mention[MAX_SYMS][MAX_NAME];
static int  g_mention_n = 0;

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool have(char list[][MAX_NAME], int n, const char *name) {
    for (int i = 0; i < n; ++i) if (strcmp(list[i], name) == 0) return true;
    return false;
}

/* Collect every `proven_xxx(` that a public header DECLARES. A declaration is a `proven_x(`
 * whose identifier is preceded by a type and followed, eventually, by a `;` - but we do not need
 * a C parser: any `proven_x(` in a header is either a declaration or a macro that expands to
 * one, and both are things a caller can write. */
static void collect_real_from(const char *dir, bool is_public) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 3 || strcmp(n + len - 2, ".h") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        char *s = read_text_file(path);
        if (!s) continue;

        for (char *p = s; (p = strstr(p, "proven_")) != NULL; ) {
            /* must start an identifier */
            if (p != s && ident_char(p[-1])) { ++p; continue; }
            char *q = p;
            while (ident_char(*q)) ++q;
            /* a declaration or macro: the identifier is immediately followed by '(' */
            if (*q == '(' && (size_t)(q - p) < MAX_NAME) {
                char name[MAX_NAME];
                memcpy(name, p, (size_t)(q - p));
                name[q - p] = '\0';
                if (!have(g_real, g_real_n, name) && g_real_n < MAX_SYMS) {
                    g_real_public[g_real_n] = is_public;
                    snprintf(g_real[g_real_n], MAX_NAME, "%s", name);
                    ++g_real_n;
                }
            }
            p = q;
        }
        free(s);
    }
    closedir(d);
}

/* The public API, plus the PAL - platform/proven_sys_*.h declares real functions, and the
 * manual references them when it explains where the platform boundary is. */
static void collect_real(void) {
    collect_real_from("include/proven", true);
    /* The PAL is real - the manual references it when it explains the platform boundary, and a
     * reference to it is not a ghost. But it is an internal layer for porting and integration,
     * not the API a caller programs against, so it is NOT held to the coverage requirement. */
    collect_real_from("platform", false);
}

/* Collect every proven_* name the manual presents AS CODE (inside `backticks` or a fence). */
static void collect_claimed(void) {
    DIR *d = opendir("manual");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 4 || strcmp(n + len - 3, ".md") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "manual/%s", n);
        char *s = read_text_file(path);
        if (!s) continue;

        bool in_code = false;   /* inside a ``` fence */
        for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
            if (strncmp(line, "```", 3) == 0) { in_code = !in_code; continue; }

            for (char *p = line; (p = strstr(p, "proven_")) != NULL; ) {
                if (p != line && ident_char(p[-1])) { ++p; continue; }

                /* In prose, only a backticked span is a claim that the symbol exists. */
                if (!in_code) {
                    bool backticked = false;
                    for (char *b = p; b > line; --b) {
                        if (*(b - 1) == '`') { backticked = true; break; }
                        if (!ident_char(*(b - 1)) && *(b - 1) != '_') break;
                    }
                    if (!backticked) { ++p; continue; }
                }

                char *q = p;
                while (ident_char(*q)) ++q;

                /* A family wildcard (`proven_fs_*`, `proven_fs_`) is not a claim. */
                if (q[-1] == '_' || *q == '*') { p = q; continue; }
                if ((size_t)(q - p) >= MAX_NAME) { p = q; continue; }

                char name[MAX_NAME];
                memcpy(name, p, (size_t)(q - p));
                name[q - p] = '\0';

                if (!have(g_mention, g_mention_n, name) && g_mention_n < MAX_SYMS) {
                    snprintf(g_mention[g_mention_n++], MAX_NAME, "%s", name);
                }
                if (*q == '(' && !have(g_call, g_call_n, name) && g_call_n < MAX_SYMS) {
                    snprintf(g_call[g_call_n], MAX_NAME, "%s", name);
                    snprintf(g_call_where[g_call_n], sizeof g_call_where[0], "%.60s", n);
                    ++g_call_n;
                }
                p = q;
            }
        }
        free(s);
    }
    closedir(d);
}

int main(void) {
    PROVEN_TEST_SUITE("the manual and the headers name the same functions",
        "Every public function is documented, and the manual does not document a function that does not exist. Both failures have happened here, and neither was caught by anything.",
        "Run this from the repository root. If it fails, the name it prints is either a function you added without documenting, or a function the manual promises and the linker will refuse.");

    collect_real();
    collect_claimed();

    PROVEN_TEST_ASSERT(g_real_n > 200,
        "the header scan must actually find the public API",
        "A near-empty scan means this test is being run from the wrong directory and is checking nothing.");
    PROVEN_TEST_ASSERT(g_mention_n > 100,
        "the manual scan must actually find documented symbols", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the manual does not document a function that does not exist",
        "A reader writes the call, and the LINKER tells them. That is the moment they stop trusting the manual.",
        "`proven_sysio_flush` (deleted, still declared as public API) and `proven_pool_free` (never existed; the real symbol is a static _trait) both lived here.");
    // ---------------------------------------------------------------
    {
        int ghosts = 0;
        for (int i = 0; i < g_call_n; ++i) {
            if (!have(g_real, g_real_n, g_call[i])) {
                PROVEN_TEST_INFO("the manual documents {}() - which no public header declares (in {})",
                                 PROVEN_ARG(g_call[i]), PROVEN_ARG(g_call_where[i]));
                ++ghosts;
            }
        }
        PROVEN_TEST_ASSERT(ghosts == 0,
            "every function the manual writes as a call must exist in a public header",
            "Named above. Either the function was deleted and the manual was not, or it never existed.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every public function is named somewhere in the manual",
        "A function nothing documents is a feature nobody can find. The streaming directory API went undocumented for months.",
        "Documenting it may mean a section, a reference-table row, or a mention in the family it belongs to - but it must appear.");
    // ---------------------------------------------------------------
    {
        int missing = 0;
        for (int i = 0; i < g_real_n; ++i) {
            if (!g_real_public[i]) continue;   /* the PAL is not the API a caller programs against */
            if (!have(g_mention, g_mention_n, g_real[i])) {
                PROVEN_TEST_INFO("no manual mentions {}()", PROVEN_ARG(g_real[i]));
                ++missing;
            }
        }
        PROVEN_TEST_ASSERT(missing == 0,
            "every public function must be named in manual/ (the PAL is exempt)",
            "Named above. Add it to the chapter that owns its module - a reference-table row is enough.");
    }

    PROVEN_TEST_PASS("the manual and the headers agree on which functions exist.");
    return 0;
}
