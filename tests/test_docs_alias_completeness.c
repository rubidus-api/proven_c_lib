#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/*
 * include/proven/alias_xcv.h offers an `xcv_*` short name for the public API.
 * An alias layer that covers most of the API is worse than none: a caller who
 * adopts it discovers the gaps one compile error at a time, at whichever call
 * site happens to need the one function nobody aliased.
 *
 * It had drifted - 25 public functions had no alias, some of them for months -
 * because nothing checked. So this test does: it parses the public headers,
 * parses the alias header, and requires every public function to have an alias.
 *
 * Adding a public function without an alias now fails the build, which is the
 * only way a list like this stays true.
 */

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

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Does `text` contain `name` as a whole identifier? */
static bool has_identifier(const char *text, const char *name) {
    size_t len = strlen(name);
    const char *p = text;
    while ((p = strstr(p, name)) != NULL) {
        bool left_ok  = (p == text) || !is_ident_char(p[-1]);
        bool right_ok = !is_ident_char(p[len]);
        if (left_ok && right_ok) return true;
        p += len;
    }
    return false;
}

/*
 * Collect `proven_xxx(` names that look like declarations. A declaration line
 * has a return type before the name, so it is enough to skip lines that begin a
 * statement (return/if/...) or a comment - the headers only contain declarations
 * and a handful of static inline bodies.
 */
static int collect_public_fns(const char *src, char names[][96], int cap, int count) {
    const char *line = src;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char buf[512];
        size_t copy = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, line, copy);
        buf[copy] = '\0';

        const char *s = buf;
        while (*s == ' ' || *s == '\t') ++s;

        bool skip = (strncmp(s, "return", 6) == 0) || (strncmp(s, "if", 2) == 0) ||
                    (*s == '}') || (*s == '*') || (*s == '/') || (*s == '#') ||
                    (strncmp(s, "for", 3) == 0) || (strncmp(s, "while", 5) == 0);

        if (!skip) {
            const char *p = strstr(buf, "proven_");
            while (p) {
                const char *q = p;
                while (is_ident_char(*q)) ++q;
                const char *paren = q;
                while (*paren == ' ') ++paren;
                bool left_ok = (p == buf) || !is_ident_char(p[-1]);
                if (*paren == '(' && left_ok && (size_t)(q - p) < 90) {
                    char name[96];
                    size_t n = (size_t)(q - p);
                    memcpy(name, p, n);
                    name[n] = '\0';
                    bool seen = false;
                    for (int i = 0; i < count; ++i) {
                        if (strcmp(names[i], name) == 0) { seen = true; break; }
                    }
                    if (!seen && count < cap) {
                        strcpy(names[count], name);
                        ++count;
                    }
                    break;   /* one declaration per line is enough */
                }
                p = strstr(q, "proven_");
            }
        }

        if (!eol) break;
        line = eol + 1;
    }
    return count;
}

int main(void) {
    PROVEN_TEST_SUITE("alias layer completeness",
        "Every public proven_* function must have an xcv_* alias.",
        "A partial alias layer fails at the caller's call site, not here. Add the alias to include/proven/alias_xcv.h.");

    PROVEN_TEST_SECTION("every public function has an xcv_ alias",
        "The alias header claims to cover the public API; this makes the claim checkable.",
        "If a name is listed below, add `#define xcv_<name-without-proven_> proven_<name>` to include/proven/alias_xcv.h, keeping the file alphabetical.");

    char *alias = read_text_file("include/proven/alias_xcv.h");
    PROVEN_TEST_ASSERT(alias != NULL,
        "could not read include/proven/alias_xcv.h",
        "This test must run from the repository root; check the build driver's working directory.");

    static char names[512][96];
    int count = 0;

    DIR *d = opendir("include/proven");
    PROVEN_TEST_ASSERT(d != NULL, "could not open include/proven", "");

    struct dirent *e;
    int headers = 0;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 3 || strcmp(n + len - 2, ".h") != 0) continue;
        if (strcmp(n, "alias_xcv.h") == 0) continue;   /* the alias header itself */

        char path[512];
        snprintf(path, sizeof path, "include/proven/%s", n);
        char *src = read_text_file(path);
        PROVEN_TEST_ASSERT(src != NULL, "could not read a public header", "");
        count = collect_public_fns(src, names, 512, count);
        free(src);
        ++headers;
    }
    closedir(d);

    PROVEN_TEST_ASSERT(headers > 20,
        "found suspiciously few public headers",
        "The scan probably ran from the wrong directory; it must run from the repository root.");
    PROVEN_TEST_ASSERT(count > 150,
        "found suspiciously few public functions",
        "The declaration scan is broken - it should find roughly 200.");

    int missing = 0;
    for (int i = 0; i < count; ++i) {
        if (!has_identifier(alias, names[i])) {
            printf("[PROVEN][TEST][INFO] no xcv_ alias for %s\n", names[i]);
            ++missing;
        }
    }

    PROVEN_TEST_ASSERT(missing == 0,
        "public functions exist with no xcv_ alias (listed above)",
        "Add each to include/proven/alias_xcv.h. A half-covered alias layer is worse than none: the caller finds the gaps one compile error at a time.");

    free(alias);
    PROVEN_TEST_PASS("every public function has an alias.");
    return 0;
}
