#include "proven.h"
#include "proven_test.h"
#include <stdlib.h>
#include <string.h>

/*
 * SHA-384 and SHA-512, HMAC, HKDF, and the two memory calls that handling a secret needs.
 *
 * The expected values are the published ones: FIPS 180-4's examples for the digests, RFC 4231
 * for HMAC, RFC 5869 for HKDF. Where the standards print no value - digests at lengths on each
 * side of the padding boundary, HKDF over SHA-384 and SHA-512 - the values were computed with
 * Python's hashlib and hmac, after the same script had reproduced the published ones.
 */

static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static bool hex_is(const proven_byte_t *got, proven_size_t n, const char *want) {
    static const char digits[] = "0123456789abcdef";
    if (strlen(want) != n * 2) return false;
    for (proven_size_t i = 0; i < n; ++i) {
        if (want[2 * i] != digits[got[i] >> 4] || want[2 * i + 1] != digits[got[i] & 15]) return false;
    }
    return true;
}

static proven_size_t unhex(const char *hex, proven_byte_t *out) {
    proven_size_t n = strlen(hex) / 2;
    for (proven_size_t i = 0; i < n; ++i) {
        char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        out[i] = (proven_byte_t)strtoul(c, NULL, 16);
    }
    return n;
}

/* The test pattern the boundary vectors were computed over. */
static void pattern(proven_byte_t *out, proven_size_t n) {
    for (proven_size_t i = 0; i < n; ++i) out[i] = (proven_byte_t)((i * 7 + 3) & 0xff);
}

int main(void) {
    PROVEN_TEST_SUITE("SHA-384, SHA-512, HMAC and HKDF",
        "The published vectors, the padding boundaries, independence from chunking, and the refusals.",
        "Inspect the SHA-512 section of src/proven/hash.c and src/proven/hmac.c. A mismatch means disagreement with the standard, not merely with an earlier version.");

    static proven_byte_t buf[1024];
    proven_byte_t d[PROVEN_SHA512_SIZE];

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("SHA-512 and SHA-384: the examples of FIPS 180-4",
        "The one-block, empty, two-block and million-a messages.",
        "Inspect sha512_compress and sha512_finish.");
    // ---------------------------------------------------------------
    {
        proven_sha512(mv("abc"), d);
        PROVEN_TEST_ASSERT(hex_is(d, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"), "SHA-512 of abc", "");
        proven_sha384(mv("abc"), d);
        PROVEN_TEST_ASSERT(hex_is(d, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"), "SHA-384 of abc", "");
        proven_sha512(mv(""), d);
        PROVEN_TEST_ASSERT(hex_is(d, 64, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"), "SHA-512 of the empty message", "");
        proven_sha384(mv(""), d);
        PROVEN_TEST_ASSERT(hex_is(d, 48, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"), "SHA-384 of the empty message", "");
        proven_sha512(mv("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"), d);
        PROVEN_TEST_ASSERT(hex_is(d, 64, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"), "SHA-512 of the 896-bit message", "");
        proven_sha384(mv("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"), d);
        PROVEN_TEST_ASSERT(hex_is(d, 48, "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039"), "SHA-384 of the 896-bit message", "");
        proven_sha512_t c5;
        proven_sha384_t c3;
        proven_sha512_init(&c5);
        proven_sha384_init(&c3);
        memset(buf, 'a', 1000);
        for (int i = 0; i < 1000; ++i) {
            proven_sha512_update(&c5, (proven_mem_view_t){ buf, 1000 });
            proven_sha384_update(&c3, (proven_mem_view_t){ buf, 1000 });
        }
        proven_sha512_final(&c5, d);
        PROVEN_TEST_ASSERT(hex_is(d, 64, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"), "SHA-512 of a million a, fed in a thousand pieces", "");
        proven_sha384_final(&c3, d);
        PROVEN_TEST_ASSERT(hex_is(d, 48, "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"), "SHA-384 of the same", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("lengths on each side of the padding and block boundaries",
        "A message of 111 bytes fits its length in the last block and one of 112 does not; 127, 128 and 129 straddle the block.",
        "A failure at one length and not its neighbours is the padding: inspect sha512_finish.");
    // ---------------------------------------------------------------
    {
        static const struct { proven_size_t n; const char *s512; const char *s384; } rows[] = {
            { 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b" },
            { 1, "e45bf5817ddf94aa2f7a407071f0eedc6beb98f768b4cd33d1176d44d1563a45a5d7212290eb7670c6786b13591aedac86478993895e8b24e612014abaa6ba04", "8a6c69af6fb6247635f837958446fb8f10e39bd5fbc244f7e635176339a3be614f6394247f01dbe1126c178c7bd48cb5" },
            { 55, "14fd424b1fcadee624da946ab03f7e1def7c0d6e00f689594319881a26ff30b875ba4c622ac13100c8cc784c9c2eb23159aecbb4a02e3999062f551193e2b256", "fc2728f452750d08b5dbad1aea53202e7fc19c06894d13e36c433b0444a899277c49f16e0577c03db0f0605578836ef6" },
            { 56, "480fa85be41ef55a41208ca28ffc8743c91cf7d24758defe6f95bfb16de614fc86b701034896b047dd571de4318853d80e0809df162f1752cb26da6ddb94a0dd", "6c969a865f38d2eaac60b7963a84c97ba27bcb76e589cbe1e2e89ead6aee4ad0337a4268f054be49c604ddd99692036d" },
            { 63, "ecd42a703a4e93e163d60d55e3785b1a763838b0351bc2e6f7c94b4bfb24f9aa15da5d744ebcebe11f0fc4315d45ba3a047b6e60e07448357f2795bf34b73502", "be67a9a1162c1d97f183980dc811f4f4f835b8b850eb32ecbf87397b26369bc6646a8f2234940dcff142b627c8e21ebb" },
            { 64, "8f3cc30b3fb5bf963688a46488249248ac2c67f0f85a145233c6c1e3c16dcd1df634c07d1d31da02576f65b9cf64e1c3fdb318b689b8a14e2e9552bcf30fb133", "99d26430f561864c2f7d90b68650b960c86c85d6bf7f54ceeee404bdd59663a7f05588b5c750b862a2a24f6a9ad4c050" },
            { 65, "22050d1b2c5bd016c4b04c3e84f513ffbbbc057f83dc9ab196fee6684e5ee19d98b84c6538b3c5e1fc67dbc484e9d0a198254e41d0fa0cf5106991e106632419", "cada16d798ec25243470f7827eb89a2402d52680b092dffc0f59f5a919e5058aebf7ad79dba85c61b179d23534d7c855" },
            { 111, "68cffa6d0d76f309c9ce0d35280939f8e25990c43b7b086ccdf709be35b07d4ddba599541ff2b1c19d34ea49aeafb9659adb7ac3c0b078bb30a22d57fc6687ef", "341388a9dc2275074e90cf394323761919761c805fd9e370977c9966a0e8c81a52135f02577670b0071638a4a26dbc31" },
            { 112, "d0865c524d1dddf7c23b799c413f5adcd7caefd3f66a9b49750ec81066012c25a8bcf94ddea6dc525691673097ca40e0101e897fc97218cfdb0704084e2bef4b", "619cc5d06138526d70659eccf602d197e63e1050e22039a7feb40a30a5b2b08fb03729e291df12f8c576e6f1cd8af22a" },
            { 113, "606314353bd419f9f7720e8297f2af8a9ed5f0bb0bed29b716bf18e34577623ae7effa46f496bd4282134e7e895284a4760a910d2ad8ec11e9dd75863058cd9c", "484a92d02b61b620ccae687fce4cd073eb995294a777d5deb5356bc8c1e2ce984f63f16420058ac5da666fda410a4feb" },
            { 119, "236bdd7f38a611b5014b239245c381ae5d20a96f1e5b3178227c00056b7fa8c44ef54880085d82b7e20cd65f2bfda1326696c3f94a6a5bad0cb5ce289aa46167", "9c284d266fc91c33a7f6ecbb0326646b3ed15cc33fd8bf968267c21991ca1a434dd65c64ee22c7dd6f5423950f605560" },
            { 120, "4bd16fefaa35cc2df9fbb8ff379ec04a4070ffd5d4992574af239fa175534df87cbedceeb96be08e15090a78b83a328a2900c5411683ab47bab914591048548f", "96faef865acb7c55ae34422444b4380e520de69f373ddb14c7ee2420c3f93cc220fca9952c0a4fe9dbfddf350ac40127" },
            { 127, "e0b6a20f1c0c88970a9340152cd5a1c1ecf3d3b8de55102741879438079473540133b812706e5dbec322c8c9523b6fc8c6d16ee626e87ad5fe3d2916afedc369", "f8234ab81f5f9641c656680825a9bbb3c2616bcc80b65d5e479a4d96742e1b8742330c9e8256ef5f03caa932487044ca" },
            { 128, "99b16f17aa0b969a5b8f08f367719d516e330ccd2660b6f0688ec031dbc783de50a1cd185a2568dba75070a2403d17d4741d163578515dfd2ff756ddfe4d47b1", "e8480e9c4dd90f88104a79cbaccec48edbd798a142b4f241d726dc252f1502350e824c7d18dadd59d7d716919fb8f9bf" },
            { 129, "a1556e29185778aa5991e34b8884c840d589f0fbb4b8ed590e51e9ac4eb03a008125000db2671f8fe7f485b59a77b518670078ecb41a54b4cd02a7f1d2ca4c6d", "430173387764874661acba1e914e50f6d0fbac534809e859b1da4396fad3d5f24c25c6a2d02486d8fbda3994713d981a" },
            { 239, "18ee83f30261c3c645d52aee6a209105b25bba39d33845ef48984cc238e4f21661fb7bd7dd4336f71c40fe87d95e5115d6c7be52e0d3e7e7877d24500b5b58df", "fc75ded15c6080ff96ac03391fc1302ca180128e4b6a4a2f17782f2ec016fc3f79b48b5fed3305fb7fdff9fd29434525" },
            { 240, "9d60ee60d29ec4fa0b9690c04c1c29413bbe3ed345639182d9d53dcc05926b77b04f4fec1562fb85182954c96b7cbb5d5e4410251ff4f352d09a2da90419fb13", "4c4bc5dae174dd68cb5a62e066e01e339e4980e4a35531b732dcf88e9c579e66f7c0d044779e2eda8a597684ca7cffd6" },
            { 241, "4aa5b7994e3c6e0b8d72a6392d63edf07d60485a692445dac2d7fc91ea6ee98cabbd640bf3d1befd0d795af06489af3bd4d616d82d39585b82118115d8b7f970", "011613cbf097010685028f1c71bb41001a85fa1fef15834ec7a871f0f2a5176442d9b937cef9356c4261ea366dbadcb8" },
            { 255, "c2e3bb67012f9eb526202efa59997933f7d3e75e7ded738818bc27d94977f4573afddb1b2793745701e62affa3b7a1c8262c992a321f488a6b1942a4795bab98", "15f3347894a150a64eb82a7044bb5ec8b1d33488716fce4932ba73beb97eabd1ad796bc0a569129ba98120ae1ed7edce" },
            { 256, "e49c208e41556e859d1a52d14784a061c2d5ae2c8690a5360e9f9344f60861c1362a9ec05a9f08a4167b3da41bdd122a387413dd06976470e4beff5053f2ac71", "23f0634552ba15289fa02c8a37e3a391e79e230dca05db03a7696630c5ba3a3352d1624f52832e4d6183c0eb70198e9a" },
            { 257, "a8f13f5f1f09e5c21370bd3e0f3a60ce9987663b95068a2cc9a2bc5cbfdc4c34611325699468e00350bd2e06a62c6cad8710001d407971f5e4e3d1dc6b484fdd", "233012efa9647f875d85d37154bffb19077acae572625ebfdc80d3a538447cb754aca3a3a5bdd41df8bf8870e1f0f2bd" },
            { 1000, "00e36fccf193e59697a92b5ab24666ce6326d7fa16bf10832d0991ddc591112e9dfa6a636950ed9c4d67344a760654c2ff7785e1d60094d651038735b5dccabd", "94c38db521ca733b8904c2d14b6e82d33dcfcc26e1318c579dbee1fc2f472019034e792263b9d46e90e700de2f6e7e91" },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            pattern(buf, rows[i].n);
            proven_sha512((proven_mem_view_t){ buf, rows[i].n }, d);
            bool ok = hex_is(d, 64, rows[i].s512);
            proven_sha384((proven_mem_view_t){ buf, rows[i].n }, d);
            ok = ok && hex_is(d, 48, rows[i].s384);
            if (!ok) PROVEN_TEST_INFO("length {}", PROVEN_ARG((proven_u64)rows[i].n));
            PROVEN_TEST_ASSERT(ok, "both digests at this length are the reference values", "");
        }

        /* However the input is cut, the digest is the same. */
        proven_byte_t whole[64], part[64];
        pattern(buf, 300);
        proven_sha512((proven_mem_view_t){ buf, 300 }, whole);
        bool same = true;
        for (proven_size_t cut = 0; cut <= 300; ++cut) {
            proven_sha512_t c;
            proven_sha512_init(&c);
            proven_sha512_update(&c, (proven_mem_view_t){ buf, cut });
            proven_sha512_update(&c, (proven_mem_view_t){ buf + cut, 300 - cut });
            proven_sha512_final(&c, part);
            same = same && memcmp(whole, part, 64) == 0;
        }
        PROVEN_TEST_ASSERT(same, "every split of 300 bytes into two updates gives the one-shot digest", "");
        proven_sha512_t c;
        proven_sha512_init(&c);
        for (proven_size_t i = 0; i < 300; ++i) proven_sha512_update(&c, (proven_mem_view_t){ buf + i, 1 });
        proven_sha512_update(&c, (proven_mem_view_t){ buf, 0 });
        proven_sha512_update(&c, (proven_mem_view_t){ NULL, 0 });
        proven_sha512_final(&c, part);
        PROVEN_TEST_ASSERT(memcmp(whole, part, 64) == 0, "and so does a byte at a time, with empty updates among them", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("HMAC: the test cases of RFC 4231",
        "Short keys, long keys, keys longer than a block, for all three hashes.",
        "Inspect proven_hmac_init for the key handling and proven_hmac_final for the outer pass.");
    // ---------------------------------------------------------------
    {
        static const struct { const char *key; const char *data; const char *h256; const char *h384; const char *h512; const char *why; } rows[] = {
            { "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "4869205468657265", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6", "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854", "test case 1" },
            { "4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649", "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737", "test case 2: a key shorter than the digest" },
            { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "88062608d3e6ad8a0aa2ace014c8a86f0aa635d947ac9febe83ef4e55966144b2a5ab39dc13814b94e3ab6e101a34f27", "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb", "test case 3" },
            { "0102030405060708090a0b0c0d0e0f10111213141516171819", "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "3e8a69b7783c25851933ab6290af6ca77a9981480850009cc5577c6e1f573b4e6801dd23c4a7d679ccf8a386c674cffb", "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3dba91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd", "test case 4" },
            { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374", "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c60c2ef6ab4030fe8296248df163f44952", "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598", "test case 6: a key longer than a block" },
            { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e", "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "6617178e941f020d351e2f254e8fd32c602420feb0b8fb9adccebb82461e99c5a678cc31e799176d3860e6110c46523e", "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58", "test case 7" },
            { "", "", "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad", "6c1f2ee938fad2e24bd91298474382ca218c75db3d83e114b3d4367776d14d3551289e75e8209cd4b792302840234adc", "b936cee86c9f87aa5d3c6f2e84cb5a4239a5fe50480a6ec66b70ab5b1f4ac6730c6c515421b327ec1d69402e53dfb49ad7381eb067b338fd7b0cb22247225d47", "an empty key and an empty message" },
            { "55555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555", "78", "a05d203ff38ae1799782be564cd00c7471528a19a7fecd341005aaaae161ec31", "0093e993386fd284026f20462cb6c0d249d98323f6bec73b550d04ea115398fbcfc86eded62bd60f9823b24ab4431a1c", "4ccfa895f05078fa5e7c6a44d722bd2fc930da25ff637c31c1b0c1248880b6bb22b56b9376b09a6b77b391068ab31c6ad659cd285d5911de21bb965fd9ebf86e", "a key of exactly one SHA-256 block" },
            { "5555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555", "78", "f688f8b0ec4f7c4c749e218fcd47cc0f317793602ec958210afc9a84dd363118", "464bd1fffad620da9441c2cbf68c147afb8535e67a815a642750acc22a36589f55132e456a21481f5327226a1422a0ea", "d8e98a2b90c6ba320bc256a5e18665cddbc5f05a4a340b32a57514e475f42483322cc09eb98e81d8071a8ab2e583cc9193b2e3477ee00c5f14db9a73f46b8022", "a key of exactly one SHA-512 block" },
            { "555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555", "78", "7b6e62884d4924d46232ee447f39a41592c227632cd2085c9ec8f39108e54bc0", "5575aa9417a5f82418eecbe6ffc7311dd7963073190f5083aa746fc90cf3f18cca5b75adc1abc19bbc1cc1df56096c52", "e8a5f0e1c2c6baebf903c4a9a4d0d1453966b55bbb37a4c02865df906afea897cad235fbf6af5da231c2b8f1685bd5f598936bba76d73a64333d548fac4664b1", "one byte more" },
        };
        static proven_byte_t key[256], data[256];
        proven_byte_t mac[PROVEN_HMAC_MAX_SIZE];
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_size_t kn = unhex(rows[i].key, key), dn = unhex(rows[i].data, data);
            const char *want[3] = { rows[i].h256, rows[i].h384, rows[i].h512 };
            for (int h = 0; h < 3; ++h) {
                proven_hmac_hash_t hash = (proven_hmac_hash_t)h;
                proven_size_t n = proven_hmac_size(hash);
                memset(mac, 0xee, sizeof mac);
                proven_err_t e = proven_hmac(hash, (proven_mem_view_t){ key, kn }, (proven_mem_view_t){ data, dn }, mac);
                bool ok = e == PROVEN_OK && hex_is(mac, n, want[h]);
                /* Nothing is written past the MAC's own size. */
                for (proven_size_t k = n; k < sizeof mac; ++k) ok = ok && mac[k] == 0xee;
                /* And fed a byte at a time it is the same MAC. */
                proven_hmac_t st;
                proven_byte_t again[PROVEN_HMAC_MAX_SIZE];
                ok = ok && proven_hmac_init(&st, hash, (proven_mem_view_t){ key, kn }) == PROVEN_OK;
                for (proven_size_t k = 0; k < dn; ++k) proven_hmac_update(&st, (proven_mem_view_t){ data + k, 1 });
                proven_hmac_final(&st, again);
                ok = ok && memcmp(mac, again, n) == 0;
                if (!ok) PROVEN_TEST_INFO("row: {} (hash {})", PROVEN_ARG(rows[i].why), PROVEN_ARG(h));
                PROVEN_TEST_ASSERT(ok, "the MAC is the reference value, one-shot and streamed, and writes only its own size", "");
            }
        }
        PROVEN_TEST_ASSERT(proven_hmac_size(PROVEN_HMAC_SHA256) == 32 && proven_hmac_size(PROVEN_HMAC_SHA384) == 48 &&
                           proven_hmac_size(PROVEN_HMAC_SHA512) == 64 && proven_hmac_size((proven_hmac_hash_t)7) == 0, "the three sizes, and 0 for a hash that is not one of them", "");

        proven_hmac_t st;
        memset(mac, 0xee, sizeof mac);
        PROVEN_TEST_ASSERT(proven_hmac_init(&st, (proven_hmac_hash_t)7, mv("k")) == PROVEN_ERR_INVALID_ARG &&
                           proven_hmac_init(&st, PROVEN_HMAC_SHA256, (proven_mem_view_t){ NULL, 4 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_hmac((proven_hmac_hash_t)7, mv("k"), mv("d"), mac) == PROVEN_ERR_INVALID_ARG && mac[0] == 0xee,
            "an unknown hash and a null key are PROVEN_ERR_INVALID_ARG, with nothing written", "");
        proven_hmac_update(&st, mv("ignored"));
        proven_hmac_final(&st, mac);
        PROVEN_TEST_ASSERT(mac[0] == 0xee, "a state that was not begun ignores update and final", "");

        /* final leaves no key material behind. */
        PROVEN_TEST_ASSERT(proven_hmac_init(&st, PROVEN_HMAC_SHA512, mv("a secret key")) == PROVEN_OK, "begin", "");
        proven_hmac_final(&st, mac);
        bool wiped = true;
        for (proven_size_t i = 0; i < sizeof st; ++i) wiped = wiped && ((const proven_byte_t *)&st)[i] == 0;
        PROVEN_TEST_ASSERT(wiped, "after final the state is all zeros: the key does not linger in it", "");
        memset(mac, 0xee, sizeof mac);
        proven_hmac_update(&st, mv("more"));
        proven_hmac_final(&st, mac);
        PROVEN_TEST_ASSERT(mac[0] == 0xee, "a finished state ignores update and a second final", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("HKDF: the test cases of RFC 5869, and the two larger hashes",
        "Extract gives the RFC's pseudorandom key and expand its output, with and without salt and info.",
        "Inspect proven_hkdf_extract and proven_hkdf_expand.");
    // ---------------------------------------------------------------
    {
        static const struct { int hash; const char *ikm; const char *salt; const char *info; proven_size_t len; const char *prk; const char *okm; const char *why; } rows[] = {
            { 0, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "RFC 5869 A.1" },
            { 0, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f", "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf", "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", 82, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244", "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87", "RFC 5869 A.2: longer inputs and output" },
            { 0, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "", "", 42, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", "RFC 5869 A.3: no salt, no info" },
            { 1, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42, "704b39990779ce1dc548052c7dc39f303570dd13fb39f7acc564680bef80e8dec70ee9a7e1f3e293ef68eceb072a5ade", "9b5097a86038b805309076a44b3a9f38063e25b516dcbf369f394cfab43685f748b6457763e4f0204fc5", "the A.1 inputs over SHA-384" },
            { 2, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42, "665799823737ded04a88e47e54a5890bb2c3d247c7a4254a8e61350723590a26c36238127d8661b88cf80ef802d57e2f7cebcf1e00e083848be19929c61b4237", "832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb", "the A.1 inputs over SHA-512" },
            { 1, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f", "", "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", 200, "3171144e2172c5b876a6f7b5d2e54c4c9daebfa2cddf473c17d9359236a79cf517ae757c03b4b81ab313ffac49f993bf", "605a8897e2933807c802427d5f219ae767d168fd8759629d08cd584c9269ee1305a97b4aa547cca36f8bb44bf63baa74baaaed79b55135002fa243a7c406397832ab12c6d0af03dfd8fc7709e55700b65fe005d208f74b5b56d28e4b14b0dc2afde1527e616e08ba080bfbe5dd95317a5d23247d7f21cdb7beae826706064d3a191ca5c30be17468e2d500e145734b22c3e7a1bb389e550909fa8fe94bc29dd9227f94b974ca8eb71c840c3763e53147974ada758777da9d77b18700ef497cd2c16b3772e88acc5c", "200 bytes over SHA-384, no salt" },
            { 2, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f", "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf", "", 1, "35672542907d4e142c00e84499e74e1de08be86535f924e022804ad775dde27ec86cd1e5b7d178c74489bdbeb30712beb82d4f97416c5a94ea81ebdf3e629e4a", "cd", "one byte over SHA-512" },
            { 0, "01", "02", "03", 32, "feb0be657eded41a4f6b028fc5ad5513c12ab1a1508f1045919706c547bbcbe1", "04de356fb510a12615c21c4fd98a0ddc67e5d35ce0d36a296f435b7cd5e48ec5", "exactly one block of output" },
            { 0, "01", "02", "03", 33, "feb0be657eded41a4f6b028fc5ad5513c12ab1a1508f1045919706c547bbcbe1", "04de356fb510a12615c21c4fd98a0ddc67e5d35ce0d36a296f435b7cd5e48ec54c", "one byte into the second block" },
        };
        static proven_byte_t ikm[128], salt[128], info[128], okm[256], prk[PROVEN_HMAC_MAX_SIZE];
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_hmac_hash_t hash = (proven_hmac_hash_t)rows[i].hash;
            proven_size_t n = proven_hmac_size(hash);
            proven_size_t in = unhex(rows[i].ikm, ikm), sn = unhex(rows[i].salt, salt), fn = unhex(rows[i].info, info);
            bool ok = proven_hkdf_extract(hash, (proven_mem_view_t){ salt, sn }, (proven_mem_view_t){ ikm, in }, prk) == PROVEN_OK && hex_is(prk, n, rows[i].prk);
            memset(okm, 0xee, sizeof okm);
            ok = ok && proven_hkdf_expand(hash, (proven_mem_view_t){ prk, n }, (proven_mem_view_t){ info, fn }, (proven_mem_mut_t){ okm, rows[i].len }) == PROVEN_OK &&
                 hex_is(okm, rows[i].len, rows[i].okm) && okm[rows[i].len] == 0xee;
            proven_byte_t both[256];
            ok = ok && proven_hkdf(hash, (proven_mem_view_t){ salt, sn }, (proven_mem_view_t){ ikm, in }, (proven_mem_view_t){ info, fn }, (proven_mem_mut_t){ both, rows[i].len }) == PROVEN_OK &&
                 memcmp(both, okm, rows[i].len) == 0;
            if (!ok) PROVEN_TEST_INFO("row: {}", PROVEN_ARG(rows[i].why));
            PROVEN_TEST_ASSERT(ok, "extract, expand and the two together give the reference values and write exactly the length asked", "");
        }

        /* Different info, unrelated keys: no byte of one need match the other, and they do not agree. */
        proven_byte_t k1[32], k2[32];
        memset(prk, 7, sizeof prk);
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, mv("encryption"), (proven_mem_mut_t){ k1, 32 }) == PROVEN_OK &&
                           proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, mv("authentication"), (proven_mem_mut_t){ k2, 32 }) == PROVEN_OK &&
                           memcmp(k1, k2, 32) != 0, "the same key with different info gives different keys", "");

        /* The limits. */
        static proven_byte_t big[255 * 64 + 1];
        memset(big, 0xee, sizeof big);
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, mv(""), (proven_mem_mut_t){ big, 255 * 32 }) == PROVEN_OK, "255 blocks is the most HKDF defines", "");
        memset(big, 0xee, sizeof big);
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, mv(""), (proven_mem_mut_t){ big, 255 * 32 + 1 }) == PROVEN_ERR_OUT_OF_BOUNDS && big[0] == 0xee &&
                           proven_hkdf(PROVEN_HMAC_SHA512, mv(""), mv("k"), mv(""), (proven_mem_mut_t){ big, 255 * 64 + 1 }) == PROVEN_ERR_OUT_OF_BOUNDS && big[0] == 0xee,
            "one byte more is PROVEN_ERR_OUT_OF_BOUNDS, with nothing written", "");
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 31 }, mv(""), (proven_mem_mut_t){ k1, 32 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_hkdf_expand((proven_hmac_hash_t)9, (proven_mem_view_t){ prk, 64 }, mv(""), (proven_mem_mut_t){ k1, 32 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_hkdf((proven_hmac_hash_t)9, mv(""), mv("k"), mv(""), (proven_mem_mut_t){ k1, 32 }) == PROVEN_ERR_INVALID_ARG &&
                           proven_hkdf_extract(PROVEN_HMAC_SHA256, mv(""), (proven_mem_view_t){ NULL, 3 }, prk) == PROVEN_ERR_INVALID_ARG,
            "a key shorter than the hash, an unknown hash and a null input are PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_hkdf_expand(PROVEN_HMAC_SHA256, (proven_mem_view_t){ prk, 32 }, mv("x"), (proven_mem_mut_t){ k1, 0 }) == PROVEN_OK, "asking for no bytes is a success that writes none", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("comparing and wiping",
        "proven_mem_equal_ct says equal only for equal ranges; proven_mem_wipe zeroes exactly what it is given.",
        "Inspect the end of src/proven/memory.c. That the comparison takes the same time whatever it finds is a property of its source, not something this test can observe.");
    // ---------------------------------------------------------------
    {
        proven_byte_t a[64], b[64];
        pattern(a, 64);
        pattern(b, 64);
        PROVEN_TEST_ASSERT(proven_mem_equal_ct((proven_mem_view_t){ a, 64 }, (proven_mem_view_t){ b, 64 }), "equal ranges are equal", "");
        bool all = true;
        for (int i = 0; i < 64; ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                b[i] ^= (proven_byte_t)(1 << bit);
                all = all && !proven_mem_equal_ct((proven_mem_view_t){ a, 64 }, (proven_mem_view_t){ b, 64 });
                b[i] ^= (proven_byte_t)(1 << bit);
            }
        }
        PROVEN_TEST_ASSERT(all, "a difference in any one of the 512 bits is a difference", "");
        PROVEN_TEST_ASSERT(!proven_mem_equal_ct((proven_mem_view_t){ a, 64 }, (proven_mem_view_t){ b, 63 }) &&
                           proven_mem_equal_ct((proven_mem_view_t){ a, 0 }, (proven_mem_view_t){ b, 0 }) &&
                           proven_mem_equal_ct((proven_mem_view_t){ NULL, 0 }, (proven_mem_view_t){ NULL, 0 }) &&
                           !proven_mem_equal_ct((proven_mem_view_t){ NULL, 4 }, (proven_mem_view_t){ b, 4 }),
            "different lengths are unequal; two empty ranges are equal; a null range of some length is not equal to anything", "");

        memset(a, 0xab, sizeof a);
        proven_mem_wipe((proven_mem_mut_t){ a + 8, 40 });
        bool exact = true;
        for (int i = 0; i < 64; ++i) exact = exact && a[i] == ((i >= 8 && i < 48) ? 0 : 0xab);
        PROVEN_TEST_ASSERT(exact, "wipe zeroes the bytes named and no others", "");
        proven_mem_wipe((proven_mem_mut_t){ NULL, 10 });
        proven_mem_wipe((proven_mem_mut_t){ a, 0 });
        PROVEN_TEST_ASSERT(a[0] == 0xab, "a null or empty range is left alone", "");
    }

    PROVEN_TEST_PASS("the digests, the MAC and the key derivation agree with their standards.");
    return 0;
}
