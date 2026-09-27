#include "example.h"
#include <string.h>

/*
 * The program thinks in UTF-8; a Windows "wide" call wants UTF-16. utf.h is the
 * crossing, in both directions, and it is strict: text that is not valid UTF-8
 * or UTF-16 is refused, never repaired. A converter that quietly swaps a bad
 * byte for a question mark has changed a file name you were about to open.
 *
 * Three shapes, as everywhere in this library: measure-then-convert into a
 * buffer you own (all or nothing), grow an owned string (all or nothing), and a
 * partial form for text that arrives in pieces.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A Korean file name ("report.txt": three Hangul syllables and ".txt"), as the rest
     * of the program holds it: UTF-8. */
    proven_u8str_view_t name = PROVEN_LIT("\xEB\xB3\xB4\xEA\xB3\xA0\xEC\x84\x9C.txt");

    /* --- measure, then convert into your own buffer ------------------------ */

    /* Ask first: the answer is in code units, and it also validates. Nine bytes
     * of Hangul are three code units here, not nine. */
    proven_result_size_t need = proven_utf8_to_utf16_size(name);
    EXAMPLE_REQUIRE(proven_is_ok(need.err) && need.value == 7, "3 syllables + \".txt\" are 7 code units");

    /* One more unit for the NUL a wide API expects; the converter writes none. */
    proven_u16 wide[16];
    proven_size_t units = 0;
    proven_err_t err = proven_utf8_to_utf16(name, wide, 15, &units);
    EXAMPLE_REQUIRE(proven_is_ok(err) && units == 7, "the name converts whole");
    wide[units] = 0;
    EXAMPLE_REQUIRE(wide[0] == 0xBCF4, "the first code unit is the syllable, not its first byte");
    /* On Windows: CreateFileW((LPCWSTR)wide, ...). Not called here, so this runs everywhere. */

    /* And back: a name a wide API returned, as UTF-8 for everything else. */
    proven_result_size_t back = proven_utf16_to_utf8_size(wide, units);
    EXAMPLE_REQUIRE(proven_is_ok(back.err) && back.value == name.size, "the reverse size is the original byte count");
    proven_byte_t bytes[32];
    proven_size_t nbytes = 0;
    err = proven_utf16_to_utf8(wide, units, bytes, sizeof bytes, &nbytes);
    EXAMPLE_REQUIRE(proven_is_ok(err) && nbytes == name.size && memcmp(bytes, name.ptr, nbytes) == 0,
                    "the round trip gives the same bytes");

    /* --- strict: malformed text is refused, and nothing is written ------- */

    /* A lone continuation byte, as a truncated or mis-decoded name would have. */
    proven_u8str_view_t broken = PROVEN_LIT("bad\x80.txt");
    wide[0] = 0x1234;
    err = proven_utf8_to_utf16(broken, wide, 15, &units);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING, "a stray continuation byte is refused");
    EXAMPLE_REQUIRE(units == 0 && wide[0] == 0x1234, "and the output is untouched");

    /* --- grow an owned string ---------------------------------------------- */

    proven_result_u16str_t r = proven_u16str_create(alloc, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the wide path must succeed");
    proven_u16str_t path = r.value;
    err = proven_utf8_append_to_u16str(alloc, &path, PROVEN_LIT("C:\\reports\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory appends");
    err = proven_utf8_append_to_u16str(alloc, &path, name);
    EXAMPLE_REQUIRE(proven_is_ok(err) && proven_u16str_len(&path) == 11 + 7, "and the name, growing the string");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&path)[18] == 0, "still NUL-terminated for the system call");

    /* A failed append changes nothing: no half-converted tail. */
    err = proven_utf8_append_to_u16str(alloc, &path, broken);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING && proven_u16str_len(&path) == 18, "a refused append leaves the path as it was");

    proven_result_u8str_t r8 = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r8.err), "creating the UTF-8 copy must succeed");
    proven_u8str_t copy = r8.value;
    err = proven_utf16_append_to_u8str(alloc, &copy, proven_u16str_as_ptr(&path), proven_u16str_len(&path));
    EXAMPLE_REQUIRE(proven_is_ok(err) && copy.internal.len == 11 + name.size, "the whole path comes back as UTF-8");

    /* --- partial: text that arrives in pieces ------------------------------ */

    /* A reader hands over 4 bytes at a time, and the second syllable is split after its first
     * byte. The partial form converts what is whole, stops with NEED_MORE, and
     * says how much it used; the caller keeps the rest and adds the next piece. */
    proven_u16 out[8];
    proven_utf_step_t st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr, 4 }, out, 8);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 3 && st.written == 1,
                    "one whole syllable, and the start of the next kept back");
    st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr + st.consumed, 6 }, out + 1, 7);
    EXAMPLE_REQUIRE(proven_is_ok(st.err) && st.written == 2 && out[1] == 0xACE0, "with the next piece it completes");

    /* The same distinction in the other direction: a high surrogate at the end
     * may be half of a pair whose other half is in the next piece. */
    const proven_u16 emoji_half[] = { 'o', 'k', 0xD83D };
    proven_byte_t u8out[16];
    st = proven_utf16_to_utf8_partial(emoji_half, 3, u8out, sizeof u8out);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 2, "a trailing high surrogate waits");

    printf("converted a %zu-byte name to %zu code units and back\n", (size_t)name.size, (size_t)need.value);

    proven_u8str_destroy(alloc, &copy);
    proven_u16str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
