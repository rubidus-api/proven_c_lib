#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>

/*
 * Written from the contract in include/proven/map.h before the keyed hash was wired in
 * (docs/TESTING.md §5.1). The security property - an attacker cannot flood one bucket -
 * cannot be a known-answer test, because the whole point is a per-process secret nobody
 * knows the answer to. So the contract is stated as what IS observable through
 * proven_map_hash:
 *
 *   - a default (untrusted) string-key map does NOT hash with plain FNV - if it did, an
 *     attacker could compute collisions offline;
 *   - a trusted map DOES hash with FNV, because that is the fast path it opts into;
 *   - both still place and find keys correctly - a keyed hash that broke lookups would be
 *     safe and useless.
 *
 * The middle assertion is the one that lands red against the stub, which is exactly the
 * point: the stub still hashes strings with FNV, and the contract says the default must not.
 */

static proven_map_key_t skey(const char *s) {
    return (proven_map_key_t){ .str = { .ptr = (const proven_u8 *)s, .size = strlen(s) } };
}

int main(void) {
    PROVEN_TEST_SUITE("map: keyed hashing for untrusted string keys",
        "A default string-key map hashes with keyed SipHash (an attacker cannot predict the bucket); a trusted one keeps fast FNV; both look keys up correctly.",
        "Inspect the hash selection in src/proven/map.c and the per-process key it draws from proven_random_bytes.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a default string-key map does not hash with plain FNV",
        "If proven_map_hash equalled proven_hash_bytes, an attacker could compute colliding keys offline and flood one bucket.",
        "The default must be keyed; the stub still uses FNV, which is why this is the red assertion.");
    // ---------------------------------------------------------------
    {
        proven_result_map_t mr = proven_map_create(heap, 16, PROVEN_KEY_TYPE_U8_BORROWED, sizeof(int), alignof(int));
        PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup: a default map", "");
        proven_map_t m = mr.value;

        PROVEN_TEST_ASSERT(m.trusted_keys == false,
            "proven_map_create must produce an untrusted (keyed) map", "");

        /* For several keys, the map's hash must differ from unkeyed FNV. A keyed hash and
         * FNV agreeing on one key is a coincidence; agreeing on all of these is FNV. */
        const char *keys[] = { "", "a", "session-token", "/etc/passwd", "GET / HTTP/1.1", "\x00\x01\x02" };
        int differ = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
            proven_u64 mh = proven_map_hash(&m, skey(keys[i]));
            proven_u64 fnv = proven_hash_bytes((proven_mem_view_t){ (const proven_u8 *)keys[i], strlen(keys[i]) });
            if (mh != fnv) differ++;
        }
        PROVEN_TEST_ASSERT(differ >= 5,
            "a default map's string hash must differ from FNV for essentially every key",
            "If it matches FNV, the map is not keyed and the HashDoS defence is absent.");

        proven_map_destroy(&m);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a trusted map hashes with FNV, on purpose",
        "The opt-out exists so a program that only hashes its own keys keeps the fast path.",
        "proven_map_create_trusted flips one flag; proven_map_hash must then equal FNV.");
    // ---------------------------------------------------------------
    {
        proven_result_map_t mr = proven_map_create_trusted(heap, 16, PROVEN_KEY_TYPE_U8_BORROWED, sizeof(int), alignof(int));
        PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup: a trusted map", "");
        proven_map_t m = mr.value;

        PROVEN_TEST_ASSERT(m.trusted_keys == true,
            "proven_map_create_trusted must produce a trusted (FNV) map", "");

        const char *keys[] = { "a", "session-token", "/etc/passwd", "GET / HTTP/1.1" };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
            proven_u64 mh = proven_map_hash(&m, skey(keys[i]));
            proven_u64 fnv = proven_hash_bytes((proven_mem_view_t){ (const proven_u8 *)keys[i], strlen(keys[i]) });
            PROVEN_TEST_ASSERT(mh == fnv,
                "a trusted map's string hash must equal FNV-1a",
                "The trusted path exists precisely to be the fast, unkeyed FNV.");
        }

        proven_map_destroy(&m);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("both kinds place and find keys correctly",
        "A keyed hash is only worth having if the table still works. Correctness is not optional because the hash changed.",
        "");
    // ---------------------------------------------------------------
    {
        for (int trusted = 0; trusted <= 1; ++trusted) {
            proven_result_map_t mr = trusted
                ? proven_map_create_trusted(heap, 16, PROVEN_KEY_TYPE_U8_OWNED, sizeof(int), alignof(int))
                : proven_map_create(heap, 16, PROVEN_KEY_TYPE_U8_OWNED, sizeof(int), alignof(int));
            PROVEN_TEST_ASSERT(proven_is_ok(mr.err), "setup", "");
            proven_map_t m = mr.value;

            /* Insert 500 distinct string keys, read them all back, remove half, re-check. */
            char kb[32];
            for (int i = 0; i < 500; ++i) {
                int n = snprintf(kb, sizeof kb, "key-%d", i);
                proven_err_t e = proven_map_set(&m, (proven_map_key_t){ .str = { (const proven_u8 *)kb, (proven_size_t)n } }, &i);
                PROVEN_TEST_ASSERT(proven_is_ok(e), "every insert must succeed", "");
            }
            PROVEN_TEST_ASSERT(m.len == 500, "all 500 keys must be live", "");

            int found = 0;
            for (int i = 0; i < 500; ++i) {
                int n = snprintf(kb, sizeof kb, "key-%d", i);
                const int *v = proven_map_get(&m, (proven_map_key_t){ .str = { (const proven_u8 *)kb, (proven_size_t)n } });
                if (v && *v == i) found++;
            }
            PROVEN_TEST_ASSERT(found == 500, "every key must be found with its value", "");

            for (int i = 0; i < 500; i += 2) {
                int n = snprintf(kb, sizeof kb, "key-%d", i);
                proven_err_t e = proven_map_remove(&m, (proven_map_key_t){ .str = { (const proven_u8 *)kb, (proven_size_t)n } });
                PROVEN_TEST_ASSERT(proven_is_ok(e), "remove must succeed", "");
            }
            PROVEN_TEST_ASSERT(m.len == 250, "half must remain", "");

            for (int i = 1; i < 500; i += 2) {
                int n = snprintf(kb, sizeof kb, "key-%d", i);
                const int *v = proven_map_get(&m, (proven_map_key_t){ .str = { (const proven_u8 *)kb, (proven_size_t)n } });
                PROVEN_TEST_ASSERT(v && *v == i, "the surviving keys must still be found", "");
            }

            proven_map_destroy(&m);
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a malformed {NULL, size>0} key is handled the same on both kinds",
        "proven_map_hash does not validate its key, so a NULL pointer with a nonzero size must not crash on a trusted map when it does not on a default one.",
        "The trusted path used a duplicate internal FNV that dereferenced the pointer; it now uses proven_hash_bytes, which guards it - so the two agree.");
    // ---------------------------------------------------------------
    {
        proven_map_key_t bad = { .str = { .ptr = NULL, .size = 5 } };

        proven_result_map_t dr = proven_map_create(heap, 16, PROVEN_KEY_TYPE_U8_BORROWED, sizeof(int), alignof(int));
        proven_result_map_t tr = proven_map_create_trusted(heap, 16, PROVEN_KEY_TYPE_U8_BORROWED, sizeof(int), alignof(int));
        PROVEN_TEST_ASSERT(proven_is_ok(dr.err) && proven_is_ok(tr.err), "setup", "");

        /* Neither must dereference the NULL (UBSan would catch it); both treat it as empty. */
        proven_u64 dh = proven_map_hash(&dr.value, bad);
        proven_u64 th = proven_map_hash(&tr.value, bad);
        PROVEN_TEST_ASSERT(th == proven_hash_bytes((proven_mem_view_t){ NULL, 0 }),
            "a trusted map hashes a NULL/size-5 key as empty FNV, not a crash",
            "It used to call an internal FNV that read through the NULL pointer.");
        (void)dh;

        proven_map_destroy(&dr.value);
        proven_map_destroy(&tr.value);
    }

    PROVEN_TEST_PASS("string keys are keyed by default, fast when trusted, and correct either way.");
    return 0;
}
