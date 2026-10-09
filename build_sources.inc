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
PROVEN_BUILD_SOURCE("src/proven/arena.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/pool.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/buffer.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/heap.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/alloc_check.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/u8str.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/u16str.c", PROVEN_SOURCE_HOSTED_ONLY)   /* UTF-16 strings are left out of the freestanding profile (PROVEN_NO_U16STR) */
PROVEN_BUILD_SOURCE("src/proven/array.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/ring.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/map.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/algorithm.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/hash.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/hash_legacy.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/encode.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/utf.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/url.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/http.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/random.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/fs.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the filesystem */
PROVEN_BUILD_SOURCE("src/proven/time.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/net.c", PROVEN_SOURCE_HOSTED_ONLY)   /* sockets */
PROVEN_BUILD_SOURCE("src/proven/fmt.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/mmap.c", PROVEN_SOURCE_HOSTED_ONLY)   /* memory mapping */
PROVEN_BUILD_SOURCE("src/proven/sysio.c", PROVEN_SOURCE_HOSTED_ONLY)   /* console and stdio */
PROVEN_BUILD_SOURCE("src/proven/job.c", PROVEN_SOURCE_HOSTED_ONLY)   /* threads */
PROVEN_BUILD_SOURCE("src/proven/scan.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_decimal.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_parse.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/float_format.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("src/proven/panic.c", PROVEN_SOURCE_FREESTANDING)
PROVEN_BUILD_SOURCE("platform/proven_sys_mem.c", PROVEN_SOURCE_HOSTED_ONLY)   /* the freestanding build uses the inline fallbacks */
PROVEN_BUILD_SOURCE("platform/proven_sys_fs.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_time.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_net.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_env.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_random.c", PROVEN_SOURCE_HOSTED_ONLY)   /* no OS CSPRNG on bare metal */
PROVEN_BUILD_SOURCE("platform/proven_sys_thread.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_io.c", PROVEN_SOURCE_HOSTED_ONLY)
PROVEN_BUILD_SOURCE("platform/proven_sys_math.c", PROVEN_SOURCE_FREESTANDING)

#undef PROVEN_SOURCE_FREESTANDING
#undef PROVEN_SOURCE_HOSTED_ONLY
