#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_tls.h"
#include "test_unit_tls_pki.h"
#include "test_unit_tls_legacy_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The legacy TLS set below the state machine: AES-CBC, HMAC with the old hashes, the PRF of
 * TLS 1.0 and 1.1, CBC records, finite-field Diffie-Hellman, and RSA used for key exchange
 * and for the signatures of the old versions.
 *
 * The script in test_unit_tls_legacy_vectors.h is replayed line by line. It was computed
 * outside this library, by a generator that first reproduced the published values it knows
 * (SP 800-38A F.2.1 and F.2.5, RFC 2202, RFC 4231). The letters:
 *
 *   A key iv plain cipher                                AES-CBC, both directions
 *   H hash key message mac                               HMAC; hash is 0 MD5, 1 SHA-1, 2 SHA-256
 *   P secret label seed out                              the PRF of TLS 1.0 and 1.1
 *   C mac etm chained version key mackey iv seq type content explicit-iv record
 *                                                        a record: sealing gives exactly it, opening gives the content
 *   Y (the same nine) content record                     a record with more padding than needed: opens
 *   X (the same nine) record                             a spoiled record: does not open
 *   D prime exponent peer public shared                  Diffie-Hellman over one of the six groups
 */

static proven_byte_t g_buf[6][2100];

static proven_size_t unhex(const char *s, proven_size_t n, proven_byte_t *out) {
    if (n == 1 && s[0] == '-') return 0;
    for (proven_size_t i = 0; i + 1 < n; i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) { char c = s[i + (proven_size_t)k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        out[i / 2] = (proven_byte_t)v;
    }
    return n / 2;
}

/* The next space-separated field of a line. */
static const char *field(const char **at, proven_size_t *len) {
    const char *p = *at;
    while (*p == ' ') ++p;
    const char *start = p;
    while (*p && *p != ' ') ++p;
    *len = (proven_size_t)(p - start);
    *at = p;
    return start;
}

static unsigned long number(const char *s, proven_size_t n, int radix) {
    char tmp[32];
    memcpy(tmp, s, n < 31 ? n : 31);
    tmp[n < 31 ? n : 31] = 0;
    return strtoul(tmp, NULL, radix);
}

/* The nine fields that set a record's direction up. */
static bool record_setup(const char **at, proven_tls_keys_t *keys, proven_tls_cbc_t *cbc, proven_byte_t *type) {
    const char *f[9];
    proven_size_t n[9];
    proven_byte_t key[32];
    for (int i = 0; i < 9; ++i) f[i] = field(at, &n[i]);
    memset(keys, 0, sizeof *keys); memset(cbc, 0, sizeof *cbc);
    cbc->mac = (proven_u8)number(f[0], n[0], 10);
    cbc->mac_len = cbc->mac == PROVEN_TLS_LH_SHA1 ? 20 : cbc->mac == PROVEN_TLS_LH_SHA256 ? 32 : 48;
    cbc->etm = f[1][0] == '1'; cbc->chained = f[2][0] == '1';
    cbc->version = (proven_u16)number(f[3], n[3], 16);
    const proven_size_t kl = unhex(f[4], n[4], key);
    (void)unhex(f[5], n[5], cbc->mac_key);
    (void)unhex(f[6], n[6], cbc->iv);
    keys->seq = strtoull(f[7], NULL, 10);
    *type = (proven_byte_t)number(f[8], n[8], 10);
    keys->active = proven_crypto_aes_gcm_init(&keys->aes, key, kl);
    return keys->active;
}

static tp_stream_t g_stream = { .seed = "test_unit_tls_legacy bytes", .counter = 0 };

/* em^e mod n for an encoding the test chose itself - the library's own encryption will not
 * make a malformed one. */
static bool raw_encrypt(const proven_crypto_rsa_key_t *k, const proven_byte_t *em, proven_byte_t *out) {
    static proven_crypto_mp_mod_t mod;
    static proven_u32 x[PROVEN_CRYPTO_MP_MAX], e[2];
    if (!proven_crypto_mp_mod_init(&mod, k->n, k->n_len) || !proven_crypto_mp_load_be(x, mod.limbs, em, k->n_len) || !proven_crypto_mp_load_be(e, 2, k->e, k->e_len)) return false;
    proven_crypto_mp_to_mont(&mod, x, x);
    proven_crypto_mp_pow(&mod, x, x, e, 2);
    proven_crypto_mp_from_mont(&mod, x, x);
    proven_crypto_mp_store_be(out, k->n_len, x, mod.limbs);
    return true;
}

int main(void) {
    PROVEN_TEST_SUITE("the legacy TLS set below the state machine",
        "AES-CBC, HMAC with MD5 and SHA-1, the PRF of TLS 1.0 and 1.1, CBC records, Diffie-Hellman and RSA key exchange: against values computed outside the library, and what each must refuse.",
        "Inspect src/proven/tls_legacy.c, src/proven/tls_dh.c, the CBC functions of src/proven/crypto_aes.c and the key-exchange functions of src/proven/crypto_rsa.c. The failing line's letter names the function.");

    (void)tp_build;                                               /* only the byte stream of that header is used here */
    PROVEN_TEST_SECTION("the script", "Every line of known answers, replayed.", "The letter printed names the kind of line; the header comment of this file lists them.");
    int counts[128] = { 0 }, failed = 0;
    for (proven_size_t i = 0; i < sizeof TLSLEGACYV / sizeof TLSLEGACYV[0]; ++i) {
        const char *at = TLSLEGACYV[i];
        const char kind = at[0];
        proven_size_t n1, n2, n3, n4, n5;
        bool ok = false;
        at += 1;
        if (kind == 'A') {
            const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4);
            proven_crypto_aes_gcm_t aes;
            proven_byte_t iv[16];
            const proven_size_t kl = unhex(f1, n1, g_buf[0]), len = unhex(f3, n3, g_buf[1]);
            (void)unhex(f4, n4, g_buf[2]);
            ok = proven_crypto_aes_gcm_init(&aes, g_buf[0], kl);
            (void)unhex(f2, n2, iv);
            memcpy(g_buf[3], g_buf[1], len);
            proven_crypto_aes_cbc_encrypt(&aes, iv, g_buf[3], len);
            ok = ok && memcmp(g_buf[3], g_buf[2], len) == 0 && memcmp(iv, g_buf[2] + len - 16, 16) == 0;       /* and the IV is left as the last block */
            (void)unhex(f2, n2, iv);
            proven_crypto_aes_cbc_decrypt(&aes, iv, g_buf[3], len);
            ok = ok && memcmp(g_buf[3], g_buf[1], len) == 0 && memcmp(iv, g_buf[2] + len - 16, 16) == 0;
            /* And the portable cipher, where the processor's would otherwise be used to encrypt. */
            proven_crypto_aes_force_portable(true);
            ok = ok && proven_crypto_aes_gcm_init(&aes, g_buf[0], kl);
            proven_crypto_aes_force_portable(false);
            (void)unhex(f2, n2, iv);
            memcpy(g_buf[3], g_buf[1], len);
            proven_crypto_aes_cbc_encrypt(&aes, iv, g_buf[3], len);
            ok = ok && memcmp(g_buf[3], g_buf[2], len) == 0;
            (void)unhex(f2, n2, iv);
            proven_crypto_aes_cbc_decrypt(&aes, iv, g_buf[3], len);
            ok = ok && memcmp(g_buf[3], g_buf[1], len) == 0 && memcmp(iv, g_buf[2] + len - 16, 16) == 0;
        } else if (kind == 'H') {
            const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4);
            const proven_tls_lh_t hash = (proven_tls_lh_t)number(f1, n1, 10);
            const proven_size_t kl = unhex(f2, n2, g_buf[0]), ml = unhex(f3, n3, g_buf[1]), hl = unhex(f4, n4, g_buf[2]);
            proven_byte_t mac[PROVEN_TLS_LH_MAX_SIZE];
            ok = true;
            /* The message in two parts, split at every place a short one can be. */
            for (proven_size_t cut = 0; cut <= ml && cut <= 70; ++cut) {
                proven_tls_legacy_hmac(hash, (proven_mem_view_t){ g_buf[0], kl }, (proven_mem_view_t){ g_buf[1], cut }, (proven_mem_view_t){ g_buf[1] + cut, ml - cut }, mac);
                ok = ok && memcmp(mac, g_buf[2], hl) == 0;
            }
        } else if (kind == 'P') {
            const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4);
            char label[64];
            const proven_size_t sl = unhex(f1, n1, g_buf[0]), ll = unhex(f2, n2, (proven_byte_t *)label), dl = unhex(f3, n3, g_buf[1]), ol = unhex(f4, n4, g_buf[2]);
            label[ll] = 0;
            proven_tls10_prf((proven_mem_view_t){ g_buf[0], sl }, label, (proven_mem_view_t){ g_buf[1], dl }, g_buf[3], ol);
            ok = memcmp(g_buf[3], g_buf[2], ol) == 0;
        } else if (kind == 'C' || kind == 'Y' || kind == 'X') {
            proven_tls_keys_t keys, again;
            proven_tls_cbc_t cbc, cbc_again;
            proven_byte_t type = 0, iv[16] = { 0 };
            proven_mem_mut_t content = { 0 };
            ok = record_setup(&at, &keys, &cbc, &type);
            again = keys; cbc_again = cbc;
            proven_size_t cl = 0, rl;
            if (kind != 'X') { const char *f1 = field(&at, &n1); cl = unhex(f1, n1, g_buf[0]); }
            if (kind == 'C') { const char *f2 = field(&at, &n2); (void)unhex(f2, n2, iv); }
            const char *f3 = field(&at, &n3);
            rl = unhex(f3, n3, g_buf[1]);
            if (kind == 'C') {
                const proven_size_t made = proven_tls_cbc_seal(&keys, &cbc, type, (proven_mem_view_t){ g_buf[0], cl }, iv, g_buf[2]);
                ok = ok && made == rl && memcmp(g_buf[2], g_buf[1], rl) == 0 && keys.seq == again.seq + 1;
            }
            const bool opened = proven_tls_cbc_open(&again, &cbc_again, g_buf[1], rl - PROVEN_TLS_RECORD_HEADER, &content);
            if (kind == 'X') ok = ok && !opened;
            else ok = ok && opened && content.size == cl && (cl == 0 || memcmp(content.ptr, g_buf[0], cl) == 0);
        } else if (kind == 'D') {
            const char *f1 = field(&at, &n1), *f2 = field(&at, &n2), *f3 = field(&at, &n3), *f4 = field(&at, &n4), *f5 = field(&at, &n5);
            const proven_size_t pl = unhex(f1, n1, g_buf[0]), xl = unhex(f2, n2, g_buf[1]), yl = unhex(f3, n3, g_buf[2]);
            proven_size_t zl = 0;
            const proven_mem_view_t p = { g_buf[0], pl };
            static const proven_byte_t two = 2;
            ok = proven_tls_dh_known(p, (proven_mem_view_t){ &two, 1 }) && proven_tls_dh_exponent_len(pl) == xl && proven_tls_dh_public(p, g_buf[1], xl, g_buf[3]);
            (void)unhex(f4, n4, g_buf[4]);
            ok = ok && memcmp(g_buf[3], g_buf[4], pl) == 0;
            ok = ok && proven_tls_dh_shared(p, g_buf[1], xl, (proven_mem_view_t){ g_buf[2], yl }, g_buf[3], &zl);
            const proven_size_t want = unhex(f5, n5, g_buf[4]);
            ok = ok && zl == want && memcmp(g_buf[3], g_buf[4], zl) == 0;
        }
        counts[(int)kind]++;
        if (!ok) { failed = 1; fprintf(stderr, "line %u (%c) differs\n", (unsigned)i, kind); }
    }
    PROVEN_TEST_ASSERT(!failed, "every line of the script is reproduced", "");
    PROVEN_TEST_ASSERT(counts['A'] == 10 && counts['H'] == 23 && counts['P'] == 10 && counts['C'] == 118 && counts['Y'] == 54 && counts['X'] == 162 && counts['D'] == 12,
        "ten AES-CBC texts, twenty-three MACs, ten PRF outputs, 118 records sealed and opened, 54 with more padding opened, 162 spoiled ones refused, twelve Diffie-Hellman exchanges", "");

    PROVEN_TEST_SECTION("what a CBC record's length alone refuses", "Records that are too short to hold a MAC, or not a whole number of blocks.", "Check the public checks at the top of proven_tls_cbc_open.");
    {
        static proven_byte_t record[5 + 400];
        proven_byte_t iv[16] = { 0 }, key[16] = { 0 };
        bool all = true;
        for (int etm = 0; etm < 2; ++etm) for (int chained = 0; chained < 2; ++chained) {
            proven_tls_keys_t w, r;
            proven_tls_cbc_t cw, cr;
            proven_mem_mut_t content;
            memset(&w, 0, sizeof w); memset(&cw, 0, sizeof cw);
            w.active = proven_crypto_aes_gcm_init(&w.aes, key, 16);
            cw.mac = PROVEN_TLS_LH_SHA1; cw.mac_len = 20; cw.etm = etm != 0; cw.chained = chained != 0; cw.version = chained ? 0x0301 : 0x0303;
            const proven_size_t n = proven_tls_cbc_seal(&w, &cw, PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ key, 5 }, iv, record);
            const proven_size_t body = n - 5;
            for (proven_size_t len = 0; len < body; ++len) {
                r = w; cr = cw; r.seq = 0; memset(cr.iv, 0, 16);
                all = all && !proven_tls_cbc_open(&r, &cr, record, len, &content);
            }
            r = w; cr = cw; r.seq = 0; memset(cr.iv, 0, 16);
            all = all && proven_tls_cbc_open(&r, &cr, record, body, &content) && content.size == 5 && r.seq == 1;
            r = w; cr = cw; r.seq = 0; memset(cr.iv, 0, 16);
            all = all && !proven_tls_cbc_open(&r, &cr, record, PROVEN_TLS_MAX_CIPHERTEXT + 1, &content);
        }
        PROVEN_TEST_ASSERT(all, "every length shorter than the record is refused, in both MAC orders and with both kinds of IV; the whole record opens; a length above the maximum is refused on sight", "");
    }

    PROVEN_TEST_SECTION("Diffie-Hellman: what is refused", "Groups that are not the six, and values that lead somewhere anyone can name.", "Check proven_tls_dh_known and the range check in proven_tls_dh_shared.");
    {
        static proven_byte_t p[600], y[600], z[600], x[44];
        static const proven_byte_t two = 2, three = 3, padded_two[3] = { 0, 0, 2 };
        const proven_mem_view_t group = proven_tls_dh_server_group();
        proven_size_t zl = 0;
        memset(x, 0x5a, sizeof x);
        PROVEN_TEST_ASSERT(group.size == 256 && proven_tls_dh_known(group, (proven_mem_view_t){ &two, 1 }) && proven_tls_dh_known(group, (proven_mem_view_t){ padded_two, 3 }),
            "the server's group is 2048 bits and known, with generator 2 however it is padded", "");
        PROVEN_TEST_ASSERT(!proven_tls_dh_known(group, (proven_mem_view_t){ &three, 1 }) && !proven_tls_dh_known(group, (proven_mem_view_t){ NULL, 0 }), "not with generator 3, or none", "");
        memcpy(p, group.ptr, group.size);
        p[100] ^= 1;
        PROVEN_TEST_ASSERT(!proven_tls_dh_known((proven_mem_view_t){ p, group.size }, (proven_mem_view_t){ &two, 1 }), "a prime with one bit changed is not known", "");
        PROVEN_TEST_ASSERT(!proven_tls_dh_known((proven_mem_view_t){ group.ptr, 128 }, (proven_mem_view_t){ &two, 1 }), "nor half of one", "");
        p[0] = 0; memcpy(p + 1, group.ptr, group.size);
        PROVEN_TEST_ASSERT(proven_tls_dh_known((proven_mem_view_t){ p, group.size + 1 }, (proven_mem_view_t){ &two, 1 }), "a leading zero byte does not make it another prime", "");
        memset(y, 0, sizeof y);
        PROVEN_TEST_ASSERT(!proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl), "a peer's value of 0 is refused", "");
        y[255] = 1;
        PROVEN_TEST_ASSERT(!proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl), "1 is refused", "");
        y[255] = 2;
        PROVEN_TEST_ASSERT(proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl) && zl > 200, "2 is the smallest that is taken", "");
        memcpy(y, group.ptr, 256);
        PROVEN_TEST_ASSERT(!proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl), "the prime itself is refused", "");
        y[255] -= 1;
        PROVEN_TEST_ASSERT(!proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl), "the prime less one is refused", "");
        y[255] -= 1;
        PROVEN_TEST_ASSERT(proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 256 }, z, &zl), "the prime less two is the largest that is taken", "");
        memset(y, 0xff, sizeof y);
        PROVEN_TEST_ASSERT(!proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 257 }, z, &zl) && !proven_tls_dh_shared(group, x, 32, (proven_mem_view_t){ y, 0 }, z, &zl),
            "a value longer than the prime, and an empty one, are refused", "");
        PROVEN_TEST_ASSERT(!proven_tls_dh_public(group, x, 0, z) && !proven_tls_dh_public(group, x, 33, z) && !proven_tls_dh_public((proven_mem_view_t){ y, 0 }, x, 32, z),
            "an exponent of no length or of a length that is not whole limbs, and an empty prime, are refused", "");
        PROVEN_TEST_ASSERT(proven_tls_dh_exponent_len(256) == 32 && proven_tls_dh_exponent_len(384) == 36 && proven_tls_dh_exponent_len(512) == 44, "exponents are 256, 288 and 352 bits for primes of 2048, 3072 and 4096", "");
    }

    PROVEN_TEST_SECTION("RSA for key exchange", "A premaster secret encrypted and recovered - and, for anything that is not one, the fallback instead, with no sign of which.", "Check proven_crypto_rsa_encrypt_pkcs1 and proven_crypto_rsa_decrypt_premaster in src/proven/crypto_rsa.c.");
    {
        static proven_crypto_rsa_key_t key;
        static proven_crypto_rsa_blind_t blind;
        static proven_byte_t d[512], seed[512], sealed[512], em[512], pad[512];
        proven_byte_t premaster[48], fallback[48], got[48];
        proven_size_t sealed_len = 0;
        proven_crypto_rsa_test_min_bits(1024);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_generate(1024, tp_stream, &g_stream, &key, d), "a 1024-bit key, made here", "");
        tp_stream(&g_stream, seed, key.n_len);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_blind_make(&key, seed, &blind), "and a blinding pair for it", "");
        const proven_mem_view_t kn = { key.n, key.n_len }, ke = { key.e, key.e_len };
        tp_stream(&g_stream, premaster, 48); tp_stream(&g_stream, pad, sizeof pad);
        premaster[0] = 3; premaster[1] = 3;
        memset(fallback, 0xee, sizeof fallback);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_encrypt_pkcs1(kn, ke, (proven_mem_view_t){ premaster, 48 }, pad, sealed, &sealed_len) && sealed_len == key.n_len, "a premaster encrypted under the public key is as long as the modulus", "");
        proven_crypto_rsa_decrypt_premaster(&key, &blind, (proven_mem_view_t){ sealed, sealed_len }, 0x0303, fallback, got);
        PROVEN_TEST_ASSERT(memcmp(got, premaster, 48) == 0, "and is recovered with the private one", "");
        proven_crypto_rsa_decrypt_premaster(&key, NULL, (proven_mem_view_t){ sealed, sealed_len }, 0x0303, fallback, got);
        PROVEN_TEST_ASSERT(memcmp(got, premaster, 48) == 0, "with or without blinding", "");
        memset(pad, 0, sizeof pad);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_encrypt_pkcs1(kn, ke, (proven_mem_view_t){ premaster, 48 }, pad, em, &sealed_len), "padding bytes that come out of the random source as zero are replaced", "");
        proven_crypto_rsa_decrypt_premaster(&key, &blind, (proven_mem_view_t){ em, sealed_len }, 0x0303, fallback, got);
        PROVEN_TEST_ASSERT(memcmp(got, premaster, 48) == 0, "so that what is sent still decrypts", "");
        PROVEN_TEST_ASSERT(!proven_crypto_rsa_encrypt_pkcs1(kn, ke, (proven_mem_view_t){ pad, key.n_len - 10 }, pad, em, &sealed_len), "a message that leaves no room for eight bytes of padding is refused", "");

        /* Everything below must give the fallback, and nothing else. */
        int wrong = 0, cases = 0;
#define FALLS_BACK(cipher, len, version) do { memset(got, 0, 48); proven_crypto_rsa_decrypt_premaster(&key, &blind, (proven_mem_view_t){ (cipher), (len) }, (version), fallback, got); \
                                              cases++; if (memcmp(got, fallback, 48) != 0) { wrong++; fprintf(stderr, "premaster case %d did not fall back\n", cases); } } while (0)
        FALLS_BACK(sealed, sealed_len, 0x0302);                       /* the client first offered another version */
        FALLS_BACK(sealed, sealed_len - 1, 0x0303);                   /* a byte short */
        FALLS_BACK(sealed, 0, 0x0303);
        memset(em, 0xff, key.n_len);
        FALLS_BACK(em, key.n_len, 0x0303);                            /* not below the modulus */
        memcpy(em, sealed, key.n_len); em[key.n_len - 1] ^= 1;
        FALLS_BACK(em, key.n_len, 0x0303);                            /* an encryption of nothing in particular */
        static proven_byte_t shape[512];
        /* Encodings that are wrong in one place each. The right one first, to prove the helper. */
        for (int flaw = 0; flaw < 7; ++flaw) {
            const proven_size_t k = key.n_len;
            shape[0] = 0; shape[1] = 2;
            for (proven_size_t i = 2; i < k - 49; ++i) shape[i] = (proven_byte_t)(i | 1);
            shape[k - 49] = 0;
            memcpy(shape + k - 48, premaster, 48);
            if (flaw == 1) shape[0] = 1;                              /* does not start with 00 */
            if (flaw == 2) shape[1] = 1;                              /* a signature's padding, not an encryption's */
            if (flaw == 3) shape[10] = 0;                             /* a zero inside the padding */
            if (flaw == 4) shape[k - 49] = 7;                         /* no separator where the premaster must start */
            if (flaw == 5) { shape[k - 49] = 9; shape[k - 50] = 0; }  /* a 49-byte message */
            if (flaw == 6) shape[k - 48] = 2;                         /* the wrong version inside */
            if (!raw_encrypt(&key, shape, em)) { wrong++; continue; }
            if (flaw == 0) {
                proven_crypto_rsa_decrypt_premaster(&key, &blind, (proven_mem_view_t){ em, k }, 0x0303, fallback, got);
                if (memcmp(got, premaster, 48) != 0) wrong++;
            } else {
                FALLS_BACK(em, k, 0x0303);
            }
        }
        PROVEN_TEST_ASSERT(cases == 11 && wrong == 0, "another version, a wrong length, a value not below the modulus, a changed ciphertext, and six encodings each wrong in one place: the fallback, every time", "");

        /* The signature of TLS 1.0 and 1.1: 36 bytes, no DigestInfo. */
        static proven_byte_t sig[512];
        proven_byte_t both[36], digest[32];
        tp_stream(&g_stream, both, sizeof both); tp_stream(&g_stream, digest, sizeof digest);
        PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1_raw(&key, &blind, (proven_mem_view_t){ both, 36 }, sig) && proven_crypto_rsa_verify_pkcs1_raw(kn, ke, (proven_mem_view_t){ both, 36 }, (proven_mem_view_t){ sig, key.n_len }),
            "36 bytes signed as they stand verify as they stand", "");
        both[35] ^= 1;
        PROVEN_TEST_ASSERT(!proven_crypto_rsa_verify_pkcs1_raw(kn, ke, (proven_mem_view_t){ both, 36 }, (proven_mem_view_t){ sig, key.n_len }) &&
                           !proven_crypto_rsa_verify_pkcs1_raw(kn, ke, (proven_mem_view_t){ both, 35 }, (proven_mem_view_t){ sig, key.n_len }), "not with a bit changed, and not as 35 of them", "");
        PROVEN_TEST_ASSERT(proven_crypto_rsa_sign_pkcs1(&key, &blind, PROVEN_HMAC_SHA256, (proven_mem_view_t){ digest, 32 }, sig) &&
                           !proven_crypto_rsa_verify_pkcs1_raw(kn, ke, (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, key.n_len }) &&
                           proven_crypto_rsa_verify_pkcs1(kn, ke, PROVEN_HMAC_SHA256, (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, key.n_len }),
            "a signature with a DigestInfo is not one without: the two kinds do not stand in for each other", "");
        PROVEN_TEST_ASSERT(!proven_crypto_rsa_sign_pkcs1_raw(&key, &blind, (proven_mem_view_t){ pad, key.n_len - 10 }, sig) && !proven_crypto_rsa_sign_pkcs1_raw(&key, &blind, (proven_mem_view_t){ pad, 0 }, sig),
            "data too long to pad, and no data, are not signed", "");
        proven_crypto_rsa_test_min_bits(0);
    }

    PROVEN_TEST_SECTION("key block layout", "Both directions' keys for a CBC suite, from one master secret.", "Check proven_tls_cbc_set_keys: MAC keys, then cipher keys, then - in TLS 1.0 only - IVs.");
    {
        proven_byte_t master[48], cr[32], sr[32];
        static proven_byte_t record[5 + 200];
        proven_byte_t iv[16] = { 9 };
        bool all = true;
        memset(master, 3, sizeof master); memset(cr, 4, sizeof cr); memset(sr, 5, sizeof sr);
        static const proven_u16 ids[5] = { 0xc013, 0xc027, 0x0035, 0x006b, 0xc028 };
        static const proven_u16 vers[3] = { 0x0301, 0x0302, 0x0303 };
        for (int s = 0; s < 5; ++s) for (int v = 0; v < 3; ++v) for (int etm = 0; etm < 2; ++etm) {
            const proven_tls12_suite_t *suite = proven_tls12_suite_find(ids[s]);
            if (vers[v] < 0x0303 && suite->mac != PROVEN_TLS_LH_SHA1) continue;
            proven_tls_keys_t cw, sw, cw2, sw2;
            proven_tls_cbc_t a[2], b[2];
            proven_mem_mut_t content;
            proven_tls_cbc_set_keys(&cw, &sw, a, suite, vers[v], etm != 0, master, cr, sr);
            proven_tls_cbc_set_keys(&cw2, &sw2, b, suite, vers[v], etm != 0, master, cr, sr);
            all = all && cw.active && sw.active && a[0].mac_len == (suite->mac == PROVEN_TLS_LH_SHA1 ? 20 : suite->mac == PROVEN_TLS_LH_SHA256 ? 32 : 48) && a[0].chained == (vers[v] == 0x0301) && a[0].etm == (etm != 0);
            all = all && memcmp(a[0].mac_key, a[1].mac_key, a[0].mac_len) != 0 && memcmp(cw.aes.rk, sw.aes.rk, 16) != 0;
            /* What the client seals the server's reading side opens, and the other way; never the same side. */
            proven_size_t n = proven_tls_cbc_seal(&cw, &a[0], PROVEN_TLS_CT_APPLICATION, (proven_mem_view_t){ master, 40 }, iv, record);
            static proven_byte_t copy[5 + 200];
            memcpy(copy, record, n);
            all = all && !proven_tls_cbc_open(&sw2, &b[1], copy, n - 5, &content);
            memcpy(copy, record, n);
            all = all && proven_tls_cbc_open(&cw2, &b[0], copy, n - 5, &content) && content.size == 40 && memcmp(content.ptr, master, 40) == 0;
            n = proven_tls_cbc_seal(&sw, &a[1], PROVEN_TLS_CT_HANDSHAKE, (proven_mem_view_t){ cr, 32 }, iv, record);
            proven_tls_cbc_set_keys(&cw2, &sw2, b, suite, vers[v], etm != 0, master, cr, sr);
            memcpy(copy, record, n);
            all = all && proven_tls_cbc_open(&sw2, &b[1], copy, n - 5, &content) && content.size == 32;
        }
        PROVEN_TEST_ASSERT(all, "five suites in every version they exist in: the two directions have different keys, and each side opens what the other seals and not what it would seal itself", "");
        const proven_tls12_suite_t *s;
        int legacy = 0, total = 0;
        for (proven_size_t i = 0; (s = proven_tls12_suite_at(i)) != NULL; ++i) { total++; if (s->legacy) legacy++; else if (legacy) legacy = -100; }
        PROVEN_TEST_ASSERT(total == 27 && legacy == 21 && proven_tls12_suite_find(0xc012) == NULL && proven_tls12_suite_find(0x000a) == NULL && proven_tls12_suite_find(0x0005) == NULL,
            "twenty-seven suites, the twenty-one legacy ones after all the others; 3DES and RC4 are not among them", "");
    }

    PROVEN_TEST_PASS("the pieces under the legacy TLS set reproduce their known answers and refuse what they must.");
    return 0;
}
