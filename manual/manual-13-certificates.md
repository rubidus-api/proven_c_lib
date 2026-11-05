# Chapter 13: Certificates and trust

**Part V - Talking to the operating system. Prerequisites: [Chapter 4](manual-04-containers-algorithms.md)
for digests; [Chapter 2](manual-02-allocation.md) for the allocator a store takes.**
**After this chapter** you can read an X.509 certificate, say which names it is for, load the
certificates you trust, and decide whether a peer's certificate chain is to be believed - and
you will know exactly which checks were made and which were not.

This chapter covers `cert.h`. Reading and verifying allocate nothing and call no operating
system: they are available in a [freestanding](manual-freestanding.md) build. A store of trust
anchors allocates through the allocator you give it. One call, `proven_cert_store_add_system`,
reads the operating system's roots and is hosted-only.

**What this is.** This is the certificate half of TLS, usable on its own: checking a certificate
you were handed, from a file, a message or another library's handshake. The protocol itself -
the handshake and the encrypted records - is [Chapter 14](manual-14-tls.md), which uses
everything here.

**And a caution that will stay in this chapter whatever is added to it.** This is a new
implementation with no external audit. It is tested against another implementation and against
published adversarial vectors (section 8 says which), and that is evidence, not proof. A program
whose users depend on it against a capable adversary should have its certificates checked by
something audited.

## Table of contents

1. [What a certificate is, in one page](#1-what-a-certificate-is-in-one-page)
2. [Reading one](#2-reading-one)
3. [PEM](#3-pem)
4. [Which names it is for](#4-which-names-it-is-for)
5. [Trust anchors](#5-trust-anchors)
6. [Verifying a chain](#6-verifying-a-chain)
7. [Pinning](#7-pinning)
8. [What is checked, what is accepted, what is not here](#8-what-is-checked-what-is-accepted-what-is-not-here)
9. [Private keys and signatures](#9-private-keys-and-signatures)
10. [DER, read and written](#10-der-read-and-written)
11. [Encrypted key files: PBKDF2 and AES-CBC](#11-encrypted-key-files-pbkdf2-and-aes-cbc)
12. [Encryption and key exchange: aead.h and kex.h](#12-encryption-and-key-exchange-aeadh-and-kexh)

## 1. What a certificate is, in one page

A public key lets you check that a message came from whoever holds the matching private key.
It does not tell you *who* that is. A **certificate** is the missing sentence: "this public key
belongs to `example.com`", signed by an **issuer**. You can check the issuer's signature if you
have the issuer's public key - which comes in the issuer's own certificate, signed by *its*
issuer. The sentences form a **chain**, and the chain has to end somewhere: at a certificate
you trust not because anyone signed it but because it was already on your machine. That is a
**trust anchor**, or root.

So believing a peer is four questions, and `proven_cert_verify` asks all four:

| Question | What goes wrong when it is skipped |
|---|---|
| Is there a chain of valid signatures from this certificate to an anchor I hold? | Anyone can make a certificate for any name. Only the chain says someone you trust vouched for it. |
| Was every certificate in the chain valid at this moment? | A key that leaked years ago still signs. The dates bound how long a leak matters. |
| Was every issuer in the chain *allowed* to issue? | The certificate of an ordinary web site is signed by a real authority. If it could sign others, every site owner could mint certificates for any name. |
| Is the name in the certificate the name I meant to reach? | A perfectly valid certificate for `attacker.example` proves nothing about `bank.example`. |

The formats: a certificate is **DER** - a compact binary encoding - and it usually travels as
**PEM**, which is that DER in Base64 between a `-----BEGIN CERTIFICATE-----` line and an `END`
line, so that it survives being pasted into a text file.

## 2. Reading one

```c
proven_err_t proven_cert_parse(proven_mem_view_t der, proven_cert_t *out);
```

`proven_cert_parse` reads one DER certificate into a `proven_cert_t`. **It copies nothing.**
Every field of the struct is a view into the bytes you passed, which must stay alive for as
long as you use the struct. There is nothing to free.

The reader is strict, on purpose. A certificate is input from a stranger, and an encoding that
two programs can read differently is how one of them gets fooled. So: definite lengths only,
each written in its shortest form; integers without needless leading zeros; a `TRUE` that is
exactly `FF`; the signature algorithm named identically inside and outside the signed part; no
extension twice; and not one byte after the end. Anything else is `PROVEN_ERR_INVALID_FORMAT`.

The fields you are most likely to want:

| Field | What it is |
|---|---|
| `der`, `tbs` | The whole certificate, and the part the issuer signed |
| `subject`, `issuer` | The two names, each as DER. Compare them as bytes; do not parse them for a host name (section 4) |
| `not_before`, `not_after` | The validity period, as seconds since 1970-01-01 UTC |
| `public_key_info` | The public key with its algorithm, as DER - what a pin hashes (section 7) |
| `key_kind`, `key`, `rsa_n`, `rsa_e` | The key itself: RSA, P-256, P-384 or Ed25519; `PROVEN_CERT_KEY_UNKNOWN` for anything else |
| `sig_kind`, `sig_hash`, `signature` | How the *issuer* signed this certificate |
| `is_ca`, `has_path_len`, `path_len` | Whether it may issue certificates, and how deep a chain below it may go |
| `key_usage`, `ext_key_usage` | What the key may be used for, as `PROVEN_CERT_KU_*` and `PROVEN_CERT_EKU_*` flags |
| `alt_names` | The names it is for. Walk them with `proven_cert_alt_name_next` |
| `unknown_critical` | It carries an extension marked "do not accept me unless you understand this", which this library does not understand |

An algorithm this library does not know - a key on another curve, a signature with SHA-1 - is
**not** a parse error. The certificate parses with the kind `UNKNOWN`, because a store of system
roots contains a few such certificates and one of them should not cost the rest. It fails later,
in verification, if a chain actually needs it.

```c
bool proven_cert_alt_name_next(const proven_cert_t *cert, proven_size_t *pos,
                               proven_cert_name_kind_t *kind, proven_mem_view_t *value);
```

`proven_cert_alt_name_next` walks the subjectAltName entries. Start with `*pos = 0` and call it
until it returns `false`. A `PROVEN_CERT_NAME_DNS` value is the name's ASCII text; a
`PROVEN_CERT_NAME_IP` value is 4 or 16 address bytes; anything else (an email address, a URI)
comes as `PROVEN_CERT_NAME_OTHER` with its raw content.

## 3. PEM

```c
proven_err_t proven_pem_next(proven_mem_view_t pem, proven_size_t *pos, proven_u8str_view_t *label,
                             proven_mem_mut_t out, proven_size_t *written);
```

`proven_pem_next` finds the next block at or after `*pos`, decodes its Base64 into `out`, sets
`label` to the words after `BEGIN` (`CERTIFICATE` for a certificate), and moves `*pos` past the
block. Call it in a loop to read a file of several. Text between blocks - comments, the
human-readable dump some tools add - is skipped.

| Returns | When |
|---|---|
| `PROVEN_OK` | A block was decoded; `*written` bytes are in `out` |
| `PROVEN_ERR_NOT_FOUND` | No further block |
| `PROVEN_ERR_OUT_OF_BOUNDS` | `out` is too small. `*written` is the size needed and `*pos` has not moved: enlarge and call again |
| `PROVEN_ERR_INVALID_ENCODING` | A block with no matching `END` line, or a body that is not Base64 |

It is not only for certificates: a key file is PEM too, with another label. This call does not
care what is inside.

## 4. Which names it is for

```c
bool proven_cert_matches_host(const proven_cert_t *cert, proven_u8str_view_t host);
```

`proven_cert_matches_host` answers whether the certificate is for `host`, which is a DNS name or
an IP address written as text. The rules are the ones browsers settled on, and each closes a
hole that was once open:

- **Only subjectAltName is consulted.** The subject's "common name" is free text that
  certificates once put a host name in. It is not a host name, and it is not looked at.
- **A wildcard is a whole left-most label and stands for exactly one label.** `*.example.com`
  matches `www.example.com`. It does not match `example.com`, nor `a.b.example.com`, nor does
  `w*.example.com` mean anything. A pattern with fewer than two labels after the star
  (`*.com`) matches nothing.
- **An IP address is matched as an address, against IP entries only.** `192.0.2.1` never matches
  a DNS entry that happens to read `192.0.2.1`, and `2001:db8::1` matches however the address
  was spelled.
- **Letter case does not matter, and one trailing dot on the host is ignored.**

Names are compared as ASCII. An internationalised name must be given in its `xn--` form, which
is how it appears in the certificate.

## 5. Trust anchors

```c
proven_err_t proven_cert_store_create(proven_allocator_t alloc, proven_cert_store_t **out);
void proven_cert_store_destroy(proven_cert_store_t *store);
proven_err_t proven_cert_store_add_der(proven_cert_store_t *store, proven_mem_view_t der);
proven_err_t proven_cert_store_add_pem(proven_cert_store_t *store, proven_mem_view_t pem, proven_size_t *added);
proven_err_t proven_cert_store_add_system(proven_cert_store_t *store, proven_size_t *added);   /* hosted */
proven_size_t proven_cert_store_count(const proven_cert_store_t *store);
```

A `proven_cert_store_t` is the set of certificates you trust. **It owns copies** of what you
add, so the bytes you added from may be freed straight away; `proven_cert_store_destroy` frees
the copies and the store.

**The library carries no roots.** A list of trusted authorities compiled into a library is
stale the day it ships and wrong for half its users. You choose:

- `proven_cert_store_add_system` takes the operating system's roots - the right set for talking
  to the public internet. On Windows it reads the `ROOT` system store. Elsewhere it reads the
  first file it can among the path in the environment variable `SSL_CERT_FILE` and the usual
  bundle locations. It returns `PROVEN_ERR_NOT_FOUND` when the machine has none it can read - a
  minimal container often has none - and that is something to report, not to work around by
  trusting everything. **On Windows the answer can be smaller than you expect:** Windows keeps
  only the roots it has needed so far and fetches others on demand when its own TLS meets them.
  This call reads what is installed and triggers no fetch, so a chain to a root that machine
  has never used is `PROVEN_ERR_UNTRUSTED` there. A program that must reach arbitrary servers
  from Windows should ship a bundle and use `proven_cert_store_add_pem`.
- `proven_cert_store_add_pem` takes a bundle you supply: your organisation's authority, or the
  single certificate of the one server your program talks to. A block that is not a certificate
  and a certificate that does not parse are skipped, and `*added` counts the ones taken. A
  bundle with no certificate at all is `PROVEN_ERR_NOT_FOUND`.
- `proven_cert_store_add_der` adds one.

A store is not changed by verification, so one store may be shared by any number of threads
that only verify. Adding to it while another thread verifies is a race.

## 6. Verifying a chain

```c
proven_err_t proven_cert_verify(const proven_mem_view_t *chain, proven_size_t count,
                                const proven_cert_verify_options_t *options,
                                proven_cert_verify_result_t *result);
```

`chain[0]` is the peer's own certificate. The rest are whatever else the peer sent, **in any
order** - servers are careless about order, and some send certificates that are not needed.
`proven_cert_verify` searches for a path from `chain[0]` to an anchor, using the others as
material. A certificate among them that does not parse is simply not used.

The options:

- `anchors` - the store. Required.
- `host` - the name you meant to reach. Leave it empty to skip the name check, which is right
  only when you check identity some other way (a pin, a client certificate mapped to an account).
- `now` - the time, in seconds since 1970-01-01 UTC. **You supply it.** A library that read the
  clock itself could not be tested against an expired certificate without waiting, and could not
  run where there is no clock. On a hosted system pass the wall-clock time; on a device with no
  battery-backed clock, decide what time you are willing to assume and know that the dates are
  only as good as that.
- `use` - `PROVEN_CERT_USE_SERVER` when the peer is a server, `PROVEN_CERT_USE_CLIENT` when it
  is a client presenting a certificate to you.

The answer comes at two levels. The **error code** is what your program branches on:

| Returns | Meaning | Reasonable response |
|---|---|---|
| `PROVEN_OK` | A path exists and every check passed | Proceed |
| `PROVEN_ERR_UNTRUSTED` | No acceptable path to an anchor | Refuse. Nothing vouches for this certificate |
| `PROVEN_ERR_EXPIRED` | A path exists, but a certificate on it is past its `not_after` | Refuse; tell the user it expired. If *every* site fails this way, suspect your clock |
| `PROVEN_ERR_NOT_YET_VALID` | A path exists, but a certificate on it is before its `not_before` | Refuse; the clock is the usual culprit |
| `PROVEN_ERR_NAME_MISMATCH` | The chain is good and the certificate is for another name | Refuse. This is what a misdirected or intercepted connection looks like |
| `PROVEN_ERR_INVALID_FORMAT` | `chain[0]` is not a certificate | Refuse |
| `PROVEN_ERR_INVALID_ARG` | No chain, no options, or no store | Fix the call |

The **fault** in `result` (optional - pass a null pointer if you do not want it) is the precise
reason, for a log line or an error message: `PROVEN_CERT_FAULT_NO_ISSUER`,
`_BAD_SIGNATURE`, `_ALGORITHM`, `_NOT_A_CA`, `_PATH_LENGTH`, `_NAME_CONSTRAINT`,
`_CRITICAL_EXTENSION`, `_USAGE`, `_EXPIRED`, `_NOT_YET_VALID`, `_NAME_MISMATCH`, `_TOO_DEEP`,
`_MALFORMED`. `result->depth` is the number of certificates in the accepted path, anchor
included.

When several things are wrong at once, the answer is the one closest to success: if a signed
path to an anchor exists but is expired, you are told it expired, not that nothing was found.
That is the difference between "renew your certificate" and "who are you?".

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_13_cert.c -->
```c
#include <string.h>

/*
 * A certificate is a signed statement that a key belongs to a name. This reads one, asks which
 * names it is for, and then decides whether to believe it: a chain to a trust anchor, the dates,
 * and the name - with the error that says which of those failed.
 */

/* A root and a certificate it issued for example.test, made for this example. Ed25519, so they
 * are short. Valid from 2026 to 2036. */
static const char ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBAjCBtaADAgECAhQxTiZoH2JgH7bPWBktoxUyd0d3ZzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMRXhhbXBsZSBSb290MCowBQYDK2VwAyEAJcpfTb5GYzM+\n"
    "bgKssEvuWEC6urqf1Znqz9I8CY2RdLujEzARMA8GA1UdEwEB/wQFMAMBAf8wBQYD\n"
    "K2VwA0EARcXC/GhPDj4gAU3zpeSAhJ033zALpz2P91wtEqbf2R4Z7drW+RSRQGp2\n"
    "Ky/CDeEyx//WjylXEoHVNvixwKrxAw==\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char LEAF_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBPTCB8KADAgECAhRBU6wAvGvpDo28++Tq/BfZ/8DkNzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMZXhhbXBsZS50ZXN0MCowBQYDK2VwAyEA3dq7G1ndiCbb\n"
    "IAS6O7cRIlxdACAdLSsPSRROKyglnTOjTjBMMAwGA1UdEwEB/wQCMAAwJwYDVR0R\n"
    "BCAwHoIMZXhhbXBsZS50ZXN0gg4qLmV4YW1wbGUudGVzdDATBgNVHSUEDDAKBggr\n"
    "BgEFBQcDATAFBgMrZXADQQBVDpl9ZAqf4j2R3NTG8JuKfPJuh+PD99QVdpjnqPAR\n"
    "dgg3sGIps+UTADlifcSOVlqJF6T9ya5lHpO5FwIpLpMI\n"
    "-----END CERTIFICATE-----\n"
    ;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- PEM: text around the bytes ---------------------------------------------
    /* Certificates travel as PEM: Base64 between BEGIN and END lines. proven_pem_next finds the
     * next block and decodes it; the bytes inside are DER, which is what everything else takes. */
    proven_byte_t leaf_der[1024];
    proven_size_t pos = 0, leaf_len = 0;
    proven_u8str_view_t label;
    proven_mem_view_t leaf_pem = { (const proven_byte_t *)LEAF_PEM, sizeof LEAF_PEM - 1 };
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, sizeof leaf_der }, &leaf_len) == PROVEN_OK,
                    "the block decodes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(label, PROVEN_LIT("CERTIFICATE")), "and says what it is");
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, 0 }, &(proven_size_t){ 0 }) == PROVEN_ERR_NOT_FOUND,
                    "there is no second block");

    // ---- reading one certificate --------------------------------------------------
    /* Parsing copies nothing: every field of proven_cert_t is a view into leaf_der, which must
     * stay alive as long as the struct is used. */
    proven_cert_t leaf;
    EXAMPLE_REQUIRE(proven_cert_parse((proven_mem_view_t){ leaf_der, leaf_len }, &leaf) == PROVEN_OK, "it parses");
    EXAMPLE_REQUIRE(leaf.version == 3 && leaf.key_kind == PROVEN_CERT_KEY_ED25519 && leaf.key.size == 32 && !leaf.is_ca,
                    "version 3, an Ed25519 key, not a CA");
    EXAMPLE_REQUIRE(leaf.not_before < 1811808000 && leaf.not_after > 1811808000, "valid in June 2027");

    proven_size_t at = 0, names = 0;
    proven_cert_name_kind_t kind;
    proven_mem_view_t name;
    while (proven_cert_alt_name_next(&leaf, &at, &kind, &name)) {
        if (kind == PROVEN_CERT_NAME_DNS) names++;
    }
    EXAMPLE_REQUIRE(names == 2, "two DNS names in subjectAltName");

    /* Which hosts is it for? Only subjectAltName counts. A wildcard stands for exactly one label. */
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("example.test")), "the name itself");
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("www.example.test")), "one label under the wildcard");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("a.b.example.test")), "but not two");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("example.org")), "and not another name");

    // ---- trust anchors ------------------------------------------------------------
    /* A store holds the certificates you already trust. It copies what it is given, so ROOT_PEM
     * could be freed after this. The library ships no roots: you supply them. */
    proven_cert_store_t *anchors = NULL;
    proven_size_t added = 0;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK, "a store");
    EXAMPLE_REQUIRE(proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &added) == PROVEN_OK &&
                    added == 1 && proven_cert_store_count(anchors) == 1, "one anchor from the bundle");

    // ---- verifying ----------------------------------------------------------------
    /* chain[0] is the peer's certificate; anything else it sent follows, in any order. The time
     * is yours to supply: a library that read the clock itself could not be tested, and could
     * not run where there is no clock. */
    proven_mem_view_t chain[1] = { { leaf_der, leaf_len } };
    proven_cert_verify_options_t options = {
        .anchors = anchors,
        .host = PROVEN_LIT("www.example.test"),
        .now = 1811808000,                       /* 2027-06-01 */
        .use = PROVEN_CERT_USE_SERVER,
    };
    proven_cert_verify_result_t result;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK && result.depth == 2, "a path of two: the leaf and the anchor");

    /* Each way of being wrong has its own answer. The error code is the coarse one; the fault in
     * the result is for the log line. */
    options.host = PROVEN_LIT("example.org");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_NAME_MISMATCH &&
                    result.fault == PROVEN_CERT_FAULT_NAME_MISMATCH, "the chain is good and the name is not");
    options.host = PROVEN_LIT("www.example.test");
    options.now = 2127427200;                    /* 2037-06-01 */
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_EXPIRED, "ten years on, it has expired");
    options.now = 1811808000;
    leaf_der[leaf_len - 1] ^= 1;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_BAD_SIGNATURE, "one changed bit, and the signature no longer verifies");
    leaf_der[leaf_len - 1] ^= 1;

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "a store");
    options.anchors = empty;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_NO_ISSUER, "with no anchors, nothing is trusted");

    // ---- a pin ---------------------------------------------------------------------
    /* A pin is the SHA-256 of the certificate's public key, as SubjectPublicKeyInfo. A program
     * that knows which key it expects compares this, in addition to or instead of the chain. */
    proven_byte_t pin[32], again[32];
    EXAMPLE_REQUIRE(proven_cert_key_sha256(&leaf, pin) == PROVEN_OK, "the pin");
    proven_sha256(leaf.public_key_info, again);
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ pin, 32 }, (proven_mem_view_t){ again, 32 }), "is the hash of the key's encoding");

    // ---- the system's roots -------------------------------------------------------
    /* For talking to the public internet, the anchors are the operating system's. Whether a
     * machine has any is the machine's business, so this example accepts both answers. */
    proven_cert_store_t *system_roots = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &system_roots) == PROVEN_OK, "a store");
    proven_err_t sys = proven_cert_store_add_system(system_roots, &added);
    EXAMPLE_REQUIRE(sys == PROVEN_OK || sys == PROVEN_ERR_NOT_FOUND, "the system store was read, or there is none");
    EXAMPLE_REQUIRE((sys == PROVEN_OK) == (proven_cert_store_count(system_roots) > 0), "and it is not empty when it was read");

    /* A single DER certificate can be added too. */
    proven_byte_t root_der[1024];
    proven_size_t root_len = 0;
    pos = 0;
    EXAMPLE_REQUIRE(proven_pem_next((proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &pos, &label,
                                    (proven_mem_mut_t){ root_der, sizeof root_der }, &root_len) == PROVEN_OK, "the block decodes");
    EXAMPLE_REQUIRE(proven_cert_store_add_der(empty, (proven_mem_view_t){ root_der, root_len }) == PROVEN_OK, "the root as DER");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK, "and now the once-empty store trusts the leaf");

    proven_cert_store_destroy(system_roots);
    proven_cert_store_destroy(empty);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
```

## 7. Pinning

```c
proven_err_t proven_cert_key_sha256(const proven_cert_t *cert, proven_byte_t out[32]);
```

`proven_cert_key_sha256` gives the SHA-256 of the certificate's public key, in its
SubjectPublicKeyInfo encoding. That is a **pin**: a program that knows in advance which key its
server holds can compare this value and refuse everything else, whatever any authority says.

Pin the key, not the certificate. A certificate is reissued every few months and its hash
changes each time; the key underneath can stay. Compare with `proven_mem_equal_ct`, and ship at
least two pins - the key in use and a spare kept offline - because a pinned program whose only
pinned key is lost cannot be fixed without an update.

A pin can be used **in addition to** verification (the chain must be good *and* the key must be
the expected one) or **instead of** it (parse the peer's certificate, compare the pin, never
build a chain). The second is right for a self-signed server you operate; it means the dates and
names in the certificate are not checked at all, so the pin is then the whole of your security.

## 8. What is checked, what is accepted, what is not here

**Checked, for every certificate on the path:**

- The issuer's signature over it.
- The validity period, against the time you supplied. This includes the anchor.
- That its issuer is a CA: `basicConstraints` says so, and `keyUsage`, when present, allows
  signing certificates. An anchor with no extensions at all (version 1) is accepted as a CA
  because you put it in the store; a version 3 certificate that does not say it is a CA is not.
- The path length an issuer allows below itself.
- Name constraints an issuer imposes, for DNS names and IP addresses - the only names this
  library asserts. Constraints on other name forms are not applied.
- Extended key usage, when present: the leaf and every intermediate must allow the use you asked
  for.
- That no critical extension was left unread.

**Accepted algorithms.** Signatures: RSA PKCS #1 v1.5 and RSA-PSS with SHA-256, SHA-384 or
SHA-512, over keys of 2048 to 8192 bits; ECDSA over P-256 and P-384 with the same three hashes;
Ed25519. A signature with SHA-1 or MD5, an RSA key below 2048 bits, and any other curve are
refused, with the fault `PROVEN_CERT_FAULT_ALGORITHM` or `_BAD_SIGNATURE`. The anchor's *own*
signature is not checked - an anchor is trusted because it is in the store - so an old root
signed with SHA-1 still anchors chains.

**Bounds.** A path is at most `PROVEN_CERT_MAX_DEPTH` (10) certificates. At most 16 certificates
beyond the leaf are considered, and the search stops after 64 signature checks: a peer cannot
make verification expensive by sending a tangle.

**Not here, and you should plan around it:**

- **Revocation.** No CRL, no OCSP, no check of a stapled response. A certificate that was valid
  and has since been revoked verifies. Short-lived certificates are the practical defence.
- **Certificate Transparency.** No check that a certificate was publicly logged.
- **Certificate policies.** The policy extensions are recognised so that a certificate carrying
  them is not refused, and are not enforced.
- **Internationalised names.** Compared as ASCII: supply the `xn--` form.
- **Private keys.** `proven_cert_*` reads and holds none; `sign.h` (section 9) does.

**How this was tested.** Certificate chains built by another implementation, one for every
acceptance and refusal listed above; chains saved from public servers, against the system's
roots; and for the signature algorithms underneath, Project Wycheproof - a collection of inputs
built to break implementations in the ways implementations are known to break. The catalog in
`TEST.md` says what each registered test covers.

## 9. Private keys and signatures

`sign.h` (since v0.109.0) signs a digest with a private key and verifies a signature with a
public one - the code the TLS engine signs its handshakes with, opened for programs that sign
files: code, time-stamp requests, CMS. RSA (PKCS #1 v1.5 and PSS) with keys of 2048 to 4096
bits, and ECDSA over P-256 and P-384.

A key is a `proven_pkey_t`, made one of three ways:

- `proven_pkey_parse(alloc, text, rng, &key)`: PEM - "PRIVATE KEY" (PKCS #8, unencrypted),
  "RSA PRIVATE KEY" or "EC PRIVATE KEY" - or the DER of any of them. An encrypted PKCS #8 is
  `PROVEN_ERR_UNSUPPORTED`: section 11 has the pieces to open it.
- `proven_pkey_from_rsa(alloc, &parts, rng, &key)`: n, e, d, p, q, dP, dQ and qInv.
- `proven_pkey_from_ec(alloc, curve, scalar, &key)`.

An RSA key is checked when it is made - n is p times q, and it signs once and verifies that - so
a key that cannot sign is refused then, not at the first signature. It also takes a generator
(`rng`): every RSA signature is blinded, so that what the private exponent works on is not what
an attacker chose, and the blinding starts from random bytes. Pass a `proven_chacha_rng_t`
seeded from `proven_random_bytes` - a cryptographic one, never xoshiro. An EC key needs none.

`proven_pkey_sign(key, scheme, hash, digest, rng, out, &len)` signs. The scheme is
`PROVEN_SIGN_RSA_PKCS1`, `PROVEN_SIGN_RSA_PSS`, `PROVEN_SIGN_ECDSA` (the DER form X.509 and CMS
use) or `PROVEN_SIGN_ECDSA_RAW` (r then s). RSA v1.5 and ECDSA are **deterministic**: ECDSA's
nonce comes from the key and the digest (RFC 6979), so the same key, digest and scheme give the
same bytes, in this version and the next. Only PSS uses `rng`, for its salt. The hash names the
digest: SHA-256, SHA-384, SHA-512, or `PROVEN_HMAC_SHA1` for formats that still ask for it.

Verification needs no key object: `proven_verify_rsa(n, e, ...)`, `proven_verify_ecdsa(curve,
point, ...)`, or `proven_verify_spki(spki, ...)` with a certificate's `public_key_info`. Each
returns `PROVEN_OK` or `PROVEN_ERR_UNTRUSTED`.

```c
/* An RSA key from its parts, as a format that carries them gives them. Each is an unsigned
 * big-endian integer; empty views here stand for the caller's bytes. */
proven_rsa_components_t parts = { 0 };
proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
proven_chacha_rng_t g;
if (!proven_random_bytes(seed, sizeof seed)) return;
proven_chacha_rng_seed(&g, seed);
proven_pkey_t *key = NULL;
if (proven_pkey_from_rsa(alloc, &parts, proven_chacha_rng(&g), &key) != PROVEN_OK) return;
proven_byte_t digest[32], sig[PROVEN_SIGN_MAX_SIZE];
proven_size_t sig_len = 0;
proven_sha256((proven_mem_view_t){ (const proven_byte_t *)"to be signed", 12 }, digest);
if (proven_pkey_sign(key, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, (proven_mem_view_t){ digest, 32 },
                     proven_chacha_rng(&g), (proven_mem_mut_t){ sig, sizeof sig }, &sig_len) == PROVEN_OK) {
    proven_rsa_components_t pub;
    (void)proven_pkey_rsa_components(key, &pub);       /* views into the key: n and e are the public half */
    proven_err_t ok = proven_verify_rsa(pub.n, pub.e, PROVEN_SIGN_RSA_PKCS1, PROVEN_HMAC_SHA256, 0,
                                        (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, sig_len });
    (void)ok;                                           /* PROVEN_OK */
}
proven_pkey_destroy(key);                               /* wipes it */
```

Since v0.111.0 one key may sign on several threads at once: the blinding pair moves on under a
flag held for one squaring, and the signature is made outside it. Destroying a key while another
thread signs with it is still a race. Its parts can
be read back - `proven_pkey_rsa_components`, `proven_pkey_ec_scalar`, `proven_pkey_ec_point` -
as views into the key, and `proven_pkey_spki` writes the public half as the
SubjectPublicKeyInfo a certificate request or a CMS structure needs.

**Certificates for signing.** `proven_cert_verify` takes two more uses for such programs:
`PROVEN_CERT_USE_CODE_SIGNING` (the leaf's extendedKeyUsage must name codeSigning) and
`PROVEN_CERT_USE_TIMESTAMPING` (RFC 3161: it must name timeStamping, alone, and be critical). For
both, a leaf with no extendedKeyUsage is refused - unlike a server's, where none means any - and
its keyUsage, if present, must allow digitalSignature. `proven_cert_t` also has the
`subject_key_id`, the `authority_key_id`, `ext_key_usage_critical` and `ext_key_usage_count`.

**Keys the caller holds** (v0.111.0). A protocol engine keeps its key in its own configuration, or
makes one for a single exchange; for it, `sign.h` also signs with the private value itself:
`proven_ed25519_public`, `proven_ed25519_sign` and `proven_ed25519_verify` (RFC 8032; the message
itself is signed, not a digest), and `proven_ecdsa_sign_scalar` (r then s; the digest may be of
any length - TLS 1.0 signs a SHA-1 one). `PROVEN_SIGN_RSA_PKCS1_RAW` is v1.5 padding over the bytes
given, with no DigestInfo - TLS 1.0 and 1.1's MD5 and SHA-1 together, 36 bytes - for
`proven_pkey_sign` and `proven_verify_rsa`; and `proven_rsa_encrypt_pkcs1` is TLS 1.2's RSA key
exchange. Both are there for old peers, not for new designs.

```c
/* Ed25519 from a 32-byte seed: the message itself is signed. */
proven_byte_t seed[32] = { 9 }, pub[32], sig[64];
const proven_mem_view_t msg = { (const proven_byte_t *)"a message", 9 };
proven_ed25519_public(pub, seed);
proven_ed25519_sign(sig, seed, pub, msg);
if (proven_ed25519_verify(pub, msg, sig) != PROVEN_OK) return;

/* ECDSA from a P-256 scalar: r then s, 64 bytes, deterministic (RFC 6979). */
proven_byte_t scalar[32] = { 1 }, digest[32], rs[64];
proven_sha256(msg, digest);
if (proven_ecdsa_sign_scalar(PROVEN_PKEY_P256, PROVEN_HMAC_SHA256, (proven_mem_view_t){ scalar, 32 },
                             (proven_mem_view_t){ digest, 32 }, rs) != PROVEN_OK) return;

/* TLS 1.2's RSA key exchange: 48 secret bytes under the server's (n, e), padded from random
 * bytes as many as the modulus has. n and e stand for a certificate's rsa_n and rsa_e. */
proven_mem_view_t n = { 0 }, e = { 0 };
proven_byte_t secret[48] = { 3 }, random[PROVEN_SIGN_MAX_SIZE], sealed[PROVEN_SIGN_MAX_SIZE];
proven_size_t sealed_len = 0;
if (!proven_random_bytes(random, sizeof random)) return;
if (proven_rsa_encrypt_pkcs1(n, e, (proven_mem_view_t){ secret, 48 }, (proven_mem_view_t){ random, sizeof random },
                             (proven_mem_mut_t){ sealed, sizeof sealed }, &sealed_len) != PROVEN_OK) return;
```

## 10. DER, read and written

`der.h` is the DER reader this library's X.509 and key code uses, and a writer for building what
a signer sends: a SignedData, a time-stamp request, a certificate request. The reader takes one
element at a time off the front of a view and accepts only DER: definite lengths in their
shortest form, nothing past the input, one-byte tags. The writer fills caller memory; a
constructed element is opened with `proven_der_begin` and its length written by
`proven_der_end`, which moves the content along when the length needs more than a byte.
`proven_der_end_set_of` sorts a SET OF into DER order on the way out - CMS's signed attributes
must be in it, and a signature over unsorted ones does not verify elsewhere. Errors are sticky:
check `proven_der_writer_finish` once at the end.

## 11. Encrypted key files: PBKDF2 and AES-CBC

An encrypted PKCS #8 ("ENCRYPTED PRIVATE KEY") or a PKCS #12 file is opened by deriving a key from
the password with PBKDF2 (`proven_pbkdf2`, `hmac.h`), decrypting with AES-CBC
(`proven_aes_cbc_decrypt`, `aes.h`, 128-, 192- or 256-bit keys) and removing the PKCS #7 padding
(`proven_pkcs7_unpad`); the salt, the iteration count, the cipher and the IV are read from the
file with `der.h`. The decrypted bytes are the PKCS #8 that `proven_pkey_parse` reads. CBC is not
authenticated; it is here to read what other tools wrote, not for new designs. For many calls
under one key - the legacy TLS suites' records - `proven_aes_cbc_key_init` prepares it once and
`proven_aes_cbc_encrypt_with` and `proven_aes_cbc_decrypt_with` use it (v0.111.0).

```c
/* One key, many records: prepared once, each call whole blocks, the IV carried along. */
proven_byte_t key[16] = { 7 }, iv[16] = { 0 }, record[32] = { 0 };
proven_aes_cbc_key_t k;
if (proven_aes_cbc_key_init(&k, (proven_mem_view_t){ key, 16 }) != PROVEN_OK) return;
if (proven_aes_cbc_encrypt_with(&k, iv, (proven_mem_mut_t){ record, 32 }) != PROVEN_OK) return;
if (proven_aes_cbc_decrypt_with(&k, iv, (proven_mem_mut_t){ record, 32 }) != PROVEN_OK) return;
proven_mem_wipe((proven_mem_mut_t){ (proven_byte_t *)&k, sizeof k });
```

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_13_sign.c -->
```c
#include <string.h>

/*
 * Signing with a private key, and the pieces a signed file format is built from: a key made from
 * its scalar and read back from DER, a signature over a digest and its verification, DER written
 * and read, and a key file's encryption opened with PBKDF2 and AES-CBC.
 */

static const proven_byte_t OID_EC[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const proven_byte_t OID_P256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- a key, and a signature ------------------------------------------------------
    /* RFC 6979's example key. A real one comes from a file (proven_pkey_parse) or a generator. */
    static const proven_byte_t scalar[32] = {
        0xc9, 0xaf, 0xa9, 0xd8, 0x45, 0xba, 0x75, 0x16, 0x6b, 0x5c, 0x21, 0x57, 0x67, 0xb1, 0xd6, 0x93,
        0x4e, 0x50, 0xc3, 0xdb, 0x36, 0xe8, 0x9b, 0x12, 0x7b, 0x8a, 0x62, 0x2b, 0x12, 0x0f, 0x67, 0x21,
    };
    proven_pkey_t *key = NULL;
    EXAMPLE_REQUIRE(proven_pkey_from_ec(heap, PROVEN_PKEY_P256, (proven_mem_view_t){ scalar, 32 }, &key) == PROVEN_OK, "a P-256 key");
    EXAMPLE_REQUIRE(proven_pkey_kind(key) == PROVEN_PKEY_P256 && proven_pkey_bits(key) == 256, "of 256 bits");

    /* What is signed is a digest: the format says which hash. */
    proven_byte_t digest[32];
    proven_sha256((proven_mem_view_t){ (const proven_byte_t *)"sample", 6 }, digest);
    proven_byte_t sig[PROVEN_SIGN_MAX_SIZE];
    proven_size_t sig_len = 0;
    /* ECDSA's nonce comes from the key and the digest (RFC 6979): no generator, and the same
     * bytes every time - which a reproducible build of a signed file needs. */
    EXAMPLE_REQUIRE(proven_pkey_sign(key, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256, (proven_mem_view_t){ digest, 32 },
                                     (proven_rng_t){ 0 }, (proven_mem_mut_t){ sig, sizeof sig }, &sig_len) == PROVEN_OK, "signed, as DER");

    /* Anyone with the public half can check it: the point, or the SubjectPublicKeyInfo a
     * certificate carries (proven_cert_t's public_key_info). */
    proven_mem_view_t point;
    EXAMPLE_REQUIRE(proven_pkey_ec_point(key, &point) == PROVEN_OK && point.size == 65, "the public point");
    EXAMPLE_REQUIRE(proven_verify_ecdsa(PROVEN_PKEY_P256, point, PROVEN_SIGN_ECDSA, (proven_mem_view_t){ digest, 32 },
                                        (proven_mem_view_t){ sig, sig_len }) == PROVEN_OK, "verifies under the point");
    proven_byte_t spki[600];
    proven_size_t spki_len = 0;
    EXAMPLE_REQUIRE(proven_pkey_spki(key, (proven_mem_mut_t){ spki, sizeof spki }, &spki_len) == PROVEN_OK, "the SubjectPublicKeyInfo");
    EXAMPLE_REQUIRE(proven_verify_spki((proven_mem_view_t){ spki, spki_len }, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256,
                                       (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, sig_len }) == PROVEN_OK, "verifies under it too");
    digest[0] ^= 1;
    EXAMPLE_REQUIRE(proven_verify_spki((proven_mem_view_t){ spki, spki_len }, PROVEN_SIGN_ECDSA, PROVEN_HMAC_SHA256,
                                       (proven_mem_view_t){ digest, 32 }, (proven_mem_view_t){ sig, sig_len }) == PROVEN_ERR_UNTRUSTED,
                    "and a different digest does not");
    proven_rsa_components_t rsa;
    EXAMPLE_REQUIRE(proven_pkey_rsa_components(key, &rsa) == PROVEN_ERR_INVALID_STATE, "an EC key has no RSA parts");

    // ---- DER, written ---------------------------------------------------------------
    /* The key as an ECPrivateKey (RFC 5915): SEQUENCE { 1, OCTET STRING scalar, [0] curve, [1] point }.
     * Lengths are written when each element closes. */
    proven_mem_view_t secret;
    EXAMPLE_REQUIRE(proven_pkey_ec_scalar(key, &secret) == PROVEN_OK, "its scalar");
    proven_byte_t der[256];
    proven_der_writer_t w;
    proven_der_writer_init(&w, (proven_mem_mut_t){ der, sizeof der });
    proven_der_begin(&w, PROVEN_DER_SEQUENCE);
    proven_der_put_u64(&w, 1);
    proven_der_put(&w, PROVEN_DER_OCTET_STRING, secret);
    proven_der_begin(&w, PROVEN_DER_CONTEXT(0));
    proven_der_put(&w, PROVEN_DER_OID, (proven_mem_view_t){ OID_P256, sizeof OID_P256 });
    proven_der_end(&w);
    proven_der_begin(&w, PROVEN_DER_CONTEXT(1));
    proven_der_put_bits(&w, point);
    proven_der_end(&w);
    proven_der_end(&w);
    proven_size_t der_len = 0;
    EXAMPLE_REQUIRE(proven_der_writer_finish(&w, &der_len) == PROVEN_OK, "written");

    /* proven_pkey_parse takes it as DER, or as PEM text ("EC PRIVATE KEY", "PRIVATE KEY",
     * "RSA PRIVATE KEY"). An RSA key needs a generator, for its blinding; an EC key does not. */
    proven_pkey_t *again = NULL;
    EXAMPLE_REQUIRE(proven_pkey_parse(heap, (proven_mem_view_t){ der, der_len }, (proven_rng_t){ 0 }, &again) == PROVEN_OK, "read back");
    proven_mem_view_t point2;
    EXAMPLE_REQUIRE(proven_pkey_ec_point(again, &point2) == PROVEN_OK && memcmp(point2.ptr, point.ptr, 65) == 0, "the same key");
    proven_pkey_destroy(again);
    proven_mem_wipe((proven_mem_mut_t){ der, sizeof der });     /* it held the secret */

    /* A SET OF closed with proven_der_end_set_of is put in DER order, as CMS requires of the
     * attributes it signs. INTEGER 2 is written before INTEGER 1 here and comes out after it. */
    proven_byte_t set[32];
    proven_der_writer_init(&w, (proven_mem_mut_t){ set, sizeof set });
    proven_der_begin(&w, PROVEN_DER_SET);
    proven_der_put_uint(&w, (proven_mem_view_t){ (const proven_byte_t *)"\x02", 1 });
    proven_der_put_raw(&w, (proven_mem_view_t){ (const proven_byte_t *)"\x02\x01\x01", 3 });
    proven_der_end_set_of(&w);
    proven_size_t set_len = 0;
    EXAMPLE_REQUIRE(proven_der_writer_finish(&w, &set_len) == PROVEN_OK && set_len == 8 && set[4] == 1 && set[7] == 2, "in order");

    // ---- DER, read ------------------------------------------------------------------
    /* SubjectPublicKeyInfo ::= SEQUENCE { SEQUENCE { OID, OID }, BIT STRING }. Each read takes an
     * element off the front of a view; the content is a view into the input, never a copy. */
    proven_mem_view_t in = { spki, spki_len }, seq, alg, oid, bits;
    proven_u8 unused = 0;
    EXAMPLE_REQUIRE(proven_der_expect(&in, PROVEN_DER_SEQUENCE, &seq) == PROVEN_OK && in.size == 0, "the outer SEQUENCE, and nothing after it");
    EXAMPLE_REQUIRE(proven_der_expect(&seq, PROVEN_DER_SEQUENCE, &alg) == PROVEN_OK, "the algorithm");
    EXAMPLE_REQUIRE(proven_der_expect(&alg, PROVEN_DER_OID, &oid) == PROVEN_OK &&
                    proven_der_oid_is(oid, (proven_mem_view_t){ OID_EC, sizeof OID_EC }), "is id-ecPublicKey");
    EXAMPLE_REQUIRE(proven_der_read_bits(&seq, &bits, &unused) == PROVEN_OK && unused == 0 && bits.size == 65, "the point, as bits");
    /* The signature is SEQUENCE { INTEGER r, INTEGER s }: magnitudes without a sign byte. */
    proven_mem_view_t sv = { sig, sig_len }, rs, r, s;
    EXAMPLE_REQUIRE(proven_der_expect(&sv, PROVEN_DER_SEQUENCE, &rs) == PROVEN_OK && proven_der_read_uint(&rs, &r) == PROVEN_OK &&
                    proven_der_read_uint(&rs, &s) == PROVEN_OK && r.size <= 32 && s.size <= 32, "r and s");
    proven_der_item_t item;
    proven_mem_view_t version = { (const proven_byte_t *)"\x02\x01\x01", 3 };
    proven_u64 v = 0;
    EXAMPLE_REQUIRE(proven_der_read(&version, &item) == PROVEN_OK && item.tag == PROVEN_DER_INTEGER, "any element, by its tag");
    version = item.whole;
    EXAMPLE_REQUIRE(proven_der_read_u64(&version, &v) == PROVEN_OK && v == 1, "a small INTEGER");

    // ---- an encrypted key file's pieces ---------------------------------------------
    /* PBES2 (PKCS #8, PKCS #12): a key from the password with PBKDF2, then AES-CBC and PKCS #7
     * padding. Here one is made, then opened. The salt, count and IV are what the file says. */
    proven_byte_t aes_key[32];
    EXAMPLE_REQUIRE(proven_pbkdf2(PROVEN_HMAC_SHA256, (proven_mem_view_t){ (const proven_byte_t *)"correct horse", 13 },
                                  (proven_mem_view_t){ (const proven_byte_t *)"8 bytes!", 8 }, 2048,
                                  (proven_mem_mut_t){ aes_key, 32 }) == PROVEN_OK, "the key from the password");
    proven_byte_t box[16] = "fourteen bytes";
    memset(box + 14, 2, 2);                    /* 2 bytes of padding, each 2: one whole block */
    proven_byte_t iv[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, iv2[16];
    memcpy(iv2, iv, 16);
    EXAMPLE_REQUIRE(proven_aes_cbc_encrypt((proven_mem_view_t){ aes_key, 32 }, iv, (proven_mem_mut_t){ box, 16 }) == PROVEN_OK, "encrypted");
    EXAMPLE_REQUIRE(proven_aes_cbc_decrypt((proven_mem_view_t){ aes_key, 32 }, iv2, (proven_mem_mut_t){ box, 16 }) == PROVEN_OK, "decrypted");
    proven_size_t plain_len = 0;
    EXAMPLE_REQUIRE(proven_pkcs7_unpad((proven_mem_view_t){ box, 16 }, 16, &plain_len) == PROVEN_OK && plain_len == 14 &&
                    memcmp(box, "fourteen bytes", 14) == 0, "and unpadded");
    proven_mem_wipe((proven_mem_mut_t){ aes_key, 32 });

    proven_pkey_destroy(key);
    return EXAMPLE_OK();
}
```

## 12. Encryption and key exchange: aead.h and kex.h

Since v0.111.0 the ciphers and key exchanges the TLS and QUIC engines use are public, so that the
network library needs nothing of this one but its headers - and so that a program can use them
for its own protocol or file format.

`aead.h` is authenticated encryption: AES-GCM (keys of 16 or 32 bytes, prepared once in a
`proven_aes_gcm_t`) and ChaCha20-Poly1305 (a 32-byte key). Seal writes the ciphertext and a 16-byte
tag; open checks the tag first and gives `PROVEN_ERR_UNTRUSTED` - leaving zeros, never text it
could not authenticate - for anything changed. **A nonce is never used twice under one key:**
doing so gives GCM's authentication key away, and the XOR of two plaintexts with either. Protocols
build the nonce from a sequence number for this reason. `proven_chacha20` (the raw stream) and
`proven_aes_encrypt_block` (one block) are the pieces QUIC's header protection is made of; they
authenticate nothing.

`kex.h` is key exchange: X25519 (RFC 7748), ECDH over P-256 and P-384, and finite-field
Diffie-Hellman over six fixed groups - ffdhe2048, 3072 and 4096 (RFC 7919) and the MODP groups of
the same sizes (RFC 3526). A prime a peer names is never examined, only compared with those six:
`proven_dh_known`. Each side's private value is random bytes from a cryptographic generator, used
for one exchange; what comes out is a shared secret, not a key - put it through
`proven_hkdf_extract`. Peer values that would give a secret anyone can name are refused: an X25519
result of all zeros (`PROVEN_ERR_UNTRUSTED`), a point not on the curve, a DH value outside 2 to
p - 2 (`PROVEN_ERR_INVALID_FORMAT`).

```c
/* Sealing and opening a record: a key, and a nonce never used twice under it. */
proven_byte_t key[32] = { 1 }, nonce[PROVEN_AEAD_NONCE_SIZE] = { 0 }, tag[PROVEN_AEAD_TAG_SIZE];
proven_byte_t text[5] = { 'h', 'e', 'l', 'l', 'o' };
const proven_mem_view_t header = { (const proven_byte_t *)"hdr", 3 };
proven_aes_gcm_t gcm;
if (proven_aes_gcm_init(&gcm, (proven_mem_view_t){ key, 32 }) != PROVEN_OK) return;
proven_aes_gcm_seal(&gcm, nonce, header, (proven_mem_view_t){ text, 5 }, text, tag);          /* in place */
if (proven_aes_gcm_open(&gcm, nonce, header, (proven_mem_view_t){ text, 5 }, tag, text) != PROVEN_OK) return;
proven_mem_wipe((proven_mem_mut_t){ (proven_byte_t *)&gcm, sizeof gcm });
proven_chacha20_poly1305_seal(key, nonce, header, (proven_mem_view_t){ text, 5 }, text, tag);
if (proven_chacha20_poly1305_open(key, nonce, header, (proven_mem_view_t){ text, 5 }, tag, text) != PROVEN_OK) return;

/* X25519: each side's 32 random bytes; the public values cross; both reach one secret. */
proven_byte_t a[32] = { 2 }, b[32] = { 3 }, pa[32], pb[32], sa[32], sb[32];
proven_x25519_public(pa, a);
proven_x25519_public(pb, b);
if (proven_x25519(sa, a, pb) != PROVEN_OK || proven_x25519(sb, b, pa) != PROVEN_OK) return;   /* sa == sb */

/* The same over P-256: points of 65 bytes, a shared X of 32. */
proven_byte_t ka[32] = { 4 }, kb[32] = { 5 }, qa[65], qb[65], za[32];
if (proven_ec_public(PROVEN_PKEY_P256, (proven_mem_view_t){ ka, 32 }, (proven_mem_mut_t){ qa, 65 }) != PROVEN_OK ||
    proven_ec_public(PROVEN_PKEY_P256, (proven_mem_view_t){ kb, 32 }, (proven_mem_mut_t){ qb, 65 }) != PROVEN_OK ||
    proven_ecdh(PROVEN_PKEY_P256, (proven_mem_view_t){ ka, 32 }, (proven_mem_view_t){ qb, 65 }, (proven_mem_mut_t){ za, 32 }) != PROVEN_OK) return;

/* Finite-field DH over ffdhe2048, for a peer that names that group. */
const proven_mem_view_t p = proven_dh_ffdhe2048();
static const proven_byte_t g = 2;
proven_byte_t x[PROVEN_DH_EXP_MAX_BYTES] = { 6 }, gx[PROVEN_DH_MAX_BYTES], z[PROVEN_DH_MAX_BYTES];
proven_size_t z_len = 0;
if (!proven_dh_known(p, (proven_mem_view_t){ &g, 1 })) return;
const proven_mem_view_t xv = { x, proven_dh_exponent_len(p.size) };
if (proven_dh_public(p, xv, gx) != PROVEN_OK || proven_dh_shared(p, xv, (proven_mem_view_t){ gx, p.size }, z, &z_len) != PROVEN_OK) return;
```

The raw pieces, for constructions that authenticate elsewhere:

```c
void proven_chacha20(const proven_byte_t key[32], proven_u32 counter, const proven_byte_t nonce[12],
                     const proven_byte_t *in, proven_byte_t *out, proven_size_t len);
proven_err_t proven_aes_init(proven_aes_t *ctx, proven_mem_view_t key);                 /* 16 or 32 bytes */
void proven_aes_encrypt_block(const proven_aes_t *ctx, const proven_byte_t in[16], proven_byte_t out[16]);
bool proven_aes_hw_available(void);
void proven_sha256_compress(proven_u32 state[8], const proven_byte_t block[64]);        /* hash.h */
void proven_sha512_compress(proven_u64 state[8], const proven_byte_t block[128]);
void proven_sha1_compress(proven_u32 state[5], const proven_byte_t block[64]);          /* hash_legacy.h */
void proven_md5_compress(proven_u32 state[4], const proven_byte_t block[64]);
```

The hashes' compression functions (one block, no padding, no length) are what a MAC that must
take the same time whatever the secret length is gets built from - the legacy TLS records'
defence against "Lucky Thirteen". They are not hashes by themselves.
