#include "proven/fmt.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the shortest float formatting path in src/proven/float_format.c and keep the round-trip oracle test-only.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static uint64_t double_bits(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

static uint32_t float_bits(float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

static void check_roundtrip_f64(double value) {
    char buf[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(
        buf,
        sizeof buf,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    require(err == PROVEN_OK, "shortest f64 formatting should succeed");
    require(written < sizeof buf, "formatted text should fit in the scratch buffer");

    char *end = NULL;
    double parsed = strtod(buf, &end);
    require(end != NULL && *end == '\0', "host strtod should consume the entire formatted string");
    require(double_bits(parsed) == double_bits(value), "formatted f64 text should round-trip through host strtod");
}

static void check_roundtrip_f32(float value) {
    char buf[128];
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f32_policy(
        buf,
        sizeof buf,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_RYU,
        proven_float_format_options_shortest(),
        &written
    );
    require(err == PROVEN_OK, "shortest f32 formatting should succeed");
    require(written < sizeof buf, "formatted text should fit in the scratch buffer");

    char *end = NULL;
    double parsed = strtod(buf, &end);
    require(end != NULL && *end == '\0', "host strtod should consume the entire formatted string");
    require(float_bits((float)parsed) == float_bits(value), "formatted f32 text should round-trip through host strtod and cast back to float");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_shortest_format_roundtrip",
        "Verify shortest float formatting round-trips through host strtod for representative f64 and f32 values.",
        "Inspect src/proven/float_format.c if the shortest output stops round-tripping, and keep the host strtod oracle limited to tests."
    );

    PROVEN_TEST_SECTION(
        "f64 round-trip coverage",
        "Confirm shortest f64 output round-trips through host strtod for edge and representative values.",
        "Inspect the shortest f64 path if any value stops round-tripping."
    );
    check_roundtrip_f64(0.0);
    check_roundtrip_f64(-0.0);
    check_roundtrip_f64(0.1);
    check_roundtrip_f64(1.0);
    check_roundtrip_f64(1e20);
    check_roundtrip_f64(DBL_MIN);
    check_roundtrip_f64(DBL_MAX);
    check_roundtrip_f64(4.9e-324);

    PROVEN_TEST_SECTION(
        "f32 round-trip coverage",
        "Confirm shortest f32 output round-trips through host strtod and preserves the original float bit pattern after casting back.",
        "Inspect the float32 policy shim if a representative float stops round-tripping."
    );
    check_roundtrip_f32(0.0f);
    check_roundtrip_f32(-0.0f);
    check_roundtrip_f32(0.1f);
    check_roundtrip_f32(1.0f);
    check_roundtrip_f32(1.0e10f);
    check_roundtrip_f32(16777216.0f);

    PROVEN_TEST_PASS("Shortest round-trip checks passed.");
    return 0;
}
