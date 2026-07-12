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
        "test_portability_float",
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
    require(contains(fmt_src, "proven_float_format_f64_policy"), "fmt.c should format floats through the shared exact policy formatter");
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
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "1.000000e+19") == 0, "scientific carry", "Inspect the scientific normalization path if the formatted scientific output changes.");
    proven_u8str_destroy(alloc, &str);

    proven_scan_t scan = proven_scan_init(proven_u8str_view_from_cstr("0.30000000000000004"));
    proven_result_f64_t fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "round-trip style decimal", "Inspect proven_scan_f64 if a representative decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == double_bits(0.30000000000000004), "round-trip style decimal", "Inspect decimal-to-double conversion if the parsed bits change.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-1.74070841063175697e+205"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal", "Inspect proven_scan_f64 if a large finite scientific decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xea8bc28d457c01f2ULL, "large scientific decimal", "Inspect decimal-to-double scaling if the parsed bits drift by one ULP at large exponents.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-68e-221"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "tiny negative scientific decimal", "Inspect proven_scan_f64 if a small finite scientific decimal with a negative exponent stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x926eb9ab6ad58f6dULL, "tiny negative scientific decimal", "Inspect decimal-to-double scaling if a tiny negative scientific decimal drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-3.2535069386238e-213"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "tiny negative scientific decimal mid-band", "Inspect proven_scan_f64 if a tiny negative scientific decimal in the mid-band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x93d1864da04d3b38ULL, "tiny negative scientific decimal mid-band", "Inspect decimal-to-double scaling if a tiny negative scientific decimal in the mid-band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("6390280087381e177"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "mid-large scientific decimal", "Inspect proven_scan_f64 if a mid-large finite scientific decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x6756f2a865275d94ULL, "mid-large scientific decimal", "Inspect decimal-to-double scaling if a mid-large scientific decimal drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("7.363189778827297e98"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "mid scientific decimal", "Inspect proven_scan_f64 if a mid scientific decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x54758b880c4516ebULL, "mid scientific decimal", "Inspect decimal-to-double scaling if a mid scientific decimal drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("6390280087381e-39"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "tiny scientific decimal", "Inspect proven_scan_f64 if a tiny scientific decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x3a7fa4a47390dbf8ULL, "tiny scientific decimal", "Inspect decimal-to-double scaling if a tiny scientific decimal drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("304951476376883325e116"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal mid-band", "Inspect proven_scan_f64 if a large scientific decimal in the mid-band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x5ba57b3d3afd21a0ULL, "large scientific decimal mid-band", "Inspect decimal-to-double scaling if a large scientific decimal in the mid-band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("6.4456509097e-267"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "tiny scientific decimal deep band", "Inspect proven_scan_f64 if a tiny scientific decimal deep in the negative band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x08aa9a677cf46aecULL, "tiny scientific decimal deep band", "Inspect decimal-to-double scaling if a tiny scientific decimal deep in the negative band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("33064920252e132"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal upper band", "Inspect proven_scan_f64 if a large scientific decimal in the upper band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x5d85b126a443e433ULL, "large scientific decimal upper band", "Inspect decimal-to-double scaling if a large scientific decimal in the upper band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("2e-235"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "tiny scientific decimal far band", "Inspect proven_scan_f64 if a tiny scientific decimal in the far negative band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x0f345962e2f6a490ULL, "tiny scientific decimal far band", "Inspect decimal-to-double scaling if a tiny scientific decimal in the far negative band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-85062e84"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal high band", "Inspect proven_scan_f64 if a large scientific decimal in the high band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xd2656144f0942e32ULL, "large scientific decimal high band", "Inspect decimal-to-double scaling if a large scientific decimal in the high band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-6.691e-126"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative scientific decimal mid band", "Inspect proven_scan_f64 if a negative scientific decimal in the mid band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xa5f21dfbb50430c4ULL, "negative scientific decimal mid band", "Inspect decimal-to-double scaling if a negative scientific decimal in the mid band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-50200632795643e-285"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative scientific decimal deep band", "Inspect proven_scan_f64 if a negative scientific decimal in the deep band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x879b283f603c922fULL, "negative scientific decimal deep band", "Inspect decimal-to-double scaling if a negative scientific decimal in the deep band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("7.2318e-179"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "mid negative scientific decimal", "Inspect proven_scan_f64 if a mid negative scientific decimal stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x1af2c15acbb0af1fULL, "mid negative scientific decimal", "Inspect decimal-to-double scaling if a mid negative scientific decimal drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("851041725486047647e176"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal far band", "Inspect proven_scan_f64 if a large scientific decimal in the far positive band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x6832a738e88c729eULL, "large scientific decimal far band", "Inspect decimal-to-double scaling if a large scientific decimal in the far positive band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("16215694128170e256"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal upper edge", "Inspect proven_scan_f64 if a large scientific decimal near the upper edge stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x77d3a4f947426897ULL, "large scientific decimal upper edge", "Inspect decimal-to-double scaling if a large scientific decimal near the upper edge drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-85e138"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative large scientific decimal high band", "Inspect proven_scan_f64 if a negative large scientific decimal in the high band stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xdcfc8d0c98b37780ULL, "negative large scientific decimal high band", "Inspect decimal-to-double scaling if a negative large scientific decimal in the high band drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-7.64e85"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative large scientific decimal compact mantissa", "Inspect proven_scan_f64 if a negative large scientific decimal with a compact mantissa stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xd1c3a9e646cdfac7ULL, "negative large scientific decimal compact mantissa", "Inspect decimal-to-double scaling if a negative large scientific decimal with a compact mantissa drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-7.8991867e-215"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative tiny scientific decimal compact mantissa", "Inspect proven_scan_f64 if a negative tiny scientific decimal with a compact mantissa stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x937b3b11148be106ULL, "negative tiny scientific decimal compact mantissa", "Inspect decimal-to-double scaling if a negative tiny scientific decimal with a compact mantissa drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-06850422e-297"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative tiny scientific decimal with leading zero", "Inspect proven_scan_f64 if a negative tiny scientific decimal with a leading zero stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x83b11726c72e94e6ULL, "negative tiny scientific decimal with leading zero", "Inspect decimal-to-double scaling if a negative tiny scientific decimal with a leading zero drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-135170513485e-284"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative tiny scientific decimal deep compact mantissa", "Inspect proven_scan_f64 if a negative tiny scientific decimal with a deep exponent stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x8747664ab3d3e8dbULL, "negative tiny scientific decimal deep compact mantissa", "Inspect decimal-to-double scaling if a negative tiny scientific decimal with a deep exponent drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-55379398548477183e-295"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative tiny scientific decimal deep full mantissa", "Inspect proven_scan_f64 if a negative tiny scientific decimal with a full mantissa stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x8629219a0a5b79d4ULL, "negative tiny scientific decimal deep full mantissa", "Inspect decimal-to-double scaling if a negative tiny scientific decimal with a full mantissa drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("7346e79"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal compact mantissa", "Inspect proven_scan_f64 if a large scientific decimal with a compact mantissa stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x51235c59e2555387ULL, "large scientific decimal compact mantissa", "Inspect decimal-to-double scaling if a large scientific decimal with a compact mantissa drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("30018820096e110"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "large scientific decimal hosted fallback", "Inspect proven_scan_f64 if a large scientific decimal in the hosted fallback stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x58f299a04f943f9fULL, "large scientific decimal hosted fallback", "Inspect decimal-to-double scaling if a large scientific decimal in the hosted fallback drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-63306334748e-173"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative scientific decimal hosted fallback", "Inspect proven_scan_f64 if a negative scientific decimal in the hosted fallback stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0x9e423a52a4991165ULL, "negative scientific decimal hosted fallback", "Inspect decimal-to-double scaling if a negative scientific decimal in the hosted fallback drifts by one ULP.");

    scan = proven_scan_init(proven_u8str_view_from_cstr("-15714e249"));
    fres = proven_scan_f64(&scan);
    PROVEN_TEST_ASSERT(fres.err == PROVEN_OK, "negative large scientific decimal hosted fallback", "Inspect proven_scan_f64 if a negative large scientific decimal in the hosted fallback stops parsing successfully.");
    PROVEN_TEST_ASSERT(double_bits(fres.val) == 0xf481258db00b5cc1ULL, "negative large scientific decimal hosted fallback", "Inspect decimal-to-double scaling if a negative large scientific decimal in the hosted fallback drifts by one ULP.");

    PROVEN_TEST_PASS("Float portability checks passed.");
    return 0;
}
