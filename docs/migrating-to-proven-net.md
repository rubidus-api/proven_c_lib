# Migrating to the proven_base / proven_net split (v0.111.0)

This guide is written to be followed by an automated agent working in a project that uses this
library, as well as by a person. Every step says what to run, what result to expect, and what to
do with each outcome. Do the steps in order; do not skip the checks.

## What changed

From v0.111.0 the tree holds two libraries under one version:

| Library | Directories | What it is |
|---|---|---|
| proven_base | `include/proven/`, `src/proven/`, `platform/` | everything that does not talk to a network: memory, strings, containers, text, numbers, files, time, jobs, compression, hashes, certificates, signatures, ciphers, key exchange, `url`, `encode`, `utf` |
| proven_net | `net/include/proven/`, `net/src/`, `net/platform/` | sockets (`net`), the event loop (`loop`), TLS (`tls`), HTTP/1.1 (`http`, `http_auth`, `http_cookie`, `http_client`, `http_server`, `http_event`, `http_event_client`), HTTP/2 (`hpack`, `h2`), HTTP/3 and QUIC, WebSocket (`ws`, `ws_conn`, `ws_event`), server-sent events (`sse`) |

What did **not** change:

- Header names. `#include "proven/http.h"` and `#include "proven/tls.h"` are still correct; only
  the directory that holds those files moved (to `net/include/proven/`).
- Function, type and macro names. Nothing was renamed.
- The base library's directories. A project that uses no network code changes nothing but the
  vendored copy.

What did change:

- The network sources are no longer in `src/proven/` or `platform/`. A build that compiles
  `src/proven/*.c` and `platform/*.c` now builds proven_base only.
- `include/proven.h` includes the network headers only when `net/include` is on the include path
  (it tests with `__has_include("proven_net.h")`). The network library's own umbrella header is
  `net/include/proven_net.h`.
- `PROVEN_NO_NET` still leaves out the hosted network parts when proven_net is compiled; leaving
  out all of proven_net is now done by not compiling `net/` at all.

## Step 1 - Is this project affected?

Run from the project's root, excluding the vendored copy of the library (adjust `vendor/proven`
to wherever the copy lives):

```sh
grep -rlE --include='*.c' --include='*.h' \
  -e '#include[[:space:]]*"proven/(net|loop|tls|http|http_auth|http_cookie|http_client|http_server|http_event|http_event_client|hpack|h2|sse|ws|ws_conn|ws_event)\.h"' \
  -e '\bproven_(net|loop|tls|http|hpack|h2|sse|ws)_[a-z0-9_]+' \
  . | grep -v '^\./vendor/proven/'
```

- **No output:** the project uses proven_base only. Do step 2 (re-vendor), skip steps 3 and 4,
  then do step 5. Done.
- **Output:** each listed file uses the network library. Do every step.

A file that uses only `proven_url_*`, `proven_encode_*`, `proven_cert_*`, `proven_pkey_*`,
`proven_der_*`, `proven_aes_*`, `proven_hmac_*` or the hashes is not affected: those stay in
proven_base. The pattern above does not match them; do not widen it.

## Step 2 - Re-vendor

Replace the vendored copy with the new release's tree. Copy, for proven_base:

- `include/` (the whole directory, `include/proven.h` included)
- `src/proven/`
- `platform/`
- `LICENSE` and `THIRD_PARTY_NOTICES.md`

and, only if step 1 printed anything, also:

- `net/` (the whole directory: `net/include/`, `net/src/`, `net/platform/`)

Check: `include/proven/version.h` in the copy says `proven_c_lib-v0.111.0` or later. If the
project keeps a record of the vendored version (a VERSION file, a comment, a lock file), update it.

Do not copy `net/` into a project that does not use it: proven_base must build without it, and
an unused `net/` only costs compile time.

## Step 3 - Build changes (network users only)

Three additions, whatever the build system:

1. Add `<vendor>/net/include` to the include path, next to `<vendor>/include`.
2. Compile `<vendor>/net/src/*.c` and `<vendor>/net/platform/*.c` together with the base sources.
3. Keep the link flags the network code already needed: `-pthread` on POSIX; on Windows
   `-lws2_32` (Winsock), `-lbcrypt` and `-lcrypt32` (the base library needs the last two too).

Examples, with the copy at `vendor/proven`:

```sh
# plain compiler call
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Ivendor/proven/include -Ivendor/proven/platform -Ivendor/proven/net/include \
  app.c vendor/proven/src/proven/*.c vendor/proven/platform/*.c \
  vendor/proven/net/src/*.c vendor/proven/net/platform/*.c -pthread -o app
```

```make
# Makefile
PROVEN := vendor/proven
CPPFLAGS += -I$(PROVEN)/include -I$(PROVEN)/platform -I$(PROVEN)/net/include
SRCS += $(wildcard $(PROVEN)/src/proven/*.c) $(wildcard $(PROVEN)/platform/*.c) \
        $(wildcard $(PROVEN)/net/src/*.c) $(wildcard $(PROVEN)/net/platform/*.c)
```

```cmake
# CMake
file(GLOB PROVEN_SRC vendor/proven/src/proven/*.c vendor/proven/platform/*.c
                     vendor/proven/net/src/*.c vendor/proven/net/platform/*.c)
target_sources(app PRIVATE ${PROVEN_SRC})
target_include_directories(app PRIVATE vendor/proven/include vendor/proven/platform vendor/proven/net/include)
```

A build that lists the library's sources one by one, rather than by wildcard, must move each
network source's path from `src/proven/X.c` to `net/src/X.c` and `platform/proven_sys_net.c` to
`net/platform/proven_sys_net.c`. The network sources are: `tls13`, `tls_keys`, `tls12_keys`,
`tls_legacy`, `tls_config`, `tls_transport`, `quic_crypto`, `quic_frame`, `quic_conn`, `qpack`,
`h3_conn`, `hpack`, `h2`, `h2_conn`, `http`, `http_auth`, `http_cookie`, `http_coding`,
`http_client`, `http_server`, `http_event`, `http_event_h2`, `http_event_h3`, `http_event_client`,
`http_event_client_h2`, `http_event_client_h3`, `ws`, `ws_conn`, `ws_deflate`, `ws_event`,
`ws_event_client`, `sse`, `net`, `loop`. Two files were renamed and stay in the base:
`src/proven/tls_dh.c` is now `src/proven/dh.c`, and `src/proven/tls_issue.c` is now
`src/proven/cert_issue.c`. A list that names `tls_dh.c` or `tls_issue.c` must use the new names.

A project that defined `PROVEN_NO_NET` to keep the network code out can now simply not compile
`net/`; the macro still works if it compiles `net/` anyway.

## Step 4 - Source changes (network users only)

Usually none. Check these cases:

- **The code includes only `proven.h` and uses network types.** It works when `net/include` is
  on the include path (step 3). If it fails with `unknown type name 'proven_http_...'` or
  `implicit declaration of function 'proven_tls_...'`, the include path is missing - fix the
  build, not the source. Adding `#include "proven_net.h"` is also correct.
- **The code includes a library-internal header** (`proven_internal_*.h`, `platform/*.h`). That
  was never supported. The network library's internal headers moved to `net/src/`; the base
  library's stayed in `src/proven/`. Replace the use with the public API where one exists:

  | Internal use | Public replacement (v0.111.0) |
  |---|---|
  | `proven_crypto_aes_gcm_*`, `proven_crypto_chacha20poly1305_*`, `proven_crypto_chacha20` | `aead.h`: `proven_aes_gcm_init/_seal/_open`, `proven_chacha20_poly1305_seal/_open`, `proven_chacha20` |
  | `proven_crypto_aes_init`, `proven_crypto_aes_block` | `aead.h`: `proven_aes_init`, `proven_aes_encrypt_block` |
  | `proven_crypto_aes_cbc_*` | `aes.h`: `proven_aes_cbc_key_init`, `proven_aes_cbc_encrypt_with/_decrypt_with` |
  | `proven_crypto_x25519*`, `proven_crypto_ecdh`, `proven_crypto_ec_public` | `kex.h`: `proven_x25519`, `proven_x25519_public`, `proven_ecdh`, `proven_ec_public` |
  | `proven_tls_dh_*` | `kex.h`: `proven_dh_ffdhe2048`, `proven_dh_known`, `proven_dh_exponent_len`, `proven_dh_public`, `proven_dh_shared` |
  | `proven_crypto_ed25519_*` | `sign.h`: `proven_ed25519_public/_sign/_verify` |
  | `proven_crypto_ecdsa_sign`, `proven_crypto_ecdsa_verify*` | `sign.h`: `proven_ecdsa_sign_scalar`, `proven_verify_ecdsa` |
  | `proven_crypto_rsa_*` | `sign.h`: `proven_pkey_*`, `proven_verify_rsa` (`PROVEN_SIGN_RSA_PKCS1_RAW` for no DigestInfo), `proven_rsa_encrypt_pkcs1` |
  | `proven_sha256_compress_` and the other `*_compress_` | `hash.h` / `hash_legacy.h`: the same names without the trailing underscore |

- **The code relied on one `proven_pkey_t` being used by one thread at a time.** Nothing to do:
  signing from several threads at once is now allowed.

## Step 5 - Verify

1. Build the project from clean. Expected: no errors and no new warnings.
2. Run the project's own tests. Expected: the same results as before the change.
3. Run the step 1 command again. Expected: the same files as before (the split does not change
   which files use the network library).
4. If the project ships byte-exact outputs (packages, signatures, generated files), compare them
   with the previous build's. Expected: identical - nothing in the split changes what the
   library computes.
5. Confirm the vendored copy contains `net/` if and only if step 1 printed something.

**Done** when all five hold. Commit the vendored copy and the build changes together, with a
message that names the library version, for example: "Vendor proven_c_lib v0.111.0 (proven_base
and proven_net split)".

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: proven/http.h: No such file or directory` (or `tls.h`, `net.h`, ...) | `net/include` not on the include path, or `net/` not vendored | Step 2 (copy `net/`) and step 3 (include path) |
| `undefined reference to 'proven_http_...'` / `'proven_tls_...'` / `'proven_sys_net_...'` | `net/src/*.c` or `net/platform/*.c` not compiled | Step 3, item 2 |
| `undefined reference to 'proven_dh_...'` or `'proven_x25519'` from the network code | an old base copy beside a new `net/` | Re-vendor the whole tree from one release (step 2) |
| `src/proven/tls13.c: No such file` in a build file list | the list names network sources by their old path | Step 3, the per-file list |
| `src/proven/tls_dh.c` or `tls_issue.c: No such file` | the two renamed base files | `src/proven/dh.c`, `src/proven/cert_issue.c` |
| `undefined reference to 'WSAStartup'` (Windows) | Winsock not linked | add `-lws2_32` |
| `unknown type name 'proven_transport_t'` in a file that includes `proven/tls.h` under `PROVEN_FREESTANDING` | the transport parts are hosted only, as before | unchanged behaviour: do not use them in a freestanding build |
