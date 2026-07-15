#include "proven/float_parse.h"
#include "proven/time.h"
#include "proven_test.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t len;
} float_parse_bench_sample_t;

typedef struct {
    uint64_t bits;
    size_t consumed;
} float_parse_bench_result_t;

typedef float_parse_bench_result_t (*float_parse_bench_fn_t)(const char *text);

typedef struct {
    const char *name;
    const float_parse_bench_sample_t *samples;
    size_t sample_count;
} float_parse_bench_group_t;

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float_parse_bench_result_t bench_parse_ascii(const char *text) {
    proven_parse_double_result_t parsed = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));
    float_parse_bench_result_t out = { double_bits(parsed.val), parsed.consumed };
    return out;
}

static float_parse_bench_result_t bench_parse_wrapper(const char *text) {
    char *end = NULL;
    double value = proven_strtod(text, &end);
    float_parse_bench_result_t out = { double_bits(value), end != NULL ? (size_t)(end - text) : 0u };
    return out;
}

static float_parse_bench_result_t bench_parse_host(const char *text) {
    char *end = NULL;
    double value = strtod(text, &end);
    float_parse_bench_result_t out = { double_bits(value), end != NULL ? (size_t)(end - text) : 0u };
    return out;
}

static void expect_host_match(const float_parse_bench_sample_t *sample) {
    char *host_end = NULL;
    double host = strtod(sample->text, &host_end);
    proven_parse_double_result_t ascii = proven_parse_double_ascii(proven_u8str_view_from_cstr(sample->text));
    char *wrapper_end = NULL;
    double wrapped = proven_strtod(sample->text, &wrapper_end);

    PROVEN_TEST_ASSERT(host_end != NULL && (size_t)(host_end - sample->text) == sample->len, sample->text, "Inspect the benchmark corpus if host strtod stops at a different byte.");
    PROVEN_TEST_ASSERT(ascii.err == PROVEN_OK, sample->text, "Inspect the ASCII parser if a benchmark sample stops parsing.");
    PROVEN_TEST_ASSERT(ascii.consumed == sample->len, sample->text, "Inspect consumed-length bookkeeping if the ASCII parser stops before the sample end.");
    PROVEN_TEST_ASSERT(double_bits(ascii.val) == double_bits(host), sample->text, "Inspect decimal rounding if the benchmark sample drifts from the host oracle.");
    PROVEN_TEST_ASSERT(double_bits(wrapped) == double_bits(host), sample->text, "Inspect the wrapper path if it stops matching the host oracle bits.");
    PROVEN_TEST_ASSERT(wrapper_end != NULL && (size_t)(wrapper_end - sample->text) == sample->len, sample->text, "Inspect wrapper endptr bookkeeping if the benchmark sample stops at the wrong byte.");
}

static uint64_t mix_checksum(uint64_t acc, uint64_t value) {
    acc ^= value + 0x9e3779b97f4a7c15ULL + (acc << 6) + (acc >> 2);
    return acc;
}

static void run_benchmark_row(const char *group_name, const char *label, float_parse_bench_fn_t fn,
                              const float_parse_bench_sample_t *samples, size_t sample_count,
                              size_t rounds) {
    uint64_t checksum = 0xcbf29ce484222325ULL;
    proven_time_t start = proven_time_now();

    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < sample_count; ++i) {
            float_parse_bench_result_t res = fn(samples[i].text);
            checksum = mix_checksum(checksum, res.bits);
            checksum = mix_checksum(checksum, (uint64_t)res.consumed);
        }
    }

    proven_time_t end = proven_time_now();
    proven_i64 elapsed_ns = end - start;
    if (elapsed_ns <= 0) {
        elapsed_ns = 1;
    }

    uint64_t total_calls = (uint64_t)sample_count * (uint64_t)rounds;
    double ns_per_call = (double)elapsed_ns / (double)total_calls;
    PROVEN_TEST_INFO("group={} backend={} total_calls={} total_ns={} ns_per_call={} checksum={}",
                     PROVEN_ARG(group_name),
                     PROVEN_ARG(label),
                     PROVEN_ARG((unsigned long long)total_calls),
                     PROVEN_ARG((long long)elapsed_ns),
                     PROVEN_ARG(ns_per_call),
                     PROVEN_ARG((unsigned long long)checksum));
}

static void run_group(const float_parse_bench_group_t *group, size_t rounds) {
    PROVEN_TEST_SECTION(
        group->name,
        "Measure the shared float parser, wrapper, and host reference on one path-specific corpus.",
        "Inspect the corpus mix or the path-specific parser fast path if the measured rows look implausible."
    );

    for (size_t i = 0; i < group->sample_count; ++i) {
        expect_host_match(&group->samples[i]);
    }

    PROVEN_TEST_INFO("group_size={} rounds={} total_calls={}",
                     PROVEN_ARG((unsigned long long)group->sample_count),
                     PROVEN_ARG((unsigned long long)rounds),
                     PROVEN_ARG((unsigned long long)((uint64_t)group->sample_count * (uint64_t)rounds)));
    run_benchmark_row(group->name, "proven_parse_double_ascii", bench_parse_ascii, group->samples, group->sample_count, rounds);
    run_benchmark_row(group->name, "proven_strtod", bench_parse_wrapper, group->samples, group->sample_count, rounds);
    run_benchmark_row(group->name, "host_strtod", bench_parse_host, group->samples, group->sample_count, rounds);
}

int main(void) {
    static const float_parse_bench_sample_t short_exact_samples[] = {
        { "0", 1u },
        { "1", 1u },
        { "3.14", 4u },
        { "0.1", 3u },
        { "1.5", 3u },
        { "12.5", 4u },
        { "9999.5", 6u },
        { "0.0001", 6u },
    };
    static const float_parse_bench_sample_t staged_scientific_samples[] = {
        { "1e10", 4u },
        { "1e40", 4u },
        { "1e-10", 5u },
        { "1e-40", 5u },
        { "1844674407370955161e27", 22u },
        { "11920928955078125e-23", 21u },
        { "9007199254740993e-1", 19u },
        { "1e100", 5u },
    };
    static const float_parse_bench_sample_t fallback_samples[] = {
        { "123456789012345678901e40", 24u },
        { "-6.3508876286570945e-242", 24u },
        { "2.4703282292062327e-324", 23u },
        { "2.2250738585072014e-308", 23u },
        { "1.7976931348623157e308", 22u },
        { "4.9406564584124654e-324", 23u },
    };
    static const float_parse_bench_sample_t boundary_samples[] = {
        { "9007199254740991", 16u },
        { "9007199254740992", 16u },
        { "9007199254740993", 16u },
        { "2.2250738585072015e-308", 23u },
        { "2.4703282292062328e-324", 23u },
        { "5e-324", 6u },
        { "2.4703282292062327e-324", 23u },
    };
    static const size_t rounds = 20000u;

    static const float_parse_bench_group_t groups[] = {
        { "short_exact", short_exact_samples, sizeof(short_exact_samples) / sizeof(short_exact_samples[0]) },
        { "staged_scientific", staged_scientific_samples, sizeof(staged_scientific_samples) / sizeof(staged_scientific_samples[0]) },
        { "fallback", fallback_samples, sizeof(fallback_samples) / sizeof(fallback_samples[0]) },
        { "boundary_tie", boundary_samples, sizeof(boundary_samples) / sizeof(boundary_samples[0]) },
    };

    PROVEN_TEST_SUITE(
        "test_bench_float_parse_paths",
        "Measure the float parse paths as separate corpora so the fast path, staged path, fallback, and boundary work can be compared independently.",
        "Inspect src/proven/float_parse.c and src/proven/float_decimal.c if a path-specific corpus starts producing implausible timing or checksum output."
    );

    PROVEN_TEST_SECTION(
        "path split benchmark",
        "Confirm each path-specific corpus still matches host strtod before the timed rows are captured.",
        "Inspect the relevant corpus group or parser path if a path-specific sample stops matching host strtod."
    );
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        run_group(&groups[i], rounds);
    }

    PROVEN_TEST_PASS("Float parse path benchmark completed.");
    return 0;
}
