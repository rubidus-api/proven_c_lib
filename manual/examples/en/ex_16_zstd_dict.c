#include "example.h"
#include <string.h>

/*
 * Zstandard with a dictionary: short messages that look alike, each its own frame, both sides
 * starting from the same past. Here the dictionary is raw - a few typical messages, as is - which
 * needs no training; `zstd --train` makes formatted ones, used the same way.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static const char dict[] =
        "{\"event\": \"login\", \"user\": \"alice\", \"ok\": true, \"region\": \"eu-west\"}\n"
        "{\"event\": \"logout\", \"user\": \"bob\", \"ok\": true, \"region\": \"us-east\"}\n"
        "{\"event\": \"purchase\", \"user\": \"carol\", \"ok\": false, \"region\": \"ap-south\"}\n";
    static const char msg[] = "{\"event\": \"login\", \"user\": \"dave\", \"ok\": false, \"region\": \"us-east\"}\n";
    const proven_mem_view_t dv = { (const proven_byte_t *)dict, sizeof dict - 1 };
    EXAMPLE_REQUIRE(proven_zstd_dictionary_id(dv) == 0, "a raw dictionary has no ID");

    /* One message with the dictionary, and without. */
    static proven_byte_t frame[256], plain[256], back[256];
    proven_size_t n = 0, p = 0;
    proven_zstd_encoder_options_t o = { .alloc = heap, .level = 3, .dictionary = dv };
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ (const proven_byte_t *)msg, sizeof msg - 1 }, (proven_mem_mut_t){ frame, sizeof frame }, &n) == PROVEN_OK,
                    "a message compressed with the dictionary");
    o.dictionary = (proven_mem_view_t){ NULL, 0 };
    EXAMPLE_REQUIRE(proven_zstd_encode_all(&o, (proven_mem_view_t){ (const proven_byte_t *)msg, sizeof msg - 1 }, (proven_mem_mut_t){ plain, sizeof plain }, &p) == PROVEN_OK,
                    "and without it");
    EXAMPLE_REQUIRE(n * 2 < p, "with it, the 73 bytes take less than half of what they take without");

    /* The other side: the same dictionary, set on a decoder once, for every frame after. */
    proven_zstd_decoder_t *z = NULL;
    EXAMPLE_REQUIRE(proven_zstd_decoder_create(heap, 0, &z) == PROVEN_OK && proven_zstd_decoder_use_dictionary(z, dv) == PROVEN_OK, "a decoder with the dictionary");
    for (int round = 0; round < 2; ++round) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        EXAMPLE_REQUIRE(proven_zstd_decode(z, (proven_mem_view_t){ frame, n }, &used, (proven_mem_mut_t){ back, sizeof back }, &made, &done) == PROVEN_OK && done &&
                        made == sizeof msg - 1 && memcmp(back, msg, made) == 0, "the message back - and again, from the same decoder");
    }
    /* Without the dictionary there is no way back: the frame needs the same past. */
    EXAMPLE_REQUIRE(proven_zstd_decoder_use_dictionary(z, (proven_mem_view_t){ NULL, 0 }) == PROVEN_OK, "the dictionary taken away");
    proven_size_t used = 0, made = 0;
    bool done = false;
    const proven_err_t e = proven_zstd_decode(z, (proven_mem_view_t){ frame, n }, &used, (proven_mem_mut_t){ back, sizeof back }, &made, &done);
    EXAMPLE_REQUIRE(e != PROVEN_OK || made != sizeof msg - 1 || memcmp(back, msg, made) != 0, "a raw dictionary's frame names none, so it decodes to something else, or fails");
    proven_zstd_decoder_destroy(z);
    return EXAMPLE_OK();
}
