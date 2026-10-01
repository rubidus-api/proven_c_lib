#include "proven.h"
#include "proven_test.h"
#include "proven_bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The float engine against the host C library - speed and accuracy in one run (B-037). docs/float-correctness-and-performance.md section 3 and the README publish these
 * comparisons; the harness that produced them lived outside the repository, so the headline had
 * no checked-in source. This is that source, in the shared row format (tests/proven_bench.h).
 *
 * Corpora, fixed-seed: NORMAL magnitudes (log-uniform in 1e-6..1e6, both signs) and UNIFORM bit
 * patterns (every finite binary64 bit pattern equally likely - extreme exponents dominate).
 * Parsing is timed on four spellings of the normal corpus: %.6g (short, human-sized), the
 * library's shortest round-trip form (~16 digits), %.17g, and %.25g (past the 19 digits a 64-bit
 * mantissa holds, so the bounded long-input path). Formatting is timed as shortest
 * (against the host's %.17g, the nearest thing it has), %f with 6 digits and %e with 16.
 *
 * Accuracy is checked in the same run, and a mismatch FAILS the benchmark - a fast wrong answer
 * is not a result: parsing must give the host's bits, %f and %e the host's bytes, and shortest
 * must round-trip through the host strtod.
 */

#define N 100000
#define ROUNDS 5

static double g_normal[N], g_uniform[N];
static char g_txt[4][N][40];
static unsigned long long g_sink;

static unsigned long long bits_of(double d) { unsigned long long b; memcpy(&b, &d, 8); return b; }

static void build_corpora(void) {
    proven_xoshiro256ss_t g;
    proven_xoshiro256ss_seed(&g, 0x5EED);
    for (int i = 0; i < N; ++i) {
        double u = (double)(proven_xoshiro256ss_next(&g) >> 11) / 9007199254740992.0;   /* [0,1) */
        double v = 1e-6;
        for (double e = u * 12.0; e >= 1.0; e -= 1.0) v *= 10.0;
        double frac = u * 12.0 - (double)(int)(u * 12.0);
        v *= 1.0 + frac * 9.0;
        g_normal[i] = (proven_xoshiro256ss_next(&g) & 1) ? -v : v;
        unsigned long long b;
        do { b = proven_xoshiro256ss_next(&g); } while (((b >> 52) & 0x7ff) == 0x7ff);
        memcpy(&g_uniform[i], &b, 8);
        snprintf(g_txt[0][i], 40, "%.6g", g_normal[i]);
        proven_size_t w = 0;
        proven_err_t e = proven_float_format_f64_policy(g_txt[1][i], 40, g_normal[i], PROVEN_FLOAT_FORMAT_POLICY_RYU,
                                                        proven_float_format_options_shortest(), &w);
        PROVEN_TEST_ASSERT(proven_is_ok(e) && w > 0, "the shortest corpus is built from real shortest output", "");
        snprintf(g_txt[2][i], 40, "%.17g", g_normal[i]);
        snprintf(g_txt[3][i], 40, "%.25g", g_normal[i]);   /* past 19 digits: the long-input path */
    }
}

typedef void (*op_fn)(int i);
static const double *g_vals;
static int g_corpus_txt;
static char g_buf[512];

static void p_proven(int i) { g_sink += bits_of(proven_strtod(g_txt[g_corpus_txt][i], NULL)); }
static void p_host(int i)   { g_sink += bits_of(strtod(g_txt[g_corpus_txt][i], NULL)); }
static proven_size_t g_w;
static proven_err_t g_err;   /* every proven call records its result: the accuracy pass checks it */
/* Shortest output is the RYU policy's (float_format.h); DEFAULT answers it with an error. */
static void f_short_proven(int i) { g_err = proven_float_format_f64_policy(g_buf, sizeof g_buf, g_vals[i], PROVEN_FLOAT_FORMAT_POLICY_RYU, proven_float_format_options_shortest(), &g_w); g_sink += g_w; }
static void f_short_host(int i)   { g_sink += (unsigned)snprintf(g_buf, sizeof g_buf, "%.17g", g_vals[i]); }
static void f_fixed_proven(int i) {
    proven_float_format_options_t o = { PROVEN_FLOAT_FORMAT_MODE_FIXED, 6, true };
    g_err = proven_float_format_f64_policy(g_buf, sizeof g_buf, g_vals[i], PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &g_w);
    g_sink += g_w;
}
static void f_fixed_host(int i) { g_sink += (unsigned)snprintf(g_buf, sizeof g_buf, "%.6f", g_vals[i]); }
static void f_sci_proven(int i) {
    proven_float_format_options_t o = { PROVEN_FLOAT_FORMAT_MODE_SCIENTIFIC, 16, false };
    g_err = proven_float_format_f64_policy(g_buf, sizeof g_buf, g_vals[i], PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &g_w);
    g_sink += g_w;
}
static void f_sci_host(int i) { g_sink += (unsigned)snprintf(g_buf, sizeof g_buf, "%.16e", g_vals[i]); }

/* Time `fn` over the corpus: one warmup pass, ROUNDS samples, ns per call. */
static double timed(const char *casename, op_fn fn) {
    double s[ROUNDS];
    for (int r = -1; r < ROUNDS; ++r) {
        double t0 = proven_bench_now_ns();
        for (int i = 0; i < N; ++i) fn(i);
        double t1 = proven_bench_now_ns();
        if (r >= 0) s[r] = (t1 - t0) / N;
    }
    proven_bench_row("float_host", casename, "ns_per_call", 1, s, ROUNDS, g_sink);
    return s[ROUNDS / 2];   /* sorted by the row: the median */
}

static void ratio(const char *what, double proven, double host) {
    printf("[PROVEN][BENCH][RATIO] bench=float_host case=%s proven_over_host=%.2f\n", what, proven / host);
}

int main(void) {
    PROVEN_TEST_SUITE("float engine against the host C library",
        "Parse and format speed against strtod/snprintf on fixed-seed corpora, with every result checked against the host in the same run: parse bits, %f and %e bytes, shortest round-trip. Any mismatch fails.",
        "A mismatch is a correctness failure in src/proven/float_*.c, not a timing issue - the row format only records speed.");
    build_corpora();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("accuracy against the host", "Every value of every corpus, before any timing.", "");
    // ---------------------------------------------------------------
    unsigned long parse_bad = 0, fixed_bad = 0, sci_bad = 0, rt_bad = 0;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < N; ++i)
            if (bits_of(proven_strtod(g_txt[c][i], NULL)) != bits_of(strtod(g_txt[c][i], NULL))) ++parse_bad;
    char host[512];
    for (int k = 0; k < 2; ++k) {
        g_vals = k ? g_uniform : g_normal;
        for (int i = 0; i < N; ++i) {
            /* The buffer is cleared before each call and the result checked: a call that fails
             * must not be scored by whatever the previous one left behind (the first version of
             * this harness scored exactly that, and reported a shortest formatter that was never
             * called as 70 times faster than the host). */
            g_buf[0] = 0; f_fixed_proven(i); snprintf(host, sizeof host, "%.6f", g_vals[i]);
            if (!proven_is_ok(g_err) || strcmp(g_buf, host) != 0) ++fixed_bad;
            g_buf[0] = 0; f_sci_proven(i); snprintf(host, sizeof host, "%.16e", g_vals[i]);
            if (!proven_is_ok(g_err) || strcmp(g_buf, host) != 0) ++sci_bad;
            g_buf[0] = 0; f_short_proven(i);
            if (!proven_is_ok(g_err) || bits_of(strtod(g_buf, NULL)) != bits_of(g_vals[i])) ++rt_bad;
        }
    }
    PROVEN_TEST_INFO("mismatches: parse {} / {}, %f {} / {}, %e {} / {}, shortest round-trip {} / {}",
                     PROVEN_ARG(parse_bad), PROVEN_ARG(3 * N), PROVEN_ARG(fixed_bad), PROVEN_ARG(2 * N),
                     PROVEN_ARG(sci_bad), PROVEN_ARG(2 * N), PROVEN_ARG(rt_bad), PROVEN_ARG(2 * N));
    PROVEN_TEST_ASSERT(parse_bad == 0 && fixed_bad == 0 && sci_bad == 0 && rt_bad == 0,
        "every result must equal the host's", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("speed", "ns per call, median of five after a warmup pass.", "");
    // ---------------------------------------------------------------
    static const char *parse_names[4] = { "short_6g", "shortest_16dig", "hard_17g", "long_25g" };
    for (int c = 0; c < 4; ++c) {
        g_corpus_txt = c;
        char a[64], b[64];
        snprintf(a, sizeof a, "parse_%s_proven", parse_names[c]);
        snprintf(b, sizeof b, "parse_%s_host", parse_names[c]);
        double p = timed(a, p_proven), h = timed(b, p_host);
        ratio(a, p, h);
    }
    static const char *corp[2] = { "normal", "uniform" };
    for (int k = 0; k < 2; ++k) {
        g_vals = k ? g_uniform : g_normal;
        char a[64], b[64];
        snprintf(a, sizeof a, "format_shortest_%s_proven", corp[k]);
        snprintf(b, sizeof b, "format_shortest_%s_host_17g", corp[k]);
        double p = timed(a, f_short_proven), h = timed(b, f_short_host); ratio(a, p, h);
        snprintf(a, sizeof a, "format_f6_%s_proven", corp[k]);
        snprintf(b, sizeof b, "format_f6_%s_host", corp[k]);
        p = timed(a, f_fixed_proven); h = timed(b, f_fixed_host); ratio(a, p, h);
        snprintf(a, sizeof a, "format_e16_%s_proven", corp[k]);
        snprintf(b, sizeof b, "format_e16_%s_host", corp[k]);
        p = timed(a, f_sci_proven); h = timed(b, f_sci_host); ratio(a, p, h);
    }

    PROVEN_TEST_PASS("float engine against the host C library");
    return 0;
}
