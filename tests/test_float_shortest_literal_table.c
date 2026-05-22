#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect src/proven/float_decimal.c if the shortest literal module source cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the source file if seeking the end fails.");
    long len = ftell(f);
    PROVEN_TEST_ASSERT(len >= 0, path, "Inspect the source file if its size cannot be measured.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the source file if it cannot be rewound.");
    char *text = (char *)malloc((size_t)len + 1u);
    PROVEN_TEST_ASSERT(text != NULL, path, "Inspect the test host if allocating the inspection buffer fails.");
    size_t got = fread(text, 1u, (size_t)len, f);
    PROVEN_TEST_ASSERT(got == (size_t)len, path, "Inspect the source file if reading it returns a short count.");
    text[len] = '\0';
    fclose(f);
    return text;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect src/proven/float_decimal.c if a documented shortest literal disappears from the shared table.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_shortest_literal_table",
        "Verify the shared float decimal module keeps the documented shortest special-case literal tables pinned for f64 and f32 while the parser-driven backend remains staged.",
        "Inspect src/proven/float_decimal.c if a documented shortest literal disappears, changes spelling, or moves out of the shared table."
    );

    char *src = read_file_text("src/proven/float_decimal.c");

    PROVEN_TEST_SECTION(
        "shared shortest literal tables",
        "Confirm the shared float decimal module still carries the documented special-case shortest literals for both widths.",
        "Inspect the literal tables in src/proven/float_decimal.c if any shortest special case drifts or is removed."
    );
    require_text_present(src, "proven_float_shortest_literal_common", "shared shortest literal helper should remain present");
    require_text_present(src, "static const proven_float_shortest_literal_entry_t f64_literals[]", "f64 shortest literal table should remain present");
    require_text_present(src, "static const proven_float_shortest_literal_entry_t f32_literals[]", "f32 shortest literal table should remain present");

    require_text_present(src, "{ 0x7ff0000000000000ULL, \"Inf\" }", "f64 Inf literal should remain present");
    require_text_present(src, "{ 0xfff0000000000000ULL, \"-Inf\" }", "f64 -Inf literal should remain present");
    require_text_present(src, "{ 0x0000000000000000ULL, \"0\" }", "f64 zero literal should remain present");
    require_text_present(src, "{ 0x8000000000000000ULL, \"-0\" }", "f64 signed-zero literal should remain present");
    require_text_present(src, "{ 0x0010000000000000ULL, \"2.2250738585072014e-308\" }", "f64 DBL_MIN literal should remain present");
    require_text_present(src, "{ 0x8010000000000000ULL, \"-2.2250738585072014e-308\" }", "f64 negative DBL_MIN literal should remain present");
    require_text_present(src, "{ 0x000fffffffffffffULL, \"2.2250738585072009e-308\" }", "f64 largest subnormal literal should remain present");
    require_text_present(src, "{ 0x800fffffffffffffULL, \"-2.2250738585072009e-308\" }", "f64 negative largest subnormal literal should remain present");
    require_text_present(src, "{ 0x7fefffffffffffffULL, \"1.7976931348623157e308\" }", "f64 DBL_MAX literal should remain present");
    require_text_present(src, "{ 0xffefffffffffffffULL, \"-1.7976931348623157e308\" }", "f64 negative DBL_MAX literal should remain present");
    require_text_present(src, "{ 0x0000000000000001ULL, \"5e-324\" }", "f64 smallest subnormal literal should remain present");
    require_text_present(src, "{ 0x8000000000000001ULL, \"-5e-324\" }", "f64 negative smallest subnormal literal should remain present");

    require_text_present(src, "{ 0x7f800000u, \"Inf\" }", "f32 Inf literal should remain present");
    require_text_present(src, "{ 0xff800000u, \"-Inf\" }", "f32 -Inf literal should remain present");
    require_text_present(src, "{ 0x00000000u, \"0\" }", "f32 zero literal should remain present");
    require_text_present(src, "{ 0x80000000u, \"-0\" }", "f32 signed-zero literal should remain present");
    require_text_present(src, "{ 0x00800000u, \"1.17549435e-38\" }", "f32 FLT_MIN literal should remain present");
    require_text_present(src, "{ 0x80800000u, \"-1.17549435e-38\" }", "f32 negative FLT_MIN literal should remain present");
    require_text_present(src, "{ 0x7f7fffffu, \"3.4028235e38\" }", "f32 FLT_MAX literal should remain present");
    require_text_present(src, "{ 0xff7fffffu, \"-3.4028235e38\" }", "f32 negative FLT_MAX literal should remain present");
    require_text_present(src, "{ 0x00000001u, \"1e-45\" }", "f32 smallest subnormal literal should remain present");
    require_text_present(src, "{ 0x80000001u, \"-1e-45\" }", "f32 negative smallest subnormal literal should remain present");

    free(src);
    PROVEN_TEST_PASS("Shared shortest literal table checks passed.");
    return 0;
}
