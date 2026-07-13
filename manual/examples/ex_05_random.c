#include "example.h"

/*
 * OS randomness, and the one honest thing it is for.
 *
 * proven_random_bytes is the operating system's CSPRNG - getrandom on Linux,
 * BCryptGenRandom on Windows - and nothing else. There is deliberately no seedable,
 * reproducible generator next to it: a fast PRNG for a simulation and a strong source for a
 * key are different tools, and handing out one under a name that suggests the other is how
 * people ship guessable tokens. This module does exactly one job: bytes you can build a
 * secret on. The return value is not decorative - false means the platform had no CSPRNG or
 * the OS call failed, and the buffer must not be used.
 */

int main(void) {
    /* Draw a 16-byte key ONCE, at startup. This is the intended use: the key that turns the
     * map's SipHash into a function an attacker who picks your keys still cannot predict. */
    proven_byte_t map_key[16];
    EXAMPLE_REQUIRE(proven_random_bytes(map_key, sizeof map_key),
                    "the OS must give us strong bytes on a hosted platform");

    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("session-token"));
    proven_u64 keyed = proven_hash_keyed(data, map_key);
    /* With a secret key the hash is unpredictable; without the key, the same bytes hash the
     * same unkeyed FNV every time - which is exactly why untrusted keys need the keyed one. */
    EXAMPLE_REQUIRE(keyed != proven_hash_bytes(data),
                    "a keyed hash is a different function from the unkeyed one");

    /* A single strong word, for a nonce or a one-off seed. Two draws differ - a fixed or
     * unseeded generator would repeat, which is the whole failure this guards against. */
    proven_u64 a = proven_random_u64();
    proven_u64 b = proven_random_u64();
    EXAMPLE_REQUIRE(a != b, "two independent draws must not be identical");

    /* Asking for nothing succeeds and touches nothing - a clean no-op, not an error. */
    EXAMPLE_REQUIRE(proven_random_bytes(NULL, 0), "zero bytes is a successful no-op");

    return EXAMPLE_OK();
}
