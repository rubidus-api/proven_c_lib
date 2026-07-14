#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from the contract in include/proven/random.h before the hook existed
 * (docs/TESTING.md §5.1).
 *
 * The library could get entropy from an operating system and from nowhere else. That is fine
 * until the target has no operating system - which is exactly the target the cryptographic
 * generator was added for. A board HAS real entropy: an on-chip TRNG, a ring oscillator, the
 * noise floor of an ADC. The library cannot know where, and had no way to be told, so
 * proven_random_bytes and the ChaCha seeding were compiled out of a freestanding build
 * entirely and the whole "ChaCha works on bare metal" story stopped one step short of being
 * usable.
 *
 * So the entropy source becomes a thing you can install: the OS is the default on a hosted
 * target, and a board installs its own. The properties that matter are the ones below, and the
 * sharpest is the last one: a source that FAILS must leave the generator inert, because the
 * failure of an entropy source is the one failure in this module that a caller can neither
 * detect afterwards nor recover from.
 */

/* A deterministic stand-in for a board's TRNG: it just counts. Real entropy it is not - which
 * is the point: the library cannot tell, and does not pretend to. It only has to prove the
 * bytes came from HERE. */
static bool counting_source(void *ctx, void *buf, proven_size_t len) {
    unsigned char *seq = (unsigned char *)ctx;
    unsigned char *p = (unsigned char *)buf;
    for (proven_size_t i = 0; i < len; ++i) p[i] = (*seq)++;
    return true;
}

static bool failing_source(void *ctx, void *buf, proven_size_t len) {
    (void)ctx; (void)buf; (void)len;
    return false;   /* a board whose TRNG is not ready, or has no entropy left */
}

static int g_calls = 0;
static bool counting_calls(void *ctx, void *buf, proven_size_t len) {
    (void)ctx;
    ++g_calls;
    memset(buf, 0xA5, len);
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("the entropy source is a thing you can install",
        "The OS is the default on a hosted target; a bare-metal target installs its own hardware source. Both feed proven_random_bytes and the ChaCha seeding, and a source that fails leaves the generator inert rather than plausible.",
        "Inspect proven_random_set_source and the source dispatch in src/proven/random.c. The OS PAL must be the default, and it must be replaceable without any change to the callers above it.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the OS source is installed by default on a hosted target",
        "You should not have to call anything to get the operating system's CSPRNG. That is what 'hosted' means.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t buf[64];
        memset(buf, 0, sizeof buf);
        PROVEN_TEST_ASSERT(proven_random_bytes(buf, sizeof buf),
            "with nothing installed, the OS CSPRNG must already be there", "");

        bool all_zero = true;
        for (proven_size_t i = 0; i < sizeof buf; ++i) if (buf[i] != 0) all_zero = false;
        PROVEN_TEST_ASSERT(!all_zero, "and it must actually produce bytes", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an installed source replaces it, and the bytes provably come from there",
        "This is the bare-metal case: a board's TRNG, handed to the library, feeding everything above it unchanged.",
        "The stand-in counts, so the bytes it produced are unmistakable.");
    // ---------------------------------------------------------------
    {
        unsigned char seq = 0;
        proven_random_set_source(counting_source, &seq);

        proven_byte_t buf[8];
        PROVEN_TEST_ASSERT(proven_random_bytes(buf, sizeof buf), "the installed source must be used", "");
        PROVEN_TEST_ASSERT(buf[0] == 0 && buf[1] == 1 && buf[7] == 7,
            "the bytes must come from the installed source, not the OS",
            "If these are not 0,1,...,7 the hook is not wired into proven_random_bytes.");

        /* And it feeds the u64 convenience too. */
        proven_u64 v = proven_random_u64();
        PROVEN_TEST_ASSERT(v != 0, "proven_random_u64 must draw from the installed source", "");

        proven_random_set_source(NULL, NULL);   /* back to the platform default */
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("NULL puts the platform default back",
        "Installing a source must not be a one-way door.",
        "");
    // ---------------------------------------------------------------
    {
        proven_byte_t a[16], b[16];
        PROVEN_TEST_ASSERT(proven_random_bytes(a, sizeof a) && proven_random_bytes(b, sizeof b),
            "the OS source must be back", "");
        PROVEN_TEST_ASSERT(memcmp(a, b, sizeof a) != 0,
            "and it is the real OS CSPRNG again, not the counter", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the cryptographic generator seeds from whatever source is installed",
        "This is the whole point on a board: a few hundred bytes of hardware entropy become an endless keystream that needs nothing further.",
        "");
    // ---------------------------------------------------------------
    {
        g_calls = 0;
        proven_random_set_source(counting_calls, NULL);

        proven_chacha_rng_t g;
        PROVEN_TEST_ASSERT(proven_chacha_rng_seed_from_entropy(&g),
            "seeding must succeed from the installed source", "");
        PROVEN_TEST_ASSERT(g_calls == 1,
            "and it must draw its seed from that source exactly once - not per byte", "");

        proven_rng_t rng = proven_chacha_rng(&g);
        PROVEN_TEST_ASSERT(proven_rng_is_valid(rng), "the seeded generator is a valid source", "");

        /* The seed was 32 bytes of 0xA5, so the stream is deterministic here - and it must be
         * a real ChaCha stream, not the seed echoed back. */
        proven_byte_t out[32];
        proven_chacha_rng_fill(&g, out, sizeof out);
        bool echoes_seed = true;
        for (proven_size_t i = 0; i < sizeof out; ++i) if (out[i] != 0xA5) echoes_seed = false;
        PROVEN_TEST_ASSERT(!echoes_seed, "the keystream must be ChaCha over the seed, not the seed", "");

        proven_random_set_source(NULL, NULL);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a source that FAILS leaves the generator inert, not plausible",
        "The failure of an entropy source is the one failure here a caller cannot detect afterwards. A board whose TRNG is not ready must not silently yield a generator that produces attacker-known bytes.",
        "This is the same guarantee the seeded marker gives an unseeded generator - it must hold for a FAILED seeding too.");
    // ---------------------------------------------------------------
    {
        proven_random_set_source(failing_source, NULL);

        proven_byte_t buf[16];
        memset(buf, 0x11, sizeof buf);
        PROVEN_TEST_ASSERT(!proven_random_bytes(buf, sizeof buf),
            "a failing source must be reported as a failure, not papered over", "");

        proven_chacha_rng_t g;
        memset(&g, 0xEE, sizeof g);   /* stack garbage, as a fresh frame holds */
        PROVEN_TEST_ASSERT(!proven_chacha_rng_seed_from_entropy(&g),
            "seeding from a failed source must return false", "");

        /* And the generator must be INERT - well past the first 64-byte block, where a zeroed
         * ChaCha state would otherwise start emitting a fixed, publicly derivable keystream. */
        proven_byte_t out[256];
        memset(out, 0, sizeof out);
        proven_chacha_rng_fill(&g, out, sizeof out);
        proven_size_t nonzero = 0;
        for (proven_size_t i = 0; i < sizeof out; ++i) if (out[i] != 0) nonzero++;
        PROVEN_TEST_ASSERT(nonzero == 0,
            "a generator whose seeding failed must yield zeros for every byte",
            "Plausible-looking bytes here are bytes an attacker can compute, handed out as a key.");

        PROVEN_TEST_ASSERT(!proven_rng_is_valid(proven_chacha_rng(&g)),
            "and it must not present itself as a valid source to the helpers", "");

        proven_random_set_source(NULL, NULL);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("len 0 and the argument guards",
        "",
        "");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(proven_random_bytes(NULL, 0), "zero bytes is a successful no-op", "");
        PROVEN_TEST_ASSERT(!proven_chacha_rng_seed_from_entropy(NULL),
            "seeding a null generator is false, not a crash", "");
    }

    PROVEN_TEST_PASS("the entropy source is installable, the OS is the default, and a failed source leaves nothing usable.");
    return 0;
}
