#include "proven/fmt.h"
#include "proven/scan.h"
#include "proven_test.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static bool float_roundtrips_f64(double value, const char *text) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(text));
    proven_result_f64_t parsed = proven_scan_f64(&scan);
    return parsed.err == PROVEN_OK && double_bits(parsed.val) == double_bits(value) && scan.cursor == strlen(text);
}

static bool format_fixed_precision(double value, int precision, char *buf, proven_size_t cap) {
    proven_size_t written = 0;
    proven_err_t err = proven_float_format_f64_policy(
        buf,
        cap,
        value,
        PROVEN_FLOAT_FORMAT_POLICY_DEFAULT,
        (proven_float_format_options_t){ .mode = PROVEN_FLOAT_FORMAT_MODE_FIXED, .precision = precision },
        &written
    );
    return err == PROVEN_OK && written == strlen(buf);
}

static bool best_fixed_shortest(double value, char *best, proven_size_t cap) {
    bool found = false;
    proven_size_t best_len = 0;
    for (int precision = 0; precision <= 17; ++precision) {
        char candidate[128];
        if (!format_fixed_precision(value, precision, candidate, sizeof candidate)) {
            continue;
        }
        if (!float_roundtrips_f64(value, candidate)) {
            continue;
        }
        proven_size_t len = strlen(candidate);
        if (!found || len < best_len) {
            if (len + 1u > cap) {
                return false;
            }
            memcpy(best, candidate, len + 1u);
            best_len = len;
            found = true;
        }
    }
    return found;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_shortest_scientific_guard",
        "Verify that the shortest float formatter handles very small finite values by producing a valid shortest candidate instead of dropping into an invalid scientific normalization result.",
        "Inspect src/proven/float_decimal.c and src/proven/float_format.c if the shortest formatter stops handling very small finite inputs or if the shortest candidate stops round-tripping."
    );

    const double values[] = {
        6.3508876286570945e-242,
        -6.3508876286570945e-242,
    };
    for (proven_size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        double value = values[i];
        char expected[128];
        PROVEN_TEST_ASSERT(best_fixed_shortest(value, expected, sizeof expected), "tiny finite value", "Inspect the fixed-precision candidate search if no round-tripping candidate can be found for a tiny finite value.");

        char actual[128];
        proven_size_t written = 0;
        proven_err_t err = proven_float_format_f64_policy(
            actual,
            sizeof actual,
            value,
            PROVEN_FLOAT_FORMAT_POLICY_RYU,
            proven_float_format_options_shortest(),
            &written
        );
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "tiny finite value", "Inspect the scientific normalization path if the shortest formatter rejects a tiny finite value.");
        PROVEN_TEST_ASSERT(strcmp(actual, expected) == 0, "tiny finite value", "Inspect shortest candidate selection if the tiny finite value changes spelling.");
        PROVEN_TEST_ASSERT(written == strlen(expected), "tiny finite value", "Inspect the written-length bookkeeping if the tiny finite value changes spelling.");
        PROVEN_TEST_ASSERT(float_roundtrips_f64(value, actual), "tiny finite value", "Inspect the final shortest formatter output if the tiny finite value does not round-trip.");
    }

    PROVEN_TEST_PASS("Scientific guard checks passed.");
    return 0;
}
