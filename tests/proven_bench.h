#ifndef PROVEN_BENCH_H
#define PROVEN_BENCH_H

/*
 * One format for every benchmark row (docs/BACKLOG.md B-037).
 *
 * A single timing on one host cannot tell an improvement from noise, so every number this
 * repository publishes comes from a row that carries its own evidence: the samples it was taken
 * from, their median and spread, the warmup that preceded them, a checksum proving the timed work
 * was the same work, and the compiler and profile that built it. One line, key=value, so a script
 * can compare two runs:
 *
 *   [PROVEN][BENCH][ROW] bench=... case=... unit=... warmup=1 samples=5 median=... min=... max=...
 *                        spread=... raw=a;b;c;d;e checksum=... compiler=... profile=...
 *
 * spread is (max - min) / median. The clock is monotonic (CLOCK_MONOTONIC, or
 * QueryPerformanceCounter on Windows) - proven_time_now() is wall-clock time and can step.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define PROVEN_BENCH_MAX_SAMPLES 64

static inline double proven_bench_now_ns(void) {
#if defined(_WIN32) || defined(_WIN64)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#endif
}

static inline const char *proven_bench_compiler(void) {
#if defined(__clang__)
    return "clang-" __clang_version__;
#elif defined(__GNUC__)
    return "gcc-" __VERSION__;
#else
    return "unknown";
#endif
}

static inline const char *proven_bench_profile(void) {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return "sanitizer";
#elif defined(__OPTIMIZE__) && defined(NDEBUG)
    return "optimized-ndebug";
#elif defined(__OPTIMIZE__)
    return "optimized";
#else
    return "O0";
#endif
}

static inline void proven_bench_sort(double *v, int n) {
    for (int i = 1; i < n; ++i) {
        double x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; }
        v[j + 1] = x;
    }
}

/* The value at quantile q (0..1) of n samples, by nearest rank. v is sorted in place. */
static inline double proven_bench_quantile(double *v, int n, double q) {
    proven_bench_sort(v, n);
    int k = (int)(q * (double)(n - 1) + 0.5);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return v[k];
}

/* Print one row. `samples` are left sorted. Spaces in the compiler string become '_'. */
static inline void proven_bench_row(const char *bench, const char *casename, const char *unit, int warmup,
                                    double *samples, int n, unsigned long long checksum) {
    double raw[PROVEN_BENCH_MAX_SAMPLES];
    if (n > PROVEN_BENCH_MAX_SAMPLES) n = PROVEN_BENCH_MAX_SAMPLES;
    memcpy(raw, samples, (size_t)n * sizeof raw[0]);
    proven_bench_sort(samples, n);
    double med = samples[n / 2], lo = samples[0], hi = samples[n - 1];
    char comp[128];
    snprintf(comp, sizeof comp, "%s", proven_bench_compiler());
    for (char *p = comp; *p; ++p) if (*p == ' ') *p = '_';
    printf("[PROVEN][BENCH][ROW] bench=%s case=%s unit=%s warmup=%d samples=%d median=%.6g min=%.6g max=%.6g "
           "spread=%.3f raw=", bench, casename, unit, warmup, n, med, lo, hi, med > 0 ? (hi - lo) / med : 0.0);
    for (int i = 0; i < n; ++i) printf("%s%.6g", i ? ";" : "", raw[i]);
    printf(" checksum=%llx compiler=%s profile=%s\n", checksum, comp, proven_bench_profile());
    fflush(stdout);
}

#endif /* PROVEN_BENCH_H */
