# Changelog

All notable changes to this project will be documented in this file.

The format follows Keep a Changelog:

- keep the log human-curated
- order entries chronologically with the newest first
- group notable changes by release or by `Unreleased`
- use the standard sections `Added`, `Changed`, `Deprecated`, `Removed`,
  `Fixed`, and `Security` when they apply
- avoid dumping raw commit history into the file

Versions are semantic - `MAJOR.MINOR.PATCH` - from v0.0.1 (2026-09-04), and a release
section is `## [x.y.z] - YYYY-MM-DD`. The entries below v0.0.1 carry the date-based
numbers those releases were published under (`proven_c_lib-v26.MM.DDx`) and are left as
written; their tags still exist.

## [Unreleased]

A MINOR release: TLS 1.2, cut down to the part of it that is still defensible. Until now a
peer that spoke nothing newer than 1.2 could not be reached at all.

**This changes what a default configuration does.** A client that used to fail against a
TLS 1.2 server with `PROVEN_ERR_PROTOCOL` now connects, and a server now accepts TLS 1.2
clients. When both sides can speak 1.3 they still do. To keep the old behaviour exactly, set
`min_version = PROVEN_TLS_VERSION_1_3` in the options.

The standing caution applies: a new implementation with no external audit; manual chapter 14
says what was checked and what could not be.

### Added

- **TLS 1.2, both roles.** Six cipher suites - `ECDHE-ECDSA` and `ECDHE-RSA`, each with
  AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305 - over X25519 or P-256, with this side's key
  being P-256, Ed25519 or RSA. Server name indication, ALPN, client certificates, and session
  tickets (RFC 5077).
- **What is deliberately absent from it:** CBC cipher suites, RSA and finite-field
  Diffie-Hellman key exchange, renegotiation, compression, SHA-1 and MD5 in handshake
  signatures, session IDs, and everything older than 1.2. A peer that needs one of those is
  refused. The extended master secret (RFC 7627) is required of every 1.2 peer.
- `proven_tls_options_t` gains `min_version` and `max_version`, with
  `PROVEN_TLS_VERSION_1_2` and `PROVEN_TLS_VERSION_1_3`; zero means the default, 1.2 to 1.3.
  An unknown version, or a minimum above the maximum, is `PROVEN_ERR_INVALID_ARG` when the
  configuration is created.
- `proven_tls_version`: the version a connection agreed on.
- Downgrade protection as RFC 8446 section 4.1.3 describes: a server that answers 1.2 though
  it could speak 1.3 marks its random value, and a client that offered 1.3 and sees the mark
  ends the handshake with `illegal_parameter`. A client hello carrying the fallback signal of
  RFC 7507 below the server's best version is refused with `inappropriate_fallback`.
- A request to renegotiate is declined with the warning `no_renegotiation` and the connection
  carries on; after sixteen such requests in a row it is ended.
- Manual chapter 14 says which part of TLS 1.2 this is and why, what is weaker about a
  resumed 1.2 connection (no fresh key exchange), and how to refuse 1.2.
- Test `test_unit_tls12_keys` (the PRF against its two published vectors, the key block and
  records against known answers); a TLS 1.2 section in `test_unit_tls`.

### Changed

- A default client or server now agrees to TLS 1.2 as well as 1.3 (see above).
- `proven_tls_key_update` returns `PROVEN_ERR_UNSUPPORTED` on a TLS 1.2 connection, which has
  no such message. `proven_tls_write` returns `PROVEN_ERR_OVERFLOW` on a TLS 1.2 connection
  that has sent 2^31 records.
- A server that requires a client certificate and gets none tells a TLS 1.2 client
  `handshake_failure`; a TLS 1.3 client is told `certificate_required`, as before. The error
  on this side is `PROVEN_ERR_UNTRUSTED` in both.
- A client whose key is Ed25519 presents no certificate on a TLS 1.2 connection.

### Fixed

- `test_unit_tls_net` judged "a silent client holds up nobody" by a stopwatch (1.5 s), which
  thread checking on an overloaded machine could exceed. It now checks the thing itself: the
  answer has arrived and the silent client's connection is still open.

## [0.21.0] - 2026-10-10

A MINOR release: a server - or a client - may now present an RSA certificate. Until now this
side of a TLS connection needed a P-256 or Ed25519 key, and a site whose only certificate is
RSA could not use the library's TLS.

The standing caution applies with more force here than anywhere: this is a new implementation
of the operation where a mistake costs the key, it has had no external audit, and manual
chapter 14 says exactly what was checked and what could not be.

### Added

- **RSA keys for this side of a TLS connection.** `proven_tls_config_create` accepts an RSA
  private key of 2048 to 4096 bits, as PKCS #8 (`PRIVATE KEY`) or PKCS #1 (`RSA PRIVATE KEY`),
  for a server's certificate or a client's. The handshake is signed with RSASSA-PSS over
  SHA-256 (`rsa_pss_rsae_sha256`, the scheme TLS 1.3 requires every peer to accept). No
  function is new: the same options, one more kind of key.
- What the signing is: the private operation modulo the two primes (CRT) with a constant-time,
  fixed-window exponentiation; the input blinded, with a pair that changes after every
  signature; and the result verified with the public key before it is released, so that a
  fault in one half cannot hand out the key's factors. The key is tried once - a signature
  made and checked - when the configuration is created.
- **What it costs**, in manual chapter 14, section 9: about 9 ms for one signature with a
  2048-bit key on the development machine, 27 ms with 3072 bits, 55 ms with 4096 - a server
  with a 2048-bit RSA key completes about a hundred new connections a second per core, against
  five hundred with P-256. And about 20 KiB of stack while signing.
- Test `test_unit_crypto_rsa`; an RSA section in `test_unit_tls`. The tests make their RSA keys
  when they start: no key is stored in the tree.

### Changed

- `proven_tls_config_create` no longer answers `PROVEN_ERR_UNSUPPORTED` for an RSA key. An RSA
  key it will not use - outside 2048 to 4096 bits, malformed, or unable to sign - is
  `PROVEN_ERR_INVALID_FORMAT`. `PROVEN_ERR_UNSUPPORTED` remains for other kinds of key (P-384)
  and for encrypted key files.

### Fixed

- `test_unit_loop` judged "no timer fires before its time" by how long the test's own thread
  had slept, and failed once on a loaded 32-bit Windows machine where that thread was kept
  waiting longer than the timer. It now compares the time each timer fired with the time it
  was due. The loop itself was not at fault and is unchanged.
- `test_unit_tls_net` allowed 300 ms for an HTTPS request made while another client's
  handshake was pending, which thread checking under load could exceed; the bound is now
  1.5 s inside a 2 s head timeout.

## [0.20.0] - 2026-10-10

A MINOR release: the event-driven server across several processor cores, and the first
measurements of what the loop of 0.18.0 carries. One function is new; the rest is what was
measured and written down.

### Added

- `proven_http_event_server_adopt`: give an event-driven server a connection that was accepted
  elsewhere. With it several loops, each on a thread with a server of its own, share one
  listening port - a thread accepts and deals connections out by posting to the loops. Manual
  chapter 15, section 13, with a program the build compiles and runs, and the three rules that
  make it correct (adopt on the server's loop thread; the connection carried in memory of its
  own; taken apart acceptor first). Test `test_unit_http_event_loops`.
- **Measurements**, in manual chapter 15, section 14 - one run of a load program that is not
  part of the test suite, on one Linux machine over loopback, with a handler that answers two
  bytes. One loop held 100,000 connections with none lost, at about 480 bytes each of the
  server's resident memory (the kernel's own share was about 7.5 KiB per connection for both
  ends). Ten active connections were served as fast among 50,000 idle ones as among 1,000.
  With 1% active: 184,000 requests a second at 10,000 held, 171,000 at 50,000, 155,000 at
  100,000. Four client processes against one, two and four loops: 167,000, 337,000 and 514,000
  requests a second. The blocking server held the same 100,000 and answered slightly faster,
  at 39 KiB allocated per connection. **On Windows**, where the loop waits with `WSAPoll`, the
  median round trip was 1 ms at 1,000 held connections, 18 ms at 5,000 and 66 ms at 10,000,
  with both ends on one machine: there it is for hundreds to a few thousand connections.

### Changed

- `proven_http_event_server_stop_listening` now also makes the server refuse
  `proven_http_event_server_adopt`, until it is told to listen again.
- Timers further away than one turn of the loop's wheel (16.4 s) - which the server's 30 s and
  60 s defaults are - are now covered by a test with a clock the test moves. No defect was
  found; nothing in the loop's behaviour changed.

## [0.19.0] - 2026-10-10

A MINOR release: the two other things that sit on the event loop of 0.18.0 - WebSocket
connections that cost no thread, and an HTTP client with any number of requests in flight on
one thread. As in 0.18.0, what a connection holds is measured and stated; how many a loop
carries is not yet.

### Added

- `ws_event.h`, WebSocket on the event-driven server (hosted-only): a connection that costs no
  thread. `proven_ws_event_accept` turns a request into a WebSocket inside `on_request` - the
  HTTP exchange ends there, with `on_done(PROVEN_OK)` - configured by
  `proven_ws_event_config_t` with `on_message` (pieces as they arrive, never assembled),
  `on_writable` and `on_closed` (exactly once, with the close code and the reason).
  `proven_ws_stream_send` and `proven_ws_stream_send_piece` take a frame whole or refuse it
  with `PROVEN_ERR_AGAIN`; also `proven_ws_stream_ping`, `_close`, `_abort`, `_pause`,
  `_resume`, `_set_user`, `_user`, `_peer`, `_buffered`. Pings are answered, a silent client
  is pinged and then dropped, and a close is timed out. An idle plain WebSocket connection
  holds 928 bytes of heap measured on x86-64 Linux; the test fails above 1 KiB.
- `http_event_client.h`, an HTTP/1.1 client on a loop (hosted-only): any number of requests in
  flight on one thread. `proven_http_event_client_create`, `_destroy`, `_requests`, `_start`;
  callbacks `on_response`, `on_body`, `on_writable`, `on_done` (exactly once for every request
  started, never inside `start`); a request body from memory or written with
  `proven_http_event_request_write` (which may take fewer bytes than offered) and `_end`;
  `proven_http_event_request_abort`, `_pause`, `_resume`, `_set_user`, `_user`. `https` through
  a `proven_tls_config_t`, and refused without one. **Deliberately narrow:** one connection per
  request, closed afterwards; no redirects, challenges, cookies or proxies; and no name
  resolution - a request is given the address to connect to, and manual chapter 15 shows the
  pattern for a name (resolve on a job, post back, start).
- `net.h`: `proven_net_connect_start` and `proven_net_connect_finish`, a connect in two halves
  for a program that waits for many sockets together.
- Manual chapter 15: sections 11 (WebSocket on the loop) and 12 (the event-driven client), with
  two more programs the build compiles and runs. Tests `test_unit_ws_event` and
  `test_unit_http_event_client`. 33 aliases.

### Changed

- `loop.h`: timers that come due in the same round of the loop now fire in the order of their
  times. Before, when the loop had been kept from looking for more than one tick - by a long
  callback, say - those due together fired latest first. Nothing documented an order; the
  order is now tested.
- An idle plain connection of the event-driven server holds 448 bytes of heap, 8 more than in
  0.18.0 (one pointer, for the protocol a connection may be handed to). The 512-byte bound the
  test enforces is unchanged.

**Still not in this release:** any measurement of load; more than one loop; a WebSocket client
on the loop.

## [0.18.0] - 2026-10-10

A MINOR release: an event loop, and an HTTP server on it in which no call waits. It is the
first step toward serving very many connections from one thread; this release states what a
connection holds and makes no claim yet about how many a loop carries.

### Added

- `loop.h`, an event loop (hosted-only). `proven_loop_create`, `_destroy`, `_run`, `_poll`,
  `_stop`; socket readiness with caller-owned registrations (`proven_loop_io_add`, `_io_set`,
  `_io_remove`, `_io_count`); timers that fire once, held by the caller and constant-time to
  set, cancel and fire, with a 16 ms tick (`proven_loop_timer_set`, `_timer_cancel`,
  `_timer_is_set`); `proven_loop_post`, safe from any thread, for work done elsewhere to come
  back; and a 64 KiB scratch buffer shared by all callbacks (`proven_loop_scratch`), plus
  `proven_loop_allocator`.
- `http_event.h`, an HTTP/1.1 server on a loop in which no call waits (hosted-only).
  `proven_http_event_server_create`, `_listen`, `_destroy`, `_connections`, `_stop_listening`,
  configured by `proven_http_event_server_config_t` with four callbacks (`on_request`,
  `on_body`, `on_writable`, `on_done`). A response is sent whole
  (`proven_http_stream_respond`) or in pieces (`proven_http_stream_begin`, `_write`, `_end`),
  at once or at any later time on the loop's thread; `proven_http_stream_write` takes only
  what fits under `max_buffered_output` and `on_writable` says when to continue. Also
  `proven_http_stream_abort`, `_pause`, `_resume`, `_set_user`, `_user`, `_peer`, `_buffered`
  and `PROVEN_HTTP_EVENT_LENGTH_UNKNOWN`. It speaks TLS when given a `proven_tls_config_t`.
  An idle plain connection holds its struct and no buffer: 440 bytes of heap measured on
  x86-64 Linux, and the test fails above 512.
- Manual chapter 15, "The event loop and the event-driven server", in English and Korean,
  with two programs the build compiles and runs. Tests `test_unit_loop` and
  `test_unit_http_event`. 41 aliases.

**Not in this release:** any measurement of how many connections or requests a loop carries;
more than one loop; WebSocket on the event-driven server; an event-driven client. The blocking
server and client are unchanged.

### Fixed

- Manual chapter 14 said a TLS handshake costs a verifying client "somewhat more" than the
  server's 2 ms. Measured, it is about 5 ms. The sentence now says so.
- `tests/test_unit_tls_vectors.h` did not compile under clang with warnings as errors
  (`-Wstring-concatenation` on a string split over two lines in an array). The generated
  strings are now parenthesised; their values are unchanged.

## [0.17.0] - 2026-10-10

A MINOR release: TLS 1.3, client and server. HTTPS and `wss` work through the HTTP client and
server with one setting each.

**This is a new TLS implementation with no external audit.** It is tested against RFC 8448's
example handshakes, against OpenSSL and GnuTLS in both roles, and against adversarial vectors
for the primitives; manual chapter 14 says exactly what was and was not done. A program whose
users depend on TLS against a capable adversary should terminate it in something audited.

### Added

- `tls.h`. A configuration (`proven_tls_config_create`, `_destroy`) built from
  `proven_tls_options_t`: trust anchors, verification by chain or by public-key pin, this
  side's certificate and key (P-256 or Ed25519; PKCS #8 or `EC PRIVATE KEY`), client
  authentication, ALPN, and the random source and clock (the operating system's by default).
  Every option error is reported when the config is made.
- The engine, with no I/O and in the freestanding profile: `proven_tls_client_create`,
  `proven_tls_server_create`, `proven_tls_conn_destroy`, `proven_tls_feed`,
  `proven_tls_pending_output`, `proven_tls_output_sent`, `proven_tls_is_established`,
  `proven_tls_read`, `proven_tls_write`, `proven_tls_close`, `proven_tls_key_update`, and the
  queries `proven_tls_cipher_suite`, `_alpn`, `_resumed`, `_peer_key_sha256`,
  `_peer_certificate`, `_peer_fault`, `_server_name`, `_alert_received`, `_alert_sent`.
- Hosted: `proven_tls_transport_client` and `proven_tls_transport_server` (the engine over a
  `proven_transport_t`, as a transport), `proven_tls_transport_conn`, and
  `proven_tls_http_wrap`, a `proven_http_tls_wrap_fn` for the HTTP client.
- `proven_http_server_config_t` has a `tls` field: with a TLS configuration every connection
  is HTTPS, the handshake driven by the server's loop without waiting on any connection. A
  WebSocket accepted on such a server is `wss`.
- TLS 1.3 (RFC 8446): `TLS_AES_128_GCM_SHA256`, `TLS_AES_256_GCM_SHA384`,
  `TLS_CHACHA20_POLY1305_SHA256` (ChaCha20 preferred where the processor has no AES
  instructions); X25519 and P-256, with HelloRetryRequest; ECDSA P-256 and Ed25519 for this
  side's key, and ECDSA P-384 and RSA-PSS verified for a peer's; ALPN, server name,
  `record_size_limit`, key update, the middlebox-compatibility messages; session resumption
  with stateless tickets under a rotating key (`proven_tls_session_t` on the client); client
  certificates; public-key pins. No 0-RTT.
- `proven_tls_self_signed`: an Ed25519 key and a self-signed certificate for given names and a
  given period, as PEM - an identity for a server you run yourself, with a pin or as the
  peer's own anchor.
- `PROVEN_CERT_FAULT_PIN_MISMATCH` in `proven_cert_fault_t`.
- Manual chapter 14, "TLS", with two compiled examples in both languages.

### Changed

- `PROVEN_ERR_PROTOCOL` is now returned (by the TLS unit); `proven_http_client_send` passes
  on whatever `tls_wrap` returns, which with `proven_tls_http_wrap` includes
  `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID`, `PROVEN_ERR_NAME_MISMATCH` and
  `PROVEN_ERR_PROTOCOL`.
- `proven_http_server_config_t` gained a field at its end. Code that zero-initialises the
  struct, as the manual says to, is unaffected.
- Manual chapters 9, 11, 12 and 13 no longer say that TLS is absent.

### Not in this release

- TLS 1.2 and earlier; RSA keys for this side's certificate; 0-RTT; revocation checking;
  choosing a certificate by server name; tickets shared between processes.

## [0.16.0] - 2026-10-10

A MINOR release: X.509 certificates, and - not public - the cryptographic primitives the TLS
unit will be built on. There is still no TLS: nothing in this version opens an encrypted
connection.

### Added

- `cert.h`: `proven_cert_parse` (a strict DER reader for the fields validation needs),
  `proven_cert_alt_name_next`, `proven_cert_matches_host` (subjectAltName only; a wildcard is
  one whole left-most label; IP literals match IP entries), `proven_cert_key_sha256` (a
  public-key pin), `proven_pem_next`, a store of trust anchors (`proven_cert_store_create`,
  `_destroy`, `_add_der`, `_add_pem`, `_add_system`, `_count`) and `proven_cert_verify`: path
  building from a peer's certificates to an anchor with signatures, validity at a time the
  caller supplies, CA and path-length rules, name constraints for DNS names and IP addresses,
  extended key usage and critical extensions checked, then the host name. The precise reason
  for a refusal is reported as a `proven_cert_fault_t`. Accepted signatures: RSA PKCS #1 v1.5
  and PSS (2048 to 8192 bits), ECDSA over P-256 and P-384, Ed25519, each with SHA-256, SHA-384
  or SHA-512. Not checked: revocation, Certificate Transparency, certificate policies.
  Everything but `proven_cert_store_add_system` is in the freestanding profile.
- `proven_cert_store_add_system` reads the operating system's roots: the `ROOT` system store
  on Windows; elsewhere the file named by `SSL_CERT_FILE`, then the usual bundle locations.
  On Windows that store holds only the roots the machine has needed so far (the test VM had
  30); the call reads what is installed and does not make Windows fetch more.
- Four error codes: `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID`,
  `PROVEN_ERR_NAME_MISMATCH` and `PROVEN_ERR_PROTOCOL`. `PROVEN_ERR_LAST` is now
  `PROVEN_ERR_PROTOCOL`. `proven_cert_verify` returns the first three and
  `PROVEN_ERR_UNTRUSTED`; nothing returns `PROVEN_ERR_PROTOCOL` yet - it is added with the
  others so that the set changes once.
- Manual chapter 13, "Certificates and trust", with a compiled example in both languages.
- Internal, not public API: ChaCha20-Poly1305; AES-GCM, bitsliced in portable C and on AES-NI
  with PCLMULQDQ or the ARMv8 cryptography extension where the processor has them, chosen at
  run time; constant-time multi-precision arithmetic; P-256 and P-384 (ECDH, ECDSA signing with
  RFC 6979 nonces, verification); X25519; Ed25519; RSA signature verification. There is no
  table-driven AES in any configuration.

### Changed

- Windows builds link `crypt32` in addition to `bcrypt` and `ws2_32`, for the system
  certificate store. A program that links the library by hand must add `-lcrypt32`.
- The manual's description of `PROVEN_ERR_UNTRUSTED` no longer says nothing returns it.

## [0.15.0] - 2026-10-10

A MINOR release: the SHA-2 digests, HMAC and HKDF that TLS will be built from, public because
programs need them for their own purposes. One new public header and additions to two;
nothing public is removed or changed.

### Added

- **`proven/hash.h`: SHA-512 and SHA-384** (FIPS 180-4). `proven_sha512`, `proven_sha384`, and
  `_init` / `_update` / `_final` for each, with `PROVEN_SHA512_SIZE` and `PROVEN_SHA384_SIZE`.
- **`proven/hmac.h`: HMAC and HKDF** (RFC 2104, RFC 5869) over SHA-256, SHA-384 and SHA-512.
  `proven_hmac`, `proven_hmac_init`, `_update`, `_final`, `proven_hmac_size`; `proven_hkdf`,
  `proven_hkdf_extract`, `proven_hkdf_expand`. The HMAC state is wiped by `_final`; HKDF
  refuses more than 255 blocks rather than truncating. Not for passwords: both are fast.
- **`proven/memory.h`:** `proven_mem_equal_ct`, a comparison whose time does not depend on where
  two ranges differ - for checking a MAC or a token - and `proven_mem_wipe`, a clear the
  compiler may not remove.
- All of it is pure computation and part of the freestanding profile.
- A section of manual chapter 4, "Authentication and key derivation", with a compiled example,
  in both editions; one test against the vectors of FIPS 180-4, RFC 4231 and RFC 5869.
- `xcv_` aliases for the above, and for `PROVEN_SHA256_SIZE` and `proven_sha256_t`, which had
  none.

## [0.14.0] - 2026-10-10

A MINOR release: readiness that does not slow down as connections are added, and the HTTP
server moved onto it. One addition to a public header; nothing public is removed or changed.

### Added

- **`proven/net.h`: a selector.** `proven_net_selector_create`, `_create_poll`, `_destroy`,
  `_kind`, `_count`, `_add`, `_modify`, `_remove`, `_wait`. A socket is registered once and a
  wait reports only the sockets that are ready - `epoll` on Linux, `kqueue` on the BSDs and
  macOS (written from the manual pages; not compiled or run by this project). Elsewhere,
  Windows included, the same interface is kept over `poll` / `WSAPoll`, behaves identically,
  and still examines every socket on every wait; `proven_net_selector_kind` says which a
  program has. Level-triggered, like `proven_net_poll`.

### Changed

- **The HTTP server's loop waits on a selector and keeps its timeouts on a timer wheel.** A
  round of the loop now costs in proportion to the connections with something to do, not to
  the connections open. Measured on Linux with one busy keep-alive connection and N idle ones:
  a request round trip took about 20 microseconds with 20,000 idle connections, as with none;
  v0.13.0 took about 0.4 ms with 1,000 idle, 3.4 ms with 5,000 and 10 ms with 20,000. No API
  changed. Two limits are unchanged: a connection holds its buffers from the moment it is
  accepted (about 38 KiB of address space with the defaults, about 11 KiB resident while
  idle), and a request occupies a thread while its handler runs.
- Timeouts of the HTTP server are kept to about a sixtieth of a second: a connection may
  outlive its limit by that much, and does not fall short of it.
- `max_connections` no longer sizes any allocation made when the server is created.

## [0.13.0] - 2026-10-09

A MINOR release: WebSocket (RFC 6455), as a codec and as connections, and the hand-over from
HTTP that it needs. Two new public headers and additions to two; nothing public is removed or
changed, and no error code is added.

As with the HTTP drivers, **there is no TLS**: a `ws://` connection can be read and altered on
the network path, and `wss://` is `PROVEN_ERR_UNSUPPORTED` without a `tls_wrap`.

### Added

- **`proven/ws.h`: the WebSocket codec.** Pure - no socket, no allocation, no random source -
  and part of the freestanding profile. The handshake: `proven_ws_make_key`,
  `proven_ws_accept_key`, `proven_ws_check_request`, `proven_ws_request_offers`,
  `proven_ws_check_response`. Frames: `proven_ws_frame_parse`, `proven_ws_frame_write`,
  `proven_ws_mask`. Closing: `proven_ws_close_write`, `proven_ws_close_parse`,
  `proven_ws_close_code_is_valid`, `proven_ws_close_code_for`. And `proven_ws_decoder_init` /
  `proven_ws_decoder_feed`, which turns a byte stream in arbitrary pieces into message pieces,
  pings, pongs and a close, without copying or holding a message, and refuses what RFC 6455
  tells a receiver to refuse: wrong masking for the direction, reserved bits, undefined
  opcodes, lengths not in shortest form, long or fragmented control frames, misplaced
  continuations, text that is not UTF-8 (checked across fragments), close codes that may not
  be sent, and messages past a limit - each with the error that names its close code.
- **`proven/ws_conn.h`: WebSocket connections.** `proven_ws_conn_connect` (through an HTTP
  client, so its proxy, timeouts and `tls_wrap` apply), `proven_ws_conn_accept` (inside an
  HTTP handler), `proven_ws_conn_open` (over any transport); `_send_text`, `_send_binary`,
  `_send_part`, `_ping`, `_receive`, `_close`, `_destroy`, `_protocol`, `_close_code`,
  `_close_reason`, `_pong_count`. Receive returns whole messages up to `max_message_bytes`,
  answers pings and counts pongs on the way; a violation by the peer ends the connection with
  the close code that says which; close is a handshake with a deadline. A connection is for
  one thread at a time, and receiving waits on one connection: a WebSocket costs a thread.
  No extension is offered or accepted - `permessage-deflate` included.
- **`proven/http_server.h`:** `proven_http_exchange_upgrade` answers `101 Switching Protocols`
  and takes the connection out of the server as a transport that owns it, with the bytes
  already received after the request head.
- **`proven/http_client.h`:** the `upgrade` field of a request asks for a protocol change, and
  `proven_http_client_upgrade` takes the connection of a `101` response out of the client in
  the same way.
- Manual chapter 12, "WebSocket", a section on changing protocol in chapter 11, and three
  compiled examples, in both editions. Sixty-three `xcv_` aliases.
- Two tests: `test_unit_ws` (the codec against the examples of RFC 6455 and its receiver
  rules, with every stream decoded whole, split at every byte and a byte at a time) and
  `test_unit_ws_conn` (connections in both handler models, a raw client and a scripted server
  that break the rules).

## [0.12.0] - 2026-10-09

A MINOR release: an HTTP/1.1 client and server, and the pieces around a message that they
need. Five new public headers and additions to three; nothing public is removed or changed,
and no error code is added.

**Neither the client nor the server has TLS.** The server speaks plain HTTP only, and the
client refuses an `https` URL with `PROVEN_ERR_UNSUPPORTED` rather than fetching it in the
clear. What they send can be read and altered on the network path.

### Added

- **`proven/http_server.h`: an HTTP/1.1 server.** One handler function and a loop:
  `proven_http_server_create`, `_listen` (up to four addresses), `_run`, `_poll`, `_stop`,
  `_destroy`, `_connection_count`. Inside a handler: `proven_http_exchange_request`, `_peer`,
  `_read`, `_respond`, `_begin`, `_write`, `_end`. Keep-alive, pipelined requests, chunked
  bodies in both directions, `Expect: 100-continue`, `HEAD`, HTTP/1.0 and the `Date` header
  are the server's business. Requests that are malformed, ambiguous, oversized or slow are
  answered `400`, `408`, `413`, `431`, `501` or `505` before any handler runs. Every wait has
  a limit (`head_timeout_ms`, `body_timeout_ms`, `write_timeout_ms`, `idle_timeout_ms`), every
  size has one (`max_head_bytes`, `max_headers`, `max_body_bytes`, `max_connections`), and
  none can be turned off. Handlers run on the loop's thread, or - with `jobs` set - on a job
  system's workers, where a full queue is answered `503`. A connection being closed is first
  shut down for writing and read from for up to a second, so that the reset a socket with
  unread input sends cannot destroy the response just written. Readiness is
  `proven_net_poll`. No router, no static files, no sessions.
- **`proven/http_client.h`: an HTTP/1.1 client.** `proven_http_client_create`, `_destroy`,
  `_send`, `_get`, `_read`, `_read_all`, `_finish`. The response body is read by the caller in
  pieces; a body cut short is `PROVEN_ERR_RESET`, never an end of file. Redirects are followed
  up to a limit with the method rules of RFC 9110, never from `https` to `http`, and without
  carrying `Authorization`, `Cookie` or `Proxy-Authorization` to another origin. A `401` with
  a Digest or Basic challenge is answered once from configured credentials, and not after a
  cross-origin redirect. Cookies through a jar; connections kept and reused, with one replay
  when a kept connection is dead; interim responses passed over, up to eight; request bodies
  from memory or, chunked, from a `proven_reader_t`; HTTP proxies (absolute-form and `CONNECT`) and SOCKS5 with the host sent
  as a name. A zeroed configuration follows no redirect, keeps no connection and sends no
  credentials. `tls_wrap` is the seam where an encrypted transport attaches; the library does
  not supply one.
- **`proven/http_auth.h`: authentication values.** `proven_http_basic_auth`,
  `proven_http_auth_offers`, `proven_http_digest_challenge_parse` and
  `proven_http_digest_auth` - Digest with MD5, MD5-sess, SHA-256 and SHA-256-sess and
  `qop=auth`, reproducing the worked examples of RFC 7616 and RFC 2617.
- **`proven/http_cookie.h`: a cookie jar.** `proven_http_cookie_jar_init`, `_destroy`,
  `_clear`, `_count`, `_store`, `_header`. `Path`, `Max-Age`, `Expires` and `Secure` per
  RFC 6265. The jar is host-only: without a public suffix list a `Domain` attribute cannot be
  honoured safely, so it is checked and then set aside, and a cookie goes back only to the
  host that set it.
- **`proven/sse.h`: server-sent events.** `proven_sse_init`, `proven_sse_feed`,
  `proven_sse_last_id`: a parser for `text/event-stream` that takes the stream in whatever
  pieces it arrives, in memory the caller supplies.
- **`proven/http.h`: ranges and multipart bodies.** `proven_http_write_range`,
  `proven_http_range_parse` (one byte range in its three spellings, with the unsatisfiable,
  unsupported and malformed cases told apart), `proven_http_write_content_range`,
  `proven_http_content_range_parse`; `proven_http_multipart_boundary` (from sixteen random
  bytes the caller supplies), `proven_http_multipart_write_content_type`, `_write_part`,
  `_write_part_end`, `_write_end`.
- **`proven/url.h`:** `proven_url_resolve` (a reference against a base, RFC 3986 section 5.2)
  and `proven_url_form_append` (one `name=value` pair of a form body).
- **`proven/net.h`:** `proven_net_pair` (two connections joined to each other) and
  `proven_net_waker_t` with `proven_net_waker_open`, `_wake`, `_drain`, `_handle`, `_close` -
  a way to interrupt `proven_net_poll` from another thread.
- The three new text headers and the additions to `url.h` and `http.h` are part of the
  freestanding profile; `http_client.h` and `http_server.h` are hosted-only and are left out
  by `PROVEN_NO_NET`.
- Manual chapter 11, "An HTTP Client and Server", new sections in chapters 9 and 10, and five
  compiled examples, in both editions. Seventy-four `xcv_` aliases.
- Five tests: `test_unit_http_helpers`, `test_unit_http_cookie`, `test_unit_sse`,
  `test_unit_http_server` (both handler models) and `test_unit_http_client` (against the
  library's server and against scripted proxy, SOCKS5 and misbehaving peers).

## [0.11.0] - 2026-10-09

A MINOR release: two new public headers. Nothing public is removed or changed, and no error
code is added.

### Added

- **`proven/url.h`: URLs.** `proven_url_parse` splits an absolute URL into views of what was
  written - nothing is decoded and nothing is repaired; `proven_url_split_target` does the same
  for an HTTP request target. `proven_url_percent_decode` and `proven_url_form_decode`,
  `proven_url_encode_component`, `_encode_path` and `_form_encode`, a query iterator, and
  default ports. Text that is not an absolute RFC 3986 URL with an authority is
  `PROVEN_ERR_INVALID_FORMAT`.
- **`proven_url_path_resolve`: a request path that cannot leave its root.** Decodes once, then
  resolves `.` and `..`; a climb above the root is `PROVEN_ERR_PERMISSION`. An encoded slash, a
  backslash, a NUL or other control character, and text that is not UTF-8 once decoded (which
  catches overlong encodings) are `PROVEN_ERR_INVALID_FORMAT`. It handles path syntax; symbolic
  links and Windows device names are still the caller's to check where the file is opened.
- **`proven/http.h`: an HTTP/1.1 message codec.** The third step of the networking work
  (RFC-0010). A codec, not a client or a server: it parses bytes you have and writes into
  memory you supply.
  - `proven_http_parse_request` and `proven_http_parse_response`: views into the buffer, a
    caller-supplied header array, `PROVEN_ERR_NEED_MORE` for any valid prefix, and a head limit
    that is enforced while the head is still incomplete.
  - Strict by design, because parser disagreement is what request smuggling is made of: bare
    LF or CR line endings, whitespace before a header's colon, obsolete line folding,
    duplicate or non-numeric `Content-Length`, `Transfer-Encoding` together with
    `Content-Length`, and any transfer coding but a single `chunked` are refused.
  - `proven_http_request_framing` and `proven_http_response_framing` decide how the body is
    delimited; `proven_http_body_init`, `_feed`, `_end` and `_received` decode it with no copy
    and a caller-stated size limit, chunked bodies included.
  - Writers that append to a buffer: request and status lines, headers, chunk framing. A
    header name that is not a token, or a value, target or reason containing CR, LF or another
    control character, is `PROVEN_ERR_INVALID_ARG` and nothing is written - response splitting
    cannot be expressed.
  - `proven_http_date_format` and `proven_http_date_parse` (the three forms RFC 9110 requires a
    recipient to read). A valid date later than `proven_time_t` can hold - April 2262 - is
    `PROVEN_ERR_OVERFLOW`, so `Expires` in the year 9999 can be told from a malformed date.
  - Header lookup, token search, method and reason-phrase tables, keep-alive.
- Both headers are pure text handling with no I/O: they are part of the freestanding profile
  (so the cross matrix compiles them for Cortex-M, RISC-V and wasm32) and are **not** removed by
  `PROVEN_NO_NET`.
- **Manual chapter 10, "URLs and HTTP messages"**, in both editions, with three compiled
  examples.
- Tests for what v0.10.0 left untested: `PROVEN_NO_NET` is now a gate
  (`test_portability_compile_nonet`); name resolution through the system resolver; and, on
  POSIX, descriptor exhaustion answering `PROVEN_ERR_BUSY`.
- **What this is not:** an HTTP client or server - nothing here opens a connection. HTTP/2,
  content codings (`gzip`), cookies, authentication, multipart bodies and relative URLs are not
  handled, and trailers after a chunked body are checked and skipped.

## [0.10.0] - 2026-10-09

A MINOR release: a new public header, five new error codes and a new clock call. Nothing public
is removed. **`PROVEN_ERR_LAST` moves**, and an exhaustive `switch` over `proven_err_t` needs
five new cases - the one source-level change a consumer can meet.

### Added

- **`proven/net.h`: sockets.** TCP, UDP and Unix-domain stream sockets on POSIX and Windows,
  behind a new platform unit (`platform/proven_sys_net.c`); hosted only. The second step of the
  networking work (RFC-0010).
  - Every call that can wait takes a deadline - an absolute reading of the monotonic clock, so
    one deadline bounds a whole exchange. `PROVEN_NET_NO_DEADLINE` and `PROVEN_NET_DONT_WAIT`
    are the two ends. A deadline that passes is `PROVEN_ERR_TIMEOUT` and leaves the socket
    usable.
  - Addresses are values: `proven_net_addr_ipv4`, `_ipv6`, `_loopback`, `_any`, `_unix`,
    `_parse`, `_format`, `_eq`. The parser takes literals only and refuses the IPv4 shorthands
    and leading zeros that `inet_aton` accepts; IPv6 is printed in the canonical form of RFC
    5952. `proven_net_resolve` asks the system resolver, and says in its documentation that it
    blocks with no deadline.
  - Streams: `proven_net_listen` (reports the address it bound), `_accept`, `_connect`, `_read`,
    `_write`, `_write_all` (reports how far it got when it fails), `_shutdown_write`, `_close`,
    and the address and `TCP_NODELAY` accessors. End of input is `PROVEN_ERR_EOF`, never a
    zero-byte success.
  - Datagrams: `proven_net_udp_open`, `_send_to`, `_recv_from`, `_addr`, `_close`. A datagram
    larger than the buffer is `PROVEN_ERR_OUT_OF_BOUNDS` with the bytes that were kept.
  - Readiness: `proven_net_poll` for up to 64 sockets and `proven_net_poll_with` for any number,
    with caller-supplied scratch memory. `poll` on POSIX, `WSAPoll` on Windows.
  - `proven_transport_t`: a connection as an interface (read, write, shutdown, close, each with
    a deadline), with `proven_reader_t` and `proven_writer_t` adapters so the line reader and
    `proven_fprint` work over a socket. This is the seam TLS will attach to; there is no TLS in
    this release, and a connection is not encrypted.
  - `PROVEN_NO_NET` leaves the header and both sources out of a hosted build.
- **Five error codes**, appended after `PROVEN_ERR_EXISTS`: `PROVEN_ERR_TIMEOUT`,
  `PROVEN_ERR_REFUSED`, `PROVEN_ERR_RESET`, `PROVEN_ERR_UNREACHABLE` and `PROVEN_ERR_UNTRUSTED`.
  `PROVEN_ERR_LAST` is now `PROVEN_ERR_UNTRUSTED`. Nothing returns `PROVEN_ERR_UNTRUSTED` yet: it
  is reserved for certificate verification and added now so that the enum grows once. **A
  `switch` over `proven_err_t` with no `default` must add the five cases.**
- **`proven_time_monotonic_now`**: nanoseconds on a clock that is never set and never goes
  backwards (`CLOCK_MONOTONIC`; `QueryPerformanceCounter` on Windows). Durations, timeouts and
  deadlines are measured with it. In a freestanding build it returns 0, as `proven_time_now`
  does.
- **Manual chapter 9, "Networking"**, in both editions, with five compiled examples; the
  Windows link line gains `-lws2_32`.

### Changed

- The manual's time section and its example measured a duration with the wall clock while
  explaining why that is wrong. They now use `proven_time_monotonic_now`; `proven_time_now` is
  described as what it is, the wall clock.
- `alias_xcv.h` and the alias index are sorted throughout (65 lines were out of order), and
  the index states the real totals: 660 aliases.
- README: networking is no longer listed as outside the platform boundary. Sockets are in;
  HTTP, WebSocket and TLS are not yet.
- **Windows:** a link of the library now needs `ws2_32` in addition to `bcrypt`. Unix-domain
  sockets need Windows 10 version 1803 or later, and a refused loopback connection is reported
  after about two seconds there, at once on POSIX.

## [0.9.0] - 2026-10-09

A MINOR release: a new public header and a new cross target. Nothing public is removed or
changed, and no error code is added. The first step of the networking work (RFC-0010): the two
digests a WebSocket handshake and old checksum formats need, and the freestanding subset
building for wasm32.

### Added

- **`proven/hash_legacy.h`: SHA-1 and MD5** - `proven_sha1`, `proven_sha1_init` / `_update` /
  `_final`, `proven_sha1_to_hex`, and the same five for `proven_md5`, with `xcv_` aliases. Both
  digests are broken against an adversary and are here only for formats that name them: the
  WebSocket accept key (RFC 6455) is now `proven_sha1` followed by `proven_base64_encode`. They
  are written from FIPS 180-4 and RFC 1321, checked against those documents' vectors and against
  `sha1sum` / `md5sum` at the padding and block boundaries, and are available in freestanding
  builds. The umbrella header includes the new header. Fingerprinting content is still
  `proven_sha256`.
- **`freestanding-wasm32` in `./nob cross`.** Clang compiles every freestanding source for
  wasm32 and `wasm-ld` links them with no C runtime; an undefined symbol fails the link. The
  target is skipped, and reported as skipped, where Clang lacks the wasm32 target or `wasm-ld` is
  missing. It is compiled and linked, not executed, by the matrix.
- **What wasm32 is and is not.** It is the freestanding subset only: no sockets, no filesystem,
  no threads. A wasm32 link needs `__multi3` from compiler-rt's builtins in addition to
  `memcpy`, `memmove`, `memset` and `memcmp`; the cross matrix's link program supplies it because
  a build host need not have those builtins installed.

### Fixed

- **The freestanding profile no longer needs C library headers.** `float_decimal.c` and
  `float_format.c` included `<string.h>` and `proven_sys_math.c` included `<math.h>` in
  freestanding builds. The Cortex-M and RISC-V toolchains ship those headers, so the cross
  matrix passed; a toolchain without them (bare Clang for wasm32) could not compile the three
  files. They now declare the memory functions they call and use the compiler's `isfinite`
  builtin. Hosted builds are unchanged. The freestanding manual said the compile check catches
  a hosted header in a portable file; that was true only where the toolchain lacks the header,
  and the manual now says so.

## [0.8.0] - 2026-10-08

A MINOR release: nothing public removed or added. Behaviour change: the two refusals 0.7.0 left
as `PROVEN_ERR_IO` are named with existing codes. No error code is added, so `PROVEN_ERR_LAST`
does not move.

### Changed

- **A directory that is not empty is `PROVEN_ERR_INVALID_STATE`** from `proven_fs_rmdir` and
  from `proven_fs_remove` (it was `PROVEN_ERR_IO`). Nothing failed: the directory has to be
  emptied first, which is what that code has always meant - fix the order of the calls.
- **A hard link or a rename across file systems is `PROVEN_ERR_UNSUPPORTED`** from
  `proven_fs_link` and `proven_fs_rename` (it was `PROVEN_ERR_IO`). Neither can cross, and
  `proven_fs_rename` never falls back to a copy; the caller copies and removes. Run on Linux
  between two file systems; the Windows mapping (`ERROR_NOT_SAME_DEVICE`) has not been run, for
  want of a second volume on the test machine.
- **`proven_fs_mkdir_all` is verified with absolute paths**: a POSIX absolute path and the root;
  on Windows 11 a drive root, an extended-length `\\?\` path and a UNC path. 0.7.0 had run it
  with relative paths only. No code change was needed.

### Fixed

- README: the `[[nodiscard]]` count is the current one (221 in the public headers).

## [0.7.0] - 2026-10-08

A MINOR release: nothing public removed, one function added. Behaviour change: six filesystem
calls that answered `PROVEN_ERR_IO` for every failure now name the ones a caller can act on. Code
that compared one of these cases against `PROVEN_ERR_IO` must compare against the new code;
`proven_is_ok` checks are unaffected. No error code is added, so `PROVEN_ERR_LAST` does not move.

### Added

- **`proven_fs_mkdir_all(scratch, path)`: a directory and every missing directory above it.**
  A level that is already a directory is not an error, so the call can be repeated and two
  callers can race to create one path. A level that exists and is not a directory stops it with
  `PROVEN_ERR_EXISTS`; the empty path is `PROVEN_ERR_INVALID_ARG`. Directories created before a
  failure stay. Alias `xcv_fs_mkdir_all`.

### Changed

- **`proven_fs_mkdir`, `proven_fs_rmdir`, `proven_fs_chmod`, `proven_fs_link`, `proven_fs_lock`
  and `proven_fs_rename` say which refusal they met.** Each took a yes-or-no answer from the
  platform layer, so a second `proven_fs_mkdir` of one path was `PROVEN_ERR_IO`, the same code
  as a failing disk, and a caller creating the directories of a path had to stat after every
  failure (reported by a downstream project). Now:
  - `proven_fs_mkdir`: `PROVEN_ERR_EXISTS` when the name is taken (by a directory or anything
    else), `PROVEN_ERR_NOT_FOUND` when the parent is missing, `PROVEN_ERR_PERMISSION` when the
    parent refuses it.
  - `proven_fs_rmdir`: `PROVEN_ERR_NOT_FOUND`, `PROVEN_ERR_PERMISSION`, `PROVEN_ERR_BUSY`. A
    directory that is not empty stays `PROVEN_ERR_IO`: no code names that case.
  - `proven_fs_chmod`: `PROVEN_ERR_NOT_FOUND`, `PROVEN_ERR_PERMISSION`.
  - `proven_fs_link`: `PROVEN_ERR_EXISTS` when the new name is taken, `PROVEN_ERR_NOT_FOUND`,
    `PROVEN_ERR_PERMISSION`. A link across file systems stays `PROVEN_ERR_IO`.
  - `proven_fs_lock` with `wait == false`: `PROVEN_ERR_BUSY` when another holder has a
    conflicting lock.
  - `proven_fs_rename`: `PROVEN_ERR_NOT_FOUND` when the source, or the destination's directory,
    is not there (it already reported `PROVEN_ERR_PERMISSION` and `PROVEN_ERR_BUSY`).

  Regression: `tests/test_regression_fs_refusal_codes`, run on Linux and on Windows 11 (x86-64
  and i686) with relative paths; drive-root and UNC paths were not exercised there.

## [0.6.0] - 2026-10-01

A MINOR release: nothing public removed. It carries the whole-library review (RFC-0009): defects
fixed, four security changes, faster CRC-32, SHA-256, directory listing and stream readers, map
iteration, in-place array editing, job groups, and a build that links its tests in parallel.
Behaviour changes, each where the old behaviour was wrong or unsafe: an exclusive create over an
existing name is `PROVEN_ERR_EXISTS` instead of `PROVEN_ERR_IO`; environment values and directory
entry names that are not valid text are `PROVEN_ERR_INVALID_ENCODING` on POSIX too (they were
returned as raw bytes; Windows substituted U+FFFD); `proven_map_hash` values for default
integer-key maps change and now differ per process; and `proven_reader_buffered_t` gains a field.

### Added

- **`PROVEN_ERR_EXISTS`: an exclusive create found the name already there** (RFC-0009 X-009).
  `proven_fs_open` with `PROVEN_FS_CREATE_NEW` over an existing name returned `PROVEN_ERR_IO`,
  the same code as every unclassified failure, so a caller could not tell "choose another name"
  from "the disk failed". It now returns the new code, on POSIX (`EEXIST`) and Windows
  (`ERROR_FILE_EXISTS`). `PROVEN_ERR_LAST` moves to it; alias `XCV_ERR_EXISTS`. Code that
  compared that case against `PROVEN_ERR_IO` must compare against `PROVEN_ERR_EXISTS`.
- **`proven_fs_is_staging_name(name)`**: true for a name with the shape of the temp file an
  atomic or durable write leaves behind when killed mid-write (today's and the old one), for a
  cleanup job. The library itself never removes one it did not just create (RFC-0009 D-001).
- **`proven_random_u64_checked(&out)`: one strong word, and `false` when there is none**
  (RFC-0009 S-002). `proven_random_u64` returns 0 when the entropy source fails - on a bare-metal
  target with no source that is every call - and its documentation offered it for tokens. A
  token of 0 is a predictable secret nothing reports. The checked form is for secrets; the
  unchecked one stays, now documented as not for them. Alias `xcv_random_u64_checked`.
- **`proven_fs_read_all_bounded(alloc, path, max_bytes)`: a whole-file read with a ceiling**
  (RFC-0009 S-003). `proven_fs_read_all` reads until the source ends, so a path naming
  `/dev/zero`, an endless FIFO, or a far larger file than expected grows the buffer until the
  allocator refuses. The bounded form returns `PROVEN_ERR_OUT_OF_BOUNDS` and no buffer past
  `max_bytes`: before allocating when the reported size says so, and as the extra byte arrives
  when the size says nothing. The unbounded functions now say they are for trusted paths. Alias
  `xcv_fs_read_all_bounded`.
- **A map can be walked: `proven_map_iter_init` / `proven_map_iter_next`, and `proven_map_len`**
  (RFC-0009 X-001). Before, a program could not list what it stored, serialise a map, or release
  what its values own without keeping a second list of keys. The walk is in bucket order; removing
  entries (including the current one) and updating values are allowed during it; a new key or a
  reserve that rehashes the map makes the next step `PROVEN_ERR_INVALID_STATE` instead of skipping
  or repeating entries. Aliases `xcv_map_iter_t`, `xcv_map_iter_init`, `xcv_map_iter_next`,
  `xcv_map_len`.
- **Arrays can be edited in place: `proven_array_clear`, `_truncate`, `_insert`, `_remove_at`,
  `_swap_remove` and `_extend`** (RFC-0009 X-002) - the operations callers wrote by hand on top
  of `get_mut` and `len`. Each changes nothing when it fails; `insert` and `extend` accept
  elements taken from the array itself (a source that only partly overlaps it is refused), and
  `extend` reallocates at most once. Aliases `xcv_array_*`.
- **Jobs: `proven_job_submit_ex` says why a submit was refused, and job groups can be waited on**
  (RFC-0009 X-003). `proven_job_submit` returns `false` both for a full queue and for a closed
  system, which call for opposite responses; `_ex` returns `PROVEN_ERR_AGAIN` or
  `PROVEN_ERR_INVALID_STATE`. `proven_job_group_t` counts submitted jobs
  (`proven_job_group_init`, `_submit`, `_pending`); `proven_job_group_wait` returns when all have
  run, running queued jobs itself while it waits, with what they wrote visible afterwards.
  Aliases `xcv_job_submit_ex`, `xcv_job_group_*`.

### Changed

- **SHA-256 is about 25% faster** (RFC-0009 P-107): `proven_sha256_update` compresses whole
  64-byte blocks straight from the input instead of copying every byte into its block buffer
  first. Measured on 4 MiB: 4.7 ns per byte, from 6.2. Digests are unchanged.
- **`./nob build -keep-going` runs every test and lists the failures** (RFC-0009 X-006, first part).
  A run stopped at the first failing test, so one failure hid every other. With the flag, each
  test still runs once and the run ends with a summary and exit status 1.
- **`./nob <mode> -jobs N` runs tests in parallel on POSIX** (RFC-0009 X-006). Tests created
  fixtures under fixed names in the working directory, so they could only run one at a time. With
  the flag each runs in a directory of its own that links to the repository's top level, its output
  is printed whole in registry order, and stress and benchmark tests still run alone. On the
  16-CPU host a cached `./nob asan` takes 8.9 s instead of 16.9, a cached `./nob build` 7.1 s
  instead of 10.1. The default stays one test at a time.
- **A clean `./nob build` takes 14 s instead of 36 on a 16-CPU host** (RFC-0009 P-108). The test
  executables were compiled and linked one at a time - 20 of the 31 seconds a clean build spent
  outside the tests themselves. They are now linked in parallel, up to one per CPU, and then run
  one at a time in registry order as before, with a link failure still reported under the test's
  name. Running the tests in parallel needs per-test scratch files first and is not done.
- **The seven `*_internal` / `*_impl` functions in public headers are marked MACRO SUPPORT and are
  not a stable interface** (RFC-0009 X-004): `proven_u8str_fmt_internal`, `proven_scan_fmt_internal`,
  `proven_scan_fmt_internal_view`, `proven_fmt_to_writer_impl`, `proven_sysio_scanner_scan_impl`,
  `proven_sysio_print_impl`, `proven_sysio_scan_chunk_impl`. Their signatures may change in a MINOR
  release; call the macros. A source-contract test keeps the list closed.
- **`proven_u16_reader_read` with a small destination no longer revalidates everything it holds**
  (RFC-0009 P-103). Each call decoded and validated the whole staged area - up to 512 units - to
  hand out `cap` of them: 261 ns per unit at `cap` 2. It now examines `cap + 1` units: 12 ns per
  unit at `cap` 2, 2.3 at 64 (from 9.6). The text delivered and the error that ends it are the
  same for every `cap`.
- **`proven_reader_read_line` searches each byte once** (RFC-0009 P-104). It searched from the
  start of the pending line, one byte at a time, after every refill, so a long line arriving in
  small pieces cost time quadratic in its length: 4.8 us per byte for a 16,000-byte line read a
  byte at a time. It now remembers how far it has searched and uses the platform `memchr`: 21-25 ns
  per byte for that case, and 0.27 ns per byte from 1.33 for 80-byte lines read from a file.
  `proven_reader_buffered_t` gains a field, `scanned`, set by `proven_reader_buffered`.
- **Listing a directory on POSIX costs one metadata call per entry, or none** (RFC-0009 P-102).
  `proven_fs_dir_next` (and so `proven_fs_list` and the walk) asked for every entry's type twice,
  following and not following symlinks. `readdir`'s `d_type` already says what most entries are:
  a directory or a special file now needs no call, a regular file one (for its size), and only a
  symlink or a filesystem that leaves `d_type` unknown takes both. Measured over 50,000 files:
  3.5 us per entry, from 5.9. What is reported is unchanged.
- **CRC-32 is about 6x faster on long inputs** (RFC-0009 P-101): slicing-by-8, eight bytes per
  step with independent table lookups. Measured on 1 MiB: 0.36 ns per byte, from 2.30. Output is
  unchanged (checked against the single-table path at every length and offset, and against
  zlib). It costs 7 KiB more read-only data; a build with `-DPROVEN_CRC32_SMALL=1` keeps the
  single 1 KiB table.
- **Integer map keys are hashed with the per-process secret in a default map** (RFC-0009 S-001).
  They used a public bit-mix finaliser whatever the map was created with, and its inverse is
  public, so anyone choosing the keys - user ids, record numbers from a request - could put any
  number of them in one bucket and make the map quadratic. A default (`proven_map_create`)
  integer-key map now uses SipHash-1-3 of the key under the same secret as string keys;
  `proven_map_create_trusted` keeps the finaliser. Measured cost on a default map: about 7 ns
  more per lookup on a 1K-key table, 15-20 ns per insert. `proven_map_hash` values for default
  integer-key maps change (and differ per process); trusted maps are unchanged.
- **Text from the platform is strict, like all other text** (RFC-0009 D-003). An environment value
  or a directory entry name that is not valid text - bytes that are not UTF-8 on POSIX, a lone
  surrogate on Windows - is now `PROVEN_ERR_INVALID_ENCODING`. Before, POSIX returned the raw
  bytes as `PROVEN_OK` and Windows silently substituted U+FFFD, which in a listing named a
  different file, or none. `proven_fs_dir_next` and `proven_fs_walk_next` report such an entry
  once (empty `name`, other fields filled in) and go on; `proven_fs_list` refuses the whole
  listing rather than leave a file out.

- **The library's source list is one manifest, `build_sources.inc`, with a freestanding attribute
  per source** (RFC-0009 X-005). It was a literal array in `nob.c`, and the hosted-only sources
  were listed twice more - for the native freestanding build and for the cross matrix - with
  nothing checking the copies agreed. Both now read the manifest; a source-contract test refuses a
  hand-kept list in `nob.c`.

### Fixed

- **`./nob build` failed in an unpacked release archive.** It runs `scripts/project-check.sh`,
  whose checks read the git repository, and the archive has none - so a build from any release
  ZIP stopped at that step. Outside a git checkout the step is now skipped with a
  `[PROVEN][PROJECT_CHECK][SKIP] reason=not-a-git-checkout` line. The archive also gains
  `build_sources.inc`, which `nob.c` now includes.
- **`./nob build` could run a test linked against a library object from the previous build.**
  A test relinked when a library object was newer than the executable, compared by whole-second
  modification time; an object rebuilt in the same second as the previous link compared equal,
  and the old binary ran. The objects' contents now go into each test link's state hash (and
  into the hash recorded after the link, so a cached build still relinks nothing).
- **The view-taking macros say how to pass a compound literal** (RFC-0009 X-008).
  `PROVEN_ARG((proven_u8str_view_t){ p, n })` and the same in `proven_scan_fmt`, the print and
  `append_fmt` macros and the `PROVEN_MAP_*_U8_*` wrappers do not compile - the preprocessor splits
  the literal at its comma - and the error names an internal function. The headers and manual
  chapter 1 (EN/KO) now say to pass a variable, a `PROVEN_LIT`, or the literal in parentheses; a
  test pins the parenthesised form.
- **`proven_random_set_source` is no longer a data race when called while other threads draw**
  (RFC-0009 S-004). The hook was two plain globals, so a late install was undefined behaviour and
  a draw could pair one source's function with the other's context. The pair is now published
  under a sequence count; a draw always sees a function with its own context. Installing once at
  startup is still the advice.
- **Eight leftover staging files no longer block every later atomic write of a path**
  (RFC-0009 D-001). `proven_fs_write_file_atomic` and `_durable` staged into the fixed names
  `<path>.pvtmp00` .. `07` and gave up after them, so eight writers killed mid-write - or anyone
  who could write the directory - made the path unwritable through the library for good, with
  `PROVEN_ERR_IO`. The suffix is now 13 random characters (64 bits from the entropy source), only
  a name collision is retried, any other failure is returned at once with its own code, and
  sixteen collisions in a row are `PROVEN_ERR_EXISTS`. Confidentiality was never affected: the
  staging file is created exclusively, so a planted name was refused, not written through.
- **An environment variable set to the empty string read as `PROVEN_ERR_IO` on Windows**
  (RFC-0009 D-002). `GetEnvironmentVariableW` returns 0 both for failure and for "stored 0
  characters"; `GetLastError` now tells them apart, so the value is an empty string as on POSIX.
  A value that grows between the sizing call and the read is re-sized instead of failing.
- **`fs.h`: `proven_fs_copy` states its contract, and `proven_fs_write_file`'s `[[nodiscard]]`
  sits on its declaration again** (RFC-0009 X-007). The copy's whole documentation was one line;
  it now says the copy is not staged (a failure leaves `dest` partly written), that symbolic
  links are followed at both ends, that `dest` takes the source's permission bits, and that the
  read-only rule covers it. The attribute had been separated from its declaration by a long
  block comment.

## [0.5.0] - 2026-10-01

A MINOR release: nothing public removed, no behaviour changed. Three additions requested by
Pnakotic (B-044): one-character UTF-8 decoding, hexadecimal scanning, and a stated ceiling on the
error-code space. The repository now carries only the library, its tests, the manual and the
public documents; design records, benchmark archives and process notes are kept outside it, and
the public history was rewritten to match.

### Added

- **One-character UTF-8 decoding: `proven_utf8_decode_next(s, pos)`.** For code that walks text a
  character at a time (requested by Pnakotic, which carried a stand-in). Same strict rules as the
  rest of `utf.h`; the returned `len` is always the step to take - the character, the maximal
  subpart of malformed input (where Unicode says to resynchronise), or the bytes left when the
  text ends mid-character (`NEED_MORE`). Nothing is substituted. Freestanding-available.
- **Hexadecimal scanning: `proven_scan_u64_hex`, `proven_scan_i64_hex`, and `{:x}` / `{:X}` in a
  scan format** for any integer argument, mirroring the formatter's spec (requested by Pnakotic for
  `0041..005A`-style ranges). `0x` is taken only before a digit, as `strtoul` takes it; overflow,
  cursor restore and the stream signal behave as for decimal, including a `0x` that arrives before
  its digits. Before, every format placeholder had to be `{}`.
- **The error-code space is a promise: `PROVEN_ERR_RESERVED_END` (0x1000) and `PROVEN_ERR_LAST`.**
  proven will never define a code at or above 0x1000, so a program can carry `proven_err_t` values
  unchanged in a wider type and number its own codes from there (requested by Pnakotic).
  `PROVEN_ERR_LAST` names today's highest code; a build-time assertion holds the ceiling and a
  source contract keeps the macro on the enum's real last value. Aliases `XCV_ERR_LAST`,
  `XCV_ERR_RESERVED_END`, and the missing `XCV_ERR_INVALID_FORMAT`.

### Changed

- **The repository history was rewritten to drop the design records, benchmark archives, process
  notes and private verification scripts** that earlier commits carried under `docs/` and
  `scripts/`. Every commit hash changed, and every tag was moved to its rewritten commit with the
  same library sources; a checkout that pinned an old hash should re-pin by tag. Public documents
  cite the moved records by label only (`RFC-0007`, `B-043`).

### Fixed

- **Chapter 1 listed `proven_err_t` without `PROVEN_ERR_NEED_MORE`,** in both the enum listing and
  the meaning table (English and Korean). Both now carry it.

## [0.4.0] - 2026-09-30

A MINOR release: nothing public removed. The whole test suite now runs natively on Windows
(`./nob build -no-run`, a mingw-w64 build for x86-64 and i686), and its first run found six
Windows defects, all fixed - an empty directory could not be removed, pread/pwrite moved the file
position, positioned calls on a pipe were not refused, and the end of a pipe read as an I/O error
(B-035). Float parsing is faster than glibc at every length measured (B-043), and 32-bit targets
use the Eisel-Lemire fast path (B-042). MSVC and clang-cl are stated as not supported. Behaviour
changes only where it was wrong: `proven_fs_remove` on an empty directory on Windows, and the
Windows pipe and position cases above.

### Added

- **The full test suite runs on Windows (B-035).** `./nob build -no-run` builds every test
  executable and runs none; the build driver asks the compiler for its target, so a mingw-w64
  cross compiler produces static `.exe` tests (with `-lbcrypt`, without `-ldl`).
  `win11kd-full-suite.sh` and `win11kd-run-suite.ps1` build both word sizes and
  run them natively, reporting PASS / FAIL / SKIP / TIMEOUT per test. First run on the Windows 11
  test VM found the six defects below. Three tests that skipped on Windows now run there
  (`test_unit_sysio_streams`, `test_regression_scanner_short_read`,
  `test_regression_scanner_float_split`). x86-64 and i686 each pass 214 with 0 failures; 7
  fixtures whose subject is POSIX skip.

### Changed

- **Float parsing is faster than glibc at every length measured (B-043).** Three changes, each
  exact: a direct Eisel-Lemire layer (Lemire 2021) proves most results from a 64x128-bit product
  with a truncated power of five, where the staged layer validated every candidate with big-
  integer arithmetic; a significand past 19 digits is bounded by its first 19 digits and the same
  plus one, instead of going to the exact path; and one pass reads the digits, where a digit-by-
  digit builder kept pending-zero bookkeeping. `test_bench_float_host`, x86-64, against glibc
  `strtod`: `%.6g` 83 -> 81 ns (0.58x), ~16 digits 186 -> 145 ns (0.72x), `%.17g` 224 -> 152 ns
  (0.75x, was 1.13x), 25 digits ~640 -> 197 ns (0.86x, a new corpus). Results are unchanged:
  every row is checked against `strtod` in the same run, and 60 million generated inputs - random
  significands and exponents, `%.15g`-`%.40g` of random doubles, integers above 2^53, subnormal
  and overflow edges - matched `strtod` bit for bit, with and without 128-bit integers. The
  cached powers come from `scripts/generate_float_decimal_tables.py`, which checks its exponent
  estimate against exact integer arithmetic.
- **The Eisel-Lemire float-parsing fast path is used on 32-bit targets too (B-042).** It was
  compiled only where the compiler has `unsigned __int128`, although the code needs none - it
  goes through the portable 64x64 multiply. Without it a 32-bit target sent every decimal past
  the Clinger path to the exact big-integer path. Measured with 128-bit integers disabled on
  x86-64 (`test_bench_float_host`, release): 16-digit parsing 287 -> 183 ns, 17-digit 473 -> 222 ns;
  the results are checked against the host `strtod` in the same run, and the Windows i686 run
  passes the differential corpora and expects the fast path in `test_unit_float_parse_api`.
- **MSVC and clang-cl are not supported (owner decision; possibly later).** Their C23 support is
  incomplete. The manual used to say recent MSVC worked, which was never verified; it now names
  what is: GCC 13+, Clang 16+, and on Windows mingw-w64 GCC (x86-64 and i686).

### Fixed

- **Windows: `proven_fs_remove` deletes an empty directory,** as POSIX `remove()` does. It called
  `DeleteFileW` only, which refuses a directory, and reported PERMISSION.
- **Windows: `proven_fs_pread` and `proven_fs_pwrite` no longer move the file position.** A
  positioned `ReadFile`/`WriteFile` on a synchronous handle leaves the pointer after the bytes;
  the position is now restored.
- **Windows: positioned calls on a pipe are UNSUPPORTED.** `proven_fs_seek`, `pread` and `pwrite`
  used `SetFilePointerEx` / an OVERLAPPED offset on a pipe, which Windows does not support, and
  `proven_sysio_scan_chunk` therefore accepted a pipe it must refuse. The handle type is now
  checked first (`FILE_TYPE_DISK`), matching POSIX `ESPIPE`.
- **Windows: the end of a pipe is EOF.** `ReadFile` reports a closed writer as `ERROR_BROKEN_PIPE`,
  which came back as `PROVEN_ERR_IO` - so `producer | program` ended in an error. Both read paths
  now map it to `PROVEN_ERR_EOF`, as `read(2)` returning 0 does.
- **Tests and examples that assumed POSIX or a 64-bit target.** Permission checks compare with the
  mode read back after `chmod` (Windows keeps only the owner-write bit); the durable-write example
  accepts `sync_dir`'s documented UNSUPPORTED on Windows; `test_unit_u128_mul` and
  `test_unit_float_bigint_divmod` have references without `__int128`; a float comparison
  uses a stored `double`, not a literal that x87 evaluates in long double; a fixture no longer
  needs a `build/` directory.
- **`scripts/release.sh` builds a release's PDFs with one date.** The site is built at the release
  commit and again by `--publish` at the site commit; both now take the date of the commit that
  last set the version, so the uploaded PDFs equal the committed ones without a re-publish.

## [0.3.0] - 2026-09-29

A MINOR release: nothing public removed, one build profile added, and one profile's behaviour
changed on purpose. `./nob release` now defines `NDEBUG`, so pool and map misuse checks are out
of release builds (pool teardown was quadratic with them), and the new `./nob hardened` keeps them
in an optimised build (B-036). `find_last` makes the forward search's choices, and its quadratic
tail is gone (B-024). The freestanding runtime contract is stated and link-proven (B-034). `./nob
cross` reports every target and cannot skip the mandatory ones (B-035). Benchmarks share one row
format and back the published numbers (B-037, B-038). The build rebuilds exactly what a header
change touched, and `./nob clean` removes the selected build root safely (B-036). English public
text is ASCII, and a gate keeps it that way.

### Added

- **One benchmark row format, and the benchmarks behind the published numbers (B-037, B-038).**
  `tests/proven_bench.h`: warmup, five samples on a monotonic clock, median, spread, raw samples,
  checksum, compiler and profile on one line; every registered benchmark uses it. New
  `tests/test_bench_float_host.c` (the float-vs-glibc comparison, now checked in, with accuracy
  asserted in the same run) and `tests/test_bench_job.c` (idle CPU for 1-32 workers, idle / burst
  / saturated wake latency, throughput). Raw results and a claim-to-row map in
  the maintainers' benchmark records, with the job system's latency budget.

### Changed

- **The build rebuilds exactly what a change touched (B-036).** Objects and test executables are
  built with compiler dependency files (`-MMD`); the cache key is the exact compile or link command
  plus the contents of every file the dependency file names. Editing `include/proven/job.h` now
  recompiles `job.c` alone (1 of 36 objects; the tests relink because they link every object), and
  editing `manual/examples/example.h` relinks the 92 examples that include it and nothing else -
  before, any header edit rebuilt everything. A missing dependency file means a rebuild, never a
  stale cache hit. The test cache hash is now taken from the exact link command (it used to be a
  hand-kept copy that differed from it), and `-ldl` is linked only into the test that interposes
  libc with `dlsym(RTLD_NEXT, ...)` instead of every POSIX test (B-035). The first build after
  updating rebuilds everything once.
- **`./nob clean` removes the selected build root, safely (B-036).** It honours `-build-root` and
  `PROVEN_BUILD_ROOT` instead of always running `rm -rf build` / `rmdir /s /q build` through the
  shell, deletes without following symlinks or junctions, refuses `.`, `/`, any `..` component
  and unusual characters, and removes a root other than the default `build` only when it carries
  the `.proven-build-root` marker nob now writes into every build root it creates. Compatibility
  note: a custom build root created by an older `nob` has no marker, so `clean` refuses it once
  with a hint; remove it by hand or build into it again first. As for building, a root with a
  drive letter or backslashes (`C:\...`) is refused; on Windows use a relative root. The Windows
  deletion path is compiled with mingw-w64 (x86-64, i686) but not yet run on Windows.
- **The job system's wake-latency budget states its condition (B-041).** The Windows idle-wake p99
  of ~8.9 ms was traced with `b041-wake-probe.c`: a bare OS semaphore shows the same tail
  whenever CPUs are scarce (the 2-vCPU VM; Linux pinned to 2 CPUs), and neither shows it on a
  quiet 16-CPU host. The job system adds ~1 us at the median. No library change; the budget in
  the job budget in the maintainers' benchmark records now says it assumes free CPUs, and `test_bench_job` prints the
  host's logical CPU count.
- **`./nob release` defines `NDEBUG`; new `./nob hardened` keeps the checks (B-036).**
  Compatibility note: a release build no longer traps a pool double free or a foreign pointer, or
  the map's key-overlap misuse - those checks are for debug and hardened builds. They made pool
  teardown quadratic: 20,000 frees took 59.6 ms with the check, 0.05 ms without
  (`b036-pool-teardown-benchmark.c`). Build with `hardened` (`-O2 -DNDEBUG
  -DPROVEN_HARDENED=1`) to keep them in an optimised build, and use `alloc_check.h` in tests.
  Every build now logs its safety profile.
- **Published float speed claims follow the checked-in benchmark.** Re-measured: parsing is
  faster than glibc on short numbers, level at ~16 digits, ~1.1x slower at 17; shortest formatting
  ~3.6x faster than `%.17g`; `%f`/`%e` faster at every magnitude measured - the June claim that
  they were 3-5x slower at extreme magnitudes did not reproduce. README (both), the float doc and
  `primitives-benchmark.md` updated.
- **English public text is ASCII, and stays so (B-036).** README.md, TEST.md, CHANGELOG.md, the
  English manual and examples, and all C sources and build files were normalised (em dashes,
  section signs, arrows and the like; 48 files), with the Markdown anchors of changed headings
  updated. `scripts/ascii_policy.py check`, run by `project-check`, fails on any new non-ASCII byte
  in that scope; Hangul is exempt, and the Korean mirrors are out of scope.
- **`./nob cross` reports every target and cannot skip the ones a release needs (B-035).** Each
  target ends PASS, FAIL or SKIP with a reason, a failure no longer stops the other targets, and a
  summary is printed. Skipping `native-gcc-hosted`, `native-clang-hosted`, `windows-x86_64-winapi`
  or `windows-i686-winapi` fails the run; before, a run that skipped nine of eleven targets
  finished green.
- **The freestanding runtime contract is explicit and linked (B-034).** A freestanding build needs
  `memcpy`, `memmove`, `memset`, `memcmp` and the compiler support library, nothing else - stated
  in the freestanding guide and proven by a new `./nob cross` stage that links every freestanding
  object with a program supplying only those four (`-nostdlib -nostartfiles -static -lgcc`) for
  Cortex-M4 and RISC-V. `float_format.c` no longer calls `strlen`, which was the one dependency
  outside that set.
- **`proven_u8str_view_find_last` makes the forward search's choices (B-024).** The same entropy
  sample; an anchored backward scan over a new portable `proven_sys_mem_rchr` on ordinary input;
  backward Shift-Or (<= 64 bytes) or a new reverse Two-Way (> 64) on low-entropy input. Measured
  with `b024-find-last-benchmark.c`: about 5x faster on ordinary text for 2-64 byte needles
  (0.30 -> 0.055 ns/byte), and the long-needle quadratic tail is gone (a 256-byte needle on a dense
  run: 1,994 -> 0.002 ns/byte). Long needles on ordinary text are slower (0.022 -> 0.055), the price
  of a portable backward scan. Results unchanged: the oracle agrees on 120,000 cases.


### Fixed

- **`tests/test_regression_fs_perms_and_types` builds with clang.** It passed `_Atomic int`
  objects to the GCC `__atomic_*_n` builtins, which clang rejects; it now uses `<stdatomic.h>`
  `atomic_load_explicit` / `atomic_store_explicit`. The full hosted suite passes under clang.
## [0.2.0] - 2026-09-28

A MINOR release: new public API, nothing removed. UTF-16 text gets a way in and out and UTF-8
shows correctly on a Windows console (B-039); the view vocabulary - split, trim, affixes,
find_last, contains, ordering (RFC-0005, B-018 to B-022); an allocator wrapper that catches the
wrong allocator at the call (B-040); Windows symlinks and the 4 GiB entropy boundary measured and
fixed (B-033); reproducible manual PDFs. Existing behaviour changes only where it was wrong: on a
Windows console, for Windows symlinks, and in `proven_time_u16_fmt` with non-ASCII locales.

### Added

- **`utf.h`: strict UTF-8 <-> UTF-16 transcoding.** Measuring (`proven_utf8_to_utf16_size`,
  `proven_utf16_to_utf8_size`), fixed-capacity all-or-nothing (`proven_utf8_to_utf16`,
  `proven_utf16_to_utf8`), partial for text read in pieces (`proven_utf8_to_utf16_partial`,
  `proven_utf16_to_utf8_partial`, reporting `proven_utf_step_t`), and growable all-or-nothing
  (`proven_utf8_append_to_u16str`, `proven_utf16_append_to_u8str`). Malformed input is always
  `PROVEN_ERR_INVALID_ENCODING` - overlongs, encoded surrogates, values above U+10FFFF, stray
  continuations, unpaired surrogates - and nothing is repaired. Input cut mid-character is
  `PROVEN_ERR_NEED_MORE` in the partial forms, malformed in the whole forms.
- **u16 text through the formatter.** `proven_arg_u16`, and `PROVEN_ARG` on a
  `proven_u16str_view_t`, render UTF-8 into every formatter sink: `proven_println`,
  `proven_eprintln`, `proven_fprintln` into any writer, `proven_u8str_append_fmt*`. Width counts
  UTF-8 bytes as for a u8 view; an unpaired surrogate fails the format.
- **u16 text through writers and readers** (`stream.h`). `proven_writer_write_u16` writes UTF-8,
  UTF-16LE or UTF-16BE (`proven_text_encoding_t`), validated before anything is written;
  `proven_writer_write_bom` writes a byte order mark only on request. `proven_u16_reader_t`
  (`proven_u16_reader_init`, `_read_line`, `_read`) decodes any of the three encodings from any
  reader into a caller-owned buffer of code units, with `PROVEN_TEXT_AUTO` choosing by BOM,
  carrying a character split across reads, and keeping the byte line reader's newline, full-buffer
  and last-line rules.
- **sysio u16 line input**: `proven_sysio_u16_lines_open`, `proven_sysio_stdin_u16_lines`,
  `proven_sysio_read_u16_line`, with `proven_sysio_u16_lines_t`. `proven_result_u16str_view_t` in
  `u16str.h`.
- **PAL**: `proven_sys_io_is_console`, `proven_sys_io_console_write_u16`,
  `proven_sys_io_console_read_u16` (Windows; POSIX answers "not a console" / unsupported).
- Manual: chapter 3 "Converting between UTF-8 and UTF-16", chapter 5 "UTF-16 text in and out,
  and the Windows console", both editions, with runnable examples `ex_03_utf` and `ex_05_u16_io`.
- `b039-console-check.c` and `build-b039-check.sh`: a native Windows check that
  makes its own console in code page 949 and verifies output by reading the screen buffer back
  and input by injecting key events.

- **`alloc_check.h`: an allocator that knows its own blocks (B-040).** `proven_alloc_check_wrap`
  puts a checker in front of any allocator and records the blocks it hands out in caller-supplied
  memory; a foreign free, a double free, a realloc of a foreign block or with the wrong old size or
  alignment, and an allocation past the record are refused with `proven_panic` at the call, and the
  refused pointer never reaches the inner allocator. `proven_alloc_checked` wraps only where
  `PROVEN_ALLOC_CHECK` is defined (before the first proven header, e.g. `-D`) and is otherwise the
  identity - it is a testing and debugging tool, and the lookup is linear. `proven_alloc_check_owns`,
  `proven_alloc_check_live` (a leak check). Tests `test_unit_alloc_check`, `test_unit_alloc_check_on`;
  manual chapter 2 section 7 with `ex_02_alloc_check`, both editions.
- **The view vocabulary (RFC-0005; B-018 to B-022).** In `u8str.h`, all pure and non-allocating,
  ill-formed views treated as empty, every empty result `{NULL, 0}`:
  `proven_u8str_view_split` / `_split_next` with `proven_u8str_view_split_t` (n separators yield
  n + 1 fields; an empty separator yields the input once; the iterator is copyable);
  `proven_u8str_view_trim`, `_trim_start`, `_trim_end` (exactly six ASCII whitespace bytes);
  `proven_u8str_view_remove_prefix` / `_remove_suffix` (unchanged when absent);
  `proven_u8str_view_find_last` (last start position, overlaps counted; size for an empty needle;
  byte scan / backward Shift-Or / repeated forward search by needle length) and
  `proven_u8str_view_contains`; `proven_u8str_view_cmp` / `_cmp_ptr` (bytewise unsigned, prefix
  first, sign only); `proven_u8str_view_is_well_formed`. Tests `test_unit_u8str_view_cmp`,
  `test_unit_u8str_view_ops`, `test_unit_u8str_split`, `test_regression_split_empty_sep`,
  `test_differential_find_last_oracle` (60,000 cases; planted defects caught). Manual chapter 3
  section 1 in both editions with `ex_03_view_ops`. The RFC-0004 benchmark now measures the
  shipped iterator: 18.1 ns/field against 16.5 for a correct hand-rolled loop (median of three).

### Changed

- **On a Windows console, sysio writes and reads UTF-16.** `proven_print`/`proven_eprint`, the
  stdout/stderr writers, the stdin reader, `proven_sysio_*_buffered`, the u8 and u16 line
  readers and `proven_sysio_scanner_t` detect a console once (`GetConsoleMode`) and use
  `WriteConsoleW`/`ReadConsoleW`, converting at the edge. Before, UTF-8 was handed to the
  console with `WriteFile` and shown in the console's code page - mojibake under 949 unless
  `chcp 65001` had been run - and console input came back in that code page. The console's code
  page is not changed. Malformed UTF-8 sent to a console is refused after the valid part; a
  character split across buffered flushes is carried in the state struct; Ctrl+Z at the start of
  a console line is end of input. Files, pipes and redirected streams stay byte-exact; POSIX is
  unchanged. `proven_writer_from_file` on a console handle stays byte-exact, as documented.
- **The allocator pairing of owned strings is now stated as a warning** (B-023, owner decision
  2026-09-28): `u8str.h`/`u16str.h` and manual chapter 3 say that the string does not remember its
  allocator and nothing checks it, with a counter-example. No field was added; an allocator-side
  ownership check is proposed as B-040.
- `proven_sysio_std_t` gains `console` and `carry` (`proven_sysio_carry_t`);
  `proven_sysio_scanner_t` gains the same two fields. Layout change for code that declares them.

- **The manual PDFs are reproducible.** `scripts/build-site.sh` sets `SOURCE_DATE_EPOCH` from the
  commit being built (a caller's own value wins), so the same commit gives byte-identical PDFs -
  two full builds matched by SHA-256 for both editions; before, they matched only in size.
  `scripts/release.sh` now compares an existing release asset with the built one by SHA-256
  (GitHub's asset digest, or the downloaded asset), not by size.

### Fixed

- **Code review of the unreleased work (2026-09-28), ten findings:**
  - A buffered writer's automatic drain flushed the inner writer, and on a Windows console that
    flush ended the text: valid UTF-8 split at a buffer boundary came back INVALID_ENCODING with
    bytes lost. Drains no longer flush the inner writer, and the console writer's flush keeps an
    open character for the next write. Reproduced on the Win11 VM before (FAIL) and fixed after
    (win64/win32 18/18).
  - A non-NULL empty view passed through `remove_prefix`, `remove_suffix` and `split` as `{p, 0}`;
    every empty result is now `{NULL, 0}` as documented.
  - The Windows symlink kind is decided from a path normalised before any `\\?\` prefix.
    Defensive: the long-path case the review predicted did not fail on Windows 11 before the fix.
  - `proven_u16_reader_t` stages 1 KiB with a cursor: about one source read per KiB instead of
    one per 64 bytes.
  - `utf.h`'s append functions grow once and convert in place (no chunk copy, no rollback).
  - One overlap rule (`proven_range_overlaps`) for u16 input in `utf.h` and the formatter; the
    formatter used to accept a view running into its output from below.
  - One padding rule (`spec_padding`) for plain, custom and UTF-16 fields.
  - `build-b033-check.sh` prints its report and cleans up when the check fails; `release.sh`
    never deletes an asset because a digest download failed.
  - New comments are ASCII.
- **Windows symlinks (B-033).** `proven_fs_symlink` created a link to a directory as a file
  link, which cannot be listed or entered, and made every relative target absolute against the
  current directory (`sub/rel -> t` pointed at `./t`), because the target went through the
  helper that calls `GetFullPathNameW`. The target is now stored as written (with `/` as `\`),
  the directory flag follows the target as the link resolves it, older Windows without
  `ALLOW_UNPRIVILEGED_CREATE` is retried, and failures are `PROVEN_ERR_PERMISSION` or
  `PROVEN_ERR_NOT_FOUND` where they can be told apart (POSIX too), not always `PROVEN_ERR_IO`.
  Measured on the Win11 VM before (7 of 12 failed) and after (win64 12/12, win32 11/11) with
  `b033-windows-check.c`, which also filled a 4 GiB + 4 KiB entropy request across the
  32-bit count boundary.
- **`proven_time_u16_fmt` widened each UTF-8 byte into a code unit.** A caller-supplied locale
  with non-ASCII names produced three meaningless units per Hangul syllable; it now transcodes.
  Reproduced red first in `tests/test_unit_time_fmt_u16_parity`.

### Verification

- New tests: `test_unit_utf` (every scalar value; UTF-8 validity against an independent
  formulation over every 1-3 byte input; planted defects caught), `test_unit_stream_u16`,
  `test_unit_sysio_console` (a fake console at every split offset and read size). Debug build:
  209 executables. Windows 11 VM, 2026-09-27: `b039-check-win64.exe` and `-win32.exe` 17/17
  each - console output under code page 949, injected console input, pipes and files. `./nob
  cross` on arch-dev: all 11 targets. Not measured: console input typed through an IME by a
  person; a legacy (pre-Windows 10) console host.

## [0.1.1] - 2026-09-21

A PATCH release: the tutorial gains three lessons, and the hand build the manual prints works
on GCC 14 with glibc. No public API changes.

### Added

- **Tutorial lessons 7-9** (`manual/manual-t-tutorial.md` and the Korean edition): memory freed
  all at once (`proven_arena_reset` in a per-round loop, and `PROVEN_ERR_NOMEM` when the arena
  is too small), a container that keeps its allocator (`PROVEN_ARRAY_*` growing past its
  starting capacity), and failures from outside the program (a whole-file write and read, then
  `PROVEN_ERR_NOT_FOUND` for the removed file). Lesson 5 already pointed at lesson 7 for the
  arena reset; it now exists. Each lesson is a runnable program in both example trees
  (`tut_07_arena`, `tut_08_containers`, `tut_09_files`), so the full run is 84 manual examples
  and 202 executables.

### Fixed

- **The hand build the manual prints now compiles on GCC 14 with glibc.** The tutorial and
  Chapter 0 give `cc -std=c23 -Iinclude your_program.c src/proven/*.c platform/*.c` with no
  `-D` flags. Under a strict `-std`, glibc hides the POSIX declarations the PAL uses
  (`pread`, `pwrite`, `ftruncate`, `clock_gettime`, `nanosleep`, `O_CLOEXEC`), and GCC 14
  treats an implicit declaration as an error, so `proven_sys_io.c`, `proven_sys_random.c`
  and `proven_sys_time.c` failed. Nothing here noticed because `nob.c` passes
  `-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L` itself. Every PAL source now requests them
  before its first include, as `proven_sys_fs.c` already did; `-D` flags given on the
  command line still win. `tests/test_portability_source_contracts` checks all eight.

## [0.1.0] - 2026-09-11

The RFC-0008 release: six security and boundary defects fixed, a rule for protected
destinations, errors a caller can act on, and native Windows verified on 64 and 32 bit.
MINOR, not PATCH, because behaviour changes: an atomic write, copy or rename over a
read-only destination is now refused, and several failures that were `PROVEN_ERR_IO` now
name themselves. `v0.0.1` was set on 2026-09-04 and tagged only now, at its own commit; it
was never published as a GitHub release.

### Changed

- **Windows: an atomic write replaces a file someone is reading, as on POSIX** (RFC-0008
  Decision 2, option (b), the owner's choice). The rename now tries the POSIX-semantics
  rename first (`SetFileInformationByHandle` with `FileRenameInfoEx`,
  REPLACE_IF_EXISTS | POSIX_SEMANTICS, Windows 10 1809+): a reader that allowed delete
  sharing - which `proven_fs_open` does - no longer blocks the write, and its open handle
  keeps the old bytes. Where Windows or the volume answers "unsupported" (older Windows,
  FAT/exFAT, many network shares) it falls back to `MoveFileExW`, and there the write is
  refused with `PROVEN_ERR_BUSY` as before. A reader that did NOT allow delete sharing
  blocks it on every Windows: BUSY. Verified on the Windows 11 VM, x86-64 and i686, plus a
  test build that forces the fallback: 41 checks each, none failed. On FAT32 and exFAT
  disks attached to the VM the POSIX rename answers `ERROR_INVALID_PARAMETER`, the
  fallback runs, and all three builds pass there too (nine runs in all). The same holds on
  a Windows SMB network drive (a share on the VM mapped back to it): INVALID_PARAMETER,
  fallback, 41 checks each for all three builds. Pre-1809 Windows and a Samba server were
  not available to run on.

### Fixed

- **Windows: a replacement blocked by a file in use now says BUSY, not PERMISSION.** Found by
  the first run on the Windows 11 test VM (2026-09-11): `MoveFileExW` answers
  `ERROR_ACCESS_DENIED` both for a read-only destination and for one another process holds
  open - with any sharing mode, delete sharing included - and never a sharing violation. The
  platform layer mapped that straight to "denied", so an atomic write over a file someone was
  merely reading told the caller the file was protected. The Windows rename now asks the file
  after a refusal (read-only attribute, then an open for DELETE) and answers
  `PROVEN_ERR_BUSY` for the in-use case. The branch that expected a sharing violation from
  `MoveFileExW` never fired; it is kept but documented as such.
- **Windows: a failed path conversion in rename no longer reports success.** The allocation
  failure path returned `false`, which is 0, which is `PROVEN_SYS_FS_RENAME_OK`.
- Verified on the VM, **x86-64 and i686**, gcc 16.2 mingw static builds: 38 checks each, none
  failed. 32-bit Windows is now confirmed rather than explained.
- **The job-system deadlock fix now has a Windows run.** `tests/test_regression_job_permit_starvation`
  was proven on the POSIX semaphore path only; built statically for x86-64 and i686 and run on
  the Windows 11 test VM (the `CreateSemaphoreW` path), it passes on both.

### Security

- **A staging file is now created private, not narrowed afterwards** (RFC-0008 H-002).
  Replacing a 0600 file with `proven_fs_write_file_atomic` or `proven_fs_write_file_durable`
  wrote the new contents into a `.pvtmpNN` sibling that was *created* with `0666 & ~umask`
  and narrowed a moment later. A `chmod` does not reach a descriptor another local user
  opened during that moment: that descriptor stays open, stays readable, and then reads the
  private payload. The staging file is now created owner-only by the creating call itself,
  and the target's mode is applied through the open handle rather than by re-resolving the
  staging name. A new destination of `proven_fs_copy` is created the same way. The process
  umask is not touched - it is shared mutable state.
- **A failed metadata lookup no longer reads as "no such file"** (RFC-0008 H-002).
  `internal_write_file_atomic` treated any `stat` failure as a missing target and carried
  on with default permissions. It now stops before creating anything unless the target is
  genuinely absent. The platform layer gained `proven_sys_fs_stat_checked`, which
  distinguishes the two; the public `proven_fs_stat` is unchanged and still answers
  `PROVEN_ERR_IO` for both.

- **Encoding sizes are computed with checked arithmetic** (RFC-0008 H-001).
  `proven_hex_encoded_size`, `proven_base64_encoded_size` and `proven_base64_decoded_size`
  multiplied and added in `proven_size_t` and wrapped at the top of the range - all three
  answered 0 for the inputs where they should have said "that does not fit", and 0 passes
  every capacity check there is. The encoders repeated the same arithmetic internally, so
  fixing the helpers alone would not have protected them: a wrapped `need` passed
  `need > out_cap` and the loop then wrote past the caller's buffer.

- **A failed atomic write no longer leaves its staging file behind on Windows** (RFC-0008
  H-005 follow-up, found by the first native run on 2026-09-10). The staging file carries
  the target's mode; when that target is read-only, the mode is the READONLY attribute, and
  Windows will not delete a read-only file - so the cleanup after a refused replacement
  failed silently and the debris stayed. Owner-write is now held back until the payload is
  written (it is not a read permission, so nothing about confidentiality changes), the exact
  target mode goes on before the rename that publishes the file, and the cleanup path
  restores write permission before removing. Found by running the code, not by reading it, and
  confirmed by a second native run on the same machine: 31 checks, none failed.

- **Windows: an atomic write can replace a file that already exists** (RFC-0008 H-005,
  first recorded as RFC-0007 C-001). `proven_sys_fs_rename` used `MoveFileW`, which fails
  outright when the destination exists - and both whole-file atomic writes rename a staging
  file over their target, so on Windows the first write to a name succeeded and every write
  after it failed. Now `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, without
  `MOVEFILE_COPY_ALLOWED` (a cross-volume copy-and-delete is not atomic) and without
  deleting the destination first (that opens an interval in which the name does not exist).
  Implemented and cross-compiled for both Windows targets; **not run natively**, so the
  behaviour against a read-only destination, ACLs, sharing modes and symlinks has no result.
- **Windows: every requested entropy byte is actually requested** (RFC-0008 H-006, first
  recorded as RFC-0007 V-003). `proven_sys_random_bytes` cast its `size_t` length once to
  the `ULONG` that `BCryptGenRandom` takes. On 64-bit Windows a length above `ULONG_MAX`
  narrowed silently - a request for exactly 2^32 bytes asked the OS for **zero** - and the
  success of that short request was returned as success for the whole buffer, so a caller
  read bytes nothing had written as fresh entropy. The request is now made in chunks the
  backend accepts, the pointer advances only after the OS reports success, and a failed
  chunk fails the whole call; there is no fallback to a PRNG. Failure may leave a filled
  prefix, which the boolean API cannot report, so a caller must discard the whole buffer.
  Implemented and cross-compiled; **not run natively**.

- **A protected destination is refused, by every whole-file replacement** (RFC-0008
  follow-up; the owner's decision, 2026-09-10). `proven_fs_write_file`,
  `proven_fs_write_file_atomic`, `proven_fs_write_file_durable` and `proven_fs_copy` now
  return `PROVEN_ERR_PERMISSION` when the destination's owner-write bit is clear, and leave
  the file exactly as it was. That bit is where both platforms record "do not write this
  file": mode `0200` on POSIX, the READONLY attribute on Windows.

  It had been three answers to one question on ONE platform, measured: `write_file` refused
  (it opens the destination for writing), `write_file_atomic` succeeded (`rename` asks the
  DIRECTORY for permission, so the file's mode was never consulted), and `copy` succeeded
  **and left a 0444 file as 0664** - a protection the caller had set, gone, with nothing
  saying so. The Windows/POSIX divergence RFC-0008 recorded was the fourth face of the same
  unresolved question, not a portability wart.

  **This is a behaviour change.** Code that replaced a read-only file through
  `write_file_atomic`, `write_file_durable` or `copy` now gets `PROVEN_ERR_PERMISSION`; the
  caller lifts the mark first, which is one line and visible. In particular the backup loop
  in `tests/test_regression_fs_perms_and_types` - copying a read-only source onto the same
  destination twice - now fails on the second run. That behaviour was deliberately added
  once, and is deliberately reversed here: the failure it produced then was
  `PROVEN_ERR_IO`, which a caller cannot act on, and the cure was stripping the
  destination's protection without saying so. It is not a security boundary: the mode is
  read before the work and acted on after it, and anyone who can `chmod` can lift the mark.

### Changed

- **`proven_fs_rename` obeys the protected-destination rule, and `proven_fs_remove` does
  not.** An audit of every public door against a `0444` file found two that did not follow
  the rule the rest do. `proven_fs_rename` replaced the protected file outright - contents
  and mode both - and it is what the atomic write is built on, so a caller refused by
  `proven_fs_write_file_atomic` got the result from `proven_fs_rename` instead. It refuses
  now. `proven_fs_remove` still deletes: a name is removed from a directory, and POSIX has
  never let the file's own mode have a say in that; refusing there would break ordinary
  cleanup of read-only files for a rule about writing. Windows does refuse it, and that
  difference is now reported as `PROVEN_ERR_PERMISSION` instead of hidden behind an I/O
  error. `tests/test_contract_protected_destination` checks every door one by one, so a
  door added to the public API without one is a build failure rather than a discovery.

- **`proven_fs_open`, `proven_fs_rename` and `proven_fs_remove` say WHICH failure it was.**
  `PROVEN_ERR_NOT_FOUND` when the name is not there, `PROVEN_ERR_PERMISSION` when the caller
  may not, `PROVEN_ERR_BUSY` when something else holds it; everything else stays
  `PROVEN_ERR_IO`. They used to answer `PROVEN_ERR_IO` for all of it, and one code for
  "ask the user", "retry" and "give up" is a code a caller cannot act on. The platform layer
  gained `proven_sys_fs_open_checked` and `proven_sys_fs_rename_checked` to carry the
  distinction, and `proven_sys_fs_remove_checked` with them; the boolean wrappers remain.

### Fixed

- **A durable write syncs the directory the file is actually in** (RFC-0008 H-003).
  `internal_parent_dir` treated `/` and `\` as separators on every platform. On POSIX a
  backslash is an ordinary character in a filename, so a durable write to `d/a\b` - one
  file called `a\b` inside `d` - tried to sync `d/a`. When that name does not exist the
  call returned an I/O error *after* the rename had already published the new contents;
  when it happened to be a directory, the wrong directory was synced and the call reported
  a durability it had not achieved. The separator rule is now the platform's own, and the
  same rule measures the basename the staging name is derived from - a long name full of
  backslashes used to be measured as a short one, so the staging name was never trimmed to
  fit. `proven_fs_is_absolute` is unchanged: it classifies a path that may have come from
  elsewhere rather than resolving one here, which is a different question.

- **The job queue no longer compares positions with a signed subtraction** (RFC-0008 H-004).
  `proven_job_submit` and `proven_job_execute_one` computed
  `(proven_ptrdiff_t)seq - (proven_ptrdiff_t)pos`. The queue's counters run forward for
  ever and wrap; only their distance is small. At the sign boundary those two casts can
  produce `PTRDIFF_MAX` and `PTRDIFF_MIN`, and subtracting them is signed overflow -
  undefined behaviour, reached by a legitimately full queue with no concurrency involved.
  The distance is now taken in the unsigned counter type, where wrapping is defined and is
  the modular arithmetic the algorithm wants, through one shared helper used by both sides.
  Memory ordering, admission, wake permits and drain behaviour are untouched.

### Changed

- **A job queue capacity at or past half the counter range is refused** (RFC-0008 H-004).
  `proven_job_system_init` answers `PROVEN_ERR_INVALID_ARG` before it allocates anything.
  Past that limit "ahead" and "behind" stop being distinguishable, so it is a correctness
  condition rather than a resource one. No reachable capacity is affected.
- **The encoded-size helpers answer `PROVEN_SIZE_MAX` for a size that cannot be
  represented** (RFC-0008 H-001). They return a size and have nowhere to put an error. A
  valid hex output is always even and a valid padded Base64 output is always a multiple of
  four, so neither can be `PROVEN_SIZE_MAX` by accident; zero was not usable as the
  sentinel because zero is the honest answer for empty input. `proven_base64_decoded_size`
  needs no sentinel - its largest value fits - and now reaches it without wrapping on the
  way. The encoders make the same judgement independently and answer the new
  `PROVEN_ERR_OVERFLOW`, which is a different answer from `PROVEN_ERR_OUT_OF_BOUNDS`: the
  size does not exist, rather than the buffer being smaller than it. Nothing is read or
  written on either refusal.
- Unchanged on purpose: a brand-new atomic target still gets `0666 & ~umask`. Restrictive
  creation carries an existing target's mode across; it is not a new default-permissions
  policy, which would be an owner decision.
- `tests/test_docs_version_sync` skips an `## [Unreleased]` section when it looks for the
  newest released entry. The gate and the maintainers' operations notes had been asking for
  opposite things.

## [0.0.1] - 2026-09-04

### Changed

- **Semantic versioning, from v0.0.1.** Releases were numbered by date
  (`v26.09.04a`); they are now `MAJOR.MINOR.PATCH`, and the numbering restarts at
  v0.0.1 with the library as it is today. `include/proven/version.h` gains
  `PROVEN_VERSION_MAJOR`, `PROVEN_VERSION_MINOR` and `PROVEN_VERSION_PATCH`, and
  `PROVEN_VERSION_ENCODE(major, minor, patch)` for `#if` comparisons;
  `PROVEN_VERSION_NUM` is now that encoding of the three numbers, and
  `PROVEN_VERSION_SUFFIX` is gone. The `xcv_` alias layer follows.
- **Breaking for one kind of caller:** a comparison such as
  `#if PROVEN_VERSION_NUM >= 260720` is wrong on every release from here on,
  because the number restarted. Rewrite it as
  `#if PROVEN_VERSION_NUM >= PROVEN_VERSION_ENCODE(0, 0, 1)`.
- The version gate (`test_docs_version_sync`) now also checks that the string
  agrees with the three numbers, and reads the changelog's `[x.y.z]` heading;
  `scripts/release.sh` takes the release notes from that heading. The rules in
  `CHECKLIST.md`, `AGENTS.md`, `CONTEXT.md`, `DOCUMENTING.md` and `TEST.md`
  say the same.

## [2026-09-04] - proven_c_lib-v26.09.04a

### Changed

- **The manual's front page is the contents and the copyright, nothing else.**
  Each edition's index used to be the spine - intent, build model, global
  contracts, the ownership matrix, behaviour classes, header map, platform
  support - with the table of contents underneath. Now, like the book, the
  index is the full contents (every chapter and every section) followed by the
  copyright, and the up-arrow button in every chapter lands there. The spine moved
  into Chapter 0 as section 6-section 15, after the plain-language sections that always
  referred to it as "the formal versions"; appendices B, C and D sit side by
  side. Chapter 0's glossary, libc map and closing section renumber to 13, 15
  and 16, and every link to the old anchors follows. Chapter 0 joins the
  code-block gate in `nob.c` so the five blocks that moved stay checked.
- **`docs/index.html`, the landing page above both editions, carries the full
  contents of both** - every chapter and every section, linked into the
  edition - instead of two bare language links. `scripts/site_root.py` writes
  it from the contents each edition's build leaves behind; the link checker
  follows its links and the web-font subset includes its characters.

## [2026-09-03] - proven_c_lib-v26.09.03a

### Added

- **A tutorial track for readers who have just finished one C book.**
  `manual/manual-t-tutorial.md` and `manual-ko/manual-t-tutorial-ko.md` teach the
  library in six short programs, each introducing exactly one idea - printing,
  views, errors, results, allocators - and ending with the Chapter 0 greeting
  program read line by line. Chapter 0 shows that program on its first page and
  it carries five new ideas at once; the tutorial hands them over one at a time.
  All six are real programs the build compiles and runs.
- **`scripts/check-example-parity.py`** - the two example trees may differ in
  their comments and in nothing else. It strips comments and string bodies and
  compares what is left, and `scripts/project-check.sh` runs it.

### Changed

- **The manual's examples are now two trees, one per language.**
  `manual/examples/en/` is quoted by the English chapters and `manual/examples/ko/`
  by the Korean ones, so a reader of the Korean manual is not made to read English
  comments to follow the code the Korean prose is explaining. **All 39 programs are
  translated**: roughly 1,400 lines of comments, with the code identical in both
  trees - `scripts/check-example-parity.py` proves that mechanically, and
  `./nob build` compiles and runs both trees (78 example executables where there
  were 33).
- **The web edition carries its table of contents in one place.** The per-chapter
  left sidebar is gone; the index page now lists every chapter *and* every section
  within it. Jumping about inside a chapter is what the panel at the top is for.
- `tests/test_docs_manual_examples.c` reads the chapter list from the manual
  directories instead of a hand-written array. Six examples had become invisible to
  it because a chapter added later was never added to that array.
- The same gate's quoted-example cap was 64 and silently dropped everything past it;
  it is now 512 and overflowing fails the test. A cap that truncates in silence turns
  a gate into decoration.

### Fixed

- `.gitattributes` marks `*.pdf` and `*.zip` binary. Regenerating the site made
  `git diff --check` - and therefore `scripts/project-check.sh` - fail on the
  published PDFs.

## [2026-09-02] - proven_c_lib-v26.09.03a

### Fixed

- **The job system deadlocked under load, and `proven_job_system_destroy`
  never returned.** A permit on the workers' semaphore meant "take exactly one
  job", which is sound only if a woken worker can always find the job its permit
  announced - and it cannot. The queue hands out its slots in order, so a
  producer that has claimed slot *n* and not yet published it hides slot *n+1*
  from every consumer. The worker woken for *n+1* reads an empty queue, spends
  the permit, and parks; slot *n* is published a moment later with no permit
  left to announce it.

  Lose enough of those and the queue stops draining. Because it never empties,
  no worker reaches its exit test either, so `close` and `destroy` wait on
  threads that will never finish. Measured under parallel load, the stress
  harness hung in **18 runs out of 40**.

  A permit now means "there may be work", and a woken worker **drains** the
  queue instead of taking one job from it, so a spent permit cannot strand the
  jobs behind it. A departing worker also posts one permit before it leaves, so
  shutdown needs one permit to reach every worker rather than exactly one each.
  The drain is what fixes the deadlock: with the baton alone the harness still
  hung in 18 runs out of 80; with the drain, 400 runs out of 400 passed.

### Added

- **`tests/test_regression_job_permit_starvation`** - six rounds of 24
  producers against a four-slot queue, each closing and destroying the system.
  A watchdog turns a hang into a reported failure naming the round, because a
  deadlock has no wrong answer to assert on: the process simply stops. Verified
  to fail against the pre-fix source, five deadlocks in five runs, where the
  existing stress harness needed heavy background load to hang at all.

## [2026-09-02] - proven_c_lib-v26.09.02a

### Added

- **Every public function is now shown in working code.** 86 of the 279 public
  functions had a reference-table row and no line of code anywhere that used
  them. Eleven new example programs cover them, in chapters 1 through 5 and 8:
  memory views with checked and unchecked slicing, the arena `_or_panic` calls
  and an allocator wrapper built on the arena traits, fixed-capacity string
  edits, UTF-16 assembly for a system call, container sizing with unsorted
  search and a streaming CRC-32, the crash-safe file-replacement recipe
  (`sync` -> `rename` -> `sync_dir`) with the record-level and link calls, readers
  and writers with the standard streams, memory-mapped durability, the
  randomness sources behind the `proven_rng_t` trait, wide-integer scanning with
  locale-free float parsing, and the argument constructors by name. All are
  compiled and executed by the build.

- **Gate G10 (`tests/test_docs_manual_usage`)**: every public function must
  appear in a runnable example or a compiled ```c block. A macro wrapper counts
  for the function it expands to; a `_Generic` dispatch table does not, and a
  ```text block does not, because it is not compiled.

- **Gate G11 (`tests/test_docs_manual_ko`)**: the Korean edition must mirror the
  English one - same chapters, same worked examples - and every English term a
  Korean chapter uses must be paired at least once with its Korean word. The
  translation had no gate at all and had fallen eleven examples behind.

- **The web and PDF editions** (`scripts/build-site.sh`): the Markdown manual is
  converted to Typst, then to one HTML page per chapter and one PDF per
  language, into `docs/en/` and `docs/ko/`. The Markdown stays the source of
  truth; the generated tree is output. `scripts/check-site-links.py` fails the
  build when a cross-reference or heading anchor does not resolve.

- **`scripts/sync-manual-examples.py`**: re-copies every quoted example body into
  the chapters that quote it, in both editions, so the verbatim requirement is
  met by running a script rather than by careful copying.

### Changed

- **The glossary now covers the whole vocabulary** (chapter 0, appendix B, both
  editions): 18 entries became 62, including every abbreviation the manual uses
  - API, EOF, BMP, CSPRNG, ASan/UBSan/TSan - and the terms the new examples
  introduce, such as durability, advisory lock, copy-on-write, open addressing,
  tombstone, rehash, back-pressure, locale, entropy and shortest round trip.

- **The Korean chapters pair each English term with its Korean word** the first
  time they use it - 할당자(allocator) - and say so in the glossary's opening.

## [2026-07-23] - proven_c_lib-v26.07.23d

### Changed

- **Idle job workers park instead of continuously polling.** The thread PAL now
  provides an opaque counting semaphore: a POSIX mutex/condition-variable
  implementation and a Windows kernel-semaphore implementation. A successful
  submission publishes its queue slot before posting one retained permit, so a
  wake cannot disappear when every worker is between an empty check and its
  wait. The bounded queue still uses atomic sequence counters, but the POSIX
  wake post may briefly acquire the semaphore mutex; documentation no longer
  describes the complete submit operation as nonblocking.

- **Job close has an explicit wake and admission protocol.** The first closer
  stops admission, waits for submitters that already entered to finish, and
  posts one final permit per worker. `close` may race with submitters; callers
  must still join every producer before `destroy` can free the system.
  Caller-driven `proven_job_execute_one` may leave a stale permit after stealing
  a queued job, which workers now treat as an empty recheck rather than work.

### Added

- Job unit coverage now starts work after workers are already parked, closes an
  entirely idle system, and deterministically exercises stale permits left by
  an external consumer. The stress test also closes while four producer threads
  are active and checks that every accepted job drains exactly once.

### Performance

- A five-run local process-CPU probe with eight workers idle for 250 ms measured
  a mean of 2000.081 ms for the former yield loop and 0.032 ms for parked
  workers. This establishes the idle-CPU mechanism on the measured POSIX host;
  the 1/2/8/32-worker matrix, wake-latency distribution, and native Windows
  runtime evidence remain tracked in RFC-0007 and B-038.

## [2026-07-23] - proven_c_lib-v26.07.23c

A whole-library correctness, portability, build, and performance audit. Confirmed defects that
could be fixed and verified locally are closed here; target-specific and measured follow-up work
is specified in RFC-0007 rather than being changed without its required platform or benchmark.

### Fixed

- **Concurrent decimal parsing no longer writes process-global path counters.** The internal
  Clinger/Eisel-Lemire/fallback metrics object was non-atomic mutable global state, so otherwise
  read-only parser calls raced in C and contended on one cache line. Production conversion now
  collects no counters. Tests use an explicit caller-owned observation sink, and a focused
  eight-thread TSAN regression covers the Clinger, staged, subnormal, and exact-fallback paths.

- **One-chunk sysio scanning no longer returns a view into its dead stack buffer.**
  `proven_sysio_scan_chunk_impl` used a local 4 KiB input array but accepted
  `proven_u8str_view_t` output, whose pointer escaped after return. It now refuses that argument
  with `PROVEN_ERR_UNSUPPORTED` before reading and leaves both the destination and cursor intact;
  the caller-owned buffered scanner remains the API for borrowed string results.

- **The test-catalog documentation gate compiles without POSIX `dirent.h`.** It now enumerates
  `tests/` through the library's portable filesystem iterator, so registering the gate no longer
  breaks the documented MSVC build at preprocessing.

- **Five more header changes can no longer reuse stale objects or executables.** The build
  dependency manifest now includes the float tables, the internal memory-range helper, both test
  framework headers, and the manual-example helper. The build and gate consume one preprocessed
  manifest with a compile-time inclusion sentinel. The gate checks both directions: every header
  in the declared roots is listed and every listed path exists. Every build profile validates
  declared inputs before checking outputs, and source/header contents are part of cache keys, so
  a missing path or a same-timestamp edit cannot produce a cached false green.

- **The ignore policy no longer hides arbitrary dotfiles or enumerates credential-like names.**
  Local private material now uses the workspace directory conventions plus `.env`, avoiding
  broad patterns that could silently hide legitimate source or configuration.

### Changed

- **The test catalog now enforces the numbers and memberships it prints.** The build driver and
  gate compile the same preprocessed registry manifest, so comments and disabled source text
  cannot masquerade as active tests. The gate rejects duplicates, requires every regression to
  remain in the hosted suite, compares the documented regression list one-to-one, and requires
  every test source to belong to the hosted, freestanding, benchmark, or explicit cross-only
  registry. The current suite is 111 hosted tests plus 22 runnable examples (133 executables),
  with a 29-test regression subset and 121 test sources. The freestanding cross smoke is resolved
  by its stable path rather than by a fragile registry index.

- **Public path examples are repository-relative.** Build-root examples now use
  `build-out/proven_c_lib`, and the repository policy check rejects Unix home-directory paths
  without relying on one machine's user name.

- **RFC-0006 is an implementation record rather than a stale proposal.** It carries the measured
  first-pass manual result and links its post-implementation gate findings to RFC-0007.

### Added

- **RFC-0007 - whole-library audit and hardening.** It separates source-proved defects,
  verification gaps, and performance hypotheses, then assigns each a regression or measurement,
  compatibility risk, and exit condition before implementation.

## [2026-07-23] - proven_c_lib-v26.07.23b

A full source-to-documentation audit, and the fixes it produced. Two were real build defects; the
rest were documents asserting things about the code that had stopped being true. The library's
behaviour is unchanged - no function was added, removed, or altered.

### Fixed

- **`nob.c` built and ran three tests twice.** `test_contract_map_hardening`,
  `test_contract_pool_misuse` and `test_contract_float_module_layout` each appeared twice in
  `all_tests[]`. Duplicated work, and a duplicated entry is a place a later edit changes one copy.

- **Editing seven headers triggered no rebuild.** `config.h`, `encode.h`, `hash.h`, `panic.h`,
  `random.h`, `stream.h` and `platform/proven_sys_random.h` were missing from the `headers[]`
  dependency list, so a change to any of them left every dependent object stale. This is the worst
  kind of build bug: it produces a binary that does not match the source and says nothing.

- **Eight cross-file manual links pointed at an anchor that does not exist.** The heading
  "4.2 Caller-owned state - no destroy, do not copy" contains an em dash, which GitHub renders as a
  *double* hyphen in the slug. Four English and four Korean links used the single-hyphen form. The
  Korean ones were doubly wrong, aiming an English slug at a Korean heading.

### Changed

- **The alias index (Appendix A) is regenerated and now complete.** It listed 440 rows and claimed
  "416 aliases"; the header defines 501. Sixty-one `xcv_` names - the whole of `hash`, `encode`,
  `stream`, most of `fs`, and the random generators - had no row. The gate that was cited as
  preventing this compares the alias header with the public headers and never reads the appendix,
  which is exactly why the appendix drifted. Both language versions now carry all 501 rows and say
  where the number comes from.

- **`TEST.md` describes the suite that actually runs.** It claimed 118 tests, a 27-test regression
  subset and 2 benchmarks; the real numbers are 110 registered tests plus 22 runnable manual
  examples, a 28-test subset and 3 benchmarks. Six of the eight per-class counts were wrong and the
  classes did not sum to the total. Ten registered tests had no catalog entry at all and now have
  one.

- **`bench-float` no longer claims to be float-only.** It has been running `test_bench_primitives`
  - hashes, encoders and generators - for as long as that test has existed.

- **The freestanding guide lists the three float sources it compiles** (`float_decimal.c`,
  `float_parse.c`, `float_format.c`), and the module table now has rows for `float_parse.h` and
  `float_format.h`. A reader could not previously tell whether `proven_strtod` works on bare metal.
  It does; the freestanding build only drops float *formatting*, and does not set `errno`.

- **`proven_println`'s one allocation is documented.** A line that does not fit the 512-byte stack
  buffer falls back to the heap for that call rather than being refused. Chapter 0's contract table
  said only functions taking an allocator can allocate, and chapter 5's measured table showed a bare
  `0` under `malloc()`. Both now state the bound.

- **The Korean mirror carries the nine sections it was missing** - five in `manual-ko.md`, including
  the reading-order table and the parts/prerequisites map, and four "why" sections in chapter 8.

### Added

- **`tests/test_docs_test_catalog` - a gate for the test catalog.** Every test registered in
  `nob.c` must have a `TEST.md` entry, and the catalog must be one-to-one with `tests/test_*.c`.
  `TEST.md` claimed its counts were checked against `nob.c`; nothing checked them, and they had
  drifted in both directions. The claim is now true.

## [2026-07-23] - proven_c_lib-v26.07.23a

Operating-convention cleanup. No library code changed; `src/` and `include/` are identical
to v26.07.20g apart from the version constants. The change is to the project's own process
docs, which had accumulated redundancy and one stale claim.

### Changed

- **The release checklist and the G7 gate description now name `README-ko.md` explicitly.**
  `CHECKLIST.md` and `DOCUMENTING.md` still said to sync "`README.md` (both language
  halves)" - wording left over from when the README was one bilingual file. The README has
  since been split into `README.md` (English) and `README-ko.md` (Korean), and the actual
  gate (`test_docs_version_sync`) already checks them separately; only the prose lagged. It
  now matches the gate. The maintainers' operations notes likewise name both READMEs in its
  document-update rules.

- **The resume-packet model is consolidated.** The maintainers' operations notes and
  `scripts/check-docs.py` now describe a single local queue (`BACKLOGS.md`) and a resume
  packet that lives in `CONTEXT.md`, replacing the previous split across separate handoff
  and backlog files.

### Removed

- **Stale reference to a retired local file** in the maintainers' backlog (it named a second
  gitignored queue that no longer exists).

## [2026-07-20] - proven_c_lib-v26.07.20g

The provenance section answers the fair objection - "every example is contrived" - instead of
dodging it. No library code changed; `src/` and `include/` are identical to v26.07.20f apart from
the version constants.

### Added

- **An honest section on why the runnable examples look artificial**, because they have to. A
  compiler only exploits provenance when it can *see* where a pointer came from, and that
  visibility is exactly what a small example has and a realistic one hides behind a `malloc` or a
  function call. Every realistic idiom that reconstructs pointers by arithmetic - tagged pointers,
  XOR linked lists, a `refcount` header reached through `data[-1]`, a `uintptr_t` round trip - was
  compiled at `-O2`/`-O3` and **all produced the correct answer**, because the model WG14 chose
  (PNVI-ae-udi, exposed addresses) was designed to keep those idioms working. The dangerous part is
  that "mostly works": the rule is still in force and only waits for enough visibility.

- **A latent example that looks like production code**, not a puzzle: a `checksum(head, 64 + 192)`
  with an off-by-count loop that walks a pointer derived from `head[]` past its end. Compiled on the
  build machine, it prints `448` at `-O0` (reads out of bounds into adjacent memory) and `64` at
  `-O2` (the compiler knows the pointer came from a 64-element array and silently drops 192 of the
  256 iterations - no warning). Neither is the sum the author intended, and the two disagree by
  optimisation level alone. This is the shape a real provenance bug ships in: not a reproducible
  crash, but a correct-looking function with an expiry date set by the toolchain - and the argument
  for keeping bounds *with* the data, where "process both buffers as one" cannot be written by
  accident.

Mirrored in `README-ko.md` in 합니다체.

## [2026-07-20] - proven_c_lib-v26.07.20f

The provenance section now opens with the shock instead of the definition. No library code changed
-- `src/` and `include/` are identical to v26.07.20e apart from the version constants.

### Changed

- **The first provenance example is now a runnable program that prints its own contradiction.** The
  previous lead - `int *p = a + 4;` with a comment - asked the reader to take the point on faith and
  invited the reaction "so what?". It is replaced by a complete program whose `-O2` output is
  `*p = 11, *q = 2`: two pointers holding a bit-for-bit identical address (checked with `memcmp`),
  where dereferencing one gives `11` and the other gives `2`. **One address, two values**,
  deterministically, every run - because the compiler tracks that `p` came from `x` and keeps `y` in
  a register. Verified on the build machine (GCC 14.2), stable across `-O2` and `-O3`.

- **The section leads with that program and the "two rules" contrast references back to it**, rather
  than showing the same provenance bug twice. The strict-aliasing example and the
  `-fno-strict-aliasing` distinguishing table are unchanged.

- The README is honest that GCC *warns* about the `&x + 1` store in the lead example - and
  miscompiles it anyway, which is more unsettling than a silent one, not less.

Mirrored in `README-ko.md` in 합니다체.

## [2026-07-20] - proven_c_lib-v26.07.20e

The provenance section is corrected and made honest. No library code changed - `src/` and
`include/` are identical to v26.07.20d apart from the version constants.

### Fixed

- **The provenance example in the README was wrong, and it was pointed out.** It showed
  `int *p = a + 4;` and claimed `*p` is undefined "because provenance". Forming a one-past-the-end
  pointer is explicitly *legal*; `*p` being undefined is just an out-of-bounds read and demonstrates
  nothing about provenance. The example now says what is actually true: `p` and `b` can hold the
  same address and still not be the same pointer, because `p` remembers it came from `a`, and using
  `p` to reach `b` is what the compiler assumes cannot happen.

### Added

- **A reproducible provenance miscompilation, verified on the build machine.** Two `int *`
  pointers, no type punning, only the origin differs: at `-O1` a write through `&x + 1` reaches
  `y`; at `-O2` the same write, to a bit-for-bit identical address, does *not* - the compiler keeps
  `y` in a register because `p` came from `x`. Every number in the README was produced by compiling
  and running the example (GCC 14.2), not asserted.

- **Provenance and strict aliasing, separated and contrasted.** They are two distinct rules - one
  asks *what type* is at an address, the other *which object* a pointer may reach - and the README
  now proves it: `-fno-strict-aliasing` fixes a strict-aliasing miscompilation and does **nothing**
  for the provenance one. Both bugs are shown, with the exact compiler output and the flag that
  distinguishes them. The strict-aliasing example is also placed in
  [manual chapter 0 section 2](manual/manual-00-start-here.md) as the motivation for `proven_byte_t`.

- **An honest limit.** The library does **not** claim strict-provenance purity, and the README says
  why: the intrusive list's `container_of` - `(type *)((proven_byte_t *)ptr - offsetof(...))` --
  recovers a whole struct from a pointer to one of its members, an idiom the strictest readings of
  the object model have never comfortably blessed and that is nonetheless everywhere real C lives,
  the Linux kernel included. So the library defends the settled, agreed-upon undefined behaviour and
  treats the unsettled frontier - where a dominant technique sits at odds with the strict model - as
  exactly that. Provenance is the direction it leans, not a finished guarantee.

## [2026-07-20] - proven_c_lib-v26.07.20d

The README explains what the library is *for*. No library code changed - `src/` and `include/`
are identical to v26.07.20c apart from the version constants.

### Changed

- **`README.md` and `README-ko.md` rewritten for a reader who has just finished one C book.**
  343 -> 567 lines. The previous README opened with a feature list; this one opens with the
  problems a beginner has already hit - `strcpy` not knowing the destination size, `malloc`'s
  ignorable `NULL`, `printf("%d", 3.0)` compiling - and works outward from there.

- **The name is explained, and it is not what it looks like.** `proven` comes from **provenance**,
  not from *prove*. The new section explains provenance as C's memory model uses it - a pointer
  carries the identity of the storage it came from, so two pointers can hold the same value and
  still not be interchangeable - with a worked example and the WG14 Technical Specification
  ([N2577](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2577.pdf) ->
  [N3005](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3005.pdf), PNVI-ae-udi), stated
  accurately as a TS pending publication rather than as part of C23.

  It also records why the library exists at all: the author met strict aliasing and provenance
  late, found that C's rules about memory are considerably stricter than the mental model he had
  been carrying, and concluded - **precisely because he does not hold those rules reliably while
  writing ordinary code** - that they belong in a library and in conventions visible in every
  signature rather than in a programmer's attention. That `proven` also happens to mean *tested*
  is a coincidence of two unrelated Latin roots, and the accident describes the project better
  than the intention did.

- **"C is not portable assembly" is argued rather than asserted.** Effective types and strict
  aliasing (memory in C's model *has a type*; assembly has no such concept), provenance, and the
  fact that undefined behaviour means the standard imposes no requirement at all - not "whatever
  the machine does". The summary the section lands on: C is permissive about what you can write
  and strict about what it promises, and the "portable assembly" view conflates the two.

- **Where systems languages are going**, because this library is C's version of the same answers:
  the length-beside-pointer table from Pascal's length prefix through C++17 `string_view`, Rust's
  `&str`, and Zig's slices; why owned and borrowed must be *different types*; the allocator as a
  parameter, with heap/arena/pool compared; and what C99, C11 and C23 already provide. Links to
  Luca Sas's ACCU 2021 talk and Andre Weissflog's *Modern C for C++ Peeps*.

- **What the library is for**, in three parts: replacing the tired parts of the standard library
  from the bottom up **without excluding it**, being pleasant for a person and safe for a person
  working with an AI (because every important fact is local to the call), and testing in public
  whether modern C actually holds up.

### Fixed

- **`README-ko.md` now uses one register throughout.** It had been mixed - the new material in
  평서체, the older half inconsistent within single sections. It is 합니다체 end to end, with the
  first-person origin story kept personal rather than bureaucratic. Verified: every code fence
  identical to the English modulo Korean comments, and zero 평서체 endings outside code.
- **Appendix A's Korean mirror** was the one reference chapter still in 경어체; it now matches
  chapters 1-8, and gained the "this is a lookup table, not reading material" opener the English
  side already had.

## [2026-07-20] - proven_c_lib-v26.07.20c

The manual becomes a book. No library code changed - `src/` and `include/` are identical to
v26.07.20b apart from the version constants.

### Added

- **Chapter 0, the on-ramp that did not exist.** Until now the manual's first page was a table of
  fixed-width type aliases and the first runnable program was 126 lines into chapter 1. A reader
  who had finished one introductory C book had nowhere to start, and no glossary for the words the
  manual used as if they were ordinary C - `view` appeared 288 times, `arena` 115, `trait` 29,
  `provenance` 26, none defined at first use.

  `manual/manual-00-start-here.md` states who the manual is for, argues why the library exists
  from five C bugs the reader has already met - `strcpy` not knowing the destination size,
  `malloc`'s ignorable `NULL`, `printf` believing the format string, `char *` meaning four
  different ownerships, `qsort`'s untypecheckable comparator - and says what each answer **costs**.
  It ships a compiled hello-world, the build model, the five contracts that recur on every page, a
  glossary, and a libc -> `proven` map.

- **A declared reading order.** The chapter order was the header dependency graph and nothing said
  so. `manual.md` now declares six parts with prerequisites and an outcome for each, every chapter
  names what you should have read first, chapter 7 is relabelled Appendix A (a lookup table, not
  reading material), and chapters 3 and 8 state which half of the text material they are.

- **Four new runnable examples** - `ex_00_hello`, `ex_04_list`, `ex_04_ring`, `ex_05_time` - each
  compiled and run by the build like the existing eighteen.

### Changed

- **Every chapter now leads with why the thing exists.** Measured against the manual's own standard
  - `test_docs_manual_depth`'s 150 words of prose outside tables, headings and fences - sections
  meeting it went from **30 of 88 to 67 of 98**. Chapter 1, the entry point, went from 0 of 9 to 6
  of 9.

  Chapter 1 opens on the error model rather than type tables. Chapter 2 opens on the heap - the
  obvious case - and shows the allocator trait fourth, once you have used three allocators.
  Chapter 3 opens on the 1972 decision to end a string with a zero byte. The dynamic array opens on
  the two bugs in the resize line everyone writes by hand. Memory mapping, which had **one word**
  of prose, now explains that its failure modes are signals rather than return values.

- **The hardest material moved out of the second chapter.** Pointer provenance, CAS, ABA, hazard
  pointers and epoch reclamation are now chapter 6 section 3, where the rest of the concurrency subject
  is, instead of arriving after six sections of ordinary allocator use.

- **The depth gate's register grew from 7 sections to 26**, so most of the new writing is enforced
  rather than merely applied.

- **The Korean mirror reflects the current English throughout**, chapter 0 included - it never had
  one. 8,310 -> 9,913 lines. Chapter 0 is deliberately written in 경어체 while the reference
  chapters keep 평서체, and says so in the text.

### Fixed

- **A code block that had never been compiled by anything.** An indented ```c fence inside a bullet
  list opens a block whose indented closer the extractor cannot see, so it hit end-of-search and
  `break` - silently skipping that block *and every block after it in the chapter*. In chapter 2 it
  was the last section, so nothing followed and nothing showed. `nob.c` now fails on an
  unterminated block instead of breaking out of the loop.
- **The chapter 1 version excerpt had drifted** and only one of its three lines was checked:
  the manual said `PROVEN_VERSION_NUM 260713` and `SUFFIX "m"` while `version.h` said `260720` and
  `"b"`. All three lines are gated now. A partly-checked quotation is worse than an unchecked one,
  because it looks verified.
- **Eleven broken internal anchors**, found by auditing every `](#...)` in all 21 manual files.
  Two were introduced by renumbering chapter 6; seven were English anchors under Korean headings in
  the Korean chapter 4; two were pre-existing, including one where an em dash in a heading produces
  a double hyphen in the slug. Zero remain.

### Note

Design work only. RFC-0006 is the plan this executed, and
B-025 ... B-032 in the maintainers' backlog record what each phase set out to do.

## [2026-07-20] - proven_c_lib-v26.07.20b

The repository's non-C checks are part of the build now, and the one thing they had ever
reported turned out to be noise.

### Fixed

- **`scripts/project-check.sh` runs from `./nob`, before anything is compiled, and a failure
  fails the build.** It had been failing continuously since 2026-07-12 - through six releases,
  v26.07.13g to v26.07.13m - and nobody found out, because it was not part of any command anyone
  ran. This is the same lesson the documentation gates were built on, arriving from the other
  direction: the gates work because the build runs them, and a check the build does not run is a
  check that stops being true without telling anyone.

  Four details, each one a defect found while wiring it:

  - **Early, not late.** Placed after the tests, a documentation failure surfaces through
    `test_portability_nob_std_probe` - which invokes this driver as a subprocess and reports
    *"build driver completes with the fallback compiler standard"*. True, useless, and three
    steps from the cause. Verified in both directions: a planted violation now stops the build
    with **zero tests run** and names the offending file.
  - **Skipped for `regression` and cross runs**, so the check does not run twice per build (the
    probe above invokes `./nob regression`), and because a cross matrix links no hosted tests.
  - **Skipped loudly, never silently.** No script, no POSIX shell, or no `python3` each log a
    SKIP with the reason rather than passing quietly.
  - **Logged under `[PROVEN][PROJECT_CHECK]`.** `[PROVEN][CHECK]` already belongs to the test
    framework's assertions (`tests/proven_test.h:29`), and two things writing the same tag makes
    both unreadable.

- **The privacy scan's only finding was a false positive, and the example was wrong, not the
  scan.** `manual/examples/ex_03_u8str.c` built a throwaway path under the host's storage-pool
  mount prefix while demonstrating string editing. That prefix is a real private location on the
  machine this library is developed on - it is where the development container's own root
  overlay lives - so the scan was right to reject it and the example had no reason to use it.
  Changed to `/srv/etc/fstab` in the example, the chapter that quotes it, and the Korean mirror.

  This entry deliberately does not reproduce the old path. The first draft of it did, and **the
  new gate failed the build on the changelog** - which is the gate working, one commit after it
  was installed.

  The pattern was **not** narrowed to make the message go away. Weakening a privacy scan to
  silence a collision is how the real leak ships six months later; the example path was
  arbitrary and the lesson it teaches - inserting a prefix - is unchanged.

### Note

No library code changed. `src/` and `include/` are identical to v26.07.20a apart from the
version constants; the diff is `nob.c`, one example program, and two manual chapters.

## [2026-07-20] - proven_c_lib-v26.07.20a

A design release. No library code changed - `src/` and `include/` are identical to v26.07.13m
apart from the version constants. What shipped is two RFCs, the work that produced them, and two
harnesses that make them checkable rather than merely readable.

### Added

- **RFC-0004 - the view vocabulary, and the splitter every caller writes wrong.** Read Luca Sas's
  *Modern C and What We Can Learn From It* (ACCU 2021) against this library. The useful result was
  mostly negative: the talk argues for the owning/non-owning string split, values over
  out-parameters, allocator-as-parameter, a typed formatter extension point and a freestanding
  posture - and this library already does all of it, in several places more thoroughly than the
  talk proposes. The gap it exposes is narrow and entirely on the **non-owning** side. Of the nine
  view operations the talk shows, this library has three.

  The centre of it is splitting, and the finding is not that splitting is slow here. It is that
  there is no splitter, so every caller writes one, and **the loop a competent person writes first
  is wrong on six of six inputs** - including `"a,b,c"`, which silently loses `c`. The measured
  alternative people actually reach for, one owned string per field, costs 3.4x the time and **one
  malloc per field**: a million allocations to read 6.8 MB.

- **RFC-0005 - implementing the view vocabulary.** Exact declarations, every boundary as a table,
  the algorithms with their real complexity, the six files a public symbol costs in this
  repository, and a commit order. Writing it invalidated four things the design document stated
  confidently, and each correction is recorded rather than quietly patched:

  - `proven_u8str_view_find` returns `start_offset` for an empty needle, not `NOT_FOUND`, so
    RFC-0004's sketched iterator advances by zero bytes and **hangs**.
  - The forward search is **not** "shift-or for short needles, Two-Way for long ones" - that is
    its entropy-triggered fallback. The default path is a rarest-byte anchored `memchr` scan,
    `O(n*m)` in the worst case. The reverse-search design no longer rests on a worst-case
    guarantee the library does not actually make.
  - `_cmp` cannot inherit NULL-safety from a platform function three layers down: `{NULL, 5}` is
    representable and would compare five bytes against a NULL pointer.
  - The first draft's commit plan put the failing test before the header, which does not compile
    and violates the test-first rule it cited.

- **Two harnesses, so the documents can be checked instead of believed.** Neither is built by
  `./nob`; both compile against the library directly and the command is in the file.

  - `rfc-0004-benchmark.c` reproduces every number in RFC-0004 section 2 - the six-of-six wrong
    table, ns/field and allocation counts for four splitting strategies, and the
    empty-versus-invalid view demonstration. RFC-0003's measurements were never committed and are
    now unreproducible; this is the correction.
  - `rfc-0005-spec-check.c` **executes RFC-0005's specification** against its own tables: all
    thirteen rows and all three properties, plus 200,000 randomised cases. This is how a hole was
    found before it shipped - an ill-formed `{NULL,5}` source was yielded as a five-byte field
    over a NULL pointer, with eleven of twelve rows green.

- **B-018 ... B-024 in the maintainers' backlog**, each with an exit condition reconciled against RFC-0005.

### Fixed

- **RFC-0004's two descriptions of the substring search**, corrected in place with a pointer to
  the evidence rather than silently rewritten.
- **Three backlog exit conditions named APIs the design had already rejected**, which made them
  unclosable - and B-022's original wording would have led a contributor to ship the exact naming
  trap RFC-0005 exists to avoid.
- **A coverage claim that was not itself checked.** `rfc-0005-spec-check.c` claimed to run "every
  row" and "the two properties" while skipping the NULL-argument row and two of the three
  properties. It now checks them, and prints how much it exercised (380,883 fields, 86,325 forked
  iterators) so a vacuous pass is visible. An unchecked coverage claim is the same defect as an
  unchecked specification, one level up.
- Both `malloc` calls in the benchmark harness report failure instead of segfaulting in `memcpy`.

### Note

Nothing in RFC-0005 is implemented. The `n` separators -> `n + 1` fields contract cannot be
revisited once callers depend on it, so it is the decision to settle before code.

## [2026-07-15] - proven_c_lib-v26.07.13m

### Added

- **The documentation rules are gates now, and the manual's claims are assertions.** You cannot
  test-drive prose - but almost nothing that has gone wrong in this manual was a matter of taste.
  It was a claim that had stopped being true, a symbol that no longer existed, a number that
  disagreed with itself, a section that was listed instead of explained. Each of those is a
  *proposition*, and a proposition can be checked by the build. Five new gates, each one
  motivated by a failure that already happened:

  - **A function the manual documents must exist.** `proven_sysio_flush` was deleted and the
    manual went on declaring it as public API, in the present tense. **`proven_pool_free` never
    existed at all** - the real symbol is a static `proven_pool_free_trait`, and freeing a pool
    slot goes through the allocator trait - and the manual described it as a callable function for
    as long as the manual has existed. Fixed, and guarded. A reader who follows the manual and
    gets a *linker* error stops believing the rest of it.
  - **Every public function must be named in the manual** (the streaming directory API went
    undocumented for months).
  - **The version string must agree with itself** across `version.h`, the README's two halves,
    TEST.md, the manual headings, chapter 1's excerpt and the CHANGELOG's newest entry.
    `CHECKLIST.md` always required it; nothing checked, and `version.h` once sat five releases
    behind the CHANGELOG.
  - **Every module section must be documented to depth** - real prose, a reference table, the
    structures the caller declares, a runnable example, and **at least one counter-example**. The
    five modules added this cycle each had an intent paragraph and a table and *not one had a
    counter-example*; they passed every check that existed and were still half-written. The gate
    found two more gaps the moment it existed (the tree walk's entry struct, and its
    borrowed-view trap), both now filled.
  - **Every factual claim must be true** - the oracle. Twenty assertions drawn straight from
    sentences the chapters state as fact: the CRC check value the interoperability promise rests
    on, the standard digest, chunk-independence, base64url's missing padding, a refused call
    writing *nothing*, an unseeded generator being inert, a line that exactly fills the buffer
    being returned while a longer one is refused.

  The rule that makes it work, and the useful half of the idea: **when you write a sentence a
  reader could act on, write the assertion for it.** If you cannot state the assertion, the
  sentence is too vague to be in the manual.

- **`DOCUMENTING.md`** - the process (survey -> plan -> edit -> verify) and the gate table, with
  the failure that motivated each gate. `CHECKLIST.md` points at it.

### Fixed

- **`proven_pool_free`, a function the manual documented and which does not exist.** Freeing a
  pool slot goes through the allocator trait (`proven_pool_as_allocator`), like every other
  allocator.
- The tree walk's `proven_fs_walk_entry_t` was never listed, and its borrowed-`path` trap - every
  entry aliases one reused buffer - had no counter-example.

## [2026-07-15] - proven_c_lib-v26.07.13l

A documentation release. The library grew five modules this cycle and the manual had kept up
only in the sense that it *mentioned* them; this brings them to the depth of the chapters
around them, and finishes the job.

### Changed

- **The ownership matrix has a second class.** It used to be a list of things you must destroy,
  and the sixteen public structs added this cycle destroy nothing - they are a different kind of
  object, and it needed a name: **caller-owned state**. You declare one, hand its address to a
  constructor, and use the handle you get back. The rule owning objects do not have is now
  stated where a reader meets it before they hit it: *a caller-owned state object must not be
  copied or moved once a handle has been made from it - the handle holds a pointer into the
  struct.* All sixteen are tabulated with what constructs them and their individual sharp edge
  (copying a seeded generator clones the keystream: two "independent" tokens become one token).

- **The new modules got their depth.** Hashing, encoding, randomness, streams and the standard
  streams each gained the three things the older chapters have and they lacked: the structures
  the caller holds, an API reference table, and - the gap that mattered most - **counter-examples**.
  The older chapters teach as much through their `Wrong:` blocks as through their prose. Now
  these do too: a keyed hash with a fixed key; CRC-32 used to decide two things are "the same"
  where someone gains by fooling you; a session token from the *reproducible* generator; a
  ChaCha seed taken from the clock; an ignored seeding bool; `% 6`; a buffered writer never
  flushed; a line view kept past the next read; the state struct copied, which is the
  use-after-free an audit reproduced.

- **README** showcases the two capabilities that previously only had a name in the module list:
  hashes/tokens/encoding (with the by-use-case table that is the point of those modules), and
  streams - reading stdin a line at a time, and printing without a syscall per line. Both
  language halves; both snippets compiled against the library before being pasted.

### Added

- **`proven_fs_dir_open` / `_next` / `_close` is documented at last** - a whole API that had no
  section. It is the answer to a real problem (`proven_fs_list` reads the entire directory before
  you see any of it and allocates a string per name: 50,000 entries cost 189 ms, +4.2 MB and
  50,008 allocations), and a reader who never learns it exists reaches for the wrong call on a
  mail spool.
- The last eleven functions nothing had ever documented. **Every public function in
  `include/proven/` is now named somewhere in `manual/`.**

## [2026-07-15] - proven_c_lib-v26.07.13k

### Added

- **`encode.h` - hex and Base64, by use case (RFC 4648).** Once you can hash a thing and draw a
  random token, you need to write those bytes somewhere that only holds text - a URL, a header,
  a log line. The library had no general encoding; the only bytes-to-text it owned was SHA-256's
  own `to_hex`. Now: `proven_hex_encode`/`_decode` (lowercase, what sha256sum and git print);
  `proven_base64_encode` (standard `+/`, `=`-padded, for HTTP/MIME/JSON);
  `proven_base64url_encode` (`-_`, no padding, for URLs and filenames); and
  `proven_base64_decode`, which accepts BOTH alphabets and padded-or-not. The two ways these are
  usually got wrong are refused: a decode validates its whole input before writing a byte
  (a stray character, bad length, bad padding, or embedded whitespace is
  `PROVEN_ERR_INVALID_ENCODING` with nothing committed - never a read past the end or a silently
  short result), and the output size is a function you call, not a number you remember (a short
  buffer is `PROVEN_ERR_OUT_OF_BOUNDS`, never a truncated prefix). Pure computation, available
  freestanding. Checked against RFC 4648's own vectors and differentially against Python and zlib.

- **A primitive throughput benchmark** (`tests/test_bench_primitives`, run through
  `./nob bench-float`), and the doc it produced, `primitives-benchmark.md`. It times the
  hashes, encoders, and generators over a fixed buffer, folding each output into a checksum so
  the work cannot be optimised away and a drift surfaces as a correctness failure.

### Changed

- **CRC-32 is ~4.2x faster.** The benchmark put it at ~104 MB/s - four times slower than FNV -
  because it was the textbook bitwise form (eight shift-and-xor iterations per byte, no table).
  Replaced with the standard 256-entry reflected table, turning eight iterations into one lookup:
  ~104 -> ~432 MB/s, for 1 KiB of `.rodata`. Byte-identical output: the table's `"123456789" ->
  0xcbf43926` check value is verified against it, and it still matches `zlib.crc32`.

### Fixed (found by the standing adversarial audit of the new encode module)

- **`proven_base64_decoded_size` under-reported for unpadded input.** It returned `(n/4)*3`,
  which floors away the 1-2 bytes an unpadded tail carries - so a caller who sized a buffer with
  the documented function could not decode the library's own `proven_base64url_encode` output
  (`OUT_OF_BOUNDS` on valid input; the audit hit it 3.3M times in 5M fuzzed inputs). Fixed to
  `((n+3)/4)*3`. Not a memory bug - the decoder checked the true length internally - a broken
  size contract.
- **The decoders lacked the `{out == NULL, out_cap > 0}` guard the encoders have**, so that shape
  stored through NULL (SEGV). Both now return `PROVEN_ERR_INVALID_ARG`, matching the encoders.

## [2026-07-15] - proven_c_lib-v26.07.13j

Two regressions the standing audit found in the previous release's own fixes - the place the
process says the next bugs are (TESTING.md section 5.2) - plus a sweep of long-standing doc debt.

### Fixed

- **A stream byte was stranded after a line too long for the buffer.** The read-line lookahead
  added last release stashes one byte when a line overruns; a following raw `proven_reader_read`
  then got a spurious `PROVEN_ERR_EOF` while that peeked byte sat unread, because
  `reader_buffered_fill` reported "did the source hand over bytes" rather than "is there
  anything to read". A read-to-EOF loop never came back for it, so the byte was silently lost.
  Fill now reports whether the buffer became non-empty; a re-inserted peek is progress too.
  Confirmed clean over 2,000,000 rounds of read_line/raw-read interleaving vs a reference.

- **The ChaCha seed scrub compiled to nothing.** `seed_from_entropy` cleared its 32-byte seed
  with a plain loop, under a comment saying not to leave key material on the stack - but nothing
  reads the seed afterward, so the stores were dead and the optimiser removed them (at -O1
  entirely). The raw OS entropy persisted in the frame. Replaced with a `secure_zero` that
  writes through a volatile pointer (an observable side effect the optimiser must keep, and
  freestanding-safe, unlike `explicit_bzero`/`memset_s`). Verified in the -O2 disassembly.

- **Documentation debt, swept.** The manual still *declared* `proven_sysio_flush` - deleted a
  release ago - as public API, and described it in the present tense. "`proven` exposes no
  fsync" was still asserted in the manual and an example, false since v26.07.12g. The
  `proven_rng_t` trait's obligation (a degenerate hand-written source makes `proven_rng_below`
  spin) was undocumented. And nothing said why sysio has both a line reader and a token scanner.
  All fixed, and TEST.md gained the suite descriptions for tests that had been counted but not
  catalogued.

### Added

- **A coverage sweep of the public functions no test had ever called** (`proven_fs_symlink`, the
  bounds-checked mem slices, the formatter's caller-scratch path, the mutable map/array lookups,
  `linear_search`, `proven_u16str_create_from_view`, and the standard-stream bridges left
  uncovered). No defect surfaced; the surface is no longer unexercised.

## [2026-07-14] - proven_c_lib-v26.07.13i

### Added

- **The entropy source is a thing you can install.** `random.h` could get entropy from an
  operating system and from nowhere else. That is fine until the target has no operating system
  - which is precisely the target the ChaCha generator was added for. The bare-metal story
  stopped one step short of being usable: the generator ran anywhere, and there was no way to
  seed it, because `proven_random_bytes` was compiled out of a freestanding build entirely.

  Entropy is the one thing a program cannot compute for itself, so it is now a hook rather than
  a hard-coded call. `proven_random_set_source(fn, ctx)` installs it:

  - **Hosted:** the OS CSPRNG is already installed. You call nothing.
  - **Bare metal:** a board *has* real entropy - an on-chip TRNG, a ring oscillator, an ADC's
    noise floor - and the library cannot know where. Hand it over once at startup, and
    `proven_random_bytes` and `proven_chacha_rng_seed_from_entropy` work unchanged: a few hundred
    bytes of hardware entropy become an endless cryptographic stream that needs nothing further.

  With no source installed, `proven_random_bytes` returns **false**. It does not fall back to a
  clock-seeded PRNG, because that looks like success and is a security hole nothing reports.

  There is deliberately **no built-in `RDRAND` / `RNDR` backend**: on a hosted target the OS
  already mixes the CPU's instruction into its own pool, so calling it directly buys nothing and
  costs you that mixing - and a raw hardware instruction used as the sole source is the
  arrangement people have argued about for a decade. It is four lines behind this hook, and then
  the choice is visibly yours.

### Fixed

- **`getentropy` was documented and never called.** The header claimed the OS backend was
  "`getrandom` on Linux, `getentropy` on the BSDs and macOS, `BCryptGenRandom` on Windows". The
  BSDs and macOS quietly fell through to `/dev/urandom` - which works, but is not what the
  documentation said, and needs a file descriptor that an fd-exhausted process cannot open,
  which is not a moment at which a key derivation should start failing. `getentropy` is now
  actually called there, in 256-byte chunks, with `/dev/urandom` kept as the last resort for
  systems that have neither call.

### Changed

- **`proven_chacha_rng_seed_from_os` is renamed `proven_chacha_rng_seed_from_entropy`.** On a
  board there is no "OS" to seed from, and the old name said there was. The call is otherwise
  unchanged. (Breaking, one release after it was introduced; the library is pre-1.0 and says so.)

## [2026-07-14] - proven_c_lib-v26.07.13h

Two modules that each offered one answer to a question that has several.

### Added

- **Randomness, by use case - a reproducible generator, a cryptographic one, and the OS.**
  `random.h` offered exactly one thing, the OS CSPRNG, and said in its own header that this was
  deliberate: a fast PRNG and a secure one are different tools, and shipping one under a name
  that suggests the other is how insecure tokens get written. The reasoning was right; the
  conclusion - offer neither - was wrong. A caller who needs a reproducible sequence does not
  stop needing one. They write `rand()`, or a hand-rolled LCG, and end up with something worse
  than what the library declined to give them. And a bare-metal target, which has no OS CSPRNG
  at all, was left with nothing.

  The module is now organised by use case, the way `hash.h` is:

  - `proven_xoshiro256ss_t` - fast and **reproducible**, for simulations, tests, games. The
    same seed replays the same run, which is what makes a failing test debuggable. Explicitly
    not secret-grade, and named so it cannot be mistaken for one. Seeded through SplitMix64, so
    seed 0 - or 1, or 2, which is what callers actually pass - lands on a well-distributed state
    instead of the all-zero state xoshiro can never leave.
  - `proven_chacha_rng_t` - cryptographic, and pure arithmetic: it needs no OS once seeded,
    which makes it the answer on a bare-metal target and the fast answer for bulk random data
    on a hosted one (no syscall per draw). Verified byte for byte against the standard's
    keystream, over six random keys and streams of 140 blocks, using OpenSSL as the oracle --
    which was itself first checked against RFC 8439's own vector.
  - `proven_rng_below` / `_range` / `_f64` / `_shuffle` - unbiased, over any source.
    `x % n` is biased whenever `n` does not divide 2^64, and everyone writes it anyway; these
    use Lemire's multiply-and-reject and an unbiased Fisher-Yates.

  The trait, `proven_rng_t`, is **infallible** by design. That is not a simplification - it is
  where the failure went: asking an operating system for entropy can fail, so that failure is
  confined to seeding, checked once, and every draw downstream is total.

  The generators are pure computation, so **`random.h` now works freestanding**; only the OS
  entropy source stays hosted. A board with no OS gets ChaCha20 seeded from its own hardware
  entropy - not a clock-seeded PRNG pretending to be a CSPRNG.

- **The standard streams are writers and readers.** `stream.h` had writers, readers, buffered
  writers and a line reader; `sysio.h` had stdin, stdout and stderr; the two had never been
  introduced. The cost was concrete: **there was no way to read stdin a line at a time** - the
  most common thing a program does with it - and the formatter could not be aimed at a standard
  stream at all, so every `proven_print` was its own write syscall.

  `proven_sysio_stdin_lines` + `proven_sysio_read_line` read stdin a line at a time;
  `proven_sysio_stdout_buffered` puts stdout behind a buffered writer, so a thousand small
  prints cost one syscall instead of a thousand, and `proven_fprintln` can finally be aimed at a
  standard stream; `proven_sysio_stdout_writer` / `_stderr_writer` / `_stdin_reader` are the
  unbuffered bridge. Nothing re-implements what `stream.c` already does - a second buffered
  reader would be a second place for the same bug.

  Buffering stays opt-in, and nothing registers an `atexit` handler to flush behind your back.
  The direct calls remain unbuffered: what they write is on its way out before they return.

### Fixed (found by the standing adversarial audit of both new modules)

The maths came back clean - ChaCha20 byte-identical to OpenSSL over 12 random keys and 10 KB
streams, xoshiro identical to the upstream reference over 2.56M outputs, Lemire's accept/reject
exact over 1M cases, the portable 64x64->128 fallback exact over 8M pairs, every chi-square
inside 2 sigma. The defects were all in the *failure* paths, and every one handed the caller bytes
that looked fine.

- **A ChaCha generator that was never usable handed back plausible bytes.** Three defects, two
  of them one bug: `proven_chacha_rng_next(NULL)` read an uninitialised local and returned this
  frame's stack as randomness (it was the only entry point in the module without a NULL guard);
  a never-seeded, stack-declared generator has `used == 0`, which the fill path read as "a full
  block of fresh keystream is ready" and copied its own uninitialised `block[]` out; and a
  generator whose `seed_from_os` *failed* was left zeroed - which produces an all-zero first
  block, so the claim "you get zeros, not plausible garbage" was true for exactly 64 bytes, and
  then the counter advanced and block 1 was a normal-looking, **fixed, publicly derivable**
  keystream. `used` alone could not encode usability, because a zero-initialised struct is the
  shape of "never seeded". The generator now carries an explicit seeded marker: unseeded or
  failed, it is inert - invalid trait, `next()` of 0, `fill()` of zeros.

- **`proven_reader_read_line` refused a line that fit.** It documented "a line LONGER than the
  buffer is `PROVEN_ERR_OUT_OF_BOUNDS`" and enforced something stricter - it rejected any line
  that *filled* the buffer. It had to, because it answered "too long" before attempting a fill,
  and a fill cannot tell "buffer full" from "source ended". But a full buffer means one of three
  things and only one is an error: the next byte is the newline that ends the line; the source
  has ended and what is held IS the final line; or the line really is too long. The middle case
  was data loss, and not exotic - **a 4-byte file with no trailing newline, read through a
  4-byte buffer, came back `OUT_OF_BOUNDS` with its entire contents unreachable.** One byte of
  lookahead tells the three apart, so the documented rule is now the enforced one.

- **`proven_sysio_out_t` / `_lines_t` contained a pointer to themselves**, so copying one
  dangled (ASan: heap-use-after-free). `proven_sysio_read_line` takes its state *by pointer* --
  the shape that says "relocatable" - so it now re-binds the inner reader on every call, making
  the implied contract true. The writer states cannot do that, and the header now says plainly
  that they must not be copied or moved once a writer has been made from one.

### Removed

- **`proven_sysio_flush` is deleted, and the delete is the point.** It claimed to flush a buffer
  that did not exist: a no-op on POSIX, a *disk sync* on Windows, and its own header told callers
  not to use it. Flushing a buffered writer's bytes to the OS is `proven_writer_flush`; pushing
  the OS's bytes to the disk is `proven_fs_sync`. They are different operations and now say so.
  Nothing in the library depended on it.

### Fixed

- **The README claimed `proven` exposes no fsync.** It has since v26.07.12g
  (`proven_fs_sync`, `proven_fs_write_file_durable`). Corrected in both language halves.

## [2026-07-13] - proven_c_lib-v26.07.13g

The time formatter's two encodings were quietly disagreeing, found and closed by the standing
adversarial audit, which otherwise came back clean across every remaining module.

### Fixed

- **`proven_time_u16_fmt` silently dropped fill/align/width specs the u8 formatter honoured.**
  The two time formatters are documented as one formatter differing only in output encoding
  ("specifiers matching `fmt.h`"), but the u16 path hand-rolled a parser that recognised only
  zero-fill `:0>N`: `{month:>4}` came back as `"3"` instead of `"   3"`, `{Weekday:>12}` was
  left unpadded, `{Month:*^10}` ignored its spec entirely. A silently discarded spec that emits
  the wrong-width string is exactly the quiet wrong answer this library refuses. The u16
  formatter now renders through the u8 path (which delegates each field to the `fmt.h` spec
  engine) and widens the result to code units, so the whole `{}` grammar is honoured for numeric
  and named fields alike and the two encodings can never again disagree. The duplicate
  hand-rolled `proven_sys_time_format_int_u16` PAL function (both the `wsprintfW` and the manual
  POSIX variant) is deleted. Pinned by `tests/test_unit_time_fmt_u16_parity`, which drives a
  broad spec matrix through both formatters and asserts byte equality.

- **A negative year under zero-fill padded one column too wide in the u16 formatter.** `{year:0>4}`
  of -44 rendered as `"-044"` through the u8 path (the sign counts toward the field width, as in
  printf's `%04d`) but `"-0044"` through the u16 path. Subsumed by the parity fix above, and kept
  pinned by `tests/test_regression_time_fmt_neg_year`.

### Changed

- **Documentation: the `hash` and `random` modules are now surfaced in `README.md`** (both language
  halves): the module index lists them, the intro sentence names hashing and OS randomness, and the
  "what it is not" section no longer claims "no cryptographic hashing" - it now scopes the real
  non-goals (signatures, key exchange, KDFs, authenticated encryption, TLS) around the hash/CSPRNG
  primitives that do exist. The `stream` module is listed too.
- **`ring.h` and `pool.h` gained contract notes** raised as non-defect observations by the audit:
  the raw `proven_ring_create` does not check `elem_size % align` (the `PROVEN_RING_INIT` macro
  always threads consistent values), and pool blocks must be freed exactly once and only through
  the owning pool (a double-free is trapped only in debug / `PROVEN_HARDENED` builds).

## [2026-07-13] - proven_c_lib-v26.07.13f

### Added

- **`{:e}` - scientific float formatting, completing the printf trio.** `{:f}` gives fixed and
  `{:g}` gives shortest, but neither is `%e`: `{:f}` never shows an exponent and `{:g}` uses one
  only when it is shorter, so there was no way to ask for "always scientific, N digits after the
  point" - the form you want for an aligned column of magnitudes or to match existing `%e`
  output. `{:e}` / `{:.Ne}` now render exactly what printf does: mantissa, a signed
  two-digit-minimum exponent, correctly rounded (half-to-even), at any magnitude including the
  smallest subnormal. The correctly-rounded scientific core was already there (the default form
  uses it at the extremes); this exposes it as `PROVEN_FLOAT_FORMAT_MODE_SCIENTIFIC`. Written
  test-first against printf's own output, and checked differentially against `%.Ne` over 200,000
  random doubles at every precision (0 mismatches).

### Fixed

- **Zero-fill on a float put the zeros before the sign.** `{:08.2f}` on -3.14 produced
  `000-3.14` and `{:+08.2f}` on 3.14 produced `000+3.14` - not numbers a reader or a numeric
  column can parse. The integer path already placed zero-fill between the sign and the digits
  (the `0000+42` fix); the float path never did, and rendered the whole string and padded it,
  sign included. Pre-existing, and made obvious by `{:e}`, which makes signed scientific columns
  common. The sign is lifted out and the zeros placed after it now, matching printf: `-0003.14`,
  `+0003.14`, `-03.14e+00`.

## [2026-07-13] - proven_c_lib-v26.07.13e

Two defects found while exercising the modules together rather than one at a time - a
fmt -> file -> scanner -> float round-trip over 20,000 lines, and a fresh audit of the
string modules.

### Fixed

- **A float split across the buffered scanner's refill boundary was scanned wrong** - in two
  invisible ways. If the boundary fell on the exponent, "-3.0448...e" parsed as a valid float
  (the mantissa) and silently dropped the "e-222" that had not arrived: a truncated value
  committed as a success. If it fell on the sign, "-" alone is a parse FAILURE, and the
  scanner dropped the byte and desynced every later scan instead of asking for the rest. An
  integer that runs out mid-token already said "need more"; a float could not, because unlike
  an integer it can look complete, or look like garbage, when it is merely unfinished.
  proven_scan_f64 now flags needs_more both when a valid float might still grow (it ran to the
  buffer end, or stopped at a splittable exponent) and when a failed parse left only a float
  PREFIX - genuine garbage like "abc" is still the error it is, and does not wedge the scanner
  waiting for it to become a number. Found by the round-trip; pinned by
  tests/test_regression_scanner_float_split, which drives the split to every byte of the token.

- **proven_u16str_append_grow sealed its 2-byte terminator at the byte index, not the unit
  index.** internal.len is a byte count, so `[len]` writes the NUL at unit `2*len` instead of
  `len`. Latent under the doubling growth policy - the stray zero lands in unused slack and the
  following append re-seals the real terminator - but an out-of-bounds heap write the instant
  growth becomes exact-fit. Found by the u16str audit's poisoned-allocator repro; the invariant
  is pinned in tests/test_regression_v26_07.

## [2026-07-13] - proven_c_lib-v26.07.13a

An adversarial audit of the modules that had never had one - the scanner and the float
engine, the containers, and the platform layer. Everything below was **reproduced** before
it was fixed, and every fix has a regression test that was checked to fail against the
unfixed source.

The theme, again: **code that is correct for the input it was tested with and silently
wrong for the input it will actually meet.**

### Fixed (third audit round: the fixes from the second round)

Pointing the audit at the code the previous audit had just produced found three regressions
in it. That is the point of B-011, made twice in one day.

- **Copying a read-only file worked once and failed forever after.** Carrying the source's
  mode meant a 0400 destination - and `open(O_WRONLY)` on a 0400 file fails, so the *second*
  copy could not even open it, returned `PROVEN_ERR_IO`, and left the destination holding its
  old contents. An unwritable destination is made writable first (we are about to overwrite it
  anyway), the payload is written under 0600, and the real mode goes on at the end - so the
  contents are still never exposed under a wider mode than the original's.

- **The buffered writer remembered an inner *error* but not an inner *stall*.** A sink that
  takes nothing and reports no error - every wedged sink looks like this - made
  `proven_writer_write` return `PROVEN_ERR_IO` while leaving the writer thinking it was
  healthy, so it went on accepting writes: the receiver got `ABC`, a 27-byte hole, and `XYZ`,
  with the second write reporting success. The flush path had always treated `{OK, 0}` as a
  failure; the write path did not, and the two disagreed.

- **A symlink to a regular file was `PROVEN_FS_TYPE_OTHER` in a listing and
  `PROVEN_FS_TYPE_FILE` from `stat`.** The walk stat'd with `AT_SYMLINK_NOFOLLOW`. Two answers
  to the same question is worse than either answer, and a caller filtering a listing on
  `type == FILE` skipped files it could open and read. The walk follows now, exactly as `stat`
  does; a *dangling* link fails the follow and is still `OTHER`, which is honest - it cannot be
  opened at all.

- **`proven_writer_from_u8str` had no flush**, so a render that ran out of memory halfway left
  the string holding a valid, NUL-terminated *prefix* of the output - and `proven_writer_flush`
  answered `PROVEN_OK` on it. Found by asking whether every writer implementation keeps the same
  contract, which is the same question that found the pool refusing to free a block.

### Fixed (second audit round: the new code, the allocators, the filesystem)

- **`close()` failures were thrown away** - and `close()` is the last chance a filesystem
  has to say a write did not land. On NFS, CIFS and quota-enforcing filesystems it is the
  *only* chance: the bytes were buffered, `write()` said yes, and the refusal surfaces
  there or nowhere. `proven_fs_write_file` returned `PROVEN_OK` for a file the filesystem
  had just refused, and `write_file_atomic` went on to `rename()` the temp over the target,
  **publishing content the disk had rejected**. Reproduced with an `LD_PRELOAD` that fails
  `close()`. `proven_fs_close` now returns `proven_err_t` and is `[[nodiscard]]`; the write
  paths check it, and a failed close aborts the rename and removes the temp.

- **An atomic write exposed its contents under a wider mode.** The temp was chmod'd to the
  target's mode at the *end*, so the entire new contents of a 0600 file sat in a
  world-readable temp for as long as the write took. A watcher thread stat'ing the temp
  during a 64 MiB rewrite saw `0644`. The mode goes on before the first byte does.

- **`proven_fs_copy` widened permissions.** Copying a 0600 file produced a 0644 one - `cp`
  does not do that. The source's mode is carried across, and set before the contents go in.

- **A symlink, a FIFO, a socket or a device was reported as `PROVEN_FS_TYPE_FILE`**, which
  tells a caller it may open it and read bytes out of it. A dangling symlink cannot even be
  opened; a FIFO blocks forever on a writer that never comes. They are `PROVEN_FS_TYPE_OTHER`.

- **`proven_mmap_sync` on a PRIVATE mapping returned `PROVEN_OK` and persisted nothing.**
  A copy-on-write mapping has nothing to write back. It is `PROVEN_ERR_UNSUPPORTED`.

- **The scanner's rollback wrapped `cursor` and `length` to ~2^64** - a defect introduced by
  the pipe fix earlier the same day, and the worst kind: the whitespace refill moved the
  cursor *before* the fill that compacts the buffer, so the reported shift exceeded the
  snapshot the rollback subtracts it from. Both indices wrapped by the same amount, so every
  guard still compared true, and the next scan read `buffer[SIZE_MAX-15]`. ASan called it a
  heap-buffer-overflow; on a pipe it silently discarded the buffered bytes instead. The
  whitespace is now stepped over *before* the snapshot, where there is nothing to roll back
  to yet.

- **`{:f}` refused any double above ~1e121** with `PROVEN_ERR_INVALID_FORMAT` - a *bad format
  string* - because the fixed form was rendered into a 128-byte scratch. The number was fine;
  the scratch was not, and no output buffer the caller supplied could help. `{:.60f}` on
  1e308 (370 characters) now renders whole.

- **The buffered reader dropped the bytes of an EOF that carried them.** A reader may return
  `{PROVEN_ERR_EOF, N}` with N nonzero - the library's own `read_all` does - and the last N
  bytes of a file are still bytes. The final line of a file vanished whenever the source
  reported its end and its last bytes in the same breath.

- **The pool refused every `realloc`, including "free it" and "make it smaller".** Every call
  was `PROVEN_ERR_INVALID_ARG` - which a trait-generic caller reads as *"you passed me
  garbage"* and goes hunting for a bug in its own code. A pool genuinely cannot grow a block,
  and that is `PROVEN_ERR_UNSUPPORTED` now ("not my job"); `realloc(ptr, 0)` frees and returns
  NULL like every other allocator, and a shrink within the item size is the no-op it obviously
  is. A size the pool does not serve is `UNSUPPORTED` for the same reason.

- **The allocator trait meant different things per allocator**: `alloc(0)` was `NOMEM` on the
  heap (a lie - nothing was out of memory) and `PROVEN_OK` with a live pointer on the arena;
  `realloc(ptr, 0)` returned NULL on the heap and a non-NULL pointer on the arena, though the
  trait documents NULL; and a pure *shrink* of a non-tail arena block failed with `NOMEM` on a
  full arena, which is an absurd answer to "please use less". All three now answer identically,
  pinned by `tests/test_contract_allocator_trait`, which runs the same code against both.

- `fmt.h` claimed `PROVEN_ARG('Z')` prints `Z`. It cannot: `'Z'` has type `int` in C. A `char`
  *variable* does print as a character; the note now says which is which.

### Fixed

- **The "correctly rounded" float parser was not correctly rounded.** The exact
  big-integer fallback - the tier whose entire job is to decide, bit for bit, which way a
  decimal sitting on a rounding boundary goes - built `5^q` for `56 <= q <= 350` by taking
  the *Eisel-Lemire* table entry and shifting it left. That entry is a 128-bit mantissa
  **rounded** to 128 bits, and `5^q` is odd, so the shift can never be exact (`q=55`: the
  table says `...078124`, the truth is `...078125`). The exactness tier was comparing against a
  corrupted power of five. Every exact halfway value in that exponent window broke the tie
  in a fixed direction instead of to even, and any value within ~1e-38 of a boundary came
  out one ULP wrong - with `PROVEN_OK`. A differential run against glibc found **2,923**
  of them. `scan.h` and manual chapter 8 both promise ties-to-even, bit-identical to a
  correct `strtod`; for any input with 20+ significant digits in that range, that promise
  was false. Above the exact table, `5^q` is now multiplied, not looked up.

- **The buffered scanner could not read a pipe** - the one thing it exists for. `read()`
  on a pipe returns whatever has arrived so far; `scanner_fill` treated *any* short read as
  end-of-input and **latched** it. So a token straddling the boundary was committed
  **truncated** (a writer sending `"123"`, a pause, then `"456 789\n"` produced the integer
  **123**, reported as a successful scan), and every later scan returned `PROVEN_ERR_EOF`
  while the rest of the stream sat unread in the pipe forever. Only a zero-byte read is an
  end of input now - which is what `read()` itself has always meant by it. Regular files hid
  the bug completely: a file read is short only at real EOF.

- **A failed read was reported as a clean end of input** (scanner). `proven_sys_io_read_once`
  reports a failed `read()` as `{PROVEN_ERR_IO, 0}`, and the end-of-input test accepted any
  zero-byte result - so every `EBADF`, `EIO` and `ECONNRESET` became a tidy EOF. A stream
  that broke halfway through was indistinguishable from one that finished.

- **A map with churn grew forever.** `remove()` cannot free a bucket - an open-addressed
  table needs the tombstone - so `used` only ever rises, and the rehash it triggers doubled
  the capacity **unconditionally**, even when the live count had not moved. That is every
  cache, every session table, every work queue. Measured: 100 live entries, two million
  operations, capacity **1,048,576**, **33 MB** held. Not a leak - every byte reachable,
  every byte freed at destroy - which is precisely why no leak checker ever mentioned it.
  The rehash now grows only when the *live* set needs the room, and otherwise reclaims
  tombstones at the same capacity (same walk, same cost). The 33 MB is now 8 KB.

- **`proven_u8str_append_grow` of an empty view left the string unterminated.** The grow
  allocated the block, then delegated to `append`, which returns early on an empty view --
  before sealing the NUL. `as_cstr()` is documented as always terminated, and it read
  straight off the end of a fresh heap block (ASan confirms). `proven_u16str_append_grow`
  had the identical bug. The seal now happens where the block is allocated, as `reserve()`
  has always done.

- **The sort handed the comparator a misaligned element.** `insertion_sort` held the moving
  element in a `proven_byte_t[128]` - alignment **1** - and passed its address to the
  caller's comparator, which reads it as the element type. For any over-aligned element that
  is a misaligned typed access: UB, flagged by UBSan at every optimisation level, and a fault
  on a strict-alignment target. It never crashed on x86, which is why it survived. The scratch
  is over-aligned now, and elements aligned more strictly than the scratch take the swap path,
  which only ever shows the comparator real array elements.

- **A fixed-buffer writer that had overflowed reported success on flush**, because it had no
  flush function at all - `proven_writer_flush` returned `PROVEN_OK` for it. "Render, render,
  render, check the flush" is what every caller does, and it was told a buffer that had refused
  half the output was fine. It reports the overflow now, and a later chunk that *would* fit is
  refused as well: writing it would land it after the hole.

- **A buffered writer that had failed reported success on the next flush.** The failing
  write emptied nothing into the buffer, so `flush` found nothing to fail on and answered
  `PROVEN_OK` - and "write, write, write, check the flush", which is how almost everyone
  uses a buffered writer, reported success on a stream missing every byte. Reproduced
  against `/dev/full`. The writer is sticky now: once it has lost bytes, every later write
  and flush returns the original error.

- **The panic handler was a data race** (TSan). It is read on the `*_or_panic` allocator
  paths, which run on every thread, and written by `proven_set_panic_handler`, which a
  program may call while those threads are running. It is atomic now.

### Changed

- **The scan engine can say "I ran out of input" (`proven_scan_t::needs_more`).** On a
  complete view, "the input ran out" and "the input is wrong" are the same fact, so the
  engine reported both as a malformed input - and the buffered scanner on top of it had no
  way to tell them apart. Over a stream they are *opposite* facts: a pipe that delivered
  `-` and then, 150 ms later, `12` produced `PROVEN_ERR_INVALID_ARG`, and `key=` against a
  pipe that had so far sent `ke` produced `PROVEN_ERR_NOT_FOUND`. Both are now refilled and
  retried. A wrong byte that is actually present is still an error - the scanner does not
  wait for input that cannot fix it.

- **`5^q` is built 27 exponents at a time.** The exact fallback multiplied by 5 once per
  unit of exponent below 64, and ran exponentiation-by-squaring over full big integers above
  it. `5^27` is the largest power of five that fits in a `u64`, so `5^350` now costs thirteen
  single-limb multiplies. It matters because the (now correct) exact tier is the only way to
  build `5^q`: the hard-input parse went 3,117 ns -> **1,945 ns**, i.e. correctness cost about
  6% over the old *wrong* answer rather than 70%.

- **`proven_u8str_append_fmt` renders each argument once, not twice.** The allocation-free
  fixed-capacity path measured the whole output and then formatted it all over again - for a
  double, that is the correctly-rounded decimal engine run twice with the first answer thrown
  away. It writes as it goes now and restores the original length if it fails, so "atomic"
  still means what it said. Measured: an int/string/int line **254 ns -> 155 ns**; a double
  **545 ns -> 277 ns**. Pinned by `tests/test_contract_fmt_atomic`, because atomicity now rests
  on a rollback rather than on never having written.

- **Interior pointers are documented as perishable.** `proven_array_get(_mut)`,
  `proven_map_get(_mut)` and `proven_u8str_as_view` return pointers into the container's
  storage that the next growing operation frees. The headers now say so; all three were
  demonstrably a use-after-free waiting to happen.

- **`platform/proven_sys_time.h` no longer calls the wall clock "monotonic".** It reads
  `CLOCK_REALTIME` (Windows: `GetSystemTimeAsFileTime`) and always has. Code measuring an
  interval by subtracting two of these can get a negative duration; the header was telling
  it that could not happen.

### Added

- **`map` is HashDoS-resistant by default (B-015).** `map` hashed untrusted string keys with
  non-keyed FNV-1a, so an attacker who controls the keys could compute collisions offline and
  flood one bucket - turning the map's O(1) into O(n^2) on demand. It now hashes string keys
  with **keyed SipHash-2-4 under a per-process secret** drawn once from the OS CSPRNG, exactly
  the switch Python, Rust and the Linux kernel made for their built-in tables. `proven_map_create`
  gives you this safe default; `proven_map_create_trusted` opts into fast FNV for keys your own
  program chooses. `proven_map_hash` exposes the function a map actually uses, so the choice is
  observable (and useful for inspecting distribution). Integer keys are unaffected; on a
  freestanding target, which has no CSPRNG and no attacker model, string keys fall back to FNV.
  The per-process key is seeded exactly once even under a 64-thread race (verified under TSan);
  a map created in one process hashes a given key differently from the same map in another,
  which is the unpredictability the defence rests on.

- **`proven/random.h` - cryptographically strong OS randomness.** `proven_random_bytes` fills a
  buffer from the OS CSPRNG (getrandom / getentropy / BCryptGenRandom), returning false where
  there is none rather than handing back weak bytes. No user-visible PRNG: a fast reproducible
  generator and a secure one are different tools, and offering one under the other's name is how
  insecure tokens ship. It is what seeds map's keyed hash, and what any caller needing a key, a
  token, or a nonce should use.

- **`proven/hash.h` - hashing, organised by use case.** lowent's case study needed a
  content-addressing digest, found proven had no hash of any kind, and hand-wrote BLAKE3-256
  - "exactly the kind of code that should not be hand-rolled". There is no single "hash", so
  the module gives one primitive per job and names the job: `proven_hash_bytes` (FNV-1a) for
  your own table on trusted input; `proven_hash_keyed` (SipHash-2-4) for a table on *untrusted*
  input, where a non-keyed hash lets an attacker collide every key into one bucket;
  `proven_crc32` (IEEE, gzip/zlib/PNG-compatible, streaming) for detecting *corruption*; and
  `proven_sha256` (FIPS 180-4, streaming, with a git/sha256sum-style hex) for *fingerprinting*
  content safely against a forged match. Every one is byte-exact and endianness-independent,
  all are royalty-free, all are implemented from their specifications rather than copied, and
  each is checked against its standard's own known-answer vectors (`tests/test_unit_hash`) and
  differentially against Python's hashlib/zlib and an independent SipHash over every length to
  300. The second feature written test-first (`TESTING.md` section 5.1).

- **`proven_fs_walk`** - recursive, pre-order directory iteration that **cannot loop and cannot
  escape**. The manual had been telling callers to guard against symlink cycles themselves ever
  since the walk learned to follow links; this is the guard. It never descends *through* a
  symlink (the symlinked directory is still reported - it exists - it is simply not entered),
  which buys both guarantees at once: a link to an ancestor cannot loop it, and a link anywhere
  else cannot walk it out of the tree you asked about. It also carries the `(dev, ino)` of every
  directory on the current path, for the loops a symlink is not needed for. A directory it cannot
  read is **reported** - `proven_fs_walk_next` returns that directory's error with the entry
  naming it, and the walk goes on - because a tree walker that silently skips an unreadable
  subtree is how a backup misses files and reports success. Memory is bounded by **depth**, not
  breadth: one handle and one `(dev, ino)` per level, plus a single reused path buffer.

  This is the first feature written under the test-first rule (`TESTING.md` section 5.1): the
  contract and a failing test in one commit, the implementation in the next, and then the
  standing adversarial audit (section 5.2). Between them they caught, before and after it shipped: the
  first draft of the contract ("follow, but stop at a cycle") quietly walking all of `/tmp`; a
  300-level tree silently truncated at 256 and reported as a clean end-of-walk; a `readdir()`
  failure reporting the whole path instead of the directory name and a depth one too high; and
  a **TOCTOU escape** - a directory swapped for a symlink between being listed and being
  entered was followed out of the tree, until the descent was made fd-relative and
  `O_NOFOLLOW`. Every one is pinned by a regression test verified to fail against the code
  before its fix.

- `tests/test_regression_float_exact_pow5`, `tests/test_regression_scanner_short_read`,
  `tests/test_regression_map_churn`, `tests/test_contract_sort_alignment`,
  `tests/test_contract_fmt_atomic`, and an empty-view NUL-seal section in
  `tests/test_regression_v26_07`.

## [2026-07-12] - proven_c_lib-v26.07.12i

Closes B-005, B-009 and B-010.

### Added

- **The format spec grammar grew the things it was missing.** It supported exactly
  four: fill, align, width, and lowercase `x`. Now:

  ```text
  {:[[fill]align][sign][#][0][width][.precision][type]}
  ```

  - **Float precision**: `{:.3}`, `{:.0}`, `{:f}` (fixed), `{:g}` (shortest
    round-trip). This is the one that mattered. Every float came out with **exactly six
    decimals, forever**, so a float column could not be aligned - 12.5 rendered nine
    characters wide and 100.0 rendered ten, and the column broke. The exact engine could
    always do precision; the `{}` grammar simply could not reach it.
  - **Bases and case**: `{:x}` `{:X}` `{:o}` `{:b}`, with `#` for the `0x` / `0X` /
    `0o` / `0b` prefix. A conventional uppercase hex dump was impossible before.
  - **Sign**: `{:+}` and `{: }`. Zero-padding goes *between* the sign and the digits -
    `{:+08}` on 42 is `+0000042`, never `0000+42`.
  - **`char` and `bool` arguments.** `PROVEN_ARG('Z')` printed `90`; there was no way
    to emit a single character at all. A `bool` renders `true` / `false`.

- **`PROVEN_ARG_OF(&obj, render)`** - the formatter is no longer a closed set. `PROVEN_ARG`
  is built on `_Generic`, which cannot be taught a type it was not told about at compile
  time, so a user struct could not reach `{}` at all: you pre-rendered it into a scratch
  string (an allocation and a copy per value, in the logging path, which is the one path
  that must keep working when allocation is what has failed) or you printed the fields one
  at a time and gave up on aligning the column.

  A renderer takes a sink, not a buffer, so it composes - it may call the formatter again.
  And the formatter runs it **twice**, once against a counting sink, which is what lets
  `{:>20}` align a user type exactly as it aligns an int with nothing allocated. A renderer
  whose two passes disagree is an error rather than a silently misaligned field, and a spec
  the library cannot interpret for your type (`{:x}`, `{:.2}`, `{:+}`) is refused rather
  than guessed at. Closes B-010.

- **`proven_fs_dir_open` / `_next` / `_close`** - streaming directory iteration.
  `proven_fs_list` reads the whole directory before the caller sees any of it. Measured
  on 20,000 entries: 80 ms and **+1,664 KB** resident, nothing visible until the last
  one. The iterator: 64 ms and **+0 KB**. The entry name is borrowed from the
  iterator's storage, which is what lets a huge directory be walked without an
  allocation per entry.

- `tests/test_unit_fmt_spec`, `tests/test_regression_stream_partial_write`, and
  directory-iteration coverage in `tests/test_unit_fs_position_and_sync`.

### Changed

- **A writer's `write_fn` now returns how many bytes the sink took**
  (`proven_result_size_t`), not just success or failure, and `proven_writer_write_partial`
  exposes that count to callers. The old trait said "consume the whole chunk or fail",
  which is a promise a pipe, a socket, or a filling disk cannot keep - so no correct sink
  could be written against it. `proven_writer_write` still means all-or-nothing; it loops.

- **The PAL directory walk reports failure.** `proven_sys_fs_dir_step` returns 1 / 0 / -1
  (entry / end / failure) in place of a bool, because `readdir()` returns NULL for both
  "the directory ended" and "the read failed", and errno is the only thing that tells
  them apart.

### Fixed

- **The buffered writer duplicated bytes on a partial write.** A sink that accepted
  4096 of 6000 bytes and then failed left the whole buffer in place, and the next flush
  re-sent it from the start: the receiver got **10,096 bytes, with the first 4096
  twice**, and no way to detect it. The flush now advances past the bytes the sink
  acknowledged and keeps only the unsent tail. Losing data is bad; silently doubling it
  is worse.

- **A failed read looked like a clean end of file.** The buffered reader had nowhere to
  put an error, so a source that broke mid-file set `eof` and the caller saw a complete,
  successfully-read file. It carries the error now. A file truncated by a disk error and
  a file that simply ended are not the same fact.

- **`{:f}` did not force the fixed form.** It set the SIMPLE policy and the dispatcher
  then ignored it, switching to scientific above 1e18 and below 1e-4 anyway - so `{:f}`
  on `1e20` rendered `1.000000e+20`, and `{}` and `{:f}` were byte-identical for every
  input in the language. The exact fixed-point engine was there the whole time; nothing
  called it.

- **A failed directory read was reported as end-of-directory.** `proven_fs_dir_next`
  mapped a NULL `readdir()` to `PROVEN_ERR_EOF` whether the directory had ended or the
  disk had failed, so a listing cut short by an I/O error was indistinguishable from a
  complete one. That is how a backup silently skips files. It is `PROVEN_ERR_IO` now,
  and `proven_fs_list` propagates it instead of returning half a directory.

- **The float engine silently rewrote precision 0 to 6.** `{:.0}` on 3.7 gave
  `3.700000`. "No decimals" is what `%.0f` means everywhere, and answering it with six
  is the same disease as accepting a spec and then ignoring it: the caller asked for
  something, got something else, and was told it worked.

- **A wide zero-fill was silently truncated.** The first version of the new integer
  renderer assembled the padded number in a fixed 128-byte buffer, so `{:#0200x}` - a
  legal request, since the parser allows a width up to 10000 - produced **127
  characters and returned PROVEN_OK**. The padding is now emitted through the same path
  that already knows how to write N of something without holding N of it.

## [2026-07-12] - proven_c_lib-v26.07.12h

Steps 4-6 of RFC-0003: the keystone. Closes B-007 and B-008.

### Added

- **`proven_writer_t` and `proven_reader_t`** (`include/proven/stream.h`). There was no
  stream abstraction at all. The formatter's only sink was a `proven_u8str_t`; a file
  was a `proven_file_t`; the two scanners read two other things again. Four types, four
  function families, no common interface - so you could not write one
  `serialize(sink, value)` that worked over both memory and a file, and you could not
  format into a file at all.

  Both are small vtables passed by value, modelled on `proven_allocator_t`, with sinks
  over a file, an owned string, and a fixed caller buffer; sources over a file and over
  bytes you already have; and buffered adapters for each.

  **Buffering uses memory you supply**, exactly like `proven_arena_create`. There is no
  hidden global buffer - which means there is no destructor to flush it for you, and
  you must flush before it goes out of scope. In exchange, the logging path never
  allocates, and a program logging its way out of an out-of-memory condition can still
  log.

- **`proven_fmt_to_writer_impl`, `proven_fprint`, `proven_fprintln`** - format straight
  into a writer, through a stack scratch buffer. No allocation.

- **`proven_reader_read_line`.** Reading a file line by line was impossible: the only
  route was loading the whole file and splitting by hand. Two contracts are pinned by
  tests because a naive implementation gets them wrong: the **final line with no
  trailing newline is still returned** (dropping it is how the last record of a file
  goes missing), and a **line too long for the buffer is an error, not a truncated
  line** (a truncated line handed back as if it were whole is a corruption the caller
  cannot detect).

- `tests/test_unit_stream`, and `manual/examples/ex_05_stream.c` - one serializer
  writing into a string, a fixed buffer and a file, then reading it back line by line.
  Compiled and run by the build.

### Fixed

- **`proven_print` no longer allocates.** It built a fresh heap `proven_u8str_t` for
  *every* call: ten thousand log lines meant ten thousand mallocs and ten thousand
  frees, on the logging path - the one place an allocation is least welcome. It now
  formats into a stack buffer and only reaches for the heap if the line will not fit.

  Measured, 10,000 lines: `malloc()` **10,000 -> 0**. A buffered writer over 8 KiB of
  caller memory takes it further: `write()` **10,000 -> 24**, `malloc()` **0**.

  `proven_print` remains one syscall per line by design. Buffering it would require
  hidden global state, which this library does not have; a caller who wants the 24
  builds a buffered writer and says so.

### Changed

- `stream.h` is hosted-only - it sits on `fs`. The freestanding build excludes it.

## [2026-07-12] - proven_c_lib-v26.07.12g

Steps 1-3 of RFC-0003. Subtraction first, then the two things
the library simply could not do.

### Removed

- **The hand-written syscall assembly.** `platform/proven_sys_io.c` implemented read,
  write and seek in inline assembly, one raw-syscall path per architecture: x86_64,
  i386, aarch64, plus an opt-in ARM32 path. It bought nothing - `proven_sys_fs.c` in
  the same library already called libc's `open`, `read`, `write` and `close`, so libc
  was always linked and always doing file I/O.

  What it cost was real. Three of the four paths could not be verified on a machine
  without the cross-toolchains, which is most machines. And because the console path
  issued raw `syscall` instructions, **standard tracing tooling was blind to every one
  of this library's console writes** - an LD_PRELOAD interposer counted zero of
  `proven_println`'s ten thousand. It now counts all of them.

  Removing it changed no behaviour: forcing every branch into the POSIX fallback passed
  12 of 12 I/O tests byte-identically before the change was made.
  `tests/test_portability_source_contracts` now *forbids* the assembly rather than
  requiring it.

### Added

- **`proven_fs_seek`, `proven_fs_tell`, `proven_fs_truncate`, `proven_fs_pread`,
  `proven_fs_pwrite`.** None of these existed. Truncating a file meant reading all of
  it and rewriting the part you kept - an O(n) copy for an O(1) operation.

  A handle that cannot seek - a pipe, a FIFO, a terminal - returns
  `PROVEN_ERR_UNSUPPORTED`, **not** `PROVEN_ERR_IO`. Not being seekable is a property
  of the thing, not a failure of the call, and code that adapts to it has to be able to
  tell them apart.

  `pread` and `pwrite` do not move the file position. That is the whole point: two
  readers sharing a handle cannot race on a cursor that neither of them moves.

- **`proven_fs_sync`, `proven_fs_sync_dir`, `proven_fs_write_file_durable`.** The
  library imported no `fsync` and no `fdatasync` - a caller who wanted their bytes on
  the disk could not ask **at any price**.

  `write_file_durable` does the three steps in the only order that works: fsync the
  temp file, rename it over the target, then fsync the directory. Atomicity and
  durability are different promises, and conflating them is how data gets lost:
  `write_file_atomic` guarantees a reader never sees a half-written file, and says
  nothing about a power cut. Syncing the file but not the *directory* leaves a window
  in which the bytes are safe and the name that points at them is not - which is
  exactly the corruption an atomic write exists to prevent.

  It is slow, and it is meant to be. It waits for the storage device, twice.

- `tests/test_unit_fs_position_and_sync` - covers all of it, including the contracts
  that are easy to get wrong: a FIFO seek is `UNSUPPORTED`, `pread`/`pwrite`/`truncate`
  leave the position alone, growing a file zero-fills, and a durable rewrite still
  preserves the target's permissions and leaves no temp debris.

### Fixed

- `proven_sysio_flush` no longer calls `FlushFileBuffers` on Windows. It did nothing on
  POSIX and forced a full disk sync on Windows: one API, two meanings, neither of them
  what "flush" promised. It now does nothing everywhere, and durability is its own
  explicit call that a caller pays for knowingly.

## [2026-07-12] - proven_c_lib-v26.07.12f

Two audits went looking for weakness in the formatter and the I/O layer. They found
several things that were quietly wrong - fixed here - and one thing that is missing,
which is now designed rather than patched: see RFC-0003.

### Fixed

- **`{:08}` was accepted and silently wrong.** The `0` was eaten as the first digit of
  the width, so `{:08}` on 42 produced `"      42"` - space-padded, eight wide, no
  error - and `{:08x}` produced `"      2a"`. That is the spelling every C, Python and
  Rust programmer reaches for. A near-miss that is accepted and quietly does the wrong
  thing is worse than one that is rejected. A leading zero now means zero-fill; an
  explicit fill still wins.
- **A format spec the argument could not honour was ignored.** `{:x}` on a double
  printed `3.500000`; on a string it printed the string. The request was parsed and
  dropped on the floor, and the call reported success. It is now
  `PROVEN_ERR_INVALID_FORMAT`.
- **`proven_sysio_flush`'s documentation was a lie.** It claimed to flush an internal
  buffer to the OS. There is no buffer: on POSIX it is a single `ret` instruction, and
  on Windows it is `FlushFileBuffers` - a full disk sync. One API, two meanings,
  neither of them the promised one. The header now says exactly that, and says not to
  use it.
- **`proven_arg_f64`'s documentation was wrong twice.** It is not round-half-up (it is
  correctly rounded, ties-to-even, matching `printf("%.6f")`), and the form is not
  always fixed-point (`5e-7` renders as `5.000000e-07`, not `0.000000`).
- Six more false claims found while making the manual's code compile: the pool's
  `item_align` is an upper bound, not an exact match; the panic fallback is `while(1)`
  on non-GCC compilers; the buffered scanner *does* refill and retry rather than
  failing on a token that reaches the end of the buffer; `proven_fs_stat_t.created_at`
  is always 0; `PROVEN_FS_TYPE_OTHER` is never produced; and `src/proven/time.c` *is*
  compiled in freestanding builds (only the clock backend is absent).

### Added

- **RFC-0003** - the design for what is missing, with the
  measurements behind it. The short version: **there is no stream abstraction.** No
  `proven_writer_t`, no `proven_reader_t`. The formatter's only sink is
  `proven_u8str_t`, so you cannot format into a file; there is no line reader, so you
  cannot read a file line by line without loading all of it; and `proven_println`
  issues **10,000 `write()` syscalls and 10,000 mallocs for 10,000 lines** (stdio: 47
  syscalls, 0). The logging path allocates - the one place an allocation is least
  welcome. Seven backlog items (B-004 ... B-010) and a ten-step plan, ordered so that
  each step is useful on its own.
- `tests/test_regression_fmt_spec_silently_wrong` - pins both silent formatter
  defects. Verified to fail against the pre-fix source.
- **The build now compiles every code block in the manual.** `nob` extracts each `c`
  block, wraps it in a function body, and syntax-checks it. A chapter whose code stops
  compiling stops the build.

### Changed

- **Every code block in the manual is now real.** Of ~190 fenced `c` blocks, four could
  be compiled before this cycle. Every one is now either a compiled-and-run program
  from `manual/examples/`, a fragment the build syntax-checks, or a `text` fence for
  the things that are not runnable code (signature listings, struct listings,
  deliberate counter-examples). Closes B-002.

## [2026-07-12] - proven_c_lib-v26.07.12e

### Added

- **Manual chapter 8, sections 7-13** - the scanner half of the chapter, which had
  never been written. The chapter listed thirteen sections and ended at a bare
  `## 7. Scanner data model` heading. Closes **B-001**.

  It was written against *measured* behaviour, not against the header, and that is
  how the surprising parts came to be documented at all:

  - `proven_scan_i64("0x10")` is the integer **zero**, cursor at 1. The integer
    scanners are decimal only - no hex, no octal, no base prefix.
  - `"1e309"` is `PROVEN_ERR_OVERFLOW`, but `"1e-400"` is `PROVEN_OK` with the value
    `0.0`. The asymmetry is deliberate: underflow to zero *is* the correctly rounded
    answer, while overflow has no correct finite answer at all.
  - `proven_scan_u64("-1")` is rejected rather than wrapping to a huge unsigned
    value - which is how a bounds check gets defeated.
  - **The structural scanner is not transactional.** When a literal fails to match,
    the placeholders *before* the mismatch have already been written through: the
    call returns `PROVEN_ERR_NOT_FOUND` and your destination holds a value anyway.
    On failure, treat every destination as clobbered.
  - Trailing input is **not** an error. The scanner matches what you asked for and
    stops; it does not police what you did not ask about.

- `manual/examples/ex_08_scan_recovery.c` - provokes every scan error code on
  purpose, including the non-transactional failure. Compiled and run by the build.
- `tests/test_docs_manual_ch08_contracts` - asserts each of the 18 behaviours
  chapter 8 states as fact. Prose is where a contract goes to drift; this one
  cannot. A false claim fails the build and names itself.

## [2026-07-12] - proven_c_lib-v26.07.12d

The manual's examples are now programs, the tests are named for what they check,
and the testing policy says out loud how this project actually develops.

### Added

- `manual/examples/` - eleven complete programs, one per topic the manual teaches.
  The build driver compiles and **runs** every one of them, under every sanitizer
  mode. They are written the way a caller writes code: explicit allocator, real
  error handling, a destroy for everything owned.
- `tests/test_docs_manual_examples` - requires every example the manual prints to
  be one of those programs, quoted verbatim; fails the build if a chapter and its
  example disagree, if a chapter quotes an example that does not exist, or if an
  example exists that no chapter shows.
- `TESTING.md` - the testing policy: the naming scheme, what each test class
  is *for*, the rules a new test must satisfy, and an honest account of how this
  project develops. It records plainly that this is not TDD: every commit that
  adds a test also changes source in the same commit, and there is not one where a
  failing test lands first.
- the maintainers' backlog - a **tracked** backlog. The repository had `BACKLOGS.md` and
  `TODO.md`, but both are gitignored: a private queue nobody else can read and no
  commit can reference. Known work that lives on one machine is not tracked work.

### Changed

- **Tests are renamed for what they check.** `test_phase1` ... `test_phase22` encoded
  the order they were written in, which is the one fact about a test nobody needs.
  Every test is now `test_<class>_<subject>`, where the class is one of `unit`,
  `contract`, `regression`, `differential`, `portability`, `stress`, `docs`,
  `bench`. 75 files renamed.
- **The test catalog has no numbers.** It ran `1..50` with `7a`, `30a`, `30b`,
  `30c`, `40a` wedged in wherever something new arrived - and five of its entries
  described files deleted months earlier. The filename is the identifier now, and
  the catalog is grouped by class.

### Fixed

- manual chapter 3 listed `proven_u8str_t` without its `borrowed` field, and told
  readers to use `internal.size` for the length. There is no `size` member -
  `proven_buf_t` is `ptr` / `len` / `cap` - so code following the manual did not
  compile.
- manual chapter 5 never said how end-of-file is reported. `proven_fs_read`
  returns `PROVEN_ERR_EOF`, not a zero-byte success, so the obvious read loop
  (`if (r.value == 0) break;`) never takes that branch and treats the end of the
  file as an I/O failure. The chapter now says so, and the worked example shows the
  correct shape.
- manual chapter 5's `proven_fs_stat_t` listing claimed a symlink file type.
  `proven_fs_type_t` has only `_FILE`, `_DIR` and `_OTHER`.

### Known

Two items are registered in the maintainers' backlog rather than rushed:

- **B-001** - manual chapter 8 ends mid-chapter at a bare `## 7. Scanner data
  model` heading. Sections 7-13 are in the table of contents and absent from the
  document: roughly half the chapter, and the half covering the scanner.
- **B-002** - of the manual's ~190 fenced code blocks, four could be compiled
  before this release. Eleven are now real programs; the rest are still sketches
  that reference imaginary helpers. They are being converted chapter by chapter,
  with the mechanism already in place to keep each finished chapter finished.

## [2026-07-12] - proven_c_lib-v26.07.12c

A documentation-currency release, plus the API-surface gap that the sweep turned up.

### Added

- 25 missing `xcv_*` aliases. `include/proven/alias_xcv.h` claims to cover the
  public API and did not: three functions added in v26.07.12b had no alias, and
  22 more had been missing for months - among them `proven_fs_write_all`,
  `proven_panic`, `proven_strtod`, `proven_pool_destroy` and the `sysio` scanner
  entry points. An alias layer that covers most of the API is worse than none:
  the caller finds the gaps one compile error at a time, at whichever call site
  happens to need the one function nobody aliased. The layer now covers all 203
  public functions.
- `tests/test_alias_completeness` - parses the public headers and the alias
  header and fails the build if any public function has no alias. `test_alias_smoke`
  could never have caught this: it hand-picks a subset of aliases and only checks
  that they compile, so it cannot notice one that is absent. This is the only way
  a list like that stays true.
- Two tests that existed on disk but were never registered in `nob.c` -
  `tests/test_float_format_shortest_roundtrip` and `tests/test_float_parse_benchmark` -
  are registered and now actually run. Both pass.

### Fixed

- `proven_array_sort`'s header comment still described it as "a robust quicksort".
  It is an introsort, and the two properties that matter to a caller - the
  O(n log n) guarantee, and duplicate keys being the fast case rather than the
  quadratic one - were documented nowhere a caller would look.
- The allocator's alignment-class contract was undocumented. v26.07.12b made the
  heap allocator route `align <= alignof(max_align_t)` through malloc/realloc (so
  growth can happen in place) and over-aligned requests through the aligned
  family. A block must therefore be reallocated and freed with the alignment it
  was allocated with - a real obligation on callers that existed only as a comment
  in a `.c` file. Now stated in `allocator.h`, `platform/proven_sys_mem.h`, and
  manual chapter 2.
- `docs/float-correctness-and-performance.md` still said the parser used
  `long double` to seed its exponent estimate. It has not since v26.07.12b; the
  whole engine is integer-only now, formatter and parser alike.
- `proven_fs_stat`'s `perms` field is documented as carrying only the nine
  permission bits, and the manual now says why that changed: it used to hand back
  the raw `st_mode`, whose file-type bits `proven_fs_chmod` rejects, so the
  obvious round-trip failed for every real file.
- `TEST.md` claimed 48 hosted tests (there are 75), documented five test files
  that were deleted months ago, and never mentioned five that exist. It now
  matches the tests on disk and the registry in `nob.c`.
- `manual/manual-07-alias-xcv-index.md` was missing 37 aliases and 340 of its 379
  rows had the wrong line number. Regenerated from the header, and the line-number
  column is gone: it was wrong after every alias inserted above it, which is worse
  than not having the column.
- `manual/manual-01-foundation.md` showed a stale `PROVEN_VERSION_NUM` and suffix.
- `CHECKLIST.md` told the maintainer to sync the version string in `SPEC.md` and
  `docs-site/index.html`, neither of which exists, and its "Active Task" was work
  finished several releases ago.
- References to `docs/internal/` now say plainly that it is maintainer-local and
  not part of the published repository, instead of reading as a path the reader
  could follow.

### Changed

- README states where the platform boundary stops - the PAL covers memory,
  filesystem, time, mmap, environment, console I/O and threads, and does *not*
  cover process control, terminal control, or networking - and names the
  deliberate non-goals (hashing, path manipulation, argument parsing, logging).
  A boundary you have to discover by running into it is a worse boundary than one
  that is written down.
- README documents the whole-file I/O added in v26.07.12b and the sort's
  guarantees, in both language halves; the Korean quick start now matches the
  English one.

## [2026-07-12] - proven_c_lib-v26.07.12b

### Fixed

- `proven_array_sort` was quadratic on duplicate keys. The Lomuto partition used
  a strict `cmp(x, pivot) < 0` test, so every element *equal* to the pivot went
  into the right partition; on a low-cardinality key - a status column, an enum,
  a bucket id - the split collapsed to 1/(n-1). Sorting 100,000 identical
  `int32` keys took **10.6 seconds**. A caller sorting data an attacker can shape
  had a denial of service, not merely a slow path. Replaced with an introsort:
  a Bentley-McIlroy three-way partition (so an equal run is final and never
  recursed into, and every element is compared exactly once), an insertion-sort
  cutoff, and a heapsort fallback past a depth of `2*log2(n)` - which is what
  makes the O(n log n) bound a guarantee rather than a hope, since
  median-of-three alone can still be driven quadratic.
- A `proven_job_system` worker could exit leaving an accepted job unrun. A
  submitter that passed `begin_submit` before the close can have claimed its slot
  with the enqueue CAS without yet publishing `cell->sequence`, which to a
  dequeuer is indistinguishable from an empty queue. The last worker could
  therefore exit, the submitter then publish, and `proven_job_submit` return true
  for a job nobody would ever run - while `proven_job_system_destroy`, documented
  to block until the queue is exhausted, returned. A worker now leaves only once
  no submitter is in flight *and* the queue is still empty when re-checked.
- `proven_fs_read_all` allocated twice the file size for every regular file: the
  buffer was seeded to the exact reported size, so the read loop filled it and
  then had to grow before it could issue the read that would observe EOF. Peak
  memory was 3x the file. EOF is now confirmed with a one-byte probe, and the
  buffer grows only if the source really does outrun its reported size.
- `proven_fs_read_all_u8str` started from a one-byte buffer for any source that
  reports no size: the chunk fallback tested the capacity, which is never 0 once
  a terminator byte is reserved. Reading `/proc/self/status` took 12 reallocs.
- `proven_fs_write_file_atomic` widened permissions. The temp sibling is created
  with `0666 & ~umask` and `rename` carries its mode onto the target, so
  atomically rewriting a `0600` key file republished it as `0644`. The target's
  mode is now copied onto the temp before the rename.
- `proven_fs_write_file_atomic` failed on legal long filenames: a 250-character
  basename - which `proven_fs_write_file` accepts - made the temp sibling exceed
  `NAME_MAX`. The copied stem is now trimmed to leave room for the suffix.
- `proven_fs_stat` put the raw `st_mode` into `perms`, a field typed
  `proven_fs_perms_t`. `st_mode` also carries the file-type bits, and
  `proven_fs_chmod` rejects any bit outside the nine it supports - so
  `chmod(path, stat(path).perms)`, the obvious use of the field, returned
  `PROVEN_ERR_INVALID_ARG` for every real file. `perms` is now masked to the low
  nine bits.

### Changed

- The decimal parser's exponent-bounds estimate no longer uses `long double`,
  the one type in C whose width differs across the targets this library builds
  for (80-bit on x86, 128-bit on aarch64, plain 64-bit on armhf and MSVC). The
  estimate happened to come out identical on all of them - verified over the
  entire input range it can see - so nothing was ever wrong, but a
  correctness-critical path had no business depending on it, and on a soft-float
  target it pulled in libgcc routines for no reason. Replaced with an integer
  fixed-point computation, bit-identical to the exact `floor(k * log2(10))` for
  every k in range, and verified to produce byte-identical parse results over
  3,000,000 randomized decimal inputs.
- `proven_fmt` appends literal text in runs instead of one character at a time.
  Each literal character used to cost a checked add, an out-of-line one-byte
  move and a NUL reseal - twice, since the format string is walked once to
  measure and once to write. A 101-character literal went from 1322 ns to 166 ns;
  a four-argument log line from 998 ns to 307 ns.
- `proven_map_set` no longer probes the same chain twice. It validated the map,
  then `set_with_scratch` validated it again, then walked the probe chain looking
  for an existing key, then called `map_insert_no_grow` - which walks the same
  chain and already overwrites a key it finds. The probe is now taken only when
  the map is about to grow, where it still saves an unnecessary rehash. 500k
  int inserts: 215.5 ns -> 190.2 ns per op.
- Sorting wide elements is faster: `swap_elements` takes a bulk-copy branch above
  16 bytes instead of swapping a byte at a time. 100k 48-byte structs:
  59.2 ms -> 16.2 ms.

## [2026-07-12] - proven_c_lib-v26.07.12a

### Fixed

- `proven_fs_read_all` silently returned an empty buffer for any source whose
  size cannot be measured up front. `proven_sys_fs_size` reports 0 for anything
  that is not a regular file, and `read_all` used that 0 as its buffer size, so
  reading a FIFO, a character device, or a `/proc` entry succeeded with zero
  bytes and dropped the contents. `proven_fs_size("/proc/self/status")` is
  `0`/`PROVEN_OK`, so a 1516-byte file read as empty. `read_all` now reads to
  EOF and uses the reported size only to seed the initial capacity: a regular
  file is still one allocation and one pass, an unmeasurable source is read
  correctly, and a regular file that grows mid-read is no longer truncated.
- Stack buffer overflow formatting a `proven_datetime_t` with a negative year.
  `year` is `proven_i32`, but it was cast to `unsigned long long` before
  conversion, so `-1` became `18446744073709551615` - twenty digits plus a NUL
  into a twenty-byte scratch buffer (ASan: stack-buffer-overflow in `itoa_raw`).
  The year now renders with its sign, and the scratch holds any 64-bit value.
- `proven_sysio_scanner_scan_impl` corrupted the stream when it rolled back a
  failed scan. `scanner_fill` compacts the buffer, but the rollback restored the
  cursor and length captured *before* that compaction, so the restored indices
  described different bytes: one byte was dropped from the front of the stream
  and one byte - already returned to the file by the rewind - was read twice.
  The rollback now accounts for how far the buffer moved. On a non-seekable
  input, where the rewind cannot succeed, the bytes already read are kept
  buffered instead of being discarded.
- `proven_u8str_reserve` and the growth path of the formatter left `ptr[len]`
  uninitialized. Both allocate, and allocators do not return zeroed memory, so
  reserving on a zero-initialized string - or formatting something that produces
  no output - broke the NUL seal that `proven_u8str_as_cstr` is documented to
  rely on, and `proven_u8str_is_valid` rejected the result. Both paths now seal
  the terminator.
- `proven_pool_init` published `bin_cap` before allocating the bin behind it, so
  a failed init left a pool claiming slots it did not have. The free trait tests
  `bin_len < bin_cap` and then writes `bin[bin_len]`, which with `bin == NULL` is
  a null write. `bin_cap` is now set only after the bin exists.
- Unchecked `count * size` arithmetic in `proven_sysio_scanner_scan_impl`, a
  public entry point that takes `args_count` from the caller. It is now routed
  through `PROVEN_CKD_MUL` like every other size computation in the library.

### Added

- `proven_fs_read_all_u8str`: the whole-file read most callers actually want,
  returning a NUL-terminated owned `proven_u8str_t` so `proven_u8str_as_view` and
  `proven_u8str_as_cstr` work on the result without a second copy. The terminator
  slot is reserved up front, so it costs no extra allocation over `read_all`.
- `proven_fs_write_file`: one-call create-or-truncate whole-file write, the half
  of the API that was missing next to `read_all`.
- `proven_fs_write_file_atomic`: writes through a sibling temp file and renames
  it over the target, so a concurrent reader never observes a half-written file.
  Atomic with respect to readers, not durable across power loss - proven exposes
  no fsync, and the header says so.
- `[[nodiscard]]` on `proven_sysio_scanner_scan_impl` and
  `proven_sysio_scan_chunk_impl`. `proven_sysio_print_impl` is deliberately left
  unannotated: `proven_print` expands to it and is used as `printf` is.

### Changed

- `proven_sys_mem_realloc` can now grow a block in place. Every allocation used
  to go through `posix_memalign` / `_aligned_malloc`, which cannot be handed to
  `realloc()`, so growth always paid a full copy. Requests at or below
  `alignof(max_align_t)` - every string, buffer, and byte array in the library --
  now come from `malloc` and grow through `realloc`, which for large blocks
  remaps pages instead of copying them. Over-aligned requests keep the aligned
  path. Windows keeps every block on the aligned family (`free` and
  `_aligned_free` are not interchangeable, and the free trait is not told the
  alignment) and uses `_aligned_realloc`. Failure atomicity is unchanged.
  Measured, with every byte of the buffer written: growing a buffer to 256 MiB by
  doubling went from 0.69s to 0.32s (2.1x); 200k small allocations with six
  reallocs each went from 0.05s to 0.035s (1.4x).

## [2026-06-24] - proven_c_lib-v26.06.24b

### Fixed

- Build break on older GCC under the `-std=c2x` fallback: `src/proven/job.c`
  used `alignof` without `<stdalign.h>`. `alignof` is a first-class keyword only
  in C23; under the documented `-std=c2x` fallback (and C11/C17) it is a macro
  that `<stdalign.h>` provides. On a compiler new enough to keyword-ify `alignof`
  under c2x (e.g. GCC 14) it built anyway, which is why builds on newer toolchains
  did not catch it; on an older GCC the c2x fallback failed to compile at
  `job.c:111`, taking down the whole hosted build. Reported by an external tester
  whose default GCC fell back to c2x (their clang regression / ASan / UBSan /
  freestanding runs passed).
- Fix is centralized: `<stdalign.h>` is now included from `include/proven/types.h`,
  the foundation header every translation unit pulls in, so all `alignof`/`alignas`
  users are covered regardless of their own include list. This also closes the same
  latent gap in `fmt.c`, `pool.c`, and `sysio.c` (which previously relied on
  transitive includes). Verified: `alignof` via `proven/types.h` compiles even
  under `-std=c11` (where it is not a keyword); full gcc build, `strict-error`, and
  `freestanding` gates pass.

## [2026-06-24] - proven_c_lib-v26.06.24a

### Changed

- Documentation release (no library code changes). Brought the manuals and README
  current with v26.06.22a and added deep-dive sections to the chapters:
  - `manual/manual-04`: how the hash map works internally (bucket layout,
    FNV-1a / bit-mix hashing, linear probing, tombstones, the 3/4 load factor and
    rehash, the three key modes, and the `set_with_scratch` alias case).
  - `manual/manual-06`: the job system's concurrency model (atomic MPMC ring,
    lifecycle state machine, memory-visibility via the destroy/join sync point) and
    the stackless-coroutine expansion with the "locals do not survive a yield" rule.
  - `manual/manual-02`: how the pool's recycle bin works, with misuse cases.
  - `manual/manual-08`: an "Inside the engine" section for the float parse tiers
    (Clinger / Eisel-Lemire / exact big-integer) and the two formatters (Grisu3 +
    Dragon4 shortest, exact integer `%f`/`%e`).
  - `manual/manual-03`: the `proven_u8str_t` internal layout (`proven_buf_t internal`
    + `borrowed`) with a borrowed-string counter-example.
  - `manual/manual-05`: `proven_fs_stat_t` now documents `uid`/`gid`.
  - `manual/manual.md`: corrected the arena ownership row (caller-backed bump
    pointer, not an owner); header map adds `config.h`, `float_parse.h`,
    `float_format.h`, `float_config.h`.
- Moved the internal-only docs (`docs/internal/`: benchmarks, RFC drafts, overhaul
  plans) out of the repository into the private workspace and gitignored the path.

## [2026-06-22] - proven_c_lib-v26.06.22a

### Added

- `proven_fs_stat` now exposes file ownership: `proven_fs_stat_t` gains
  `unsigned long long uid` and `gid`, populated from `st_uid` / `st_gid` on
  POSIX and set to `0` on Windows (which has no POSIX ownership). The sys-level
  `proven_sys_fs_stat_t` carries the same two fields. Resolves the prov_text_editor
  enhancement request (REPORT.md, 2026-06-19) that blocked the file browser's
  owner/group columns. Verified in `tests/test_phase14_fs_advanced.c` (uid/gid
  equal `getuid()`/`getgid()` for a just-created file on POSIX).

## [2026-06-21] - proven_c_lib-v26.06.21a

### Fixed

- `map.c`: silenced a `-Wunused-parameter` warning on the `map` argument of
  `map_key_is_valid`. Its only use is the hardened overlap check, which is
  compiled out on `-DNDEBUG` non-hardened builds, so downstream release builds
  (`-Wall -Wextra -DNDEBUG`) saw the warning. Added `(void)map;`. Reported via
  `REPORT.md`.

### Changed

- Synced the version string to `proven_c_lib-v26.06.21a` across
  `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/`
  chapters. Also corrected the `manual-01` version-macro example, whose
  `STRING`/`NUM`/`SUFFIX` lines had drifted out of sync with each other.

## [2026-06-18] - proven_c_lib-v26.06.18b

### Added

- `proven_mem_move(dst, dst_cap, src_view)` (`memory.h`): a bounded,
  overlap-safe byte move with the same guards as `proven_mem_copy` (overflow ->
  `PROVEN_ERR_OUT_OF_BOUNDS` without writing, null with size -> `INVALID_ARG`,
  zero size -> no-op). Lets downstream code drop libc `memmove` for overlapping
  array-element shifts. XCV alias `xcv_mem_move`.

## [2026-06-18] - proven_c_lib-v26.06.18a

### Added

- `proven_u8str_borrow(buf, cap)` and `proven_u8str_reset(str)` (`u8str.h`):
  wrap caller-owned memory as a fixed-capacity string and truncate-to-empty for
  reuse. A new `borrowed` flag on `proven_u8str_t` defaults to owned, so a
  zero-initialized handle keeps its existing semantics. The fixed-capacity
  operations and `proven_u8str_append_fmt` work on a borrowed string; the
  growing operations (`reserve`, `*_grow`, `append_byte`, `append_fmt_grow`)
  still succeed while the data fits but return `PROVEN_ERR_OUT_OF_BOUNDS`
  instead of reallocating caller memory, and `proven_u8str_destroy` is a no-op
  for a borrowed string. This lets allocator-free and per-frame call sites use
  the proven string system / formatter without heap allocation. Requested by a
  downstream project (`REPORT.md`, 2026-06-18).
- `proven_mem_copy(dst, dst_cap, src_view)` (`memory.h`): a bounded byte copy
  that rejects overflow without writing, treats a zero-size source as a no-op,
  and rejects null pointers.
- XCV aliases `xcv_u8str_borrow`, `xcv_u8str_reset`, `xcv_mem_copy`.

### Changed

- `proven_u8str_t` gains a trailing `bool borrowed` field. `proven_buf_t`
  layout is unchanged. No public API consumes `sizeof(proven_u8str_t)` by
  contract; source compatibility holds after a recompile.

### Fixed

- `proven_diy_fp_normalize` (`float_decimal.c`) left-shifted a 64-bit value by
  the type width when the significand was zero (`clz` returns 64), which is
  undefined behavior surfaced by UBSan. Guard the zero case to mirror the
  previously-masked result (significand stays 0) without the UB; no change on
  any non-zero input, so formatter output is unchanged.

## [2026-06-17] - proven_c_lib-v26.06.17a

### Fixed

- Made `src/proven/float_decimal.c` compile on hosted targets without 128-bit
  integers (e.g. 32-bit ARM / `linux-armhf-hosted`). The Eisel-Lemire fast path
  is `__int128`-only, but its guard boundaries were inconsistent: helper calls
  sat outside the guard while definitions sat inside (and vice versa), so the
  cross matrix's new Windows link smoke exposed it via `arm-linux-gnueabihf-gcc`
  failures (implicit declarations, used-but-undefined, and unused-function
  `-Werror`). Moved the int128-free `proven_float_pack_binary64_candidate` out
  of the guard, added `#else` stubs for the two Eisel-Lemire entry helpers so
  the unconditional dispatcher links and reports "unsupported" (falling back to
  the scalar exact path, which already had a non-int128 multiply), and marked
  the Eisel-Lemire-only helpers `[[maybe_unused]]`. No behavior change on
  targets with `__int128` (x86-64 output is unchanged). `./nob cross` now passes
  every target, including both Windows link smokes.

## [2026-06-16] - proven_c_lib-v26.06.16x

### Fixed

- Made the panic handler link on Windows / PE-COFF (mingw-w64). The previous
  weakly-linked `proven_panic_handler` default linked on ELF but not on PE: a
  weak function definition in a separate object did not satisfy references,
  producing `undefined reference to proven_panic_handler` on every Windows link
  (the cross matrix is compile-only, so this was latent). Reported in
  `REPORT.md`.

### Added

- Cross matrix link smoke for the Windows targets (`./nob cross`): the
  `windows-*` targets now link the full proven object set into an executable
  (`tests/test_cross_link_smoke.c`) instead of compiling only, so link-time
  symbol-resolution differences from ELF (such as the PE/COFF weak-symbol issue
  above) are caught instead of slipping through.

### Changed

- Replaced the weak-symbol panic override with a portable registration model:
  the library now raises panics via `proven_panic()` and installs handlers via
  `proven_set_panic_handler(proven_panic_handler_t)` (pass `NULL` to restore the
  trapping default). **Breaking:** defining a strong `proven_panic_handler` no
  longer overrides the handler; call `proven_set_panic_handler()` instead.
  Updated call sites (`pool.c`, `arena.h`), the override tests, and the panic
  documentation in `manual/` and `TEST.md`.
- Bumped the version to `proven_c_lib-v26.06.16x` and synced the version string
  across `include/proven/version.h`, `README.md`, `TEST.md`, and `manual/`.

## [2026-06-16] - proven_c_lib-v26.06.16w

### Changed

- Bumped the version to `proven_c_lib-v26.06.16w`, releasing the editor-oriented `proven_u8str` work: the growing in-place edit variants and the multi-algorithm substring search. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.

### Added

- Added growing variants of the in-place string edits: `proven_u8str_insert_grow` and `proven_u8str_replace_at_grow` (plus `xcv_` aliases). They have the same semantics as `proven_u8str_insert` / `proven_u8str_replace_at` but grow the buffer (doubling capacity) when the edit does not fit instead of returning `PROVEN_ERR_OUT_OF_BOUNDS`, so callers (for example a text editor making mid-buffer edits) no longer have to `reserve` manually before every insert. On allocation failure the string is left unchanged. New unit coverage in `tests/test_phase7_u8str_mut`.
- Added `proven_sys_mem_chr` to the platform memory layer: the system `memchr` when hosted, a freestanding-safe SWAR (word-at-a-time) scan otherwise. Verified against `memchr` over 2,000,000 randomized cases.

### Changed

- Rewrote `proven_u8str_view_find` from a naive O(n*m) byte loop to a multi-algorithm search that is self-contained (does not rely exclusively on `memchr`) and behaves identically under freestanding. The fast path samples the haystack, anchors on the rarest needle byte, scans with `proven_sys_mem_chr`, and verifies - fast on real text because a typical needle has a rare or absent byte. When the sample shows a low-entropy haystack (small effective alphabet: DNA, binary, long runs), it falls back to a linear, alphabet-independent algorithm: **Shift-Or / bitap** for needles up to 64 bytes, and **Two-Way (Crochemore-Perrin)** for longer needles. The long-needle fallback is compile-time selectable via `PROVEN_U8STR_FIND_LONG` (1 = Two-Way default, 2 = memchr-adaptive); Two-Way was chosen by benchmark (0.97x vs glibc `memmem` on a long-needle/long-verify input where memchr-adaptive is 3.1x). On realistic text the search is 4-30x faster than glibc `memmem`; single-byte search equals `memchr`; low-entropy cases stay at or below `memmem`. Validated for first-match equivalence against host `memmem` over 3,000,000 cases per alphabet (2/4/26 symbols, needles 0-139 bytes) for the dispatch and each forced algorithm, with zero mismatches; ASan/UBSan clean. Benchmark: `docs/internal/benchmarks/20260616-152810-u8str-find-multi-algorithm.md`.

## [2026-06-16] - proven_c_lib-v26.06.16v

### Changed

- Bumped the version to `proven_c_lib-v26.06.16v` (`PROVEN_VERSION_NUM` 260616), releasing the exact/fast floating-point parser and formatter work, the exhaustive and large-scale validation, and the documentation overhaul. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.
- Documentation reorganization and refresh. Moved the dated benchmark reports and the design proposals/RFC audits under `docs/internal/` (with a `docs/internal/README.md` describing the folder), since they are development records rather than user docs; `docs/float-correctness-and-performance.md` remains the user-facing summary. Updated the floating-point section of `manual/manual-08-fmt-scan.md` to describe the current exact, correctly-rounded (round-half-to-even) formatter and three-tier parser with worked examples (the old text still described an approximate six-digit/round-half-up formatter). Rewrote the relevant parts of `README.md` (both language halves) to add a "correct, fast number conversion" section with example code and objective validation/benchmark numbers, list the `float_parse`/`float_format` modules, and fix the documentation index, which pointed at files that are not part of the published repository (`SPEC.md`, `AGENTS.md`, `MEMORY.md`).

- Replaced the shortest float formatter. First with a single-pass exact algorithm (Burger-Dybvig / Dragon4, round-to-nearest-ties-to-even) for binary64 and binary32, then with a Grisu3 fast path (64-bit diy_fp + a generated cached-power table) that falls back to the exact path only when it cannot prove the result is shortest. Net result is about 670x faster than the original round-trip-search formatter (59,018 -> 88 ns/call on a mixed corpus) and uniform across magnitudes, with the same correctly-rounded minimal output (validated round-trip and minimality over ~3M doubles and ~5M floats).

### Removed

- Removed the obsolete hand-maintained shortest literal table (`proven_float_shortest_literal_f64`/`_f32` and their tables) now that the shortest formatter computes every value directly, along with the structural tests that pinned the old staged round-trip-search backend.
- Removed the dead round-trip-search fixed-precision machinery in `float_format.c` (`proven_float_format_roundtrip_search_fixed`, `candidate_exact`, `candidate_roundtrips`, `roundtrips_f64`/`_f32`, `adjust_fixed_neighbor`, `build_scientific_ld`, `normalize_scientific_ld`). It was reachable only through the `RYU` policy in `FIXED` mode, which no caller or test used; that combination now routes to the same exact integer path as the other policies. This removes the formatter's last `long double` use, so `float_format.c` is now entirely integer-based. Behavior of the exercised paths is unchanged (re-validated against host `snprintf` over ~3M doubles at precisions 1..18, zero mismatches).

### Fixed

- Canonicalized shortest float output for values just below a power of ten. The digit generators (both Grisu3 and Dragon4) could leave a spurious leading zero with the decimal exponent one too high - e.g. `9.995442674871462e-265` was emitted as `0.9995442674871462e-264` - which round-tripped correctly but was non-canonical and inflated the reported significant-digit count by one. `proven_float_shortest_digits`/`_f32` now strip leading zeros and lower the decimal exponent. Found by a 2.56-billion-value `binary64` differential check against host `strtod` (93 affected values, all near a power of ten); the value, round-trip property, and minimal length are unchanged. The exhaustive `binary32` sweep was unaffected (it had no such cases) and still passes.
- Made fixed-precision float formatting (`%f`/`%e` via `proven_float_format_f64_policy` and the `{}` formatter) exact and correctly rounded. The previous path used `double`/`long double` arithmetic capped at 18 fractional digits and produced wrong digits for high precision, values at or above 2^64, subnormals, and boundary cases; a differential check against host `snprintf` went from roughly 20% mismatches to zero across 4,000,000 value/precision pairs. The new path is integer-only (no long double), correctly rounds to nearest-even, and supports arbitrary precision up to the big-integer capacity.
- Fixed an out-of-bounds read in `proven_float_bigint_cmp_shift_left` when the shift was a multiple of 64: the low zero-padding limbs were not compared and the index underflowed. This helper is shared with the decimal parser's exact comparison; the parser's differential fuzz remains at zero mismatches after the fix.
- Corrected the shortest float formatter to emit the true minimal round-tripping form. The shortest search now generates candidates with the exact digit engine and no longer consults the hand-maintained literal table, several of whose entries were non-minimal (for example the largest subnormal and `FLT_MIN`).

### Added

- Added `docs/float-correctness-and-performance.md`, a self-contained reference describing the parsing/formatting algorithms (three-tier Clinger / Eisel-Lemire / exact-fallback parser; Grisu3 + Dragon4 shortest; exact big-integer `%f`/`%e`), the validation methodology, and the performance comparison against the host C library. Backed by an **exhaustive** sweep of all 4,278,190,080 finite `binary32` values (shortest round-trip + minimality via host `strtof`, parser bit-exact vs host `strtod`) and a **2,560,000,000-value** randomized `binary64` differential sweep against host `strtod`, both with **zero** failures, plus a host-comparison benchmark. Dated raw outputs live under `docs/internal/benchmarks/` (`*-f32-exhaustive-validation.md`, `*-float-vs-host-benchmark.md`). The benchmark shows the library is faster than glibc at parsing typical numbers, shortest formatting (~4-5x), and `%f`/`%e` at normal magnitudes, while staying bit-identical to `strtod`/`snprintf`.
- Added a big-integer division helper (`proven_float_bigint_divmod`, Knuth Algorithm D on base-2^32 limbs, no `__int128`, freestanding-safe) with a limb-array entry point `proven_float_bigint_divmod_u64` and a unit test (`tests/test_float_bigint_divmod`). It is a validated reusable primitive; a measured experiment showed that using it to compute the exact-fallback float result is slower than the existing estimate-seeded search, so the parser keeps the search and the division is not on the parse hot path.
- Made the exact-fallback big-integer capacity configurable through `PROVEN_FLOAT_BIGINT_LIMBS` (in `include/proven/float_config.h`, default 160). Lowering it shrinks the exact-fallback and division stack footprint for embedded targets (for example `-DPROVEN_FLOAT_BIGINT_LIMBS=48` cuts the division frame from ~10.5 KB to ~3.3 KB and the converter from ~6.7 KB to ~2.2 KB). The kept-significand cap is derived from the capacity, so reduced builds still parse correctly up to that many significant digits and stay within one ULP beyond it; the Clinger and Eisel-Lemire fast paths never use the big integer. The build driver now also forwards `-cflags` to test compilation so such config macros stay consistent between library objects and tests.

### Changed

- Made the float parser treat a dangling exponent marker the way `strtod` does. A trailing `e`, `e+`, or `e-` that is not followed by exponent digits (for example `1e`, `1e+`, `1.5eZ`) is no longer rejected; the parser keeps the mantissa parsed so far and stops at the `e`. This affects `proven_parse_double_ascii`, `proven_strtod`, and `proven_scan_f64`, which share the ASCII token scanner.

### Fixed

- Removed the hard limit that rejected decimal inputs with more significant digits than the exact-fallback bigint could hold (about 3080 digits), which previously returned `0` and consumed nothing. The exact significand is now capped at a fixed number of kept digits (800, above the 767-digit worst case for binary64 rounding); digits past the cap shift `exp10` and set a sticky flag that breaks an exact-equal comparison upward, so arbitrarily long inputs parse correctly in bounded time. Validated against host `strtod` with a 300,000-case fuzz over 700-3099 digit mantissas and targeted midpoint sticky tie-break cases.
- Corrected `significant_digits` in the decimal metadata so it no longer counts trailing zeros that are already folded into `exp10`. The inflated count biased the magnitude estimate and the derived binary-exponent search bounds high, which could place the true result outside the exact-search range and yield a power-of-two result for some long-mantissa inputs (for example `12345678901234567890` and `109.31074080952665007690591502623020`). A 5,000,000-case randomized differential check against host `strtod` now reports zero mismatches.

### Changed

- Seeded the exact-fallback binary search from a cheap reconstructed double estimate, narrowing the search to a verified window around it before falling back to the full exponent-bracket range. The window is only adopted after a two-point bracket check, so the rounded result is unchanged while the `fallback` and `boundary_tie` benchmark groups drop by roughly one half.

### Removed

- Removed four unused static float helpers (`proven_float_bigint_add`, `proven_float_compare_mantissa_to_scaled`, `proven_float_compare_decimal_to_bits_legacy`, `proven_float_compare_decimal_to_midpoint_legacy`) left over from the decimal-to-binary64 rewrite; they were dead code and tripped the `-Werror` strict and freestanding builds.

### Changed

- Reverted the staged positive-exponent scalar helper experiment after benchmark runs showed it regressed the path corpus, restoring the prior baseline for `staged_scientific`, `fallback`, and `boundary_tie`.
- Optimized the legacy negative-exponent exact compare path to build a cached pow5 factor once before comparing, which shaved a little more off `fallback` and `boundary_tie`.
- Kept the cached pow5 factor reuse on the legacy negative-exponent compare path after the follow-up compare-path tweak showed mixed results; the retained change avoids repeated `5` multiplication while leaving the public parser behavior unchanged.
- Added an equal-exponent fast path in the adjacent-midpoint helper so the exact boundary compare can skip two generic shifts when both sides already share the same exponent.
- Reworked the hot `proven_float_bigint_mul_u64_factor()` carry step to use a plain low-limb add-and-carry extraction, which remains the retained optimization in the latest benchmarked state after the latest path benchmark run.

### Added

- Added `proven_parse_f64_ascii()` and `proven_strtod()` as public float-parse entry points over the shared decimal-to-binary64 backend.
- Added internal float-parse path counters so tests can distinguish Clinger hits, staged Eisel-Lemire hits, and exact bigint fallback hits.
- Added `THIRD_PARTY_NOTICES.md` to record the clean-room status of the decimal-to-binary64 parser rewrite.
- Added `scripts/generate_float_decimal_tables.py` and a generated cached-`5^q` header so the current fast path no longer depends on hand-maintained power tables.
- Added an opt-in `bench-float` build-driver command plus a dated `2026-06-13-float-parse-benchmark.md` report comparing `proven_parse_double_ascii`, `proven_strtod`, and host `strtod` on a representative decimal corpus.
- Added a dated `2026-06-13-float-parse-path-matrix.md` guide that breaks the float parse workload into Clinger, staged cached-power, exact fallback, wrapper, and host-reference paths.
- Added a dated `2026-06-13-float-parse-path-benchmark.md` report that splits the float parse workload into short-exact, staged-scientific, fallback, and boundary-tie corpora.
- Added a timestamped `2026-06-12-194411-float-parse-path-benchmark.md` report capturing a fresh path benchmark run against host `strtod`.
- Added a timestamped `2026-06-12-192443-float-parse-path-benchmark.md` report capturing the updated path benchmark after the fast-path significand handling fix.
- Adjusted fast-path significand preparation so the staged Eisel-Lemire validation can keep its representative scientific inputs on the staged path without regressing the Clinger-only case.
- Reduced exact-fallback comparison cost by caching the shared `5^q` state across fallback midpoint checks, which cuts the fallback-heavy and boundary-tie benchmark groups materially without changing the public parser API.
- Deferred exact-bigint construction until the parser actually falls back, and switched staged Eisel-Lemire validation to the lightweight mantissa/exponent representation, which pulled the `staged_scientific` benchmark back into the sub-microsecond band.
- Reused generated cached `5^q` tables when preparing exact-compare state, which removed a large repeated-multiply cost from the remaining exact and staged validation paths.
- Reverted a pow5-state-hoist experiment after benchmark runs showed slower `boundary_tie` corpora, restoring the prior wrapper-owned validation state path.
- Collapsed the decimal metadata build into a single input scan instead of scanning the same token twice before fallback or staged validation.
- Replaced the cached-factor bigint multiply loop with a single schoolbook pass, which cut the remaining fallback and boundary-tie exact-compare cost substantially without changing parse results.
- Built adjacent-float midpoints directly from raw mantissa/exponent words instead of through two bigint shifts and an add, which trimmed the remaining boundary-tie validation cost.
- Specialized the common 1-limb cached-factor multiply case, which shaved more time off the remaining fallback-heavy and boundary-tie paths.
- Fixed exact-compare state prep so negative exponents cache the reciprocal pow5 factor again instead of dropping to the legacy multiply loop, which restored the intended cached-factor path in the fallback-heavy cases.
- Fixed `proven_float_bigint_copy_mul_factor()` so the 1-limb fast path copies the source into the working product before multiplying, keeping the negative-exponent exact path correct.
- Added a fused 1-limb copy-multiply loop so the negative-exponent exact path no longer does a separate copy pass before multiplying small cached factors.
- Simplified the fused 1-limb copy-multiply loop to a single carry scan, then benchmarked a direct 128-bit variant and reverted it after it regressed `fallback` and `boundary_tie`; the single carry scan remains the retained baseline.
- Verified the retained fused 1-limb copy-multiply loop on the path benchmark: `short_exact` and `staged_scientific` improved again, while the slower groups stayed at the retained baseline after the reverted experiment.
- Specialized the negative-exponent exact compare path for 1-limb rhs operands, which lowered `fallback` and `boundary_tie` again while keeping `short_exact` and `staged_scientific` near the same band.
- Re-ran the path benchmark on that compare-site specialization and kept the improvement: `fallback` and `boundary_tie` stayed materially faster while the short/staged corpora stayed close to the prior run.
- Applied the same 1-limb rhs specialization to staged validation, which nudged `staged_scientific` down while keeping the fallback-heavy corpora improved.
- Folded the 1-limb rhs multiply into a shared helper used by both the staged and exact negative-exponent compare sites, which kept the benchmark gains while reducing duplicated path logic.
- Added a 2-limb copy-multiply specialization for the negative-exponent exact compare path, which trimmed the fallback-heavy and boundary-tie benchmark groups again.
- Added a tiny-limb cached-factor multiply path for the remaining exact-comparison cases, which kept the fallback path moving while preserving corpus agreement.
- Avoided full 160-limb bigint zeroing when preparing hot compare operands, which materially reduced the cost of the remaining exact fallback and boundary-tie paths.
- Narrowed bigint copies to the live limb prefix instead of copying the full 160-limb backing store, which cut more memory traffic from the remaining exact compare path.
- Removed the last full-buffer zeroing from `proven_float_bigint_mul_factor()` by clearing only the live output prefix, which kept the remaining exact compare work moving down.
- Switched cached pow5 setup and the hot exact-compare bigint constructors to skip unnecessary full-buffer zeroing, which simplified the preparation path without changing parse results.
- Removed the extra bigint copy from the exact compare shift path so only the shifted operand is materialized in the common fallback and boundary-tie validation cases.
- Specialized hot bigint copies so 1- and 2-limb operands use direct assignments instead of a generic limb loop, which pulled the exact fallback and boundary-tie paths down again.
- Kept the hot bigint copy fast path specialized through four limbs and collapsed copy-plus-factor multiplication into one helper, which shaved more time off the exact compare-heavy benchmark groups.
- Re-ran the path benchmark after the copy-plus-factor helper change and confirmed the exact-compare-heavy corpora still match host `strtod`; a later small-rhs shortcut was reverted after it regressed `fallback` and `boundary_tie`, restoring the better numbers.
- Split the exact shifted-compare loop to handle the lowest shifted limb separately from the main walk, which shaved another small amount of work off the fallback-heavy corpora without changing results.
- Turned the remaining lower zero-prefix check in the exact shifted-compare loop into a branchless accumulation, which cut a little more overhead out of the fallback-heavy corpora.
- Removed an unnecessary full-biginteger zeroing from decimal token setup, which cut a large amount of common-path overhead out of the short-exact, staged, fallback, and boundary corpora.
- Delayed the legacy exact-compare RHS copy until the compare actually needs it, which removed another avoidable copy from the positive-exponent compare cases.
- Replaced the bigint limb-shift loop with `memmove`/`memset` in the exact compare path, which shaved more time off the fallback-heavy and boundary-tie corpora.
- Unrolled the common 1- and 2-limb shift case in the bigint exact-compare path, which cut another slice of overhead out of the fallback-heavy and boundary-tie corpora.
- Compared shifted bigints directly without materializing a shifted copy in the exact compare path, which removed another avoidable pass from the fallback-heavy and boundary-tie corpora.
- Rewrote the direct shift-compare loop to compute shifted limbs inline instead of routing through a per-limb helper, which shaved more time off `fallback` while keeping checksum agreement intact.
- Split the shifted-biginteger compare loop into separate shift and no-shift regions, which cut a little more branch noise out of the fallback-heavy path without changing results.
- Reworked `proven_strtod()` to reuse the parser's nonzero-digit metadata instead of rescanning the parsed token for underflow bookkeeping, which trimmed wrapper overhead without changing parse results.
- Switched the hosted `proven_strtod()` input-length scan to `strlen()`, which shaved a little more off the wrapper path in the hosted benchmark run.
- Reused the parser's sign flag for overflow fallback selection instead of re-reading the first token byte, which cleaned up the rare overflow path without changing parse results.
- Split the exact compare loop's top shifted limb into a separate check before the main walk, which removed a small amount of work from the fallback-heavy corpora without changing rounding.
- Removed unused exponent accumulation from token validation, which drops one redundant scan over exponent digits before the parser reaches the exact backend.
- Removed a dead zero-case branch from the decimal builder, which keeps the zero path simple without changing parser behavior.
- Skipped full 160-limb zeroing when rebuilding the exact significand, which cuts unnecessary memory traffic from the fallback path.
- Restored the safe direct shifted-compare helper after a copy-eliding simplification caused a `1e100` benchmark mismatch, keeping the benchmark corpus aligned with host `strtod`.
- Switched bigint zeroing and prefix clearing to `memset` and skipped unnecessary full-buffer zeroing in bigint constructors, which kept the fallback-heavy exact path moving without changing the parser API.
- Reverted a regressing midpoint-fast-path experiment in the exact compare helper after benchmark runs showed slower `fallback` and `boundary_tie` corpora.
- Restored the small-compare helper's cached-power overflow checks after the midpoint experiment, which improved the fallback-heavy and boundary-tie corpora again while keeping the benchmark corpus checksum-stable.

### Fixed

- `proven_sysio_scanner_deinit()` now clears the full scanner state after releasing the buffer.
- `proven_map_is_valid()` now checks the public `internal.size` against the bucket layout.
- `proven_fs_open()` now rejects unsupported mode bits before reaching the PAL layer.
- `proven_fs_open()` now rejects truncation requests that do not carry write intent.
- `proven_fs_lock()` now rejects unsupported lock modes instead of treating them as unlock.
- `proven_fs_chmod()` now rejects unsupported permission bits before reaching the PAL layer.
- `proven_float_format_f64_policy()` now rejects out-of-range fixed-mode precision values with `INVALID_ARG`.
- `proven_float_format_f64_policy()` now keeps tiny finite subnormals on the shortest formatting path instead of falling back to `UNSUPPORTED`.
- `proven_scan_f64()` now routes decimal parsing through a shared exact backend that tokenizes ASCII input once, compares candidate midpoints with bigint arithmetic, and preserves correct rounding at normal, subnormal, and overflow boundaries without host `strtod`.
- The staged `proven_float_try_eisel_lemire()` layer now handles a conservative exact negative-exponent subset in addition to the generated-`5^q` positive-exponent subset.
- The staged `proven_float_try_eisel_lemire()` layer now also uses `__uint128_t` to round a bounded negative-exponent ratio subset for normal-range candidates.
- The staged `proven_float_try_eisel_lemire()` layer now accepts wide-shift normal-range negative exponent ratios such as `1e-27` instead of forcing them into the bigint fallback.
- The staged `proven_float_try_eisel_lemire()` layer now validates candidate bits against exact midpoint comparisons before accepting them, which lets tie-to-even zero-exponent integers such as `9007199254740993` stay on the staged fast path without reintroducing one-ULP regressions.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated `u128` power-of-5 table for wider exact positive-exponent subsets such as `1e40` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now also uses the generated `u128` power-of-5 table for wider negative-exponent ratio subsets such as `1e-30` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now lets the `u256` negative-ratio scaler keep deeper wide-shift cases such as `1e-40` on the staged fast path before exact fallback.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated reciprocal `5^-q` cache for wider negative-exponent candidates such as `1e-100` before falling back to the exact bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated scaled `5^q` cache for wider positive-exponent candidates such as `1e100` before falling back to the exact bigint path.
- The widened staged `proven_float_try_eisel_lemire()` paths now share common cached-power candidate finalization so positive, negative, and subnormal candidates all pass through the same exact midpoint validator.
- The widened staged `proven_float_try_eisel_lemire()` paths now also share one cached-power `u64 x u128 -> u256` product helper for both positive scaled-`5^q` and negative reciprocal-`5^-q` candidate assembly.
- The widened staged `proven_float_try_eisel_lemire()` paths now round wide products into a shared `53-bit significand + unbiased exponent` packing step, which keeps representative subnormals such as `5e-324` on the staged fast path while leaving below-half true-min cases on exact fallback.
- The staged `proven_float_try_eisel_lemire()` entry logic now routes positive exponents through one generated power-of-5 product path and negative exponents through one bounded denominator-or-reciprocal normalization path instead of keeping separate exact-cancellation branches.
- Negative exponents now try the generated reciprocal cached-power candidate path first across the whole staged band, using the older small-`q` denominator normalization only when the reciprocal candidate remains uncertain.
- The staged `proven_float_try_eisel_lemire()` layer now builds explicit candidate plans for positive products, negative reciprocals, and denominator fallbacks, then executes them through one shared plan-dispatch seam.
- The staged `proven_float_try_eisel_lemire()` layer now feeds both positive `5^q` products and negative reciprocal `5^-q` candidates through one signed cached-power product-plan builder before considering the narrow negative denominator fallback.
- The staged `proven_float_try_eisel_lemire()` layer now drops the separate negative denominator-normalization family; uncertain negative cached-power candidates defer directly to the exact bigint fallback.
- Internal staged-path metrics now expose shared cached-power product-plan hits so tests can verify the staged representative corpus stays on that single success family.
- Added an explicit `rfc-0001` audit corpus that pins the public parser against the named RFC cases for basics, specials, 2^53 ties-even boundaries, true-min midpoint below/exact/above, very long significands, huge exponents, and malformed/endptr behavior.
- `proven_scan_f64()` now uses an exact cached-`5^q` positive-exponent fast path ahead of the bigint fallback for a broader range of large finite decimal integers.
- `proven_u8str_view_slice()` now allows empty slices at the end of a view.
- `proven_float_format_*_policy()` now rejects invalid policy enums before mode-specific dispatch.
- `proven_sysio_scan_chunk_impl()` now accepts exact 4096-byte chunk fits instead of treating them as truncation.
- `proven_u8str_fmt_internal()` now rejects unknown argument types instead of silently dropping them.

### Changed

- Tightened repository documentation rules and clarified how local-only notes
  differ from public release notes.

## [2026-06-02]

### Changed

- Removed local-only project state files from public Git history and restored
  them locally as ignored files.
- Updated local handoff documents so the next session resumes from the
  post-float-backend state.

### Fixed

- Added `.gitignore` entries for the restored local-only project files.

## [2026-06-01]

### Changed

- Closed the staged float backend work.
- Extended float boundary and shortest-literal coverage.

### Fixed

- Corrected the float scan underflow edge.
- Fixed scientific exponent padding and shortest-float candidate selection.

[0.0.1]: https://github.com/rubidus-api/proven_c_lib/releases/tag/v0.0.1
