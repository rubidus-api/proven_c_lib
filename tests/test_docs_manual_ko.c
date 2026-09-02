#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/*
 * The Korean edition is a mirror, and a mirror rots silently.
 *
 * Nothing checked manual-ko/ at all. The English chapters are guarded by six gates - their
 * examples are compiled and run, their symbols are matched against the headers, their claims are
 * asserted - and the translation next to them was guarded by nobody, which meant a chapter could
 * gain a whole worked example on one side and not the other and the build would still pass. That
 * is exactly what had happened: eleven examples existed in English and in no Korean chapter.
 *
 * So this gate asks the three questions that keep a translation honest:
 *
 *   1. Structure. Every English chapter has a Korean counterpart file.
 *   2. Coverage. Every example the English chapters quote is quoted by the Korean chapters too,
 *      and no Korean chapter quotes one the English side does not. The example bodies themselves
 *      are checked verbatim by test_docs_manual_examples, which reads both directories.
 *   3. Vocabulary. A Korean chapter that uses an English term must, somewhere in that chapter,
 *      write it paired with its Korean word - 할당자(allocator). The manual promises this in
 *      chapter 0's glossary: no untranslated word is left for the reader to guess at, and no
 *      Korean word is left without the English original a reader may have learned the idea by.
 *
 * The pairing is checked per chapter, not per occurrence: once a chapter has introduced the
 * term, later uses of the bare word are the ordinary way to write it.
 */

#define MAX_FILES 64
#define MAX_NAME 128
#define MAX_MARKS 128
#define MAX_MARK 160

static char g_en_marks[MAX_MARKS][MAX_MARK];
static int  g_en_mark_n = 0;
static char g_ko_marks[MAX_MARKS][MAX_MARK];
static int  g_ko_mark_n = 0;

/* The terms the Korean chapters must introduce in pairs. Each entry is the English word as the
 * chapters write it, and the Korean word chapter 0's glossary gives for it. */
static const struct { const char *eng; const char *kor; } TERMS[] = {
    { "allocator",    "할당자" },
    { "arena",        "아레나" },
    { "pool",         "풀" },
    { "heap",         "힙" },
    { "view",         "뷰" },
    { "slice",        "슬라이스" },
    { "trait",        "트레잇" },
    { "panic",        "패닉" },
    { "owned",        "소유" },
    { "borrowed",     "빌려 쓰는" },
    { "hosted",       "호스티드" },
    { "freestanding", "프리스탠딩" },
    { "writer",       "쓰기 스트림" },
    { "reader",       "읽기 스트림" },
    { "scratch",      "임시 작업용" },
    { "cursor",       "커서" },
};

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

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }

static bool have_mark(char list[][MAX_MARK], int n, const char *m) {
    for (int i = 0; i < n; ++i) if (strcmp(list[i], m) == 0) return true;
    return false;
}

/* Collect the `<!-- example: path -->` markers a directory's chapters carry. */
static void collect_marks(const char *dir, char list[][MAX_MARK], int *count) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 4 || strcmp(n + len - 3, ".md") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        char *s = read_text_file(path);
        if (!s) continue;

        for (char *p = s; (p = strstr(p, "<!-- example: ")) != NULL; ) {
            p += strlen("<!-- example: ");
            char *q = strstr(p, " -->");
            if (!q) break;
            size_t n2 = (size_t)(q - p);
            if (n2 < MAX_MARK && *count < MAX_MARKS) {
                char m[MAX_MARK];
                memcpy(m, p, n2);
                m[n2] = '\0';
                if (!have_mark(list, *count, m)) {
                    snprintf(list[*count], MAX_MARK, "%s", m);
                    ++(*count);
                }
            }
            p = q;
        }
        free(s);
    }
    closedir(d);
}

/* Is `word` present as a standalone word, outside a fenced code block? Inline code is left in:
 * a term written as `allocator` in backticks still needs the reader to know what it means. */
static bool uses_term(const char *text, const char *word) {
    size_t wl = strlen(word);
    bool in_fence = false;
    for (const char *line = text; line && *line; ) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (len >= 3 && strncmp(line, "```", 3) == 0) {
            in_fence = !in_fence;
        } else if (!in_fence) {
            for (size_t i = 0; i + wl <= len; ++i) {
                if (strncmp(line + i, word, wl) != 0) continue;
                if (i > 0 && word_char(line[i - 1])) continue;
                if (i + wl < len && word_char(line[i + wl])) continue;
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

int main(void) {
    PROVEN_TEST_SUITE("the Korean edition mirrors the English one",
        "manual-ko/ had no gate at all, so it could - and did - fall behind by eleven worked examples without the build noticing. Structure, example coverage, and the term-pairing promise are checked here.",
        "Run this from the repository root. A failure names the missing chapter, the example one edition quotes and the other does not, or the chapter that uses an English term without ever pairing it with its Korean word.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every English chapter has a Korean counterpart",
        "A missing file is a chapter that silently does not exist in the translation.",
        "manual/x.md is mirrored by manual-ko/x-ko.md.");
    // ---------------------------------------------------------------
    {
        DIR *d = opendir("manual");
        PROVEN_TEST_ASSERT(d != NULL, "manual/ must be readable from the repository root",
            "Run this test from the repository root.");
        int missing = 0, seen = 0;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            size_t len = strlen(n);
            if (len < 4 || strcmp(n + len - 3, ".md") != 0) continue;
            ++seen;

            char ko[512];
            snprintf(ko, sizeof ko, "manual-ko/%.*s-ko.md", (int)(len - 3), n);
            char *s = read_text_file(ko);
            if (!s) {
                PROVEN_TEST_INFO("manual/{} has no Korean counterpart", PROVEN_ARG(n));
                ++missing;
            } else {
                free(s);
            }
        }
        if (d) closedir(d);
        PROVEN_TEST_ASSERT(seen > 5, "the chapter scan must actually find chapters", "");
        PROVEN_TEST_ASSERT(missing == 0, "every English chapter must have a Korean counterpart",
            "Named above. Translate it, or the Korean edition is missing a chapter nobody will notice.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("both editions print the same worked examples",
        "Eleven examples were added to the English chapters and to no Korean one. Nothing failed.",
        "Quote the example from the matching Korean chapter with the same `<!-- example: -->` marker; scripts/sync-manual-examples.py copies the body in.");
    // ---------------------------------------------------------------
    {
        collect_marks("manual", g_en_marks, &g_en_mark_n);
        collect_marks("manual-ko", g_ko_marks, &g_ko_mark_n);

        PROVEN_TEST_ASSERT(g_en_mark_n > 10, "the English marker scan must actually find examples", "");

        int only_en = 0, only_ko = 0;
        for (int i = 0; i < g_en_mark_n; ++i) {
            if (!have_mark(g_ko_marks, g_ko_mark_n, g_en_marks[i])) {
                PROVEN_TEST_INFO("the Korean edition does not print {}", PROVEN_ARG(g_en_marks[i]));
                ++only_en;
            }
        }
        for (int i = 0; i < g_ko_mark_n; ++i) {
            if (!have_mark(g_en_marks, g_en_mark_n, g_ko_marks[i])) {
                PROVEN_TEST_INFO("the English edition does not print {}", PROVEN_ARG(g_ko_marks[i]));
                ++only_ko;
            }
        }
        PROVEN_TEST_ASSERT(only_en == 0 && only_ko == 0,
            "every worked example must appear in both editions",
            "Named above. The example file is the source of truth for both; only the surrounding prose is translated.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a Korean chapter that uses an English term pairs it with the Korean word",
        "The manual promises the reader no untranslated word and no Korean word without its English original. A promise nothing checks is a promise that stops holding.",
        "Write the pair once in the chapter - 할당자(allocator) - and the rest of the chapter may use the bare word.");
    // ---------------------------------------------------------------
    {
        int unglossed = 0, scanned = 0;
        DIR *d = opendir("manual-ko");
        PROVEN_TEST_ASSERT(d != NULL, "manual-ko/ must be readable from the repository root", "");
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            size_t len = strlen(n);
            if (len < 4 || strcmp(n + len - 3, ".md") != 0) continue;

            char path[512];
            snprintf(path, sizeof path, "manual-ko/%s", n);
            char *s = read_text_file(path);
            if (!s) continue;
            ++scanned;

            for (size_t i = 0; i < sizeof TERMS / sizeof TERMS[0]; ++i) {
                if (!uses_term(s, TERMS[i].eng)) continue;

                /* Either order counts as a pairing: the chapters write 할당자(allocator),
                 * and the glossary's own rows write allocator(할당자). Both put the two words
                 * in front of the reader together, which is the whole requirement. */
                char pair[128], reversed[128];
                snprintf(pair, sizeof pair, "%s(%s)", TERMS[i].kor, TERMS[i].eng);
                snprintf(reversed, sizeof reversed, "%s(%s)", TERMS[i].eng, TERMS[i].kor);
                if (strstr(s, pair) == NULL && strstr(s, reversed) == NULL) {
                    PROVEN_TEST_INFO("manual-ko/{} uses \"{}\" without ever writing {}",
                                     PROVEN_ARG(n), PROVEN_ARG(TERMS[i].eng), PROVEN_ARG(pair));
                    ++unglossed;
                }
            }
            free(s);
        }
        if (d) closedir(d);
        PROVEN_TEST_ASSERT(scanned > 5, "the Korean chapter scan must actually find chapters", "");
        PROVEN_TEST_ASSERT(unglossed == 0,
            "every English term a Korean chapter uses must be paired with its Korean word somewhere in that chapter",
            "Named above, with the exact pair to write. The glossary in chapter 0 is the list of words this applies to.");
    }

    PROVEN_TEST_PASS("the Korean edition mirrors the English one, and pairs its terms.");
    return 0;
}
