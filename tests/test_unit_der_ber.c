#include "proven.h"
#include "proven_test.h"
#include <string.h>

/* One conversion: the error, and on PROVEN_OK the bytes against `want`. */
static bool conv(const proven_byte_t *in, proven_size_t n, proven_err_t want_err, const proven_byte_t *want, proven_size_t want_n) {
    proven_byte_t out[600];
    proven_size_t len = 12345;
    proven_err_t e = proven_der_from_ber((proven_mem_view_t){ in, n }, (proven_mem_mut_t){ out, sizeof out }, &len);
    if (e != want_err) return false;
    if (e != PROVEN_OK) return true;
    return len == want_n && memcmp(out, want, want_n) == 0;
}

#define B(...) ((const proven_byte_t[]){ __VA_ARGS__ }), sizeof((const proven_byte_t[]){ __VA_ARGS__ })

int main(void) {
    PROVEN_TEST_SUITE("der.h: BER to DER, the rewrite PKCS#12 files need",
        "RFC-0044: indefinite lengths made definite, lengths shortest, constructed OCTET STRINGs joined; everything else as it was; the inputs refused are those of the converter rubrapack used before.",
        "A failure names the case. Inspect proven_der_from_ber at the end of crypto/src/der.c; byte-identity with rubrapack's converter is the private ber-diff check.");

    PROVEN_TEST_SECTION("what is rewritten", "Indefinite lengths, long lengths, constructed OCTET STRINGs.", "");
    {
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x01, 0x41), PROVEN_OK, B(0x04, 0x01, 0x41)), "DER is left as it is", "");
        PROVEN_TEST_ASSERT(conv(B(0x30, 0x80, 0x02, 0x01, 0x05, 0x00, 0x00), PROVEN_OK, B(0x30, 0x03, 0x02, 0x01, 0x05)), "an indefinite length", "");
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x82, 0x00, 0x01, 0x41), PROVEN_OK, B(0x04, 0x01, 0x41)), "a long length made short", "");
        PROVEN_TEST_ASSERT(conv(B(0x30, 0x80, 0x24, 0x80, 0x04, 0x02, 'a', 'b', 0x04, 0x01, 'c', 0x00, 0x00, 0x00, 0x00), PROVEN_OK,
                                B(0x30, 0x05, 0x04, 0x03, 'a', 'b', 'c')), "a constructed OCTET STRING joined", "");
        PROVEN_TEST_ASSERT(conv(B(0x24, 0x09, 0x24, 0x80, 0x04, 0x01, 'x', 0x00, 0x00, 0x04, 0x00), PROVEN_OK, B(0x04, 0x01, 'x')),
                           "pieces constructed themselves, and an empty piece", "");
        PROVEN_TEST_ASSERT(conv(B(0xa0, 0x80, 0x31, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00), PROVEN_OK, B(0xa0, 0x04, 0x31, 0x02, 0x05, 0x00)),
                           "context and SET tags kept", "");
        PROVEN_TEST_ASSERT(conv(B(0x31, 0x06, 0x02, 0x01, 0x02, 0x02, 0x01, 0x01), PROVEN_OK, B(0x31, 0x06, 0x02, 0x01, 0x02, 0x02, 0x01, 0x01)),
                           "a SET's order is not touched", "");
        PROVEN_TEST_ASSERT(conv(B(0x23, 0x80, 0x03, 0x01, 0x00, 0x00, 0x00), PROVEN_OK, B(0x23, 0x03, 0x03, 0x01, 0x00)),
                           "a constructed BIT STRING stays constructed", "");
    }

    PROVEN_TEST_SECTION("what is refused", "PROVEN_ERR_INVALID_FORMAT for each.", "");
    {
        const proven_err_t F = PROVEN_ERR_INVALID_FORMAT;
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x01, 0x41, 0x00), F, NULL, 0), "bytes after the element", "");
        PROVEN_TEST_ASSERT(conv(B(0x1f, 0x01, 0x00), F, NULL, 0), "a high-tag-number tag", "");
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x80, 0x00, 0x00), F, NULL, 0), "an indefinite primitive", "");
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x85, 0, 0, 0, 0, 1, 0x41), F, NULL, 0), "a length of 5 bytes", "");
        PROVEN_TEST_ASSERT(conv(B(0x04, 0x02, 0x41), F, NULL, 0), "a length past the input", "");
        PROVEN_TEST_ASSERT(conv(B(0x30, 0x80, 0x05, 0x00), F, NULL, 0), "no end-of-contents", "");
        PROVEN_TEST_ASSERT(conv(B(0x24, 0x80, 0x05, 0x00, 0x00, 0x00), F, NULL, 0), "a piece that is not an OCTET STRING", "");
        PROVEN_TEST_ASSERT(conv(B(0x04), F, NULL, 0), "one byte", "");
        PROVEN_TEST_ASSERT(conv(NULL, 0, F, NULL, 0), "nothing", "");
    }

    PROVEN_TEST_SECTION("depth", "The outermost is 0; PROVEN_DER_BER_MAX_DEPTH levels below it are read, one more is not.", "");
    {
        enum { MAXD = PROVEN_DER_BER_MAX_DEPTH };
        proven_byte_t in[2 * (MAXD + 2) + 2 * (MAXD + 2)];
        proven_byte_t out[600];
        proven_size_t len = 0;
        for (int levels = MAXD + 1; levels <= MAXD + 2; ++levels) {
            proven_size_t n = 0;
            for (int i = 0; i < levels - 1; ++i) { in[n++] = 0x30; in[n++] = 0x80; }
            in[n++] = 0x05; in[n++] = 0x00;
            for (int i = 0; i < levels - 1; ++i) { in[n++] = 0x00; in[n++] = 0x00; }
            proven_err_t e = proven_der_from_ber((proven_mem_view_t){ in, n }, (proven_mem_mut_t){ out, sizeof out }, &len);
            if (levels == MAXD + 1) PROVEN_TEST_ASSERT(e == PROVEN_OK, "depth PROVEN_DER_BER_MAX_DEPTH is read", "");
            else PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "one deeper is refused", "");
        }
    }

    PROVEN_TEST_SECTION("the caller's buffer", "Measuring, too small, longer than the input.", "");
    {
        const proven_byte_t in[] = { 0x30, 0x80, 0x02, 0x01, 0x05, 0x00, 0x00 };
        proven_byte_t out[5];
        proven_size_t len = 0;
        PROVEN_TEST_ASSERT(proven_der_from_ber((proven_mem_view_t){ in, sizeof in }, (proven_mem_mut_t){ NULL, 0 }, &len) == PROVEN_ERR_OUT_OF_BOUNDS && len == 5,
                           "an empty buffer measures", "");
        PROVEN_TEST_ASSERT(proven_der_from_ber((proven_mem_view_t){ in, sizeof in }, (proven_mem_mut_t){ out, 4 }, &len) == PROVEN_ERR_OUT_OF_BOUNDS && len == 5,
                           "too small: the size it needs", "");
        PROVEN_TEST_ASSERT(proven_der_from_ber((proven_mem_view_t){ in, sizeof in }, (proven_mem_mut_t){ out, 5 }, &len) == PROVEN_OK && len == 5,
                           "exactly enough", "");
        PROVEN_TEST_ASSERT(proven_der_from_ber((proven_mem_view_t){ in, sizeof in }, (proven_mem_mut_t){ out, 5 }, NULL) == PROVEN_ERR_INVALID_ARG &&
                           proven_der_from_ber((proven_mem_view_t){ NULL, 3 }, (proven_mem_mut_t){ out, 5 }, &len) == PROVEN_ERR_INVALID_ARG,
                           "NULL len, an ill-formed view", "");

        /* 70000 bytes of content under an indefinite SEQUENCE: 4 header bytes in, 5 out (30 83 ..). */
        static proven_byte_t big[2 + 5 + 70000 + 2];
        static proven_byte_t bigout[sizeof big + 8];
        const proven_size_t n = sizeof big;
        big[0] = 0x30; big[1] = 0x80;
        big[2] = 0x04; big[3] = 0x83; big[4] = 0x01; big[5] = 0x11; big[6] = 0x70;   /* 70000 = 0x011170; zero content */
        big[n - 2] = 0x00; big[n - 1] = 0x00;
        proven_err_t e = proven_der_from_ber((proven_mem_view_t){ big, n }, (proven_mem_mut_t){ bigout, sizeof bigout }, &len);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && len == n + 1 && bigout[0] == 0x30 && bigout[1] == 0x83, "the DER is a byte longer than the BER", "");
        proven_mem_view_t rd = { bigout, len };
        proven_der_item_t item;
        PROVEN_TEST_ASSERT(proven_der_read(&rd, &item) == PROVEN_OK && rd.size == 0 && item.tag == PROVEN_DER_SEQUENCE, "and proven_der_read reads it", "");
    }

    PROVEN_TEST_PASS("BER to DER: the rewrite and the refusals as documented");
    return 0;
}
