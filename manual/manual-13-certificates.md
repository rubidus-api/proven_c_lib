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

**What this is, and what it is not yet.** This is the certificate half of TLS. The protocol
itself - the handshake and the encrypted records - is not in this version: nothing here opens
an `https` connection. What is here is the part you can already use on its own: checking a
certificate you were handed, from a file, a message or another library's handshake.

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
  trusting everything.
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
- **The TLS protocol.** See the top of this chapter.
- **Private keys.** Nothing here reads or holds one.

**How this was tested.** Certificate chains built by another implementation, one for every
acceptance and refusal listed above; chains saved from public servers, against the system's
roots; and for the signature algorithms underneath, Project Wycheproof - a collection of inputs
built to break implementations in the ways implementations are known to break. The catalog in
`TEST.md` says what each registered test covers.
