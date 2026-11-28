#include "example.h"

/*
 * numfmt.h: 어느 지역이 쓰는 방식대로의 수, 값으로. C 의 setlocale 은 그 프로세스의 모든 printf 와
 * strtod 가 소수점을 쓰는 방식을 바꾼다. 여기서는 표기 방식이 그것이 필요한 호출 하나에만 넘기는
 * proven_numfmt_t 이고, `{}` 는 어디서나 그대로다.
 *
 * 한 금액을 여러 독자를 위해 쓰고 다시 읽는다. 그다음 직접 정한 표기.
 */

static void show(const char *who, const proven_numfmt_t *f, double amount) {
    proven_byte_t buf[64];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_numfmt_f64(f, amount, 2, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK, "written");
    printf("%-8s %.*s\n", who, (int)n, (const char *)buf);
    double back = 0;
    EXAMPLE_REQUIRE(proven_numfmt_parse_f64(f, (proven_u8str_view_t){ buf, n }, NULL, &back) == PROVEN_OK && back == (double)1234567.89, "and read back");
}

int main(void) {
    const double amount = 1234567.891;
    show("C", &proven_numfmt_c, amount);        /* 1234567.89 */
    show("English", &proven_numfmt_en, amount); /* 1,234,567.89 */
    show("German", &proven_numfmt_de, amount);  /* 1.234.567,89 */
    show("French", &proven_numfmt_fr, amount);  /* 좁은 줄바꿈 없는 공백으로 1 234 567,89 */
    show("Swiss", &proven_numfmt_ch, amount);   /* 1'234'567.89 */
    show("Indian", &proven_numfmt_in, amount);  /* 12,34,567.89 */

    /* 정수, 그리고 한 줄의 앞부분: `used` 는 그중 얼마가 수였는지 말한다. */
    proven_byte_t buf[32];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_numfmt_i64(&proven_numfmt_en, -1500000, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK
                    && proven_u8str_view_eq(proven_u8str_view_from_bytes(buf, n), PROVEN_LIT("-1,500,000")), "an integer");
    EXAMPLE_REQUIRE(proven_numfmt_u64(&proven_numfmt_de, 2026, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK
                    && proven_u8str_view_eq(proven_u8str_view_from_bytes(buf, n), PROVEN_LIT("2.026")), "an unsigned one");
    proven_i64 qty = 0;
    proven_size_t used = 0;
    EXAMPLE_REQUIRE(proven_numfmt_parse_i64(&proven_numfmt_en, PROVEN_LIT("12,000 units"), &used, &qty) == PROVEN_OK && qty == 12000 && used == 6,
                    "the number at the front of a line");

    /* 직접 정한 표기: 네 자리씩, 공백, 그리고 빼기 기호 U+2212. */
    const proven_numfmt_t wan = { PROVEN_LIT("."), PROVEN_LIT(" "), { 4 }, PROVEN_LIT("\xe2\x88\x92") };
    EXAMPLE_REQUIRE(proven_numfmt_i64(&wan, -123456789, (proven_mem_mut_t){ buf, sizeof buf }, &n) == PROVEN_OK
                    && proven_u8str_view_eq(proven_u8str_view_from_bytes(buf, n), PROVEN_LIT("\xe2\x88\x92" "1 2345 6789")), "grouped by ten thousands");

    /* 자르지 않고 거부한다. */
    proven_byte_t tiny[4];
    EXAMPLE_REQUIRE(proven_numfmt_u64(&proven_numfmt_en, 12345, (proven_mem_mut_t){ tiny, sizeof tiny }, &n) == PROVEN_ERR_OUT_OF_BOUNDS, "too small: refused");
    return EXAMPLE_OK();
}
