#include "proven/fmt.h"
#include "proven/u16str.h"
#include "proven/utf.h"
#include "proven_test.h"

/* utf.h without u16str.h's string type: the transcoder works on raw proven_u16 arrays, so it
 * must build, link and convert with PROVEN_NO_U16STR (the freestanding guide says so). */
int main(void) {
    proven_u16 out[4];
    proven_size_t n = 0;
    proven_err_t e = proven_utf8_to_utf16((proven_u8str_view_t){ (const proven_byte_t *)"A\xEA\xB0\x80", 4 }, out, 4, &n);
    PROVEN_TEST_ASSERT(proven_is_ok(e) && n == 2 && out[0] == 'A' && out[1] == 0xAC00,
        "utf.h converts without the u16 string type", "");
    PROVEN_TEST_PASS("test_portability_compile_nou16str (Successfully linked with PROVEN_NO_U16STR)");
    return 0;
}
