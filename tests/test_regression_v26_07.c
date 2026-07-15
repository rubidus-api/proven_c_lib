#include "proven/arena.h"
#include "proven/heap.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
#include "proven/time.h"
#include "proven/pool.h"
#include "proven_test.h"
#include <stdbool.h>

/*
 * v26.07 regressions. Each check below corresponds to a defect that shipped.
 *
 * The u8str checks use an arena over deliberately poisoned backing memory.
 * Allocators do not hand back zeroed memory, and on a quiet heap a fresh block
 * often happens to be zero - which is exactly why these defects went unnoticed.
 * Poisoning the storage makes the bug deterministic rather than lucky.
 */

static proven_byte_t poisoned[8192];

static proven_allocator_t poisoned_arena(proven_arena_t *a) {
    for (proven_size_t i = 0; i < sizeof poisoned; ++i) poisoned[i] = 0xAA;
    *a = proven_arena_create((proven_mem_mut_t){ .ptr = poisoned, .size = sizeof poisoned });
    return proven_arena_as_allocator(a);
}

int main(void) {
    PROVEN_TEST_SUITE("v26.07 regressions",
        "Protect the fixed u8str NUL-seal, datetime formatting, and pool init defects.",
        "Read the failing section name, then inspect only the area it names.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("u8str_reserve seals the terminator",
        "proven_u8str_as_cstr is documented as a zero-cost read, so every path that allocates must leave ptr[len] == 0.",
        "Inspect proven_u8str_reserve: reserving on a zero-initialized string performs the first allocation, and allocators do not return zeroed memory.");
    // ---------------------------------------------------------------
    {
        proven_arena_t a;
        proven_allocator_t A = poisoned_arena(&a);

        proven_u8str_t s = {0};   /* documented: zero-init is an owned, empty string */
        proven_err_t e = proven_u8str_reserve(A, &s, 32);
        PROVEN_TEST_ASSERT(proven_is_ok(e), "reserve failed", "");
        PROVEN_TEST_ASSERT(proven_u8str_is_valid(&s),
            "reserve left the string without its NUL seal",
            "proven_u8str_is_valid requires ptr[len] == 0; reserve must write it after allocating");
        PROVEN_TEST_ASSERT(proven_u8str_as_cstr(&s)[0] == '\0',
            "as_cstr on a reserved-but-empty string read uninitialized memory",
            "the terminator must be written by reserve, not left to chance");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a zero-output fmt grow seals the terminator",
        "A format that appends nothing still allocates; the string must remain readable as a C string.",
        "Inspect the growth branch of proven_u8str_fmt_internal: it must seal ptr[len] after reallocating, because the write pass may emit no bytes.");
    // ---------------------------------------------------------------
    {
        proven_arena_t a;
        proven_allocator_t A = poisoned_arena(&a);

        proven_u8str_t s = {0};
        proven_fmt_result_t r = proven_u8str_append_fmt_grow(A, &s, "");
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "zero-output fmt failed", "");
        PROVEN_TEST_ASSERT(proven_u8str_is_valid(&s),
            "a zero-output fmt left the string without its NUL seal",
            "the growth path must write ptr[len] = 0 itself; the write pass emits nothing here");
        PROVEN_TEST_ASSERT(proven_u8str_as_cstr(&s)[0] == '\0',
            "as_cstr after a zero-output fmt read uninitialized memory", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("append_grow of an EMPTY view seals the terminator",
        "The same disease, one function along: proven_u8str_append copies nothing and returns OK before it can seal, so the grow that just allocated the block left it unterminated.",
        "Inspect proven_u8str_append_grow (and proven_u16str_append_grow): the seal must happen where the block is allocated, not in the append that may return early. reserve() has always done it there.");
    // ---------------------------------------------------------------
    {
        proven_arena_t a;
        proven_allocator_t A = poisoned_arena(&a);

        proven_u8str_t s = {0};
        proven_err_t e = proven_u8str_append_grow(A, &s, (proven_u8str_view_t){ .ptr = NULL, .size = 0 });
        PROVEN_TEST_ASSERT(proven_is_ok(e), "appending nothing must succeed", "");
        PROVEN_TEST_ASSERT(proven_u8str_is_valid(&s),
            "appending an empty view left the string without its NUL seal",
            "as_cstr is documented as always terminated - 'guaranteed by internal structure'. It was reading past the end of a fresh heap block.");
        PROVEN_TEST_ASSERT(proven_u8str_as_cstr(&s)[0] == '\0',
            "as_cstr after an empty append read uninitialized memory", "");

        /* And the string must still work afterwards. */
        e = proven_u8str_append_grow(A, &s, PROVEN_LIT("ok"));
        PROVEN_TEST_ASSERT(proven_is_ok(e) && proven_u8str_view_eq(proven_u8str_as_view(&s), PROVEN_LIT("ok")),
            "and a real append after the empty one must still work", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("u16str append_grow seals its 2-byte terminator at the right UNIT index",
        "internal.len is a byte count; the u16 terminator's index is len/sizeof(u16), not len - the latter writes at twice the offset.",
        "Latent under the doubling growth policy (the stray write lands in unused slack and append re-seals the real NUL), but an out-of-bounds heap write the moment growth becomes exact-fit. This pins the invariant.");
    // ---------------------------------------------------------------
    {
        proven_arena_t a;
        proven_allocator_t A = poisoned_arena(&a);

        proven_u16str_t s = {0};
        proven_err_t e = proven_u16str_append_grow(A, &s, (proven_u16str_view_t){ .ptr = NULL, .size = 0 });
        PROVEN_TEST_ASSERT(proven_is_ok(e), "an empty u16 append must succeed", "");
        PROVEN_TEST_ASSERT(proven_u16str_len(&s) == 0 && proven_u16str_as_ptr(&s)[0] == 0,
            "a fresh empty u16 string is terminated at unit 0", "");

        /* A non-empty grow: the content must be intact and the terminator must sit at the
         * unit AFTER it (index len), which is where a reader looks - not at 2*len. */
        proven_u16 chars[3] = { 'H', 'i', '!' };
        e = proven_u16str_append_grow(A, &s, (proven_u16str_view_t){ .ptr = chars, .size = 3 });
        PROVEN_TEST_ASSERT(proven_is_ok(e), "a real u16 append must work", "");
        const proven_u16 *p16 = proven_u16str_as_ptr(&s);
        PROVEN_TEST_ASSERT(proven_u16str_len(&s) == 3 && p16[0] == 'H' && p16[1] == 'i' && p16[2] == '!',
            "the content must be intact", "");
        PROVEN_TEST_ASSERT(p16[3] == 0,
            "and the 2-byte terminator must sit at unit index len (3), where a reader expects it",
            "The seal index used to be the BYTE length, placing it at unit 2*len.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a negative datetime year formats as a negative number",
        "dt.year is signed. Casting it to unsigned long long turned -1 into twenty digits, which overran the twenty-byte conversion scratch.",
        "Inspect the PROVEN_ARG_DATETIME case in render_arg: the year must be rendered with its sign, and itoa_raw needs room for 20 digits plus a NUL.");
    // ---------------------------------------------------------------
    {
        proven_allocator_t heap = proven_heap_allocator();
        proven_u8str_t s = {0};
        proven_datetime_t dt = { .year = -1, .month = 1, .day = 1 };
        proven_fmt_result_t r = proven_u8str_append_fmt_grow(heap, &s, "{}", PROVEN_ARG(dt));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "formatting a negative year failed", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_u8str_as_view(&s), PROVEN_LIT("-0001-01-01 00:00:00")),
            "a negative year did not render as a negative number",
            "an unsigned reinterpretation of the year renders 18446744073709551615 and overflows the scratch buffer");
        proven_u8str_destroy(heap, &s);

        /* INT32_MIN is the case a naive negation gets wrong. */
        proven_u8str_t s2 = {0};
        proven_datetime_t dt2 = { .year = -2147483647 - 1, .month = 12, .day = 31 };
        r = proven_u8str_append_fmt_grow(heap, &s2, "{}", PROVEN_ARG(dt2));
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "formatting INT32_MIN as a year failed", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_u8str_as_view(&s2), PROVEN_LIT("-2147483648-12-31 00:00:00")),
            "INT32_MIN did not render correctly",
            "negate through a wider signed type so the magnitude stays representable");
        proven_u8str_destroy(heap, &s2);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a failed pool init leaves no capacity claim behind",
        "A pool whose bin allocation failed must not claim slots it does not have.",
        "Inspect proven_pool_init: bin_cap must be published only after the bin exists, or the free trait writes through a null bin.");
    // ---------------------------------------------------------------
    {
        /* An arena far too small for the requested bin: the bin allocation fails. */
        static proven_byte_t tiny_backing[16];
        proven_arena_t tiny = proven_arena_create((proven_mem_mut_t){ .ptr = tiny_backing, .size = sizeof tiny_backing });
        proven_allocator_t T = proven_arena_as_allocator(&tiny);

        proven_pool_t pool = {0};
        proven_err_t e = proven_pool_init(&pool, T, 64, alignof(max_align_t), 4096);
        PROVEN_TEST_ASSERT(!proven_is_ok(e), "the fixture must actually exhaust the arena", "grow the requested bin_cap if this trips");
        PROVEN_TEST_ASSERT(pool.bin == 0, "a failed init must not leave a bin pointer", "");
        PROVEN_TEST_ASSERT(pool.bin_cap == 0,
            "a failed init left bin_cap claiming slots that do not exist",
            "the free trait tests bin_len < bin_cap and then writes bin[bin_len]: with bin == NULL that is a null write");
    }

    PROVEN_TEST_PASS("v26.07 regressions held.");
    return 0;
}
