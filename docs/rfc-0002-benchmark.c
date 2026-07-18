/*
 * docs/rfc-0002-benchmark.c - the measurements in docs/RFC-0002-view-vocabulary-and-splitting.md
 *
 * This file is NOT part of the library and is not built by ./nob. It exists so that every
 * number in RFC-0002 section 2 can be re-run instead of believed. RFC-0001's measurements
 * were not committed and are now unreproducible; this is the correction.
 *
 * Build and run:
 *
 *   gcc -std=c2x -O2 -D_GNU_SOURCE -Iinclude -Iplatform \
 *       -o /tmp/rfc0002 docs/rfc-0002-benchmark.c src/proven/*.c platform/*.c && /tmp/rfc0002
 *
 * It reproduces three things:
 *   2.1  the natural hand-written split loop is wrong on six of six inputs
 *   2.2  ns/field and allocation counts for four ways of splitting
 *   2.3  a legitimately empty view and an out-of-range view are bit-identical
 *
 * Variant C prototypes the iterator RFC-0002 section 4.1 proposes. It is a local static in
 * this file precisely because the library does not have it yet - that is the proposal.
 */

#include "proven.h"
#include "proven/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 200000
#define FIELDS_PER_RECORD 5

static proven_byte_t *g_corpus;
static proven_size_t  g_corpus_len;

static void build_corpus(void) {
    /* "2021/03/12,alpha,beta,gamma,delta\n" repeated - realistic log/CSV shape */
    static const char rec[] = "2021/03/12,alpha,beta,gamma,delta\n";
    proven_size_t rl = sizeof(rec) - 1;
    g_corpus_len = rl * RECORDS;
    g_corpus = malloc(g_corpus_len);
    for (proven_size_t i = 0; i < RECORDS; ++i) memcpy(g_corpus + i * rl, rec, rl);
}

static uint64_t mix(uint64_t h, proven_u8str_view_t f) {
    for (proven_size_t i = 0; i < f.size; ++i) { h ^= f.ptr[i]; h *= 0x100000001b3ULL; }
    h ^= f.size; h *= 0x100000001b3ULL;
    return h;
}

/* --- counting allocator, to make variant B's allocation count a number, not a guess --- */
static long g_allocs;
static proven_result_mem_mut_t c_alloc(void *ctx, proven_size_t s, proven_size_t a) {
    g_allocs++; return proven_heap_allocator().alloc_fn(ctx, s, a);
}
static proven_result_mem_mut_t c_realloc(void *ctx, void *p, proven_size_t o, proven_size_t n, proven_size_t a) {
    g_allocs++; return proven_heap_allocator().realloc_fn(ctx, p, o, n, a);
}
static void c_free(void *ctx, void *p) { proven_heap_allocator().free_fn(ctx, p); }
static proven_allocator_t counting_allocator(void) {
    proven_allocator_t h = proven_heap_allocator();
    return (proven_allocator_t){ .ctx = h.ctx, .alloc_fn = c_alloc, .realloc_fn = c_realloc, .free_fn = c_free };
}

/* ============ A: hand-rolled with today's API (find + slice) ============ */
static uint64_t run_handrolled(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    proven_u8str_view_t all = { g_corpus, g_corpus_len };
    proven_size_t line_start = 0;
    while (line_start < all.size) {
        proven_size_t nl = proven_u8str_view_find(all, line_start, PROVEN_LIT("\n"));
        if (nl == PROVEN_INDEX_NOT_FOUND) nl = all.size;
        proven_u8str_view_t line = proven_u8str_view_slice(all, line_start, nl - line_start);
        proven_size_t off = 0;
        for (;;) {
            proven_size_t c = proven_u8str_view_find(line, off, PROVEN_LIT(","));
            if (c == PROVEN_INDEX_NOT_FOUND) {
                h = mix(h, proven_u8str_view_slice(line, off, line.size - off));
                break;
            }
            h = mix(h, proven_u8str_view_slice(line, off, c - off));
            off = c + 1;
        }
        line_start = nl + 1;
    }
    return h;
}

/* ============ B: one owned string per field ============ */
static uint64_t run_owned(proven_allocator_t alloc) {
    uint64_t h = 0xcbf29ce484222325ULL;
    proven_u8str_view_t all = { g_corpus, g_corpus_len };
    proven_size_t line_start = 0;
    while (line_start < all.size) {
        proven_size_t nl = proven_u8str_view_find(all, line_start, PROVEN_LIT("\n"));
        if (nl == PROVEN_INDEX_NOT_FOUND) nl = all.size;
        proven_u8str_view_t line = proven_u8str_view_slice(all, line_start, nl - line_start);
        proven_size_t off = 0;
        for (;;) {
            proven_size_t c = proven_u8str_view_find(line, off, PROVEN_LIT(","));
            proven_size_t end = (c == PROVEN_INDEX_NOT_FOUND) ? line.size : c;
            proven_u8str_view_t fv = proven_u8str_view_slice(line, off, end - off);
            proven_result_u8str_t owned = proven_u8str_create_from_view(alloc, fv);
            if (PROVEN_IS_OK(owned.err)) {
                h = mix(h, proven_u8str_as_view(&owned.value));
                proven_u8str_destroy(alloc, &owned.value);
            }
            if (c == PROVEN_INDEX_NOT_FOUND) break;
            off = c + 1;
        }
        line_start = nl + 1;
    }
    return h;
}

/* ============ C: the proposed iterator, prototyped ============ */
typedef struct {
    proven_u8str_view_t rest;
    proven_u8str_view_t sep;
    bool                done;
} split_iter_t;

static split_iter_t split_begin(proven_u8str_view_t src, proven_u8str_view_t sep) {
    return (split_iter_t){ .rest = src, .sep = sep, .done = false };
}
static bool split_next(split_iter_t *it, proven_u8str_view_t *out) {
    if (it->done) return false;
    proven_size_t at = proven_u8str_view_find(it->rest, 0, it->sep);
    if (at == PROVEN_INDEX_NOT_FOUND) {
        *out = it->rest;
        it->done = true;                       /* last field, then stop */
        return true;
    }
    *out = proven_u8str_view_slice(it->rest, 0, at);
    it->rest = proven_u8str_view_slice(it->rest, at + it->sep.size, it->rest.size - at - it->sep.size);
    return true;
}

static uint64_t run_proposed(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    proven_u8str_view_t all = { g_corpus, g_corpus_len };
    split_iter_t lines = split_begin(all, PROVEN_LIT("\n"));
    proven_u8str_view_t line;
    while (split_next(&lines, &line)) {
        if (line.size == 0 && lines.done) break;   /* trailing newline -> empty last field */
        split_iter_t fs = split_begin(line, PROVEN_LIT(","));
        proven_u8str_view_t f;
        while (split_next(&fs, &f)) h = mix(h, f);
    }
    return h;
}

/* ============ D: libc strtok_r (needs a mutable copy) ============ */
static uint64_t run_strtok(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    char *copy = malloc(g_corpus_len + 1);
    memcpy(copy, g_corpus, g_corpus_len);
    copy[g_corpus_len] = 0;
    char *ls = NULL;
    for (char *line = strtok_r(copy, "\n", &ls); line; line = strtok_r(NULL, "\n", &ls)) {
        char *fs = NULL;
        for (char *f = strtok_r(line, ",", &fs); f; f = strtok_r(NULL, ",", &fs)) {
            proven_u8str_view_t v = { (const proven_byte_t *)f, strlen(f) };
            h = mix(h, v);
        }
    }
    free(copy);
    return h;
}


/* ===================== 2.1 the natural loop is wrong ===================== */

static int count_natural(proven_u8str_view_t s, proven_u8str_view_t sep) {
    /* what a competent person writes first: loop while a separator is found */
    int n = 0; proven_size_t off = 0, at;
    while ((at = proven_u8str_view_find(s, off, sep)) != PROVEN_INDEX_NOT_FOUND) {
        (void)proven_u8str_view_slice(s, off, at - off);
        n++; off = at + sep.size;
    }
    return n;   /* the tail after the last separator is never emitted */
}

/* the proposed iterator, counted the same way, must get all six right */
static int count_proposed(proven_u8str_view_t s, proven_u8str_view_t sep) {
    split_iter_t it = split_begin(s, sep);
    proven_u8str_view_t f; int n = 0;
    while (split_next(&it, &f)) n++;
    return n;
}

static int section_2_1(void) {
    struct { const char *in; int expect; } cases[] = {
        { "a,b,c", 3 }, { "a", 1 }, { "a,", 2 }, { ",a", 2 }, { "a,,b", 3 }, { "", 1 },
    };
    int wrong = 0, proposed_wrong = 0;
    printf("--- 2.1 n separators must yield n+1 fields ---\n");
    printf("%-8s %8s %8s %10s %s\n", "input", "correct", "natural", "proposed", "verdict");
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        proven_u8str_view_t v = proven_u8str_view_from_cstr(cases[i].in);
        int nat = count_natural(v, PROVEN_LIT(","));
        int pro = count_proposed(v, PROVEN_LIT(","));
        if (nat != cases[i].expect) wrong++;
        if (pro != cases[i].expect) proposed_wrong++;
        printf("%-8s %8d %8d %10d %s\n", cases[i].in[0] ? cases[i].in : "\"\"",
               cases[i].expect, nat, pro, nat == cases[i].expect ? "ok" : "WRONG");
    }
    printf("natural loop wrong on %d of 6; proposed iterator wrong on %d of 6\n\n",
           wrong, proposed_wrong);
    return proposed_wrong;
}

/* ============ 2.3 an empty view and an invalid view are one value ============ */

static void section_2_3(void) {
    printf("--- 2.3 empty vs out-of-range ---\n");
    proven_u8str_view_t s = proven_u8str_view_from_cstr("a,,b");
    proven_u8str_view_t empty = proven_u8str_view_slice(s, 2, 0);   /* legitimate empty field */
    proven_u8str_view_t oob   = proven_u8str_view_slice(s, 99, 3);  /* out-of-range slice */
    printf("empty field  : ptr=%s size=%zu\n", empty.ptr ? "non-null" : "NULL", (size_t)empty.size);
    printf("out-of-range : ptr=%s size=%zu\n", oob.ptr ? "non-null" : "NULL", (size_t)oob.size);
    printf("bit-identical: %s  => validity cannot terminate an iteration\n\n",
           (empty.ptr == oob.ptr && empty.size == oob.size) ? "yes" : "no");
}

static double ns_now(void) { return (double)proven_time_now(); }

int main(void) {
    int bad = section_2_1();
    section_2_3();
    printf("--- 2.2 throughput ---\n");
    build_corpus();
    proven_allocator_t counting = counting_allocator();
    const proven_size_t nfields = (proven_size_t)RECORDS * FIELDS_PER_RECORD;

    /* warm */
    volatile uint64_t sink = 0;
    sink ^= run_handrolled(); sink ^= run_proposed();

    double t0, t1;
    uint64_t hA, hB, hC, hD;

    t0 = ns_now(); hA = run_handrolled(); t1 = ns_now();
    double nsA = (t1 - t0) / (double)nfields;

    g_allocs = 0;
    t0 = ns_now(); hB = run_owned(counting); t1 = ns_now();
    double nsB = (t1 - t0) / (double)nfields;
    long allocsB = g_allocs;

    t0 = ns_now(); hC = run_proposed(); t1 = ns_now();
    double nsC = (t1 - t0) / (double)nfields;

    t0 = ns_now(); hD = run_strtok(); t1 = ns_now();
    double nsD = (t1 - t0) / (double)nfields;

    printf("corpus: %ld records, %ld fields, %ld bytes\n",
           (long)RECORDS, (long)nfields, (long)g_corpus_len);
    printf("%-24s %10s %14s %12s\n", "variant", "ns/field", "allocations", "checksum");
    printf("%-24s %10.1f %14ld %12llx\n", "A hand-rolled (today)", nsA, 0L, (unsigned long long)hA);
    printf("%-24s %10.1f %14ld %12llx\n", "B owned per field",     nsB, allocsB, (unsigned long long)hB);
    printf("%-24s %10.1f %14ld %12llx\n", "C proposed iterator",   nsC, 0L, (unsigned long long)hC);
    printf("%-24s %10.1f %14s %12llx\n", "D libc strtok_r",        nsD, "1 (copy)", (unsigned long long)hD);
    printf("checksums agree A==C: %s | A==D: %s | A==B: %s\n",
           hA == hC ? "yes" : "NO", hA == hD ? "yes" : "NO", hA == hB ? "yes" : "NO");
    printf("B/A slowdown: %.1fx   B allocations per field: %.2f\n", nsB / nsA, (double)allocsB / (double)nfields);
    (void)sink;
    return bad != 0;
}
