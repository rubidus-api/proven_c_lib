# Migrating to the proven_crypto / proven_codec split (v0.113.0)

This guide is written to be followed by an automated agent working in a project that uses this
library, as well as by a person. Every step says what to run, what result to expect, and what to
do with each outcome. Do the steps in order; do not skip the checks.

## What changed

From v0.113.0 the cryptography, certificate and compression code leaves proven_base for two
libraries of its own, in the same tree and under the same version:

| Library | Directories | What it is |
|---|---|---|
| proven_base | `include/proven/`, `src/proven/`, `platform/` | memory, strings, containers, text, numbers, files, time, jobs, the random generators (`random.h`), `url`, `encode`, `utf`, and `hash.h`: CRC-32 and the table hashes (`proven_hash_bytes`, `proven_hash_keyed`) |
| proven_crypto | `crypto/include/proven/`, `crypto/src/`, `crypto/platform/` | SHA-2 (`sha2.h`, new), HMAC (`hmac.h`), the legacy hashes (`hash_legacy.h`), AES (`aes.h`), AEAD ciphers (`aead.h`), key exchange (`kex.h`), signing keys (`sign.h`), DER (`der.h`), X.509 certificates (`cert.h`) |
| proven_codec | `codec/include/proven/`, `codec/src/`, `codec/platform/` | DEFLATE with zlib and gzip framing (`deflate.h`), Zstandard (`zstd.h`), Brotli (`brotli.h`) |
| proven_net | `net/` | unchanged in place; it now stands on proven_crypto and proven_codec as well as proven_base |

What did **not** change:

- Header names, except one (below). `#include "proven/sign.h"` and `#include "proven/zstd.h"` are
  still correct; only the directories that hold them moved.
- Function, type and macro names. Nothing was renamed. No algorithm or output changed.
- `random.h` and `hash.h` stay in proven_base.

What did change:

- **SHA-256, SHA-384 and SHA-512 left `hash.h`** for `proven/sha2.h` in proven_crypto. Code that
  calls `proven_sha256*`, `proven_sha384*` or `proven_sha512*` (or uses `PROVEN_SHA256_SIZE` and
  the like) and includes only `proven/hash.h` must include `proven/sha2.h`. `hash.h` keeps
  `proven_crc32`, `proven_crc32_update`, `proven_hash_bytes` and `proven_hash_keyed`.
- A build that compiles `src/proven/*.c` and `platform/*.c` now builds proven_base only.
- `include/proven.h` includes the crypto and codec headers only when `crypto/include` and
  `codec/include` are on the include path (`__has_include("proven_crypto.h")`,
  `__has_include("proven_codec.h")`). Their own umbrella headers are
  `crypto/include/proven_crypto.h` and `codec/include/proven_codec.h`.
- **Every user of proven_net needs both new libraries**: TLS uses proven_crypto, the HTTP and
  WebSocket drivers proven_codec.
- The system trust store (`proven_cert_store_add_system`) reads `SSL_CERT_FILE` through the base
  library's `proven_env_get`: a value that is not valid UTF-8 is not taken (it was before).

## Step 1 - Is this project affected?

Run from the project's root, excluding the vendored copy (adjust `vendor/proven`):

```sh
grep -rlE --include='*.c' --include='*.h' \
  -e '#include[[:space:]]*"proven/(sha2|hmac|hash_legacy|aes|aead|kex|sign|der|cert)\.h"' \
  -e '\bproven_(sha256|sha384|sha512|hmac|md5|sha1|aes|gcm|chacha20|x25519|ecdh|ec|dh|ed25519|ecdsa|pkey|verify|rsa|der|cert|pbkdf2)_?[a-z0-9_]*\b' \
  . | grep -v '^\./vendor/proven/' > /tmp/proven-crypto-users.txt
grep -rlE --include='*.c' --include='*.h' \
  -e '#include[[:space:]]*"proven/(deflate|zstd|brotli)\.h"' \
  -e '\bproven_(deflate|inflate|gzip|zlib|zstd|brotli)_[a-z0-9_]+' \
  . | grep -v '^\./vendor/proven/' > /tmp/proven-codec-users.txt
grep -rlE --include='*.c' --include='*.h' -e '\bproven_(net|loop|tls|http|hpack|h2|sse|ws)_[a-z0-9_]+' \
  . | grep -v '^\./vendor/proven/' > /tmp/proven-net-users.txt
wc -l /tmp/proven-crypto-users.txt /tmp/proven-codec-users.txt /tmp/proven-net-users.txt
```

- **crypto users** (the first list is not empty) **or net users** (the third): the project needs
  proven_crypto.
- **codec users** (the second) **or net users**: the project needs proven_codec.
- All three empty: the project uses proven_base only. Do step 2, skip steps 3 and 4, do step 5.

## Step 2 - Re-vendor

Replace the vendored copy with the new release's tree: `include/`, `src/proven/`, `platform/`,
`LICENSE` and `THIRD_PARTY_NOTICES.md`; then `crypto/` if the project needs proven_crypto, `codec/`
if it needs proven_codec, and `net/` if it uses the network library (as before).

Check: `include/proven/version.h` in the copy says `proven_c_lib-v0.113.0` or later. Update the
project's record of the vendored version, if it keeps one.

## Step 3 - Build changes

For each library the project needs, add its `include` directory to the include path and compile
its `src/*.c` and `platform/*.c` with the base sources. Link flags are unchanged (`-pthread` on
POSIX; on Windows `-lbcrypt` and `-lcrypt32`, and `-lws2_32` for the network library).

Examples, with the copy at `vendor/proven` and all four libraries:

```sh
# plain compiler call
P=vendor/proven
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -I$P/include -I$P/platform -I$P/crypto/include -I$P/codec/include -I$P/net/include \
  app.c $P/src/proven/*.c $P/platform/*.c $P/crypto/src/*.c $P/crypto/platform/*.c \
  $P/codec/src/*.c $P/codec/platform/*.c $P/net/src/*.c $P/net/platform/*.c -pthread -o app
```

```make
# Makefile
PROVEN := vendor/proven
CPPFLAGS += -I$(PROVEN)/include -I$(PROVEN)/platform -I$(PROVEN)/crypto/include -I$(PROVEN)/codec/include -I$(PROVEN)/net/include
SRCS += $(wildcard $(PROVEN)/src/proven/*.c) $(wildcard $(PROVEN)/platform/*.c) \
        $(wildcard $(PROVEN)/crypto/src/*.c) $(wildcard $(PROVEN)/crypto/platform/*.c) \
        $(wildcard $(PROVEN)/codec/src/*.c) $(wildcard $(PROVEN)/codec/platform/*.c) \
        $(wildcard $(PROVEN)/net/src/*.c) $(wildcard $(PROVEN)/net/platform/*.c)
```

```cmake
# CMake
file(GLOB PROVEN_SRC vendor/proven/src/proven/*.c vendor/proven/platform/*.c
                     vendor/proven/crypto/src/*.c vendor/proven/crypto/platform/*.c
                     vendor/proven/codec/src/*.c vendor/proven/codec/platform/*.c
                     vendor/proven/net/src/*.c vendor/proven/net/platform/*.c)
target_sources(app PRIVATE ${PROVEN_SRC})
target_include_directories(app PRIVATE vendor/proven/include vendor/proven/platform
                           vendor/proven/crypto/include vendor/proven/codec/include vendor/proven/net/include)
```

Leave out the lines of a library the project does not need. A build that lists sources one by one
moves these paths:

- to `crypto/src/`: `aead`, `aes`, `cert`, `cert_issue`, `cert_system`, `crypto_25519`, `crypto_aes`,
  `crypto_chacha`, `crypto_ec`, `crypto_mp`, `crypto_rsa`, `der`, `dh`, `hash_legacy`, `hmac`,
  `kex`, `sign`, and the new `sha2` (SHA-2, formerly inside `src/proven/hash.c`, which stays);
- to `crypto/platform/`: `proven_sys_aes.c`, `proven_sys_chacha.c`, `proven_sys_trust.c`;
- to `codec/src/`: `deflate`, `zstd`, `zstd_encode`, `brotli`;
- to `codec/platform/`: `proven_sys_cpu.c`.

`PROVEN_NO_BROTLI` still leaves Brotli out of proven_codec.

## Step 4 - Source changes

1. SHA-2 through `hash.h`. List the files:

   ```sh
   grep -rlE --include='*.c' --include='*.h' -e '\b(proven_sha(256|384|512)|PROVEN_SHA(256|384|512)_SIZE)' . \
     | grep -v '^\./vendor/proven/' | xargs grep -L 'proven/sha2.h\|proven.h\|proven_crypto.h'
   ```

   For each file printed, add `#include "proven/sha2.h"` next to its `#include "proven/hash.h"`.
   A file that includes `proven.h` or `proven_crypto.h` needs nothing once the include path is set.
2. **The code includes only `proven.h`** and uses crypto or codec names: it works once the include
   paths of step 3 are set. `unknown type name 'proven_pkey_t'` or `implicit declaration of
   function 'proven_zstd_...'` means an include path is missing - fix the build, not the source.
3. **Library-internal headers** (`proven_internal_crypto.h`, `proven_internal_zstd.h`,
   `platform/proven_sys_aes.h`, ...) were never supported; they moved to `crypto/src/`,
   `crypto/platform/`, `codec/src/` and `codec/platform/`. Use the public API instead.

## Step 5 - Verify

1. Build the project from clean. Expected: no errors and no new warnings.
2. Run the project's own tests. Expected: the same results as before.
3. If the project ships byte-exact outputs (signatures, compressed files, digests), compare them
   with the previous build's. Expected: identical.
4. Confirm the vendored copy holds `crypto/` and `codec/` if and only if step 1 said they are
   needed.

**Done** when all four hold. Commit the vendored copy and the build changes together, for example:
"Vendor proven_c_lib v0.113.0 (proven_crypto and proven_codec split)".

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: proven/sign.h: No such file or directory` (or `cert.h`, `hmac.h`, `aead.h`, ...) | `crypto/include` not on the include path, or `crypto/` not vendored | Steps 2 and 3 |
| `fatal error: proven/zstd.h: No such file or directory` (or `deflate.h`, `brotli.h`) | `codec/include` not on the include path, or `codec/` not vendored | Steps 2 and 3 |
| `implicit declaration of function 'proven_sha256'` in a file that includes `proven/hash.h` | SHA-2 moved to `sha2.h` | Step 4, item 1 |
| `undefined reference to 'proven_sha256_...'`, `'proven_pkey_...'`, `'proven_sys_aes_...'` | `crypto/src` or `crypto/platform` not compiled | Step 3 |
| `undefined reference to 'proven_zstd_...'`, `'proven_deflate_...'`, `'proven_sys_cpu_bmi2'` | `codec/src` or `codec/platform` not compiled | Step 3 |
| the same from inside `net/src/...` | a network user without crypto or codec | Both are needed by every network user (step 1) |
| `src/proven/cert.c: No such file` in a build file list | the list names moved sources by their old paths | Step 3, the per-file list |
