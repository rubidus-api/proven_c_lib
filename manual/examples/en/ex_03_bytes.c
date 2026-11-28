#include "example.h"

/*
 * bytes.h: C's memchr, strspn, strcspn, strpbrk and <ctype.h>, over views and without a
 * locale - and integers in any base from 2 to 36. The program reads one configuration line,
 *
 *     mode = 0o755; mask=0xFF ; name="Readme.TXT" # comment
 *
 * the way a small tokenizer would: cut the comment, split at ';', split each field at '=',
 * trim the blanks, read the numbers. Nothing is copied and nothing is allocated: every piece is
 * a view into the line, and no search reads past the view it is given.
 */

static proven_mem_view_t mem_of(proven_u8str_view_t s) { return (proven_mem_view_t){ s.ptr, s.size }; }

/* One letter per ASCII class, the first that fits: what a lexer asks of each byte. */
static char class_of(proven_byte_t c) {
    if (proven_ascii_is_blank(c)) return 'b';             /* space and tab */
    if (proven_ascii_is_space(c)) return 's';             /* newline, vertical tab, form feed, CR */
    if (proven_ascii_is_cntrl(c)) return 'c';             /* the other controls, and DEL */
    if (proven_ascii_is_digit(c)) return 'd';
    if (proven_ascii_is_xdigit(c)) return 'x';            /* a-f, A-F */
    if (proven_ascii_is_upper(c)) return 'U';
    if (proven_ascii_is_lower(c)) return 'l';
    if (proven_ascii_is_punct(c)) return 'p';
    return '?';                                           /* from 0x80 up: in no class */
}

int main(void) {
    const proven_u8str_view_t line = PROVEN_LIT("mode = 0o755; mask=0xFF ; name=\"Readme.TXT\" # comment");

    /* memchr: the comment starts at the first '#'; strrchr: the last quote. */
    const proven_size_t hash = proven_mem_find_byte(mem_of(line), '#');
    EXAMPLE_REQUIRE(hash == 44, "the comment begins at byte 44");
    const proven_u8str_view_t body = proven_u8str_view_slice(line, 0, hash);
    EXAMPLE_REQUIRE(proven_mem_find_last_byte(mem_of(body), '"') == 42, "the closing quote");
    EXAMPLE_REQUIRE(proven_mem_find_byte(mem_of(body), '@') == PROVEN_INDEX_NOT_FOUND, "no '@' at all");

    /* Sets of bytes for strspn / strcspn / strpbrk. */
    proven_byteset_t blanks = { { 0, 0, 0, 0 } };
    proven_byteset_add(&blanks, ' ');
    proven_byteset_add(&blanks, '\t');
    const proven_byteset_t punct = proven_byteset_of(mem_of(PROVEN_LIT(";=\"")));
    EXAMPLE_REQUIRE(proven_byteset_has(&punct, '=') && !proven_byteset_has(&punct, '#'), "set membership");
    EXAMPLE_REQUIRE(proven_mem_find_any(mem_of(body), &punct) == 5, "strpbrk: the first of ; = \" is the '='");
    EXAMPLE_REQUIRE(proven_mem_find_last_any(mem_of(body), &punct) == 42, "and the last is the closing quote");

    /* Split at ';', then each field at '=', trimming blanks with the span functions. */
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
        /* Numbers: "0o" means octal here, "0x" hex - the parser takes a base, it does not guess. */
        if (val.size > 2 && val.ptr[0] == '0' && proven_ascii_lower(val.ptr[1]) == 'o' && fields < 2) {
            EXAMPLE_REQUIRE(proven_u8str_view_parse_u64(proven_u8str_view_slice(val, 2, val.size), 8, NULL, &values[fields]) == PROVEN_OK, "octal");
        } else if (val.size > 0 && proven_ascii_is_digit(val.ptr[0]) && fields < 2) {
            EXAMPLE_REQUIRE(proven_u8str_view_parse_u64(val, 16, NULL, &values[fields]) == PROVEN_OK, "hex, its 0x taken by the parser");
        } else {
            /* A word: its letters folded to upper case, by bytes. */
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

    /* Signed numbers, and other bases: 2 to 36, digits '0'-'9' then letters in either case. */
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

    /* The classes of each byte, as a lexer sees them. */
    const proven_u8str_view_t sample = PROVEN_LIT("a1 F\t#\x7f\xc3");
    char classes[16];
    for (proven_size_t i = 0; i < sample.size; i++) classes[i] = class_of(sample.ptr[i]);
    classes[sample.size] = 0;
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_view_from_bytes(classes, sample.size), PROVEN_LIT("xdbxbpc?")), "a1 F<tab>#<del><0xC3>: hex letter, digit, blank, hex letter, blank, punct, control, none");
    EXAMPLE_REQUIRE(proven_ascii_is_print(' ') && !proven_ascii_is_graph(' ') && proven_ascii_is_graph('~'), "print includes space, graph does not");

    printf("mode %llo, mask %llu, classes %s\n", (unsigned long long)values[0], (unsigned long long)values[1], classes);
    return EXAMPLE_OK();
}
