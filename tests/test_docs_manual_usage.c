#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/*
 * Being NAMED in the manual is not the same as being SHOWN.
 *
 * tests/test_docs_manual_symbols already requires that every public function appears somewhere
 * in manual/. That gate is satisfied by a single reference-table row - "proven_fs_pwrite(file,
 * src, offset) | write at an absolute offset | proven_result_size_t" - which tells a reader the
 * spelling of the call and nothing about when to reach for it, what the arguments have to be
 * true of, or what the failure means. Half the public API was documented exactly that way: 86
 * of 279 functions had a row in a table and no line of code anywhere that used them.
 *
 * A reader does not learn an API from a table. They learn it from a program that uses it for
 * something, next to the sentence explaining why. So this gate asks the stronger question:
 *
 *     Is every public function USED in code the build compiles and runs?
 *
 * "Used" means the name appears in a manual/examples C program - every one of which is
 * compiled and executed by ./nob - or in a ```c block in the chapters, all of which are
 * compiled by check_manual_code_blocks in nob.c. A ```text block does not count: those are the
 * signature listings and the deliberate counter-examples, and neither is a demonstration.
 *
 * Macros count for the function they expand to. PROVEN_ARRAY_PUSH is how a caller is meant to
 * write proven_array_push, and an example that uses the wrapper has demonstrated the function
 * underneath it. So this reads the function-like macro definitions out of the public headers
 * and follows them: a use of PROVEN_ARRAY_PUSH is a use of proven_array_push.
 *
 * A `_Generic` dispatch macro is the exception, and deliberately so. PROVEN_ARG names every
 * argument constructor there is, so counting a single use of it as a use of all of them would
 * mark proven_arg_ptr, proven_arg_datetime and five others as demonstrated by an example that
 * formats one integer. A dispatch table is not a wrapper: it lists alternatives rather than
 * calling them, so its body is not followed.
 *
 * The PAL (platform/proven_sys_ headers) is exempt, for the same reason it is exempt from the
 * naming gate: it is the porting layer, not the API a caller programs against.
 */

#define MAX_SYMS 1600
#define MAX_NAME 96
#define MAX_BODY 8192

static char g_fn[MAX_SYMS][MAX_NAME];        /* public functions that are not macros */
static int  g_fn_n = 0;

static char g_macro[MAX_SYMS][MAX_NAME];     /* function-like macros declared in public headers */
static char g_macro_body[MAX_SYMS][MAX_BODY];
static int  g_macro_n = 0;

static char g_used[MAX_SYMS][MAX_NAME];      /* every proven_/PROVEN_ name that appears in code */
static int  g_used_n = 0;

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

static void add_used(const char *name) {
    if (!have(g_used, g_used_n, name) && g_used_n < MAX_SYMS) {
        snprintf(g_used[g_used_n++], MAX_NAME, "%s", name);
    }
}

/* Every proven_* / PROVEN_* identifier in a stretch of code. */
static void scan_identifiers(const char *text, void (*sink)(const char *)) {
    for (const char *p = text; *p; ) {
        if ((strncmp(p, "proven_", 7) == 0 || strncmp(p, "PROVEN_", 7) == 0) &&
            (p == text || !ident_char(p[-1]))) {
            const char *q = p;
            while (ident_char(*q)) ++q;
            if ((size_t)(q - p) < MAX_NAME) {
                char name[MAX_NAME];
                memcpy(name, p, (size_t)(q - p));
                name[q - p] = '\0';
                sink(name);
            }
            p = q;
        } else {
            ++p;
        }
    }
}

/* The public API: every `proven_x(` a header declares, minus the ones that are macros, plus a
 * record of what each macro expands to. */
static void collect_headers(void) {
    DIR *d = opendir("include/proven");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 3 || strcmp(n + len - 2, ".h") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "include/proven/%s", n);
        char *s = read_text_file(path);
        if (!s) continue;

        /* Pass 1: function-like macro definitions and their bodies. A body runs to the end of
         * the line, continuing while the line ends in a backslash. */
        for (char *p = s; (p = strstr(p, "#define ")) != NULL; ) {
            char *name = p + 8;
            while (*name == ' ' || *name == '\t') ++name;
            if (strncmp(name, "proven_", 7) != 0 && strncmp(name, "PROVEN_", 7) != 0) { p = name; continue; }
            char *q = name;
            while (ident_char(*q)) ++q;
            if (*q != '(') { p = q; continue; }               /* object-like macro: not a call */
            if ((size_t)(q - name) >= MAX_NAME) { p = q; continue; }

            char *body = strchr(q, ')');
            if (!body) { p = q; continue; }
            ++body;

            char *end = body;
            for (;;) {
                char *nl = strchr(end, '\n');
                if (!nl) { end = end + strlen(end); break; }
                if (nl > body && nl[-1] == '\\') { end = nl + 1; continue; }
                end = nl;
                break;
            }

            if (g_macro_n < MAX_SYMS) {
                size_t blen = (size_t)(end - body);
                if (blen >= MAX_BODY) blen = MAX_BODY - 1;
                memcpy(g_macro_body[g_macro_n], body, blen);
                g_macro_body[g_macro_n][blen] = '\0';
                memcpy(g_macro[g_macro_n], name, (size_t)(q - name));
                g_macro[g_macro_n][q - name] = '\0';
                ++g_macro_n;
            }
            p = end;
        }

        /* Pass 2: every declared `proven_x(`. */
        for (char *p = s; (p = strstr(p, "proven_")) != NULL; ) {
            if (p != s && ident_char(p[-1])) { ++p; continue; }
            char *q = p;
            while (ident_char(*q)) ++q;
            if (*q == '(' && (size_t)(q - p) < MAX_NAME) {
                char name[MAX_NAME];
                memcpy(name, p, (size_t)(q - p));
                name[q - p] = '\0';
                if (!have(g_macro, g_macro_n, name) && !have(g_fn, g_fn_n, name) && g_fn_n < MAX_SYMS) {
                    snprintf(g_fn[g_fn_n++], MAX_NAME, "%s", name);
                }
            }
            p = q;
        }
        free(s);
    }
    closedir(d);
}

/* Everything the examples use. */
static void collect_example_uses(void) {
    DIR *d = opendir("manual/examples");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 3 || strcmp(n + len - 2, ".c") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "manual/examples/%s", n);
        char *s = read_text_file(path);
        if (!s) continue;
        scan_identifiers(s, add_used);
        free(s);
    }
    closedir(d);
}

/* Everything the chapters use in a ```c block. A ```text block is a listing or a deliberate
 * counter-example, and neither demonstrates anything, so those are skipped. */
static void collect_chapter_uses(void) {
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

        bool in_c = false;
        for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
            if (strncmp(line, "```", 3) == 0) {
                if (in_c) {
                    in_c = false;
                } else {
                    const char *lang = line + 3;
                    in_c = (strcmp(lang, "c") == 0 || strcmp(lang, "C") == 0);
                }
                continue;
            }
            if (in_c) scan_identifiers(line, add_used);
        }
        free(s);
    }
    closedir(d);
}

/* A use of a macro is a use of everything the macro expands to. Repeated until it settles,
 * because a macro may be written in terms of another one. */
static void expand_macro_uses(void) {
    for (int round = 0; round < 8; ++round) {
        int before = g_used_n;
        for (int i = 0; i < g_macro_n; ++i) {
            if (strstr(g_macro_body[i], "_Generic") != NULL) continue;   /* a table, not a wrapper */
            if (have(g_used, g_used_n, g_macro[i])) {
                scan_identifiers(g_macro_body[i], add_used);
            }
        }
        if (g_used_n == before) return;
    }
}

int main(void) {
    PROVEN_TEST_SUITE("every public function is shown in working code",
        "A reference-table row tells a reader the spelling of a call and nothing about when to use it. This gate requires that every public function appears in a program the build compiles and runs, or in a compiled ```c block - not merely that it is named somewhere.",
        "Run this from the repository root. A failure names a function nothing demonstrates: add it to an existing example where it belongs, or write one, and explain in the chapter why a reader would reach for it.");

    collect_headers();
    collect_example_uses();
    collect_chapter_uses();
    expand_macro_uses();

    PROVEN_TEST_ASSERT(g_fn_n > 200,
        "the header scan must actually find the public API",
        "A near-empty scan means this test is being run from the wrong directory and is checking nothing.");
    PROVEN_TEST_ASSERT(g_used_n > 100,
        "the example and chapter scan must actually find used symbols",
        "A near-empty scan means the manual or its examples were not read, and this gate is checking nothing.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("every public function appears in code that is compiled and run",
        "86 of 279 public functions once had a table row and no demonstration anywhere - including proven_fs_sync_dir, without which an atomic file replacement is not actually durable.",
        "Named below. Put the call in a manual/examples program (compiled and run) or a ```c block (compiled), next to the sentence saying when a reader would want it.");
    // ---------------------------------------------------------------
    {
        int undemonstrated = 0;
        for (int i = 0; i < g_fn_n; ++i) {
            if (!have(g_used, g_used_n, g_fn[i])) {
                PROVEN_TEST_INFO("nothing demonstrates {}()", PROVEN_ARG(g_fn[i]));
                ++undemonstrated;
            }
        }
        PROVEN_TEST_ASSERT(undemonstrated == 0,
            "every public function must be used in a runnable example or a compiled c block",
            "Named above. A macro wrapper counts for the function it expands to; a ```text block does not count, because it is not compiled.");
    }

    PROVEN_TEST_PASS("every public function is demonstrated in code the build checks.");
    return 0;
}
