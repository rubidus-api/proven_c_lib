#include "proven_test.h"
#include "proven/job.h"
#include "proven/heap.h"
#include "proven_sys_thread.h"
#include "proven_bench.h"
#include <stdatomic.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

/*
 * The job system's idle and latency behaviour, measured (docs/BACKLOG.md B-038).
 *
 * Idle workers park on a semaphore instead of spinning. That claim was backed by one five-run
 * probe on one host; this benchmark is the checked-in version, in the row format every benchmark
 * here uses (tests/proven_bench.h), so each number carries its samples and spread:
 *
 *   - idle CPU: process CPU time consumed over a 250 ms idle window, with 1, 2, 8 and 32 workers;
 *   - submit-to-start latency, median and p99, when the pool is IDLE (every worker parked), under
 *     a BURST of 64 submissions, and SATURATED by four producers keeping the queue full;
 *   - saturated multi-producer submission throughput.
 *
 * Timings are this host's. The latency budget they are held to is written down in
 * docs/benchmarks/README.md; a run that misses it is a finding, not a test failure - timing on a
 * shared machine is not a pass/fail signal. The run fails only if a job is lost.
 */

#if defined(_WIN32) || defined(_WIN64)
static double cpu_ns(void) {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a, b;
    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (double)(a.QuadPart + b.QuadPart) * 100.0;
}
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
static double cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) { }
}
#endif

#define ROUNDS 5
#define MAX_LAT 100000

typedef struct {
    double submitted;
    double *slot;              /* where the job writes its latency */
    atomic_int *done;
} lat_job_t;

static void lat_routine(void *arg) {
    lat_job_t *j = arg;
    *j->slot = proven_bench_now_ns() - j->submitted;
    atomic_fetch_add_explicit(j->done, 1, memory_order_release);
}

static atomic_ulong g_executed;
static void noop_routine(void *arg) { (void)arg; atomic_fetch_add_explicit(&g_executed, 1, memory_order_relaxed); }

static proven_job_sys_t *make_pool(proven_size_t workers, proven_size_t cap) {
    proven_job_sys_t *sys = NULL;
    proven_err_t e = proven_job_system_init(proven_heap_allocator(), workers, cap, &sys);
    PROVEN_TEST_ASSERT(proven_is_ok(e) && sys, "job system init", "");
    return sys;
}

static void wait_count(atomic_int *done, int want) {
    while (atomic_load_explicit(done, memory_order_acquire) < want) proven_sys_thread_yield();
}

static double g_lat[MAX_LAT];
static lat_job_t g_jobs[MAX_LAT];

/* Latency when every worker is parked: one job at a time, after a pause long enough to park. */
static void lat_idle(proven_job_sys_t *sys, int n, double *p50, double *p99) {
    atomic_int done = 0;
    for (int i = 0; i < n; ++i) {
        sleep_ms(2);
        g_jobs[i] = (lat_job_t){ proven_bench_now_ns(), &g_lat[i], &done };
        while (!proven_job_submit(sys, lat_routine, &g_jobs[i])) proven_sys_thread_yield();
        wait_count(&done, i + 1);
    }
    *p50 = proven_bench_quantile(g_lat, n, 0.5) / 1000.0;
    *p99 = proven_bench_quantile(g_lat, n, 0.99) / 1000.0;
}

/* Latency of a burst of 64 submissions arriving at a parked pool. */
static void lat_burst(proven_job_sys_t *sys, int bursts, double *p50, double *p99) {
    atomic_int done = 0;
    int k = 0;
    for (int b = 0; b < bursts; ++b) {
        sleep_ms(5);
        for (int i = 0; i < 64; ++i, ++k) {
            g_jobs[k] = (lat_job_t){ proven_bench_now_ns(), &g_lat[k], &done };
            while (!proven_job_submit(sys, lat_routine, &g_jobs[k])) proven_sys_thread_yield();
        }
        wait_count(&done, k);
    }
    *p50 = proven_bench_quantile(g_lat, k, 0.5) / 1000.0;
    *p99 = proven_bench_quantile(g_lat, k, 0.99) / 1000.0;
}

typedef struct { proven_job_sys_t *sys; int n; int base; atomic_int *done; } producer_t;

static void *producer(void *arg) {
    producer_t *p = arg;
    for (int i = 0; i < p->n; ++i) {
        lat_job_t *j = &g_jobs[p->base + i];
        *j = (lat_job_t){ proven_bench_now_ns(), &g_lat[p->base + i], p->done };
        while (!proven_job_submit(p->sys, lat_routine, j)) {
            proven_sys_thread_yield();
            j->submitted = proven_bench_now_ns();   /* latency counts from the accepted submit */
        }
    }
    return NULL;
}

/* Four producers keeping a 256-slot queue full: latency of accepted jobs, and throughput. */
static void saturated(proven_job_sys_t *sys, int per_producer, double *p50, double *p99, double *jobs_per_s) {
    atomic_int done = 0;
    producer_t ps[4];
    proven_sys_thread_t th[4];
    double t0 = proven_bench_now_ns();
    for (int i = 0; i < 4; ++i) {
        ps[i] = (producer_t){ sys, per_producer, i * per_producer, &done };
        th[i] = proven_sys_thread_create(producer, &ps[i]);
    }
    for (int i = 0; i < 4; ++i) proven_sys_thread_join(th[i]);
    wait_count(&done, 4 * per_producer);
    double t1 = proven_bench_now_ns();
    *jobs_per_s = (4.0 * per_producer) / ((t1 - t0) / 1e9);
    *p50 = proven_bench_quantile(g_lat, 4 * per_producer, 0.5) / 1000.0;
    *p99 = proven_bench_quantile(g_lat, 4 * per_producer, 0.99) / 1000.0;
}

int main(void) {
    PROVEN_TEST_SUITE("job system idle cost and latency",
        "Idle CPU for 1, 2, 8 and 32 parked workers; median and p99 submit-to-start latency when idle, under a burst and saturated; saturated four-producer throughput - five rounds each, in the shared benchmark row format.",
        "Timings are reported, not judged: compare against docs/benchmarks/README.md. The run fails only if a submitted job is lost.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("idle CPU", "Process CPU time over 250 ms with every worker parked.", "");
    // ---------------------------------------------------------------
    static const proven_size_t workers[] = { 1, 2, 8, 32 };
    for (size_t w = 0; w < 4; ++w) {
        double s[ROUNDS];
        for (int r = -1; r < ROUNDS; ++r) {             /* r = -1 is the warmup */
            proven_job_sys_t *sys = make_pool(workers[w], 64);
            sleep_ms(50);                               /* let every worker reach its park */
            double c0 = cpu_ns();
            sleep_ms(250);
            double c1 = cpu_ns();
            proven_job_system_destroy(sys);
            if (r >= 0) s[r] = (c1 - c0) / 1e6;
        }
        char name[32];
        snprintf(name, sizeof name, "idle_cpu_w%u", (unsigned)workers[w]);
        proven_bench_row("job", name, "cpu_ms_per_250ms", 1, s, ROUNDS, workers[w]);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("latency and throughput", "8 workers; microseconds from an accepted submit to the job starting.", "");
    /* The wake tail is the OS scheduler's when the host has fewer free CPUs than parked workers
     * plus this spin-waiting thread (B-041), so every run says how many it had. */
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO si; GetSystemInfo(&si);
    printf("[PROVEN][BENCH][HOST] bench=job logical_cpus=%lu\n", (unsigned long)si.dwNumberOfProcessors);
#else
    printf("[PROVEN][BENCH][HOST] bench=job logical_cpus=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
#endif
    // ---------------------------------------------------------------
    double ip50[ROUNDS], ip99[ROUNDS], bp50[ROUNDS], bp99[ROUNDS], sp50[ROUNDS], sp99[ROUNDS], tput[ROUNDS];
    for (int r = -1; r < ROUNDS; ++r) {
        proven_job_sys_t *sys = make_pool(8, 256);
        double a, b, c, d, e, f, g;
        lat_idle(sys, 200, &a, &b);
        lat_burst(sys, 20, &c, &d);
        saturated(sys, 20000, &e, &f, &g);
        proven_job_system_destroy(sys);
        if (r >= 0) { ip50[r] = a; ip99[r] = b; bp50[r] = c; bp99[r] = d; sp50[r] = e; sp99[r] = f; tput[r] = g; }
    }
    proven_bench_row("job", "latency_idle_p50", "us", 1, ip50, ROUNDS, 200);
    proven_bench_row("job", "latency_idle_p99", "us", 1, ip99, ROUNDS, 200);
    proven_bench_row("job", "latency_burst64_p50", "us", 1, bp50, ROUNDS, 20 * 64);
    proven_bench_row("job", "latency_burst64_p99", "us", 1, bp99, ROUNDS, 20 * 64);
    proven_bench_row("job", "latency_saturated_p50", "us", 1, sp50, ROUNDS, 80000);
    proven_bench_row("job", "latency_saturated_p99", "us", 1, sp99, ROUNDS, 80000);
    proven_bench_row("job", "saturated_4producer_throughput", "jobs_per_s", 1, tput, ROUNDS, 80000);

    /* Nothing lost: a no-op flood, counted. */
    atomic_store(&g_executed, 0);
    proven_job_sys_t *sys = make_pool(4, 64);
    for (int i = 0; i < 10000; ++i) while (!proven_job_submit(sys, noop_routine, NULL)) proven_sys_thread_yield();
    proven_job_system_destroy(sys);
    PROVEN_TEST_ASSERT(atomic_load(&g_executed) == 10000, "every submitted job ran", "");

    PROVEN_TEST_PASS("job system idle cost and latency");
    return 0;
}
