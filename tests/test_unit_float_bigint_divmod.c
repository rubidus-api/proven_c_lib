#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "proven/float_config.h"
#include "../src/proven/float_decimal.h"

/* Largest operand limb count to exercise, kept within the configured capacity so
 * the test stays valid under reduced PROVEN_FLOAT_BIGINT_LIMBS embedded builds. */
#define DIVMOD_TEST_MAX_LIMBS \
    ((PROVEN_FLOAT_BIGINT_LIMBS > 32u) ? 30u : (PROVEN_FLOAT_BIGINT_LIMBS - 2u))

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect proven_float_bigint_divmod in src/proven/float_decimal.c (Knuth Algorithm D).\n");
        exit(1);
    }
}
#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

/* ---- independent reference helpers on little-endian base-2^64 limb arrays ---- */

static size_t trim(const uint64_t *a, size_t n) {
    while (n > 0 && a[n - 1] == 0) --n;
    return n;
}

/* dst[0..*dn) = a[0..an) * b[0..bn)  (schoolbook, base 2^64 via __uint128_t) */
static size_t ref_mul(const uint64_t *a, size_t an, const uint64_t *b, size_t bn, uint64_t *dst) {
    for (size_t i = 0; i < an + bn; ++i) dst[i] = 0;
    for (size_t i = 0; i < an; ++i) {
        unsigned __int128 carry = 0;
        for (size_t j = 0; j < bn; ++j) {
            unsigned __int128 cur = (unsigned __int128)a[i] * b[j] + dst[i + j] + carry;
            dst[i + j] = (uint64_t)cur;
            carry = cur >> 64;
        }
        dst[i + bn] += (uint64_t)carry;
    }
    return trim(dst, an + bn);
}

/* dst[0..*dn) = a + b  (a has an limbs, b has bn limbs) */
static size_t ref_add(const uint64_t *a, size_t an, const uint64_t *b, size_t bn, uint64_t *dst) {
    size_t n = an > bn ? an : bn;
    unsigned __int128 carry = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 cur = carry;
        if (i < an) cur += a[i];
        if (i < bn) cur += b[i];
        dst[i] = (uint64_t)cur;
        carry = cur >> 64;
    }
    if (carry) dst[n++] = (uint64_t)carry;
    return trim(dst, n);
}

static int ref_cmp(const uint64_t *a, size_t an, const uint64_t *b, size_t bn) {
    an = trim(a, an); bn = trim(b, bn);
    if (an != bn) return an < bn ? -1 : 1;
    for (size_t i = an; i > 0; --i) {
        if (a[i - 1] != b[i - 1]) return a[i - 1] < b[i - 1] ? -1 : 1;
    }
    return 0;
}

static uint64_t st = 0xc0ffee123456789aull;
static uint64_t rnd(void){ st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }

static void check_divmod(const uint64_t *num, size_t nlen, const uint64_t *den, size_t dlen) {
    uint64_t q[200], r[200], prod[400], recon[400];
    size_t qlen = 0, rlen = 0;
    bool ok = proven_float_bigint_divmod_u64(num, nlen, den, dlen, q, &qlen, r, &rlen);
    require(ok, "divmod should succeed for nonzero divisor");
    /* remainder < divisor */
    require(ref_cmp(r, rlen, den, dlen) < 0, "remainder must be strictly less than divisor");
    /* q*den + r == num */
    size_t plen = ref_mul(q, qlen, den, dlen, prod);
    size_t reclen = ref_add(prod, plen, r, rlen, recon);
    require(ref_cmp(recon, reclen, num, trim(num, nlen)) == 0, "q*den + r must reconstruct num");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_bigint_divmod",
        "Verify the shared big-integer division helper returns an exact quotient and remainder.",
        "Inspect proven_float_bigint_divmod (Knuth Algorithm D) in src/proven/float_decimal.c if reconstruction fails."
    );

    PROVEN_TEST_SECTION(
        "small and edge cases",
        "Check single-limb division, num<den, and exact multiples against a reference.",
        "Inspect the single-limb and num<den fast paths if these diverge."
    );
    {
        uint64_t n, d;
        n = 0; d = 1; check_divmod(&n, 1, &d, 1);
        n = 100; d = 7; check_divmod(&n, 1, &d, 1);
        n = 7; d = 100; check_divmod(&n, 1, &d, 1);            /* num < den */
        n = 0xffffffffffffffffull; d = 1; check_divmod(&n, 1, &d, 1);
        n = 0xffffffffffffffffull; d = 0xffffffffffffffffull; check_divmod(&n, 1, &d, 1);
        { uint64_t num2[2] = {0, 1}; uint64_t den1 = 2; check_divmod(num2, 2, &den1, 1); } /* 2^64 / 2 */
    }

    PROVEN_TEST_SECTION(
        "random single-limb vs C arithmetic",
        "Compare quotient and remainder against native 64-bit division.",
        "Inspect the n==1 division path if a single-limb case diverges."
    );
    for (int i = 0; i < 500000; ++i) {
        uint64_t a = rnd() >> (rnd() % 64);
        uint64_t b = rnd() >> (rnd() % 64);
        if (b == 0) b = 1;
        uint64_t q[2], r[2]; size_t ql, rl;
        require(proven_float_bigint_divmod_u64(&a, 1, &b, 1, q, &ql, r, &rl), "single-limb divmod");
        uint64_t qe = a / b, re = a % b;
        require((ql == 0 ? 0u : q[0]) == qe && (rl == 0 ? 0u : r[0]) == re, "single-limb quotient/remainder match C");
    }

    PROVEN_TEST_SECTION(
        "random multi-limb reconstruction",
        "Verify q*den + r == num and r < den for random multi-limb operands.",
        "Inspect normalization, qhat estimation, or the add-back step in Algorithm D if reconstruction fails."
    );
    for (int i = 0; i < 200000; ++i) {
        uint64_t num[40], den[40];
        size_t nlen = 1 + (rnd() % DIVMOD_TEST_MAX_LIMBS);
        size_t dlen = 1 + (rnd() % DIVMOD_TEST_MAX_LIMBS);
        for (size_t k = 0; k < nlen; ++k) num[k] = rnd() >> (rnd() % 64);
        for (size_t k = 0; k < dlen; ++k) den[k] = rnd() >> (rnd() % 64);
        if (den[dlen - 1] == 0) den[dlen - 1] = 1;
        if (trim(den, dlen) == 0) { den[0] = 1; dlen = 1; }
        check_divmod(num, nlen, den, dlen);
    }

    PROVEN_TEST_PASS("Big-integer divmod reconstruction checks passed.");
    return 0;
}
