#include "proven.h"
#include "proven_test.h"

#ifndef PROVEN_NO_U16STR

#define V(s) PROVEN_U16_LIT(s)

static bool same(proven_u16str_view_t a, proven_u16str_view_t b) { return proven_u16str_view_eq(a, b) == 1; }

int main(void) {
    PROVEN_TEST_SUITE("u16str views: wcslen, wcscmp, wcsstr, wcstok, wcstoul and wcsncpy without their traps",
        "RFC-0044: the view vocabulary of u8str.h in 16-bit units, for code that holds Win32 wide strings.",
        "A failure names the function. Its rules are u8str.h's view rules: an ill-formed view is empty, every empty result is {NULL, 0}, a miss is PROVEN_INDEX_NOT_FOUND.");

    PROVEN_TEST_SECTION("length, equality, order", "from_cstr, eq, cmp and the ASCII case-insensitive forms.", "See src/proven/u16str_view.c.");
    {
        static const proven_u16 z[] = u"abc";
        PROVEN_TEST_ASSERT(proven_u16_cstr_len(z) == 3 && proven_u16_cstr_len(NULL) == 0, "the units before the 0", "");
        proven_u16str_view_t v = proven_u16str_view_from_cstr(z);
        PROVEN_TEST_ASSERT(v.ptr == z && v.size == 3 && proven_u16str_view_from_cstr(NULL).ptr == NULL, "a view of a C string", "");
#ifdef _WIN32
        /* wchar_t is proven_u16's type on Windows: no cast (RFC-0044; -Werror would catch one). */
        const wchar_t *w = L"wide";
        PROVEN_TEST_ASSERT(proven_u16str_view_from_cstr(w).size == 4, "a wchar_t string passes as it is", "");
#endif
        PROVEN_TEST_ASSERT(same(V("abc"), v) && !same(V("abd"), v) && !same(V("ab"), v), "eq", "");
        PROVEN_TEST_ASSERT(same((proven_u16str_view_t){ NULL, 5 }, V("")), "an ill-formed view is empty", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_cmp(V("ab"), V("abc")) < 0 && proven_u16str_view_cmp(V("b"), V("abc")) > 0 &&
                           proven_u16str_view_cmp(V("abc"), V("abc")) == 0, "cmp: unit order, a prefix first", "");
        static const proven_u16 hi[] = { 0xFFFF }, lo[] = { 'a' };
        PROVEN_TEST_ASSERT(proven_u16str_view_cmp((proven_u16str_view_t){ hi, 1 }, (proven_u16str_view_t){ lo, 1 }) > 0, "cmp is unsigned", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_eq_ascii_ci(V("Setup.EXE"), V("setup.exe")) &&
                           !proven_u16str_view_eq_ascii_ci(V("@"), V("`")) &&
                           !proven_u16str_view_eq_ascii_ci((proven_u16str_view_t){ (const proven_u16[]){ 0x00C4 }, 1 }, (proven_u16str_view_t){ (const proven_u16[]){ 0x00E4 }, 1 }), "ASCII letters only", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_cmp_ascii_ci(V("apple"), V("Banana")) < 0 &&
                           proven_u16str_view_cmp_ascii_ci(V("ABC"), V("abc")) == 0, "cmp_ascii_ci", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_starts_with_ascii_ci(V("/LOG=x"), V("/log=")) &&
                           proven_u16str_view_ends_with_ascii_ci(V("a.TXT"), V(".txt")) &&
                           !proven_u16str_view_ends_with_ascii_ci(V("txt"), V(".txt")), "edges without case", "");
        PROVEN_TEST_ASSERT(proven_u16_ascii_lower(u'Q') == u'q' && proven_u16_ascii_lower(0x00C4) == 0x00C4, "ascii_lower", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_starts_with(V("abc"), V("")) && proven_u16str_view_ends_with(V("abc"), V("bc")) &&
                           !proven_u16str_view_starts_with(V("ab"), V("abc")), "starts_with, ends_with", "");
    }

    PROVEN_TEST_SECTION("search, slice, trim", "find, find_last, the unit searches, contains, slice, trim, remove_*.", "");
    {
        proven_u16str_view_t h = V("a.b.c");
        PROVEN_TEST_ASSERT(proven_u16str_view_find(h, 0, V(".")) == 1 && proven_u16str_view_find(h, 2, V(".")) == 3 &&
                           proven_u16str_view_find(h, 0, V("x")) == PROVEN_INDEX_NOT_FOUND &&
                           proven_u16str_view_find(h, 9, V("")) == PROVEN_INDEX_NOT_FOUND &&
                           proven_u16str_view_find(h, 2, V("")) == 2, "find", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_find_last(V("aaa"), V("aa")) == 1 && proven_u16str_view_find_last(h, V("")) == 5 &&
                           proven_u16str_view_find_last(h, V("b.c")) == 2, "find_last", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_find_unit(h, 2, u'.') == 3 && proven_u16str_view_find_last_unit(h, u'.') == 3 &&
                           proven_u16str_view_find_last_unit(h, u'x') == PROVEN_INDEX_NOT_FOUND, "the unit searches", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_contains(h, V("b.")) && !proven_u16str_view_contains(h, V("ba")), "contains", "");
        PROVEN_TEST_ASSERT(same(proven_u16str_view_slice(h, 2, 99), V("b.c")) && proven_u16str_view_slice(h, 5, 1).ptr == NULL,
                           "slice clamps; an empty result is {NULL, 0}", "");
        proven_u16str_view_t t = proven_u16str_view_trim(V(" \t x y \r\n"));
        PROVEN_TEST_ASSERT(same(t, V("x y")) && proven_u16str_view_trim(V("  ")).ptr == NULL, "trim", "");
        static const proven_u16 nbsp[] = { 0x00A0, 'x' };
        PROVEN_TEST_ASSERT(proven_u16str_view_trim((proven_u16str_view_t){ nbsp, 2 }).size == 2, "only the six ASCII units", "");
        PROVEN_TEST_ASSERT(same(proven_u16str_view_remove_prefix(V("/retry:5"), V("/retry:")), V("5")) &&
                           same(proven_u16str_view_remove_suffix(V("a.log"), V(".txt")), V("a.log")), "remove_prefix, remove_suffix", "");
    }

    PROVEN_TEST_SECTION("split and tokens", "split keeps empty fields; tokens is wcstok without its state.", "");
    {
        proven_u16str_view_split_t it = proven_u16str_view_split(V("a,,b,"), V(","));
        proven_u16str_view_t f[6];
        int n = 0;
        while (n < 6 && proven_u16str_view_split_next(&it, &f[n])) ++n;
        PROVEN_TEST_ASSERT(n == 4 && same(f[0], V("a")) && f[1].size == 0 && same(f[2], V("b")) && f[3].size == 0, "n separators, n + 1 fields", "");
        proven_u16str_view_split_t one = proven_u16str_view_split(V(""), V(","));
        PROVEN_TEST_ASSERT(proven_u16str_view_split_next(&one, &f[0]) && f[0].size == 0 && !proven_u16str_view_split_next(&one, &f[0]), "\"\" is one field", "");

        proven_u16str_view_tokens_t tk = proven_u16str_view_tokens(V(";; a;b ;"), V("; "));
        n = 0;
        while (n < 6 && proven_u16str_view_tokens_next(&tk, &f[n])) ++n;
        PROVEN_TEST_ASSERT(n == 2 && same(f[0], V("a")) && same(f[1], V("b")), "runs of the set are one separation, no empty token", "");
        proven_u16str_view_tokens_t a = proven_u16str_view_tokens(V("x y"), V(" ")), b = a;
        PROVEN_TEST_ASSERT(proven_u16str_view_tokens_next(&a, &f[0]) && proven_u16str_view_tokens_next(&b, &f[1]) && same(f[0], f[1]),
                           "two iterations run independently", "");
        proven_u16str_view_tokens_t whole = proven_u16str_view_tokens(V("ab"), V(""));
        PROVEN_TEST_ASSERT(proven_u16str_view_tokens_next(&whole, &f[0]) && same(f[0], V("ab")) && !proven_u16str_view_tokens_next(&whole, &f[0]),
                           "an empty set: the whole as one token", "");
        proven_u16str_view_tokens_t none = proven_u16str_view_tokens(V(" ; "), V("; "));
        PROVEN_TEST_ASSERT(!proven_u16str_view_tokens_next(&none, &f[0]), "only separators: no token", "");
    }

    PROVEN_TEST_SECTION("integers", "base 10 and 16, the whole view or a prefix, overflow refused.", "");
    {
        proven_u64 u = 0;
        proven_i64 i = 0;
        proven_u32 u32 = 0;
        proven_i32 i32 = 0;
        proven_size_t used = 0;
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("18446744073709551615"), 10, NULL, &u) == PROVEN_OK && u == UINT64_MAX, "u64 max", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("18446744073709551616"), 10, NULL, &u) == PROVEN_ERR_OVERFLOW, "u64 max + 1", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("-1"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG, "no sign on an unsigned value", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V(" 1"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG, "no whitespace skipped", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("12x"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG, "the whole view, with used NULL", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("12x"), 10, &used, &u) == PROVEN_OK && u == 12 && used == 2, "a prefix, with used", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("0xFf"), 16, NULL, &u) == PROVEN_OK && u == 255, "hex with 0x", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("0xg"), 16, &used, &u) == PROVEN_OK && u == 0 && used == 1, "0x taken only before a hex digit", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("ff"), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG &&
                           proven_u16str_view_parse_u64(V("1"), 8, NULL, &u) == PROVEN_ERR_INVALID_ARG &&
                           proven_u16str_view_parse_u64(V(""), 10, NULL, &u) == PROVEN_ERR_INVALID_ARG, "no digit, another base", "");
        u = 7;
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u64(V("x"), 10, NULL, &u) != PROVEN_OK && u == 7, "*out untouched on error", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_i64(V("-9223372036854775808"), 10, NULL, &i) == PROVEN_OK && i == INT64_MIN &&
                           proven_u16str_view_parse_i64(V("9223372036854775808"), 10, NULL, &i) == PROVEN_ERR_OVERFLOW &&
                           proven_u16str_view_parse_i64(V("+42"), 10, NULL, &i) == PROVEN_OK && i == 42 &&
                           proven_u16str_view_parse_i64(V("-0x10"), 16, NULL, &i) == PROVEN_OK && i == -16, "i64", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_u32(V("4294967295"), 10, NULL, &u32) == PROVEN_OK && u32 == UINT32_MAX &&
                           proven_u16str_view_parse_u32(V("4294967296"), 10, NULL, &u32) == PROVEN_ERR_OVERFLOW, "u32", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_parse_i32(V("-2147483648"), 10, NULL, &i32) == PROVEN_OK && i32 == INT32_MIN &&
                           proven_u16str_view_parse_i32(V("2147483648"), 10, NULL, &i32) == PROVEN_ERR_OVERFLOW &&
                           proven_u16str_view_parse_i32(V("-2147483649"), 10, NULL, &i32) == PROVEN_ERR_OVERFLOW, "i32", "");
    }

    PROVEN_TEST_SECTION("copy_to", "Always ends with a 0 unit; a cut is OUT_OF_BOUNDS.", "");
    {
        proven_u16 buf[4] = { 9, 9, 9, 9 };
        proven_result_size_t r = proven_u16str_view_copy_to(buf, 4, V("abc"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 3 && buf[3] == 0 && buf[0] == 'a', "it fits", "");
        r = proven_u16str_view_copy_to(buf, 4, V("abcd"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == 3 && buf[3] == 0, "cut at a unit", "");
        static const proven_u16 pair[] = { 'a', 'b', 0xD83D, 0xDE00 };
        r = proven_u16str_view_copy_to(buf, 4, (proven_u16str_view_t){ pair, 4 });
        PROVEN_TEST_ASSERT(r.value == 3 && buf[2] == 0xD83D, "a surrogate pair is two units: it may be cut", "");
        r = proven_u16str_view_copy_to(buf, 0, V("a"));
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == 0, "cap 0", "");
        r = proven_u16str_view_copy_to(buf, 1, V(""));
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && buf[0] == 0, "an empty view: just the 0", "");
    }

    PROVEN_TEST_SECTION("an owned string as a view", "proven_u16str_as_view.", "");
    {
        proven_result_u16str_t s = proven_u16str_create_from_view(proven_heap_allocator(), V("x.y"));
        PROVEN_TEST_ASSERT(proven_is_ok(s.err), "created", "");
        PROVEN_TEST_ASSERT(proven_u16str_view_find_unit(proven_u16str_as_view(&s.value), 0, u'.') == 1, "a view of it", "");
        proven_u16str_destroy(proven_heap_allocator(), &s.value);
        PROVEN_TEST_ASSERT(proven_u16str_as_view(NULL).size == 0, "NULL is empty", "");
    }

    PROVEN_TEST_PASS("the 16-bit view vocabulary keeps u8str.h's rules");
    return 0;
}

#else
int main(void) { return 0; }
#endif
