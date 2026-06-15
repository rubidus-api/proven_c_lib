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

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float_parse_bench_result_t bench_parse_ascii(const char *text) {
    proven_parse_double_result_t parsed = proven_parse_double_ascii(proven_u8str_view_from_cstr(text));
    float_parse_bench_result_t out = {
        double_bits(parsed.val),
        parsed.consumed
    };
    return out;
}

static float_parse_bench_result_t bench_parse_wrapper(const char *text) {
    char *end = NULL;
    double value = proven_strtod(text, &end);
    float_parse_bench_result_t out = {
        double_bits(value),
        end != NULL ? (size_t)(end - text) : 0u
    };
    return out;
}

static float_parse_bench_result_t bench_parse_host(const char *text) {
    char *end = NULL;
    double value = strtod(text, &end);
    float_parse_bench_result_t out = {
        double_bits(value),
        end != NULL ? (size_t)(end - text) : 0u
    };
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

static void run_benchmark_row(const char *label, float_parse_bench_fn_t fn,
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
    PROVEN_TEST_INFO("backend={} total_calls={} total_ns={} ns_per_call={} checksum={}",
                     PROVEN_ARG(label),
                     PROVEN_ARG((unsigned long long)total_calls),
                     PROVEN_ARG((long long)elapsed_ns),
                     PROVEN_ARG(ns_per_call),
                     PROVEN_ARG((unsigned long long)checksum));
}

int main(void) {
    static const float_parse_bench_sample_t samples[] = {
        { "3.14", 4u },
        { "1", 1u },
        { "1e10", 4u },
        { "1e-10", 5u },
        { "1e40", 4u },
        { "1e-40", 5u },
        { "9007199254740993", 16u },
        { "1844674407370955161e27", 22u },
        { "5e-324", 6u },
        { "2.4703282292062327e-324", 23u },
        { "2.2250738585072014e-308", 23u },
        { "1.7976931348623157e308", 22u },
        { "123456789012345678901e40", 24u },
        { "-6.3508876286570945e-242", 24u },
    };
    static const size_t rounds = 25000u;
    const size_t sample_count = sizeof(samples) / sizeof(samples[0]);

    PROVEN_TEST_SUITE(
        "test_float_parse_benchmark",
        "Measure the shared float parser and wrapper against host strtod on a fixed representative decimal corpus.",
        "Inspect src/proven/float_parse.c, src/proven/float_decimal.c, and the benchmark corpus if the sanity checks fail or the benchmark output looks implausible."
    );

    PROVEN_TEST_SECTION(
        "sanity corpus",
        "Confirm the benchmark corpus still parses to the same host bits before the timed runs start.",
        "Inspect the corpus literals or parser paths if one of the benchmark samples stops matching host strtod."
    );
    for (size_t i = 0; i < sample_count; ++i) {
        expect_host_match(&samples[i]);
    }

    PROVEN_TEST_SECTION(
        "timed corpus",
        "Measure the shared parser, the strtod-style wrapper, and the host reference on the same valid decimal corpus.",
        "Inspect the benchmark harness, parser fast paths, or host environment if the reported rows look inconsistent."
    );
    PROVEN_TEST_INFO("corpus_size={} rounds={} total_calls={}",
                     PROVEN_ARG((unsigned long long)sample_count),
                     PROVEN_ARG((unsigned long long)rounds),
                     PROVEN_ARG((unsigned long long)((uint64_t)sample_count * (uint64_t)rounds)));
    run_benchmark_row("proven_parse_double_ascii", bench_parse_ascii, samples, sample_count, rounds);
    run_benchmark_row("proven_strtod", bench_parse_wrapper, samples, sample_count, rounds);
    run_benchmark_row("host_strtod", bench_parse_host, samples, sample_count, rounds);

    PROVEN_TEST_PASS("Float parse benchmark completed.");
    return 0;
}
