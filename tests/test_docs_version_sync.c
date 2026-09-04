#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * CHECKLIST.md requires the version string to be updated in six places at once - version.h,
 * README (both language halves), TEST.md, the manual chapter headings, the version.h excerpt in
 * manual chapter 1, and CHANGELOG's newest entry. Nothing checked, so it drifted: version.h sat
 * at v26.07.12i while the CHANGELOG had already shipped v26.07.13f, and the README claimed a
 * third value that matched neither.
 *
 * A version string that disagrees with itself is not cosmetic. It is the number a downstream
 * project pins, the number a bug report quotes, and the number that decides whether a fix is in
 * the copy someone is holding.
 *
 * PROVEN_VERSION_STRING is the source of truth. Everything else must agree with it - and the
 * string itself must agree with the three numbers it is built from: versions are semantic,
 * MAJOR.MINOR.PATCH, from v0.0.1 (2026-09-04). The date-based numbers before that are history.
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

/* Does `path` contain `needle`, at least `times` times? */
static int count_in_file(const char *path, const char *needle) {
    char *s = read_text_file(path);
    if (!s) return -1;
    int n = 0;
    for (const char *p = s; (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    free(s);
    return n;
}

int main(void) {
    PROVEN_TEST_SUITE("the version string agrees with itself everywhere",
        "PROVEN_VERSION_STRING is the source of truth; the README (README.md English + README-ko.md Korean), TEST.md, the manual headings and the CHANGELOG's newest entry must all carry the same version.",
        "CHECKLIST.md says to update these together. Nothing checked, and version.h once sat five releases behind the CHANGELOG while the README claimed a third value.");

    /* The string the library itself reports. Strip the "proven_c_lib-" prefix to get "v0.0.1",
     * which is the form the manual headings, TEST.md and the release tag use. */
    const char *full = PROVEN_VERSION_STRING;           /* proven_c_lib-v0.0.1 */
    const char *dash = strrchr(full, '-');
    PROVEN_TEST_ASSERT(dash != NULL && dash[1] == 'v',
        "PROVEN_VERSION_STRING must look like proven_c_lib-vMAJOR.MINOR.PATCH", "");
    const char *v = dash + 1;                            /* v0.0.1 */

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the string is built from the three numbers",
        "MAJOR, MINOR and PATCH are what a caller compares against; the string is what a reader sees. They are one version, stated twice.",
        "");
    // ---------------------------------------------------------------
    {
        char expect[64];
        snprintf(expect, sizeof expect, "proven_c_lib-v%d.%d.%d",
                 PROVEN_VERSION_MAJOR, PROVEN_VERSION_MINOR, PROVEN_VERSION_PATCH);
        PROVEN_TEST_ASSERT(strcmp(full, expect) == 0,
            "PROVEN_VERSION_STRING must equal proven_c_lib-v<MAJOR>.<MINOR>.<PATCH>",
            "The numbers were bumped and the string was not, or the other way round. Change all four lines of version.h together.");
        PROVEN_TEST_INFO("{} == {}", PROVEN_ARG(full), PROVEN_ARG(expect));
        PROVEN_TEST_ASSERT(PROVEN_VERSION_NUM == PROVEN_VERSION_ENCODE(PROVEN_VERSION_MAJOR, PROVEN_VERSION_MINOR, PROVEN_VERSION_PATCH),
            "PROVEN_VERSION_NUM must be the encoding of the three numbers", "");
    }

    PROVEN_TEST_INFO("version.h says {} (short form {})", PROVEN_ARG(full), PROVEN_ARG(v));

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the README carries it, in both languages",
        "A downstream reader pins the number they see at the top of the README.",
        "The README is now split: README.md is English, README-ko.md is Korean. Each must carry the current version, or the two files tell different stories.");
    // ---------------------------------------------------------------
    {
        int en = count_in_file("README.md", full);
        PROVEN_TEST_ASSERT(en >= 1,
            "the full version string must appear in README.md (English)",
            "A version bump missed the English README.");
        int ko = count_in_file("README-ko.md", full);
        PROVEN_TEST_ASSERT(ko >= 1,
            "the full version string must appear in README-ko.md (Korean)",
            "A version bump updated the English README and not the Korean one - which is how the two end up disagreeing.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("TEST.md and the manual headings carry it",
        "These are the documents a reader checks to decide whether what they are reading describes the code they have.",
        "");
    // ---------------------------------------------------------------
    {
        struct { const char *path; const char *what; } docs[] = {
            { "TEST.md",                      "the test matrix" },
            { "manual/manual.md",             "the manual's title" },
            { "manual/manual-freestanding.md","the freestanding guide" },
            { "manual/manual-08-fmt-scan.md", "the fmt/scan chapter" },
        };
        for (size_t i = 0; i < sizeof docs / sizeof docs[0]; ++i) {
            int n = count_in_file(docs[i].path, v);
            PROVEN_TEST_ASSERT(n >= 1,
                "this document must carry the current version",
                "The heading still names an older release, so a reader cannot tell whether it describes the code they have.");
            PROVEN_TEST_INFO("{} : {} occurrence(s) of {}",
                             PROVEN_ARG(docs[i].path), PROVEN_ARG(n), PROVEN_ARG(v));
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("chapter 1 prints the real contents of version.h",
        "It quotes the macros verbatim. A stale excerpt is the manual showing the reader a different library than the one they linked.",
        "");
    // ---------------------------------------------------------------
    {
        int n = count_in_file("manual/manual-01-foundation.md", full);
        PROVEN_TEST_ASSERT(n >= 1,
            "manual chapter 1's version.h excerpt must quote the current PROVEN_VERSION_STRING", "");

        /* The string was gated and the other macros were not, so they drifted: the excerpt once
         * claimed a version number two releases behind version.h. A partly-checked quotation is
         * worse than an unchecked one, because it looks verified. Every numbered line of the
         * excerpt is checked now, in both editions, and chapter 1 says so in prose. */
        struct { const char *name; int value; } nums[] = {
            { "PROVEN_VERSION_MAJOR", PROVEN_VERSION_MAJOR },
            { "PROVEN_VERSION_MINOR", PROVEN_VERSION_MINOR },
            { "PROVEN_VERSION_PATCH", PROVEN_VERSION_PATCH },
        };
        for (size_t i = 0; i < sizeof nums / sizeof nums[0]; ++i) {
            char line[80];
            snprintf(line, sizeof line, "#define %s  %d", nums[i].name, nums[i].value);
            PROVEN_TEST_ASSERT(count_in_file("manual/manual-01-foundation.md", line) >= 1,
                "chapter 1's excerpt must quote the current MAJOR, MINOR and PATCH",
                "version.h changed and the chapter's excerpt did not. Copy the whole excerpt, not one line of it.");
            PROVEN_TEST_ASSERT(count_in_file("manual-ko/manual-01-foundation-ko.md", line) >= 1,
                "the Korean chapter 1 excerpt must quote the same numbers",
                "The English excerpt was updated and the Korean one was not.");
        }
        PROVEN_TEST_ASSERT(count_in_file("manual-ko/manual-01-foundation-ko.md", full) >= 1,
            "the Korean chapter 1 excerpt must quote the current PROVEN_VERSION_STRING", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the CHANGELOG's newest entry is this version",
        "A release with no entry is a release nobody can read the reason for; an entry for a version that is not the current one means the bump and the note came apart.",
        "");
    // ---------------------------------------------------------------
    {
        char *s = read_text_file("CHANGELOG.md");
        PROVEN_TEST_ASSERT(s != NULL, "CHANGELOG.md must be readable", "");
        if (s) {
            /* The newest entry is the first "## [" heading in the file. */
            const char *first = strstr(s, "\n## [");
            PROVEN_TEST_ASSERT(first != NULL, "CHANGELOG.md must have at least one entry", "");
            if (first) {
                const char *eol = strchr(first + 1, '\n');
                size_t len = eol ? (size_t)(eol - first - 1) : strlen(first + 1);
                char heading[256];
                if (len >= sizeof heading) len = sizeof heading - 1;
                memcpy(heading, first + 1, len);
                heading[len] = '\0';

                PROVEN_TEST_INFO("newest CHANGELOG entry: {}", PROVEN_ARG(heading));
                /* Keep-a-Changelog style: `## [x.y.z] - YYYY-MM-DD`. The bracket holds the bare
                 * number, which is the tag without its v. */
                char bracket[64];
                snprintf(bracket, sizeof bracket, "## [%s]", v + 1);
                PROVEN_TEST_ASSERT(strncmp(heading, bracket, strlen(bracket)) == 0,
                    "the newest CHANGELOG entry must be `## [MAJOR.MINOR.PATCH] - date` for the current version",
                    "Either the version was bumped without a changelog entry, or an entry was written for a version that was never bumped to.");
            }
            free(s);
        }
    }

    PROVEN_TEST_PASS("every document names the version the library actually reports.");
    return 0;
}
