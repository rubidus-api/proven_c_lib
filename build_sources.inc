/*
 * The library's translation units, in link order, and which of them the freestanding profile
 * builds (RFC-0009 X-005). nob.c's native build and its cross matrix both read this list; it
 * used to be a literal array in nob.c plus two separate copies of the hosted-only exclusions,
 * and nothing checked that the copies agreed. Adding a hosted source here without
 * PROVEN_SOURCE_HOSTED_ONLY puts it in the freestanding build, which then fails loudly.
 *
 * No include guard: define PROVEN_BUILD_SOURCE(path, freestanding) before including it.
 */
#ifndef PROVEN_BUILD_SOURCE
#error "define PROVEN_BUILD_SOURCE before including build_sources.inc"
#endif
#define PROVEN_BUILD_SOURCES_MANIFEST_INCLUDED 1
#define PROVEN_SOURCE_FREESTANDING 1
#define PROVEN_SOURCE_HOSTED_ONLY 0

PROVEN_BUILD_SOURCE("src/proven/stream.c", PROVEN_SOURCE_HOSTED_ONLY)   /* streams sit on fs */
PROVEN_BUILD_SOURCE("src/proven/memory.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/bytes.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/error.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/arena.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/pool.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/buffer.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/heap.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/alloc_check.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/u8str.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/u16str.c", PROVEN_SOURCE_HOSTED_ONLY)   /* UTF-16 strings are left out of the freestanding profile (PROVEN_NO_U16STR) */
PROVEN_BUILD_SOURCE("src/proven/u16str_view.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the view vocabulary (RFC-0044), with u16str.c */
PROVEN_BUILD_SOURCE("src/proven/array.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/ring.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/map.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/algorithm.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/hash.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/sha2.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/hash_legacy.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/hmac.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("codec/src/deflate.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("codec/src/zstd.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("codec/src/zstd_encode.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("codec/src/brotli.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_chacha.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_aes.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_mp.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_ec.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_25519.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/crypto_rsa.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/cert.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("crypto/src/der.c", PROVEN_SOURCE_FREESTANDING)   /* der.h: the strict DER reader and the writer */
PROVEN_BUILD_SOURCE("crypto/src/sign.c", PROVEN_SOURCE_FREESTANDING)   /* sign.h: private keys, signing and verification */
PROVEN_BUILD_SOURCE("crypto/src/aes.c", PROVEN_SOURCE_FREESTANDING)   /* aes.h: CBC and PKCS #7 padding */
PROVEN_BUILD_SOURCE("crypto/src/aead.c", PROVEN_SOURCE_FREESTANDING)   /* aead.h: AES-GCM, ChaCha20-Poly1305, raw ChaCha20 and AES */
PROVEN_BUILD_SOURCE("crypto/src/kex.c", PROVEN_SOURCE_FREESTANDING)   /* kex.h: X25519, ECDH */
PROVEN_BUILD_SOURCE("crypto/src/dh.c", PROVEN_SOURCE_FREESTANDING)   /* kex.h: finite-field DH over six known groups */
PROVEN_BUILD_SOURCE("net/src/tls_keys.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/tls12_keys.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/tls_legacy.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/tls_config.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/tls13.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/quic_crypto.c", PROVEN_SOURCE_FREESTANDING)   /* QUIC packet protection and transport parameters; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("net/src/quic_frame.c", PROVEN_SOURCE_FREESTANDING)   /* QUIC packet headers and frames; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("net/src/quic_conn.c", PROVEN_SOURCE_FREESTANDING)   /* a QUIC connection: streams, loss recovery, NewReno, no socket; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("net/src/qpack.c", PROVEN_SOURCE_FREESTANDING)   /* QPACK with a dynamic table of capacity 0; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("net/src/h3_conn.c", PROVEN_SOURCE_FREESTANDING)   /* an HTTP/3 connection over a QUIC one, no socket; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("crypto/src/cert_issue.c", PROVEN_SOURCE_FREESTANDING)   /* certificates and keys written, for tests (proven_internal_cert_issue.h) */
PROVEN_BUILD_SOURCE("net/src/tls_transport.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the engine over a transport */
PROVEN_BUILD_SOURCE("net/src/loop.c", PROVEN_SOURCE_HOSTED_ONLY)   /* an event loop over the selector */
PROVEN_BUILD_SOURCE("net/src/http_event.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the event-driven HTTP server */
PROVEN_BUILD_SOURCE("net/src/http_event_h2.c", PROVEN_SOURCE_HOSTED_ONLY)   /* HTTP/2 in the event-driven server: the driver of proven_h2_conn_t */
PROVEN_BUILD_SOURCE("net/src/http_event_h3.c", PROVEN_SOURCE_HOSTED_ONLY)   /* HTTP/3 in the event-driven server: UDP sockets, connections by their IDs, the driver of h3_conn; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("net/src/ws_event.c", PROVEN_SOURCE_HOSTED_ONLY)   /* WebSocket on the event-driven server */
PROVEN_BUILD_SOURCE("net/src/ws_event_client.c", PROVEN_SOURCE_HOSTED_ONLY)   /* a WebSocket connection made by this side, on a loop */
PROVEN_BUILD_SOURCE("net/src/http_event_client.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the event-driven HTTP client */
PROVEN_BUILD_SOURCE("net/src/http_event_client_h2.c", PROVEN_SOURCE_HOSTED_ONLY)   /* HTTP/2 in the event-driven client: connections kept, requests on them */
PROVEN_BUILD_SOURCE("net/src/http_event_client_h3.c", PROVEN_SOURCE_HOSTED_ONLY)   /* HTTP/3 in the event-driven client: a QUIC connection per origin on a UDP socket of its own, the driver of h3_conn; empty under PROVEN_NO_HTTP3 */
PROVEN_BUILD_SOURCE("crypto/src/cert_system.c", PROVEN_SOURCE_HOSTED_ONLY)   /* reads the system trust store */
PROVEN_BUILD_SOURCE("src/proven/encode.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/utf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/url.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/http.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/http_auth.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/http_cookie.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/hpack.c", PROVEN_SOURCE_FREESTANDING)   /* header compression for HTTP/2 */
PROVEN_BUILD_SOURCE("net/src/h2.c", PROVEN_SOURCE_FREESTANDING)   /* HTTP/2 frames and header-list rules */
PROVEN_BUILD_SOURCE("net/src/h2_conn.c", PROVEN_SOURCE_FREESTANDING)   /* an HTTP/2 connection: streams and flow control, no socket */
PROVEN_BUILD_SOURCE("net/src/http_coding.c", PROVEN_SOURCE_FREESTANDING)   /* content codings for the HTTP drivers */
PROVEN_BUILD_SOURCE("net/src/sse.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/ws.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/ws_deflate.c", PROVEN_SOURCE_FREESTANDING)   /* permessage-deflate for the WebSocket drivers */
PROVEN_BUILD_SOURCE("src/proven/random.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/fs.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the filesystem */
PROVEN_BUILD_SOURCE("src/proven/time.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("net/src/net.c", PROVEN_SOURCE_HOSTED_ONLY)   /* sockets */
PROVEN_BUILD_SOURCE("src/proven/fmt.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/mmap.c", PROVEN_SOURCE_HOSTED_ONLY)   /* memory mapping */
PROVEN_BUILD_SOURCE("src/proven/sysio.c", PROVEN_SOURCE_HOSTED_ONLY)   /* console and stdio */
PROVEN_BUILD_SOURCE("src/proven/job.c", PROVEN_SOURCE_HOSTED_ONLY)   /* threads */
PROVEN_BUILD_SOURCE("net/src/http_client.c", PROVEN_SOURCE_HOSTED_ONLY)   /* sockets */
PROVEN_BUILD_SOURCE("net/src/http_server.c", PROVEN_SOURCE_HOSTED_ONLY)   /* sockets and threads */
PROVEN_BUILD_SOURCE("net/src/ws_conn.c", PROVEN_SOURCE_HOSTED_ONLY)   /* sockets */
PROVEN_BUILD_SOURCE("src/proven/scan.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_decimal.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_parse.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_format.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/numfmt.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/float_ops.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/float_math.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/complex.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/exp.c", PROVEN_SOURCE_FREESTANDING)   /* CORE-MATH (MIT), the FPU path */
PROVEN_BUILD_SOURCE("math/src/cm/log.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/sin.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cos.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/tan.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atan2.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/pow.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/expf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/logf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/sinf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cosf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/tanf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atan2f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/powf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/exp2.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/exp2f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/exp10.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/exp10f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log2.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log2f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log10.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log10f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/expm1.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/expm1f.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log1p.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/log1pf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cbrt.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cbrtf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/hypot.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/hypotf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/asin.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/asinf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/acos.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/acosf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atan.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atanf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/sinh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/sinhf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cosh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/coshf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/tanh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/tanhf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/asinh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/asinhf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/acosh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/acoshf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atanh.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/atanhf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("math/src/cm/cm_rt_wasm.c", PROVEN_SOURCE_FREESTANDING)   /* the 128-bit helpers on wasm (weak) */
PROVEN_BUILD_SOURCE("src/proven/panic.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("platform/proven_sys_mem.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the freestanding build uses the inline fallbacks */
PROVEN_BUILD_SOURCE("platform/proven_sys_fs.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_time.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("net/platform/proven_sys_net.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_env.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("crypto/platform/proven_sys_trust.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("crypto/platform/proven_sys_aes.c", PROVEN_SOURCE_HOSTED_ONLY)   /* hardware AES; the freestanding build is portable only */
PROVEN_BUILD_SOURCE("crypto/platform/proven_sys_chacha.c", PROVEN_SOURCE_HOSTED_ONLY)   /* ChaCha20 on vector lanes; the freestanding build is portable only */
PROVEN_BUILD_SOURCE("codec/platform/proven_sys_cpu.c", PROVEN_SOURCE_HOSTED_ONLY)   /* processor features asked at run time; the freestanding build asks none */
PROVEN_BUILD_SOURCE("platform/proven_sys_random.c", PROVEN_SOURCE_HOSTED_ONLY)   /* no OS CSPRNG on bare metal */
PROVEN_BUILD_SOURCE("platform/proven_sys_thread.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_io.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_crc.c", PROVEN_SOURCE_HOSTED_ONLY)   /* carry-less multiply asked at run time; the freestanding build uses the tables */
PROVEN_BUILD_SOURCE("platform/proven_sys_math.c", PROVEN_SOURCE_FREESTANDING)

#undef PROVEN_SOURCE_FREESTANDING
#undef PROVEN_SOURCE_HOSTED_ONLY
