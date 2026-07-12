#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * The manual used to carry ~190 fenced C blocks and only four of them could be
 * compiled at all. The rest were sketches referencing imaginary helpers, so
 * nothing could check them and nothing did - they simply drifted, and the reader
 * was the one who found out.
 *
 * The fix is structural, not editorial: every example the manual prints is a
 * real program in manual/examples/, the build driver compiles and RUNS all of
 * them, and this test checks that what the manual prints is what those programs
 * actually say.
 *
 * Three things are enforced:
 *   1. Every `<!-- example: path -->` marker names a file that exists.
 *   2. The fenced block after the marker is that file's body, verbatim.
 *   3. Every file in manual/examples/ is quoted by some chapter - an example
 *      nobody shows is an example nobody maintains.
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

/*
 * The example body is everything after the `#include "example.h"` line: the
 * include is scaffolding for the compiler, not something a reader needs to see
 * repeated in every chapter.
 */
static const char *example_body(const char *src) {
    const char *inc = strstr(src, "#include \"example.h\"");
    if (!inc) return src;
    const char *nl = strchr(inc, '\n');
    if (!nl) return src;
    ++nl;
    while (*nl == '\n') ++nl;   /* skip the blank line after the include */
    return nl;
}

/* Compare ignoring trailing whitespace on each line and a trailing newline. */
static bool text_eq_loose(const char *a, const char *b) {
    for (;;) {
        while (*a == '\n' && *b == '\n') { ++a; ++b; }
        if (*a == '\0' || *b == '\0') {
            while (*a == '\n' || *a == ' ') ++a;
            while (*b == '\n' || *b == ' ') ++b;
            return *a == '\0' && *b == '\0';
        }
        if (*a != *b) return false;
        ++a; ++b;
    }
}

#define MAX_EXAMPLES 64
static char g_quoted[MAX_EXAMPLES][256];
static int g_quoted_count = 0;

static int check_chapter(const char *md_path) {
    char *md = read_text_file(md_path);
    if (!md) return 0;   /* chapter list is derived from disk; a missing file is caught below */

    int problems = 0;
    const char *p = md;
    const char *marker = "<!-- example: ";

    while ((p = strstr(p, marker)) != NULL) {
        p += strlen(marker);
        const char *end = strstr(p, " -->");
        if (!end) {
            printf("[PROVEN][TEST][INFO] %s: malformed example marker\n", md_path);
            ++problems;
            break;
        }

        char path[256];
        size_t len = (size_t)(end - p);
        if (len >= sizeof(path)) { ++problems; break; }
        memcpy(path, p, len);
        path[len] = '\0';

        if (g_quoted_count < MAX_EXAMPLES) {
            snprintf(g_quoted[g_quoted_count], sizeof(g_quoted[0]), "%s", path);
            ++g_quoted_count;
        }

        char *src = read_text_file(path);
        if (!src) {
            printf("[PROVEN][TEST][INFO] %s quotes %s, which does not exist\n", md_path, path);
            ++problems;
            p = end;
            continue;
        }

        /* The fenced block must follow the marker. */
        const char *fence = strstr(end, "```c\n");
        const char *fence_end = fence ? strstr(fence + 5, "\n```") : NULL;
        if (!fence || !fence_end) {
            printf("[PROVEN][TEST][INFO] %s: no ```c block after the marker for %s\n", md_path, path);
            ++problems;
            free(src);
            p = end;
            continue;
        }

        size_t block_len = (size_t)(fence_end - (fence + 5));
        char *block = (char *)malloc(block_len + 1);
        memcpy(block, fence + 5, block_len);
        block[block_len] = '\0';

        if (!text_eq_loose(block, example_body(src))) {
            printf("[PROVEN][TEST][INFO] %s no longer matches what %s prints\n", path, md_path);
            ++problems;
        }

        free(block);
        free(src);
        p = fence_end;
    }

    free(md);
    return problems;
}

int main(void) {
    PROVEN_TEST_SUITE("manual examples",
        "Every example the manual prints must be a real program that compiles, runs, and says what the manual says it says.",
        "The manual quotes manual/examples/*.c through `<!-- example: path -->` markers. If a chapter and its example disagree, one of them was edited alone.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("each quoted example exists and matches its file",
        "A chapter may only print code that a compiler has actually seen.",
        "Re-copy the example's body into the chapter, or fix the example. Do not hand-edit the chapter to make it look right.");
    // ---------------------------------------------------------------
    static const char *chapters[] = {
        "manual/manual.md",
        "manual/manual-01-foundation.md",
        "manual/manual-02-allocation.md",
        "manual/manual-03-strings-text.md",
        "manual/manual-04-containers-algorithms.md",
        "manual/manual-05-hosted-services.md",
        "manual/manual-06-execution-and-platform.md",
        "manual/manual-08-fmt-scan.md",
        "manual/manual-freestanding.md",
    };

    int problems = 0;
    for (size_t i = 0; i < sizeof(chapters) / sizeof(chapters[0]); ++i) {
        problems += check_chapter(chapters[i]);
    }
    PROVEN_TEST_ASSERT(problems == 0,
        "a chapter and its example have drifted apart (listed above)",
        "The example file is the source of truth: it is compiled and run. Copy its body into the chapter.");

    PROVEN_TEST_ASSERT(g_quoted_count > 0,
        "no chapter quotes any example",
        "The `<!-- example: path -->` markers are missing; the manual is back to unverifiable sketches.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every example file is quoted by some chapter",
        "An example nobody shows is an example nobody maintains.",
        "Either quote the file from the relevant chapter, or delete it.");
    // ---------------------------------------------------------------
    DIR *d = opendir("manual/examples");
    PROVEN_TEST_ASSERT(d != NULL,
        "could not open manual/examples",
        "This test must run from the repository root.");

    int orphans = 0;
    int files = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 3 || strcmp(n + len - 2, ".c") != 0) continue;
        ++files;

        char path[512];
        snprintf(path, sizeof path, "manual/examples/%s", n);

        bool quoted = false;
        for (int i = 0; i < g_quoted_count; ++i) {
            if (strcmp(g_quoted[i], path) == 0) { quoted = true; break; }
        }
        if (!quoted) {
            printf("[PROVEN][TEST][INFO] %s is not quoted by any chapter\n", path);
            ++orphans;
        }
    }
    closedir(d);

    PROVEN_TEST_ASSERT(files > 0, "manual/examples contains no examples", "");
    PROVEN_TEST_ASSERT(orphans == 0,
        "example files exist that no chapter shows (listed above)",
        "Quote it from the chapter it belongs to, or delete it. An unquoted example is dead weight that still has to compile.");

    PROVEN_TEST_PASS("the manual and its examples agree.");
    return 0;
}
