#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/*
 * TEST.md is the catalog: one entry per test, saying what the test answers and
 * what to inspect when it fails. Nothing checked that the catalog matched the
 * build, and both halves had already stopped being true - ten tests registered
 * in nob.c had no entry at all, and the headline counts named numbers that
 * matched neither nob.c nor the tree.
 *
 * A catalog nobody checks is a catalog that documents the suite as it was. So
 * this test checks two propositions:
 *
 *   1. Every test nob.c registers has a `### `tests/NAME`` entry in TEST.md.
 *   2. TEST.md has exactly one entry per tests/test_*.c file - no test file
 *      undocumented, no entry describing a file that was deleted.
 *
 * The second is the stronger one: it fails when a test is added and when a
 * test is removed, which is when the catalog actually rots.
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

/* Does TEST.md carry a catalog heading for this test name? */
static bool catalog_has_entry(const char *test_md, const char *name) {
    char needle[256];
    snprintf(needle, sizeof needle, "### `tests/%s`", name);
    return strstr(test_md, needle) != NULL;
}

/* Count `### `tests/test_...`` headings at the start of a line. */
static int count_catalog_entries(const char *test_md) {
    int n = 0;
    const char *p = test_md;
    const char *hit;
    while ((hit = strstr(p, "### `tests/test_")) != NULL) {
        if (hit == test_md || hit[-1] == '\n') ++n;
        p = hit + 1;
    }
    return n;
}

int main(void) {
    PROVEN_TEST_SUITE("the test catalog matches the build",
        "Every test nob.c registers must have an entry in TEST.md, and TEST.md must carry exactly one entry per tests/test_*.c file.",
        "TEST.md is the catalog. Ten registered tests once had no entry and the headline counts had drifted, because nothing compared the file to nob.c or to the tree.");

    char *nob = read_text_file("nob.c");
    PROVEN_TEST_ASSERT(nob != NULL,
        "could not read nob.c",
        "This test must run from the repository root.");

    char *test_md = read_text_file("TEST.md");
    PROVEN_TEST_ASSERT(test_md != NULL,
        "could not read TEST.md",
        "This test must run from the repository root.");

    PROVEN_TEST_SECTION("every test nob.c registers is in the catalog",
        "A registered test with no entry is a test nobody can look up when it fails.",
        "Add a `### `tests/NAME`` section under the matching class heading in TEST.md.");
    {
        static char seen[400][200];
        int registered = 0;
        int missing = 0;
        const char *p = nob;
        const char *hit;
        while ((hit = strstr(p, "\"tests/test_")) != NULL) {
            const char *start = hit + strlen("\"tests/");
            const char *end = start;
            while (*end && *end != '"') ++end;
            if (*end == '"') {
                size_t len = (size_t)(end - start);
                if (len > 0 && len < 200) {
                    char name[200];
                    memcpy(name, start, len);
                    name[len] = '\0';
                    /* nob.c names some tests as source paths ("tests/x.c") and
                     * others as executables ("tests/x"). They are one test. */
                    size_t nlen = strlen(name);
                    if (nlen > 2 && strcmp(name + nlen - 2, ".c") == 0) name[nlen - 2] = '\0';
                    /* Count each distinct name once. */
                    bool seen_before = false;
                    for (int i = 0; i < registered; ++i) {
                        if (strcmp(seen[i], name) == 0) { seen_before = true; break; }
                    }
                    if (!seen_before && registered < (int)(sizeof seen / sizeof seen[0])) {
                        snprintf(seen[registered], sizeof seen[0], "%s", name);
                        ++registered;
                        if (!catalog_has_entry(test_md, name)) {
                            printf("[PROVEN][TEST][INFO] registered in nob.c but absent from TEST.md: %s\n", name);
                            ++missing;
                        }
                    }
                }
            }
            p = hit + 1;
        }

        PROVEN_TEST_ASSERT(registered > 100,
            "found suspiciously few registered tests in nob.c",
            "The scan is broken - nob.c registers well over a hundred tests.");
        PROVEN_TEST_INFO("nob.c registers {} distinct tests", PROVEN_ARG(registered));
        PROVEN_TEST_ASSERT(missing == 0,
            "tests are registered in nob.c with no TEST.md entry (listed above)",
            "Add each one to TEST.md under its class heading, with an Intent line and a Failure tip.");
    }

    PROVEN_TEST_SECTION("the catalog has one entry per test file",
        "An entry per file, both directions: a new test that nobody documented, and an entry describing a file that was deleted, are the two ways this file rots.",
        "Compare `ls tests/test_*.c` with the `### `tests/...`` headings in TEST.md.");
    {
        DIR *d = opendir("tests");
        PROVEN_TEST_ASSERT(d != NULL,
            "could not open tests/",
            "This test must run from the repository root.");

        int files = 0;
        int undocumented = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            size_t len = strlen(n);
            if (len < 8) continue;
            if (strncmp(n, "test_", 5) != 0) continue;
            if (strcmp(n + len - 2, ".c") != 0) continue;
            char name[200];
            if (len - 2 >= sizeof name) continue;
            memcpy(name, n, len - 2);
            name[len - 2] = '\0';
            ++files;
            if (!catalog_has_entry(test_md, name)) {
                printf("[PROVEN][TEST][INFO] test file with no TEST.md entry: %s\n", name);
                ++undocumented;
            }
        }
        closedir(d);

        int entries = count_catalog_entries(test_md);
        PROVEN_TEST_INFO("tests/test_*.c files: {}, TEST.md entries: {}",
            PROVEN_ARG(files), PROVEN_ARG(entries));

        PROVEN_TEST_ASSERT(undocumented == 0,
            "test files exist with no TEST.md entry (listed above)",
            "Every test file gets a catalog entry. If the test is not worth describing, it is not worth keeping.");
        PROVEN_TEST_ASSERT(entries == files,
            "TEST.md entry count does not match the number of test files",
            "Either a test was deleted and its entry left behind, or an entry was duplicated. The catalog must be one-to-one with tests/test_*.c.");
    }

    free(nob);
    free(test_md);
    PROVEN_TEST_PASS("the catalog matches nob.c and the tests/ tree.");
    return 0;
}
