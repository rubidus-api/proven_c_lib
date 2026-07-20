#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * A GATE on the SHAPE of a module section.
 *
 * The symbol checks (test_docs_manual_symbols) prove a module is *mentioned*. They cannot prove
 * it is *documented*, and the difference is where the manual actually failed: the five modules
 * added in the v26.07.13 line each had an intent paragraph and a table, and none of them had a
 * single counter-example, a structure listing, or a reference table saying which call can fail.
 * They passed every check there was and were still half-written.
 *
 * You cannot TDD prose. What you CAN do is state, for each thing a section must contain, a
 * proposition that is either true or false of the text - and then let the build decide. That is
 * what this is: the depth checklist in docs/DOCUMENTING.md §3, turned from advice into a gate.
 *
 * For every module section registered below, the section must contain:
 *
 *   1. INTENT        - prose before the first table. Why this exists, and what goes wrong
 *                      without it. (Checked as: real prose, not just a heading and a table.)
 *   2. REFERENCE     - a table. What each call does, and what it returns.
 *   3. STRUCTURES    - a ```text listing of the structs the caller declares, IF the module has
 *                      any. (Registered per section, because some modules have none.)
 *   4. EXAMPLE       - a runnable one: an `<!-- example: -->` marker, so the build compiles and
 *                      runs it and the chapter cannot drift from it.
 *   5. COUNTER-EXAMPLE - at least one ```text block that shows the WRONG code. This is the one
 *                      that was always missing, and it is the one that teaches: a reader learns
 *                      more from the mistake they were about to make than from the correct call.
 *
 * A section that is exempt from a requirement says so here, in code, with a reason - not by
 * quietly not having it.
 */

typedef struct {
    const char *chapter;      /* file under manual/ */
    const char *heading;      /* the section's exact heading line */
    bool        needs_structs;/* does this module hand the caller a struct to declare? */
    bool        needs_example;/* must it have a runnable example? */
    const char *why_exempt;   /* if it is exempt from something, why - or NULL */
} Section;

static const Section SECTIONS[] = {
    /* Chapter 0 is the on-ramp: it is the one chapter a reader meets before they know anything,
     * so it is the one that must not decay into a reference. Registered when it was written
     * (RFC-0004 phase 1) rather than later, because a new chapter is at its best on the day it
     * lands and every edit after that is a chance to lose the explanation. */
    { "manual-00-start-here.md",            "## 2. Why this library exists",            false, false,
      "the section argues from C's own APIs - strcpy, malloc, printf, qsort - so its code blocks "
      "are the failing C, not proven structs; the first program is section 3." },
    { "manual-00-start-here.md",            "## 5. The five contracts you will meet on every page", false, false,
      "each contract is one paragraph and one counter-example; the compiled program that shows all "
      "five together is ex_00_hello, quoted in section 3." },
    /* Chapter 1 (RFC-0004 phase 3). Before the rewrite it passed 0 of its 9 sections against this
     * gate's own prose floor, while being the first chapter anyone reads. Six of them are
     * registered here; the three that are not are the table of contents, the version excerpt
     * (which is deliberately three lines and a gate of its own), and the examples section, which
     * is code by design. */
    { "manual-01-foundation.md",            "## 1. Errors are values",                  true,  true,  NULL },
    { "manual-01-foundation.md",            "## 2. The types, and why they are spelled differently", true, false,
      "the types are aliases and one result struct; the compiled program that uses them is "
      "ex_01_errors, quoted in section 1." },
    { "manual-01-foundation.md",            "## 3. Memory views: a pointer and a length, together", true, false,
      "views are shown in the worked slicing blocks in section 8; a separate example file would "
      "duplicate them." },
    { "manual-01-foundation.md",            "## 4. Size arithmetic that cannot wrap",   false, false,
      "the checked-arithmetic macros have no struct, and the correct and wrong forms are three "
      "lines each - a whole program would bury them." },
    { "manual-01-foundation.md",            "## 5. Alignment",                          false, false,
      "align.h is two macros and three functions over integers; the arena in chapter 2 is where "
      "alignment does visible work." },
    { "manual-01-foundation.md",            "## 6. Panic: when there is no one left to return an error to", true, false,
      "a panic handler must not return, so an example that ran one would have to end the process." },
    /* Chapter 2 (RFC-0004 phase 3). Reordered so the obvious case comes first: you use a heap,
     * an arena and a pool before being shown the trait they share. */
    { "manual-02-allocation.md",            "## 1. Why allocation is a parameter, and the heap allocator", false, false,
      "the heap allocator is one function returning a value; the compiled programs that use it "
      "are ex_02_arena and ex_02_pool in section 6." },
    { "manual-02-allocation.md",            "## 2. Arena: many things, one lifetime",   true,  false,
      "the arena's worked example is ex_02_arena, quoted in section 6 where the other worked "
      "examples live." },
    { "manual-02-allocation.md",            "## 3. Pool: many things, one size",        true,  false,
      "the pool's worked example is ex_02_pool, quoted in section 6." },
    { "manual-02-allocation.md",            "## 4. The allocator trait",                true,  false,
      "the trait is an interface; every example in this chapter is already using it through "
      "proven_heap_allocator, proven_arena_as_allocator or proven_pool_as_allocator." },
    /* Chapter 3 (RFC-0004 phase 4). Highest jargon density in the manual - "view" appears 288
     * times across manual/ - so this is the chapter where the ownership vocabulary has to be
     * taught rather than assumed. */
    { "manual-03-strings-text.md",          "## 1. U8 strings and views",               true,  false,
      "the owned/borrowed pair has a worked example - ex_03_u8str - quoted in section 5, where "
      "this chapter keeps all of its runnable programs." },
    { "manual-03-strings-text.md",          "## 2. U16 strings and views",              true,  false,
      "u16str is a boundary type for the Windows W APIs; a runnable example would need a Windows "
      "API call, which no other example in the manual makes." },
    { "manual-03-strings-text.md",          "## 3. Formatting",                         true,  false,
      "the formatter's worked examples are ex_08_fmt_scan and ex_08_fmt_custom, quoted in chapter "
      "8, which is the reference half of this material." },
    { "manual-03-strings-text.md",          "## 4. Scanning",                           true,  false,
      "the scanner's worked example is ex_08_scan_recovery, quoted in chapter 8 alongside the "
      "error-code guide it demonstrates." },
    /* Chapter 4 and 5 gap-fill (RFC-0004 phase 5, B-028). These three modules were documented
     * only as tables - the symbols gate was satisfied by a row and nothing checked that the hard
     * part was explained. Each now has motivation, a counter-example and a compiled program. */
    { "manual-04-containers-algorithms.md", "## 2. Intrusive list",                     true,  true,  NULL },
    { "manual-04-containers-algorithms.md", "## 3. Ring buffer",                        true,  true,  NULL },
    { "manual-05-hosted-services.md",       "## 4. Time API",                           true,  true,  NULL },
    { "manual-04-containers-algorithms.md", "## 6. Hashing, by use case",              true,  true,  NULL },
    { "manual-04-containers-algorithms.md", "## 7. Bytes to text: hex and Base64",      false, true,
      "encode.h hands the caller no struct: every call takes a view and a caller-owned buffer." },
    { "manual-05-hosted-services.md",       "## Randomness, by use case",               true,  true,  NULL },
    { "manual-05-hosted-services.md",       "## Streams: writers and readers",          true,  true,  NULL },
    { "manual-05-hosted-services.md",       "## The standard streams",                  true,  false,
      "the standard streams are shown inside the streams example above; a second one would be the same program." },
    { "manual-05-hosted-services.md",       "## Reading a directory one entry at a time", true, false,
      "the worked loop is a compiled fragment; a separate example file would duplicate ex_05_fs_walk." },
    { "manual-05-hosted-services.md",       "## Walking a tree",                        true,  true,  NULL },
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

/* Lowercase copy, for case-insensitive searching. */
static void lower_into(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* Extract [heading .. next "\n## "] from the chapter text. Caller frees. */
static char *section_body(const char *chapter_text, const char *heading) {
    const char *start = strstr(chapter_text, heading);
    if (!start) return NULL;
    const char *end = strstr(start + strlen(heading), "\n## ");
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char *body = (char *)malloc(len + 1);
    if (!body) return NULL;
    memcpy(body, start, len);
    body[len] = '\0';
    return body;
}

/* Does the body contain a ```text block whose contents look like a counter-example?
 * The convention: the prose says "Wrong" and the block that follows is the bad code. We accept
 * either the word in the surrounding prose or a `wrong` comment inside the fence. */
static bool has_counter_example(const char *body_lower) {
    const char *p = body_lower;
    while ((p = strstr(p, "```text")) != NULL) {
        /* Look 200 bytes back for the word "wrong", and inside the block for it too. */
        const char *back = (p - body_lower > 200) ? p - 200 : body_lower;
        const char *end = strstr(p + 7, "```");
        size_t blen = end ? (size_t)(end - p) : strlen(p);

        char before[256];
        size_t n = (size_t)(p - back);
        if (n >= sizeof before) n = sizeof before - 1;
        memcpy(before, back, n);
        before[n] = '\0';

        if (strstr(before, "wrong")) return true;

        char inside[2048];
        size_t m = blen < sizeof inside - 1 ? blen : sizeof inside - 1;
        memcpy(inside, p, m);
        inside[m] = '\0';
        if (strstr(inside, "wrong")) return true;

        p = end ? end + 3 : p + 7;
    }
    return false;
}

/* Is there enough real PROSE - explanation, not tables and not code?
 *
 * The proposition being tested is not "the section opens with a paragraph" (intent is often
 * split around the table that makes the choice for the reader, and that is good writing). It is
 * the one that actually distinguishes a documented module from a listed one:
 *
 *     a section made of a heading, a table and some code explains WHAT the calls are and never
 *     WHY - and why is the half a reader cannot reconstruct from the header file.
 *
 * So: count words in lines that are neither table rows, nor inside a fence, nor headings. */
static int prose_word_count(const char *body) {
    int words = 0;
    bool in_fence = false;

    const char *line = body;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        if (len >= 3 && strncmp(line, "```", 3) == 0) {
            in_fence = !in_fence;
        } else if (!in_fence && len > 0 && line[0] != '|' && line[0] != '#') {
            bool in_word = false;
            for (size_t i = 0; i < len; ++i) {
                if (isalpha((unsigned char)line[i])) {
                    if (!in_word) { ++words; in_word = true; }
                } else {
                    in_word = false;
                }
            }
        }

        if (!eol) break;
        line = eol + 1;
    }
    return words;
}

int main(void) {
    PROVEN_TEST_SUITE("every module section is documented to depth, not merely mentioned",
        "A section must carry its intent, a reference table, the structures the caller declares, a runnable example, and at least one COUNTER-EXAMPLE. This is docs/DOCUMENTING.md §3 turned from advice into a gate.",
        "You cannot TDD prose - but you can state, for each thing a section must contain, a proposition that is either true or false of the text, and let the build decide. The five modules added this cycle passed every check there was and were still half-written: not one had a counter-example.");

    for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; ++i) {
        const Section *sec = &SECTIONS[i];

        char path[256];
        snprintf(path, sizeof path, "manual/%s", sec->chapter);
        char *text = read_text_file(path);
        PROVEN_TEST_ASSERT(text != NULL, "the chapter must be readable (run from the repo root)", "");
        if (!text) continue;

        char *body = section_body(text, sec->heading);
        PROVEN_TEST_ASSERT(body != NULL,
            "the section named in this test must exist in the chapter",
            "A heading was renamed without updating this gate. Rename it here too - the gate is the register of what must stay documented.");
        if (!body) { free(text); continue; }

        char *lower = (char *)malloc(strlen(body) + 1);
        if (lower) lower_into(lower, body, strlen(body) + 1);

        PROVEN_TEST_INFO("checking section: {}", PROVEN_ARG(sec->heading));

        /* 1. INTENT - the section must EXPLAIN, not merely list. */
        {
            int words = prose_word_count(body);
            PROVEN_TEST_INFO("  prose words (outside tables and code): {}", PROVEN_ARG(words));
            PROVEN_TEST_ASSERT(words >= 150,
                "the section must carry real prose: why this exists, and what goes wrong without it",
                "A section made of a heading, a table and some code tells a reader WHAT the calls are and never WHY - and why is the half they cannot reconstruct from the header file.");
        }

        /* 2. REFERENCE TABLE */
        PROVEN_TEST_ASSERT(strstr(body, "\n| ") != NULL,
            "the section must carry a reference table",
            "What each call does, what it returns, and - the part that matters - which one can FAIL and what it leaves behind when it does.");

        /* 3. STRUCTURES */
        if (sec->needs_structs) {
            PROVEN_TEST_ASSERT(strstr(body, "```text") != NULL && lower && strstr(lower, "typedef"),
                "the section must list the structures the caller declares",
                "A caller who declares a struct needs to know the role of each field - and, for caller-owned state, that it must not be copied.");
        } else {
            PROVEN_TEST_INFO("  (exempt from structures: {})", PROVEN_ARG(sec->why_exempt));
        }

        /* 4. RUNNABLE EXAMPLE */
        if (sec->needs_example) {
            PROVEN_TEST_ASSERT(strstr(body, "<!-- example: manual/examples/") != NULL,
                "the section must quote a runnable example",
                "An example the build does not compile and run is an example that will stop being true and nothing will say so.");
        } else {
            PROVEN_TEST_INFO("  (exempt from a separate example: {})", PROVEN_ARG(sec->why_exempt));
        }

        /* 5. COUNTER-EXAMPLE - the one that was always missing */
        PROVEN_TEST_ASSERT(lower && has_counter_example(lower),
            "the section must show at least one WRONG way to use it",
            "This is the requirement the new modules all failed. A reader learns more from the mistake they were about to make than from the correct call - so show the code they would actually write, fenced as ```text so it is never compiled, and then the correct form as ```c so the build checks it.");

        free(lower);
        free(body);
        free(text);
    }

    PROVEN_TEST_PASS("every registered module section carries its intent, reference, structures, example and counter-example.");
    return 0;
}
