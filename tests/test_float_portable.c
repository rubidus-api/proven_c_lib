#include "proven/fmt.h"
#include "proven/heap.h"
#include "proven/scan.h"
#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the float conversion paths in src/proven/scan.c and src/proven/fmt.c. The conversion code is designed to stay double-only and target-deterministic.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    require(f != NULL, path);
    require(fseek(f, 0, SEEK_END) == 0, "seek end");
    long n = ftell(f);
    require(n >= 0, "tell");
    require(fseek(f, 0, SEEK_SET) == 0, "seek set");
    char *buf = (char*)malloc((size_t)n + 1u);
    require(buf != NULL, "malloc");
    size_t got = fread(buf, 1u, (size_t)n, f);
    require(got == (size_t)n, "read full file");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static uint64_t double_bits(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_portable",
        "Verify that float scanning and formatting use double-only conversion paths with documented target-deterministic behavior.",
        "Inspect src/proven/scan.c and src/proven/fmt.c if long double returns or casts reappear, or if portable float outputs drift."
    );

    char *scan_src = read_text_file("src/proven/scan.c");
    require(!contains(scan_src, "long double"), "scan.c should not depend on long double for decimal conversion");
    require(contains(scan_src, "#include \"float_decimal.h\""), "scan.c should include the shared float helper header");
    require(!contains(scan_src, "static double proven_scan_scale_pow10("), "scan.c should not define the shared pow10 scaling helper inline");
    require(!contains(scan_src, "static double proven_scan_convert_decimal("), "scan.c should not define the shared decimal conversion helper inline");
    require(contains(scan_src, "proven_float_convert_decimal"), "scan.c should convert decimal mantissas through the shared helper");
    free(scan_src);

    char *fmt_src = read_text_file("src/proven/fmt.c");
    require(!contains(fmt_src, "long double"), "fmt.c should not depend on long double for float formatting");
    require(contains(fmt_src, "#include \"float_decimal.h\""), "fmt.c should include the shared float helper header");
    require(!contains(fmt_src, "static bool proven_fmt_normalize_scientific("), "fmt.c should not define the shared scientific normalization helper inline");
    require(contains(fmt_src, "proven_float_normalize_scientific"), "fmt.c should normalize scientific notation through the shared helper");
    require(contains(fmt_src, "double abs_v = sign ? -v : v;"), "fmt.c should hold the absolute working value in double precision");
    free(fmt_src);

    PROVEN_TEST_SECTION(
        "representative runtime values",
        "Confirm that the double-only code paths still produce the documented float strings and parsed values for representative inputs.",
        "Inspect the float formatter or scanner path if these representative values drift after the portability cleanup.");
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t sres = proven_u8str_create(alloc, 8);
    require(sres.err == PROVEN_OK, "create a scratch string for float formatting");
    proven_u8str_t str = sres.value;
    PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "{}", PROVEN_ARG(9.9999995e18))), "scientific carry", "Inspect the scientific normalization path if near-10 values stop producing the documented text.");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "9.999999e+18") == 0, "scientific carry", "Inspect the scientific normalization path if the formatted scientific output changes.");
    proven_u8str_destroy(alloc, &str);

    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr("0.30000000000000004"));
    proven_result_f64_t fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "round-trip style decimal", "Inspect proven_scan_f64 if a representative decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == double_bits(0.30000000000000004), "round-trip style decimal", "Inspect decimal-to-double conversion if the parsed bits change.");

    PROVEN_TEST_PASS("Float portability checks passed.");
    return 0;
}
