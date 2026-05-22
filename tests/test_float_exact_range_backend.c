#include "proven/scan.h"
#include "proven_test.h"
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits(double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void expect_scan_bits(const char *label, const char *input, double expected) {
    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr(input));
    proven_result_f64_t res = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(res.err == PROVEN_OK, label, "Inspect the exact-range decimal backend if a representative spelling stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(res.val) == double_bits(expected), label, "Inspect decimal-to-double rounding if the parsed bits drift from the documented exact-range corpus.");
    PROVEN_TEST_ASSERT(scan.cursor == strlen(input), label, "Inspect cursor advancement if the exact-range backend stops consuming the full token.");
}

static void require_text_missing(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) == NULL, label, "Inspect src/proven/scan.c if the host-dependent float fallback reappears.");
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect src/proven/float_format.c if the staged shortest-policy helper is missing.");
}

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect the repository path if the exact-range backend source file cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the source file if seeking the end fails.");
    long n = ftell(f);
    PROVEN_TEST_ASSERT(n >= 0, path, "Inspect the source file if its size cannot be determined.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the source file if rewinding fails.");
    char *buf = (char*)malloc((size_t)n + 1u);
    PROVEN_TEST_ASSERT(buf != NULL, path, "Inspect the test host if the temporary buffer cannot be allocated.");
    size_t got = fread(buf, 1u, (size_t)n, f);
    PROVEN_TEST_ASSERT(got == (size_t)n, path, "Inspect the source file if the test cannot read its full contents.");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_exact_range_backend",
        "Verify that the decimal-to-double path stays deterministic without the host strtod fallback and preserves representative exact-range spellings.",
        "Inspect src/proven/scan.c and the float decimal helper if the exact-range backend falls back to host strtod or the representative corpus drifts."
    );

    PROVEN_TEST_SECTION(
        "representative exact-range scans",
        "Confirm that representative values near 1.0, subnormal boundaries, and the 2^53 boundary still round to the documented bit patterns.",
        "Inspect the exact-range decimal backend if these corpus values stop matching their expected bits."
    );
    expect_scan_bits("near one", "0.9999999999999999", 0x1.fffffffffffffp-1);
    expect_scan_bits("just above one", "1.0000000000000001", strtod("1.0000000000000001", NULL));
    expect_scan_bits("upper mantissa and negative exponent", "9999999999999999e-16", 0x1.fffffffffffffp-1);
    expect_scan_bits("half-way style exact-range spelling", "9007199254740993e-1", 0x1.999999999999ap+49);
    expect_scan_bits("2^53 boundary", "9007199254740992", 0x1.0000000000000p+53);
    expect_scan_bits("subnormal boundary", "2.2250738585072014e-308", 0x1.0000000000000p-1022);
    expect_scan_bits("subnormal boundary plus one ulp", "2.2250738585072015e-308", strtod("2.2250738585072015e-308", NULL));
    expect_scan_bits("smallest subnormal", "4.9406564584124654e-324", 0x0.0000000000001p-1022);
    expect_scan_bits("smallest subnormal plus one ulp", "4.9406564584124655e-324", strtod("4.9406564584124655e-324", NULL));
    expect_scan_bits("largest boundary low", "1.7976931348623150e308", strtod("1.7976931348623150e308", NULL));
    expect_scan_bits("largest boundary low plus one", "1.7976931348623151e308", strtod("1.7976931348623151e308", NULL));
    expect_scan_bits("largest boundary low plus two", "1.7976931348623152e308", strtod("1.7976931348623152e308", NULL));
    expect_scan_bits("largest boundary mid", "1.7976931348623153e308", strtod("1.7976931348623153e308", NULL));
    expect_scan_bits("largest boundary mid plus one", "1.7976931348623154e308", strtod("1.7976931348623154e308", NULL));
    expect_scan_bits("largest boundary high", "1.7976931348623155e308", strtod("1.7976931348623155e308", NULL));
    expect_scan_bits("largest boundary high plus one", "1.7976931348623156e308", strtod("1.7976931348623156e308", NULL));
    expect_scan_bits("largest finite boundary", "1.7976931348623157e308", strtod("1.7976931348623157e308", NULL));
    expect_scan_bits("largest finite boundary plus one ulp spelling", "1.7976931348623158e308", strtod("1.7976931348623158e308", NULL));

    PROVEN_TEST_SECTION(
        "source contract",
        "Confirm the scanner no longer depends on host strtod for decimal conversion.",
        "Inspect src/proven/scan.c if the host-dependent fallback or strtod call reappears."
    );
    char *scan_src = read_file_text("src/proven/scan.c");
    require_text_missing(scan_src, "strtod(", "scan.c should not call host strtod for decimal conversion");
    require_text_missing(scan_src, "proven_scan_host_f64_fallback", "scan.c should not keep the host fallback helper");
    free(scan_src);

    PROVEN_TEST_SECTION(
        "shortest-policy f32 helper contract",
        "Confirm the float32 shortest-policy path reuses the staged helper instead of keeping a separate brute-force precision sweep.",
        "Inspect src/proven/float_format.c if the f32 shortest backend stops sharing the same first-roundtrip helper shape as f64."
    );
    char *fmt_src = read_file_text("src/proven/float_format.c");
    require_text_present(fmt_src, "proven_float_format_build_shortest_common", "float_format.c should keep the shared shortest helper");
    require_text_present(fmt_src, "proven_float_format_build_shortest_f32", "float_format.c should keep the float32 shortest shim");
    require_text_present(fmt_src, "proven_float_format_build_shortest_f64", "float_format.c should keep the float64 shortest shim");
    require_text_present(fmt_src, "return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);", "float_format.c should dispatch shortest f64 through the width-specific shim");
    require_text_present(fmt_src, "return proven_float_format_build_shortest_f32(buf, buf_cap, value, written_out);", "float_format.c should dispatch shortest f32 through the width-specific shim");
    require_text_missing(fmt_src, "proven_float_format_best_shortest_style_f32", "float_format.c should stop using the old float32 shortest helper name");
    require_text_missing(fmt_src, "proven_float_format_best_shortest_style_f64", "float_format.c should stop using the old float64 shortest helper name");
    free(fmt_src);

    PROVEN_TEST_PASS("Exact-range float backend checks passed.");
    return 0;
}
