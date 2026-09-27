#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * proven_u8str_view_find_last against a brute-force oracle (docs/RFC-0003 §5).
 *
 * The oracle tries every start position from the end with memcmp: obviously correct and slow.
 * The implementation has three paths - a backward byte scan for one-byte needles, a backward
 * Shift-Or up to 64 bytes (the one piece of new algorithm in RFC-0003, which is why a table of
 * hand-picked rows is not enough), and repeated forward search beyond. Every path is driven here
 * over randomised haystacks and needles with a fixed seed, so a failure reproduces: small
 * alphabets so matches are dense, runs of one byte, the periodic "aab" pattern, needles taken
 * from the haystack itself so they are found, and needles of exactly 1, 64 and 65 bytes, the
 * path boundaries.
 */

static proven_size_t oracle(const unsigned char *h, size_t n, const unsigned char *nd, size_t m) {
    if (m == 0) return n;
    if (m > n) return PROVEN_INDEX_NOT_FOUND;
    for (size_t i = n - m + 1; i-- > 0;) {
        if (memcmp(h + i, nd, m) == 0) return i;
    }
    return PROVEN_INDEX_NOT_FOUND;
}

static unsigned long long g_state = 0x9E3779B97F4A7C15ull;
static unsigned rnd(unsigned bound) {
    g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17;
    return (unsigned)(g_state % bound);
}

int main(void) {
    PROVEN_TEST_SUITE("find_last against a brute-force oracle",
        "Randomised with a fixed seed across all three paths and their boundaries (needle length 1, 64, 65), dense alphabets, single-byte runs and periodic haystacks: the answer must equal the oracle's every time.",
        "Inspect proven_u8str_view_find_last in src/proven/u8str.c. The printed case names the path by needle length: 1 is the byte scan, 2-64 backward Shift-Or, 65+ repeated forward search.");

    static unsigned char hay[600];
    static unsigned char nd[80];
    unsigned long cases = 0, found = 0, mismatches = 0;

    for (int iter = 0; iter < 60000; ++iter) {
        size_t n = rnd(300) + (iter % 7 == 0 ? 300 : 0);
        unsigned alpha = 1 + rnd(4);                  /* 1..4 symbols: matches are dense */
        int shape = rnd(4);
        for (size_t i = 0; i < n; ++i) {
            if (shape == 0) hay[i] = (unsigned char)('a' + rnd(alpha));
            else if (shape == 1) hay[i] = 'a';                                   /* one long run */
            else if (shape == 2) hay[i] = (unsigned char)("aab"[i % 3]);         /* periodic */
            else hay[i] = (unsigned char)rnd(256);                              /* any byte */
        }
        size_t m;
        unsigned pick = rnd(10);
        if (pick == 0) m = 1;
        else if (pick == 1) m = 64;
        else if (pick == 2) m = 65;
        else m = 1 + rnd(pick < 6 ? 8 : 70);
        if (m > sizeof nd) m = sizeof nd;
        if (n >= m && rnd(2) == 0) {
            memcpy(nd, hay + rnd((unsigned)(n - m + 1)), m);                   /* present */
        } else {
            for (size_t i = 0; i < m; ++i) nd[i] = (unsigned char)(shape == 3 ? rnd(256) : 'a' + rnd(alpha + 1));
        }

        proven_u8str_view_t hv = { n ? hay : NULL, n };
        proven_u8str_view_t nv = { nd, m };
        proven_size_t want = oracle(hay, n, nd, m);
        proven_size_t got = proven_u8str_view_find_last(hv, nv);
        ++cases;
        if (want != PROVEN_INDEX_NOT_FOUND) ++found;
        if (got != want && mismatches++ < 5) {
            PROVEN_TEST_INFO("iter {} n {} m {} shape {}: got {} want {}", PROVEN_ARG(iter), PROVEN_ARG((unsigned long long)n),
                             PROVEN_ARG((unsigned long long)m), PROVEN_ARG(shape),
                             PROVEN_ARG((unsigned long long)got), PROVEN_ARG((unsigned long long)want));
        }
    }
    /* Printed so that a vacuous pass - nothing ever found - is visible. */
    PROVEN_TEST_INFO("{} cases, {} with an occurrence", PROVEN_ARG(cases), PROVEN_ARG(found));
    PROVEN_TEST_ASSERT(mismatches == 0, "find_last must agree with the oracle on every case", "");
    PROVEN_TEST_ASSERT(found > cases / 4, "the corpus must actually contain matches, or the agreement means little", "");

    PROVEN_TEST_PASS("find_last against a brute-force oracle");
    return 0;
}
