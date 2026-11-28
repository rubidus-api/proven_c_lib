#include "example.h"

/*
 * bytes.h: C 의 memchr, strspn, strcspn, strpbrk 와 <ctype.h> 를 뷰 위에서, 로케일 없이 -
 * 그리고 2 에서 36 까지 아무 진법의 정수. 이 프로그램은 설정 한 줄을
 *
 *     mode = 0o755; mask=0xFF ; name="Readme.TXT" # comment
 *
 * 작은 토크나이저가 읽듯이 읽는다. 주석을 잘라 내고, ';' 에서 나누고, 각 필드를 '=' 에서
 * 나누고, 공백을 다듬고, 수를 읽는다. 복사도 할당도 없다. 조각은 모두 이 줄을 가리키는 뷰이고,
 * 어느 검색도 받은 뷰를 넘어 읽지 않는다.
 */

static proven_mem_view_t mem_of(proven_u8str_view_t s) { return (proven_mem_view_t){ s.ptr, s.size }; }

/* ASCII 분류마다 글자 하나, 처음 맞는 것: 렉서가 바이트마다 묻는 것. */
static char class_of(proven_byte_t c) {
    if (proven_ascii_is_blank(c)) return 'b';             /* 공백과 탭 */
    if (proven_ascii_is_space(c)) return 's';             /* 줄바꿈, 수직 탭, 폼 피드, CR */
    if (proven_ascii_is_cntrl(c)) return 'c';             /* 나머지 제어 문자와 DEL */
    if (proven_ascii_is_digit(c)) return 'd';
    if (proven_ascii_is_xdigit(c)) return 'x';            /* a-f, A-F */
    if (proven_ascii_is_upper(c)) return 'U';
    if (proven_ascii_is_lower(c)) return 'l';
    if (proven_ascii_is_punct(c)) return 'p';
    return '?';                                           /* 0x80 부터: 어느 분류에도 들지 않음 */
}

int main(void) {
    const proven_u8str_view_t line = PROVEN_LIT("mode = 0o755; mask=0xFF ; name=\"Readme.TXT\" # comment");

    /* memchr: 주석은 첫 '#' 에서 시작한다. strrchr: 마지막 따옴표. */
    const proven_size_t hash = proven_mem_find_byte(mem_of(line), '#');
    EXAMPLE_REQUIRE(hash == 44, "the comment begins at byte 44");
    const proven_u8str_view_t body = proven_u8str_view_slice(line, 0, hash);
    EXAMPLE_REQUIRE(proven_mem_find_last_byte(mem_of(body), '"') == 42, "the closing quote");
    EXAMPLE_REQUIRE(proven_mem_find_byte(mem_of(body), '@') == PROVEN_INDEX_NOT_FOUND, "no '@' at all");

    /* strspn / strcspn / strpbrk 를 위한 바이트 집합. */
    proven_byteset_t blanks = { { 0, 0, 0, 0 } };
    proven_byteset_add(&blanks, ' ');
    proven_byteset_add(&blanks, '\t');
    const proven_byteset_t punct = proven_byteset_of(mem_of(PROVEN_LIT(";=\"")));
    EXAMPLE_REQUIRE(proven_byteset_has(&punct, '=') && !proven_byteset_has(&punct, '#'), "set membership");
    EXAMPLE_REQUIRE(proven_mem_find_any(mem_of(body), &punct) == 5, "strpbrk: the first of ; = \" is the '='");
    EXAMPLE_REQUIRE(proven_mem_find_last_any(mem_of(body), &punct) == 42, "and the last is the closing quote");

    /* ';' 에서 나누고, 필드마다 '=' 에서 나누고, span 함수로 공백을 다듬는다. */
    const proven_byteset_t semi = proven_byteset_of(mem_of(PROVEN_LIT(";")));
    const proven_byteset_t eq = proven_byteset_of(mem_of(PROVEN_LIT("=")));
    proven_u64 values[2] = { 0, 0 };
    int fields = 0;
    proven_u8str_view_t rest = body;
    while (rest.size) {
        const proven_size_t len = proven_mem_span_not(mem_of(rest), &semi);          /* strcspn */
        proven_u8str_view_t field = proven_u8str_view_slice(rest, 0, len);
        rest = proven_u8str_view_slice(rest, len < rest.size ? len + 1 : len, rest.size);
        const proven_size_t lead = proven_mem_span(mem_of(field), &blanks);          /* strspn */
        field = proven_u8str_view_slice(field, lead, field.size);
        const proven_size_t keylen = proven_mem_span_not(mem_of(field), &eq);
        proven_u8str_view_t key = proven_u8str_view_trim_end(proven_u8str_view_slice(field, 0, keylen));
        proven_u8str_view_t val = proven_u8str_view_trim(proven_u8str_view_slice(field, keylen + 1, field.size));
        /* 수: 여기서 "0o" 는 8진, "0x" 는 16진 - 파서는 진법을 받을 뿐 짐작하지 않는다. */
        if (val.size > 2 && val.ptr[0] == '0' && proven_ascii_lower(val.ptr[1]) == 'o' && fields < 2) {
            EXAMPLE_REQUIRE(proven_u8str_view_parse_u64(proven_u8str_view_slice(val, 2, val.size), 8, NULL, &values[fields]) == PROVEN_OK, "octal");
        } else if (val.size > 0 && proven_ascii_is_digit(val.ptr[0]) && fields < 2) {
            EXAMPLE_REQUIRE(proven_u8str_view_parse_u64(val, 16, NULL, &values[fields]) == PROVEN_OK, "hex, its 0x taken by the parser");
        } else {
            /* 단어: 글자를 바이트 단위로 대문자로. */
            proven_byte_t up[32];
            proven_size_t n = 0;
            for (proven_size_t i = 0; i < val.size && n < sizeof up; i++)
                if (proven_ascii_is_alnum(val.ptr[i]) || val.ptr[i] == '.') up[n++] = proven_ascii_upper(val.ptr[i]);
            EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_view_from_bytes(up, n), PROVEN_LIT("README.TXT")), "the name, upper case");
        }
        EXAMPLE_REQUIRE(key.size > 0 && proven_ascii_is_alpha(key.ptr[0]), "every key starts with a letter");
        fields++;
    }
    EXAMPLE_REQUIRE(fields == 3 && values[0] == 0755 && values[1] == 255, "mode 0o755 and mask 0xFF");

    /* 부호 있는 수와 다른 진법: 2 에서 36, 숫자 '0'-'9' 다음 대소문자 글자. */
    proven_i64 delta = 0;
    proven_u32 bits = 0;
    proven_i32 small = 0;
    proven_size_t used = 0;
    EXAMPLE_REQUIRE(proven_u8str_view_parse_i64(PROVEN_LIT("-42px"), 10, &used, &delta) == PROVEN_OK && delta == -42 && used == 3,
                    "with `used`, the number at the front and how far it went");
    EXAMPLE_REQUIRE(proven_u8str_view_parse_u32(PROVEN_LIT("1010"), 2, NULL, &bits) == PROVEN_OK && bits == 10, "binary");
    EXAMPLE_REQUIRE(proven_u8str_view_parse_i32(PROVEN_LIT("zz"), 36, NULL, &small) == PROVEN_OK && small == 1295, "base 36");
    EXAMPLE_REQUIRE(proven_u8str_view_parse_u32(PROVEN_LIT("4294967296"), 10, NULL, &bits) == PROVEN_ERR_OVERFLOW, "too big is an error");
    EXAMPLE_REQUIRE(proven_u8str_view_parse_u32(PROVEN_LIT(" 7"), 10, NULL, &bits) == PROVEN_ERR_INVALID_ARG, "no whitespace skipped");
    EXAMPLE_REQUIRE(proven_ascii_digit_value('f') == 15 && proven_ascii_digit_value('Z') == 35 && proven_ascii_digit_value('-') == 255, "digit values");

    /* 렉서가 보는 각 바이트의 분류. */
    const proven_u8str_view_t sample = PROVEN_LIT("a1 F\t#\x7f\xc3");
    char classes[16];
    for (proven_size_t i = 0; i < sample.size; i++) classes[i] = class_of(sample.ptr[i]);
    classes[sample.size] = 0;
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_view_from_bytes(classes, sample.size), PROVEN_LIT("xdbxbpc?")), "a1 F<tab>#<del><0xC3>: hex letter, digit, blank, hex letter, blank, punct, control, none");
    EXAMPLE_REQUIRE(proven_ascii_is_print(' ') && !proven_ascii_is_graph(' ') && proven_ascii_is_graph('~'), "print includes space, graph does not");

    printf("mode %llo, mask %llu, classes %s\n", (unsigned long long)values[0], (unsigned long long)values[1], classes);
    return EXAMPLE_OK();
}
