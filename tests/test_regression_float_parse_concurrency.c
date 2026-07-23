#include "proven_test.h"
#include "proven/float_parse.h"
#include "proven_sys_thread.h"
#include <stdint.h>
#include <string.h>

enum {
    FLOAT_PARSE_THREAD_COUNT = 8,
    FLOAT_PARSE_ITERATIONS = 2000
};

typedef struct {
    const char *text;
    proven_size_t text_size;
    uint64_t expected_bits;
} float_parse_case_t;

typedef struct {
    unsigned int failures;
} float_parse_thread_ctx_t;

static const float_parse_case_t float_parse_cases[] = {
    /* Clinger fast path. */
    { "3.14", sizeof("3.14") - 1u, UINT64_C(0x40091eb851eb851f) },
    /* Eisel-Lemire fast path. */
    { "1844674407370955161e27", sizeof("1844674407370955161e27") - 1u, UINT64_C(0x4954adf4b7320335) },
    /* Smallest positive subnormal. */
    { "5e-324", sizeof("5e-324") - 1u, UINT64_C(0x0000000000000001) },
    /* Long-significand exact fallback. */
    { "123456789012345678901e40", sizeof("123456789012345678901e40") - 1u, UINT64_C(0x4c6895b2461deb26) },
};

static uint64_t float_parse_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void *float_parse_worker(void *arg) {
    float_parse_thread_ctx_t *ctx = (float_parse_thread_ctx_t *)arg;
    const proven_size_t case_count = sizeof float_parse_cases / sizeof float_parse_cases[0];

    for (unsigned int iteration = 0; iteration < FLOAT_PARSE_ITERATIONS; ++iteration) {
        for (proven_size_t case_index = 0; case_index < case_count; ++case_index) {
            const float_parse_case_t *test_case = &float_parse_cases[case_index];
            proven_u8str_view_t input = {
                .ptr = (const proven_byte_t *)test_case->text,
                .size = test_case->text_size,
            };
            proven_parse_double_result_t parsed = proven_parse_double_ascii(input);

            if (parsed.err != PROVEN_OK ||
                parsed.consumed != test_case->text_size ||
                float_parse_bits(parsed.val) != test_case->expected_bits) {
                ++ctx->failures;
            }
        }

        if ((iteration & 31u) == 0u) {
            proven_sys_thread_yield();
        }
    }

    return NULL;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_regression_float_parse_concurrency",
        "Parse representative Clinger, Eisel-Lemire, subnormal, and exact-fallback decimals concurrently through the public ASCII parser.",
        "Run under TSAN and inspect src/proven/float_decimal.c for shared writable parser state if concurrent results race or drift."
    );

    PROVEN_TEST_SECTION(
        "independent concurrent parses",
        "Launch multiple PAL threads whose only writable state is their caller-owned result context, then verify every parsed binary64 bit pattern.",
        "Inspect decimal conversion path instrumentation and scratch storage if an otherwise stable parse becomes thread-dependent."
    );

    float_parse_thread_ctx_t contexts[FLOAT_PARSE_THREAD_COUNT] = {0};
    proven_sys_thread_t threads[FLOAT_PARSE_THREAD_COUNT] = {0};
    unsigned int created = 0;

    for (; created < FLOAT_PARSE_THREAD_COUNT; ++created) {
        threads[created] = proven_sys_thread_create(float_parse_worker, &contexts[created]);
        if (threads[created].internal == NULL) {
            break;
        }
    }

    for (unsigned int thread_index = 0; thread_index < created; ++thread_index) {
        proven_sys_thread_join(threads[thread_index]);
    }

    PROVEN_TEST_ASSERT(
        created == FLOAT_PARSE_THREAD_COUNT,
        "all parser worker threads should start",
        "Inspect the thread PAL or host resource limits if a worker cannot be created."
    );

    for (unsigned int thread_index = 0; thread_index < FLOAT_PARSE_THREAD_COUNT; ++thread_index) {
        PROVEN_TEST_ASSERT(
            contexts[thread_index].failures == 0u,
            "every concurrent parse should return the expected consumed length and binary64 bits",
            "Run this test under TSAN and inspect decimal path state if a concurrent parse fails or races."
        );
    }

    PROVEN_TEST_PASS("concurrent public float parsing remained deterministic.");
    return 0;
}
