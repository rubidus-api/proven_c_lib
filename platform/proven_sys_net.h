#ifndef PROVEN_PLATFORM_SYS_NET_H
#define PROVEN_PLATFORM_SYS_NET_H

#include "proven/types.h"

/**
 * @file proven_sys_net.h
 * @brief Platform Abstraction Layer for sockets: POSIX sockets and Winsock.
 *
 * This is the ONLY place a socket header is included. Every call answers with a reason from
 * proven_sys_net_result_t - never errno, never a Winsock code - and the portable layer
 * (src/proven/net.c) turns reasons into proven_err_t in one function.
 *
 * What the unit takes care of, so that nothing above it has to know:
 *
 *   - Every socket is created non-blocking and not inherited by child processes. Blocking
 *     behaviour is built above this unit from readiness and a deadline, which is what makes a
 *     timeout possible at all.
 *   - A write to a closed peer never raises SIGPIPE: MSG_NOSIGNAL where it exists, SO_NOSIGPIPE
 *     where it does not. No signal handler is installed on the caller's behalf.
 *   - EINTR is retried here, except in the wait, which reports "nothing ready" so that the
 *     caller re-reads its deadline rather than waiting the full interval again.
 *   - Winsock is started once per process, lazily, before the first call that needs it -
 *     name resolution included. The handle is a SOCKET and is carried at full width.
 *   - A listening TCP socket sets SO_REUSEADDR on POSIX, so a restarted server can bind while
 *     old connections linger. On Windows that option means something else - it lets a second
 *     process take the port - so SO_EXCLUSIVEADDRUSE is set there instead.
 *   - An IPv6 socket is IPv6-only on every platform, so the same program does not accept IPv4
 *     on one system and refuse it on another.
 *
 * Hosted only. A freestanding build has no sockets and does not compile this unit.
 */

/** A socket, at the platform's full handle width. */
typedef proven_uintptr_t proven_sys_socket_t;
#define PROVEN_SYS_SOCKET_INVALID (~(proven_sys_socket_t)0)

typedef enum {
    PROVEN_SYS_NET_OK = 0,
    PROVEN_SYS_NET_WOULD_BLOCK,   /**< not now; wait for readiness and try again */
    PROVEN_SYS_NET_IN_PROGRESS,   /**< a connect was started and has not finished */
    PROVEN_SYS_NET_EOF,           /**< the peer closed its sending side */
    PROVEN_SYS_NET_TIMEOUT,       /**< the OS itself gave up (a connect that was never answered) */
    PROVEN_SYS_NET_REFUSED,       /**< nothing is listening there */
    PROVEN_SYS_NET_RESET,         /**< the peer reset or aborted the connection, or is gone */
    PROVEN_SYS_NET_UNREACHABLE,   /**< no route to the network or host */
    PROVEN_SYS_NET_ADDR_IN_USE,   /**< the address to bind is taken */
    PROVEN_SYS_NET_ADDR_INVALID,  /**< the address cannot be used here (not local, wrong family) */
    PROVEN_SYS_NET_NOT_FOUND,     /**< the host name has no address; a Unix-domain path is not there */
    PROVEN_SYS_NET_DENIED,        /**< permission refused it (a privileged port, a firewall rule) */
    PROVEN_SYS_NET_UNSUPPORTED,   /**< this address family or socket kind does not exist here */
    PROVEN_SYS_NET_TRUNCATED,     /**< a datagram was larger than the buffer; the rest is gone */
    PROVEN_SYS_NET_TOO_BIG,       /**< a datagram is larger than the network will carry */
    PROVEN_SYS_NET_LIMIT,         /**< out of descriptors, buffers or memory */
    PROVEN_SYS_NET_EXISTS,        /**< a selector already holds this socket */
    PROVEN_SYS_NET_ERROR          /**< anything else */
} proven_sys_net_result_t;

typedef enum {
    PROVEN_SYS_NET_FAMILY_NONE = 0,
    PROVEN_SYS_NET_FAMILY_IPV4,
    PROVEN_SYS_NET_FAMILY_IPV6,
    PROVEN_SYS_NET_FAMILY_UNIX
} proven_sys_net_family_t;

typedef enum {
    PROVEN_SYS_NET_STREAM = 0,
    PROVEN_SYS_NET_DATAGRAM
} proven_sys_net_kind_t;

/** Longest Unix-domain path carried, NUL excluded. The smallest sun_path in use is 104 bytes. */
#define PROVEN_SYS_NET_UNIX_PATH_MAX 103

/** An address in a form no OS header is needed to read. Network byte order in `ip`. */
typedef struct {
    proven_sys_net_family_t family;
    proven_u16 port;        /**< host byte order */
    proven_u32 scope_id;    /**< IPv6 zone index; 0 when none */
    proven_u8  ip[16];      /**< IPv4 in the first four bytes */
    proven_u8  path_len;    /**< Unix-domain only */
    char       path[PROVEN_SYS_NET_UNIX_PATH_MAX + 1];
} proven_sys_net_addr_t;

/** Readiness to ask for, and readiness reported. */
enum {
    PROVEN_SYS_NET_READABLE = 1 << 0,
    PROVEN_SYS_NET_WRITABLE = 1 << 1,
    PROVEN_SYS_NET_FAILED   = 1 << 2   /**< reported only: error or hang-up; the next call says which */
};

typedef struct {
    proven_sys_socket_t sock;
    proven_u8 want;
    proven_u8 got;
} proven_sys_net_wait_t;

/** Open a listening stream socket bound to `addr` (port 0 lets the OS choose). */
proven_sys_net_result_t proven_sys_net_open_listener(const proven_sys_net_addr_t *addr, int backlog,
                                                     proven_sys_socket_t *out);

/** Open a datagram socket bound to `addr`. IPv4 and IPv6 only. */
proven_sys_net_result_t proven_sys_net_open_datagram(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out);

/**
 * Create a stream socket and start connecting it. OK means connected already; IN_PROGRESS means
 * wait for it to become writable and then ask proven_sys_net_connect_result. On any other
 * answer no socket is left open.
 */
proven_sys_net_result_t proven_sys_net_connect_start(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out);

/** The outcome of a connect that was IN_PROGRESS, once the socket reported ready. */
proven_sys_net_result_t proven_sys_net_connect_result(proven_sys_socket_t sock);

/**
 * Make two stream sockets connected to each other, both non-blocking and not inherited: what
 * one writes the other reads. A Unix-domain socket pair on POSIX; on Windows, which has none,
 * a TCP connection through the loopback interface.
 */
proven_sys_net_result_t proven_sys_net_pair(proven_sys_socket_t *a, proven_sys_socket_t *b);

/** Take one pending connection. WOULD_BLOCK when none is waiting. `peer` may be NULL. */
proven_sys_net_result_t proven_sys_net_accept(proven_sys_socket_t listener, proven_sys_socket_t *out,
                                              proven_sys_net_addr_t *peer);

/** Receive up to `cap` bytes. EOF when the peer has closed; never OK with zero bytes for cap > 0. */
proven_sys_net_result_t proven_sys_net_recv(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                            proven_size_t *received);

/** Send up to `len` bytes; `sent` says how many went. */
proven_sys_net_result_t proven_sys_net_send(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                            proven_size_t *sent);

/** Receive one datagram. TRUNCATED when it did not fit: `received` bytes are in `buf`, the rest is lost. */
proven_sys_net_result_t proven_sys_net_recv_from(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                 proven_size_t *received, proven_sys_net_addr_t *from);

/** Send one datagram, whole or not at all. */
proven_sys_net_result_t proven_sys_net_send_to(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                               const proven_sys_net_addr_t *to);

/** Close the sending side; the peer reads end of input, and this side can still receive. */
proven_sys_net_result_t proven_sys_net_shutdown_write(proven_sys_socket_t sock);

proven_sys_net_result_t proven_sys_net_close(proven_sys_socket_t sock);

proven_sys_net_result_t proven_sys_net_local_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out);
proven_sys_net_result_t proven_sys_net_peer_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out);

/** Turn Nagle's algorithm off (true) or on (false) for a TCP socket. */
proven_sys_net_result_t proven_sys_net_set_nodelay(proven_sys_socket_t sock, bool on);

/** Bytes of scratch memory proven_sys_net_wait needs for `count` items; SIZE_MAX if not representable. */
proven_size_t proven_sys_net_wait_scratch_size(proven_size_t count);

/**
 * Wait until one of `items` is ready, or `timeout_ms` passes (negative: no limit).
 *
 * `scratch` is caller memory of at least proven_sys_net_wait_scratch_size(count) bytes, aligned
 * for a pointer: the OS wants its own array, and this unit does not allocate.
 *
 * `ready` is the number of items with a non-zero `got`. OK with `ready == 0` means the time
 * passed - or the wait was interrupted, which the caller cannot and need not tell apart: it
 * re-reads its deadline either way.
 *
 * POSIX poll; Windows WSAPoll. WSAPoll did not report a failed connect before Windows 10
 * version 2004; this library is not tested on anything older.
 */
proven_sys_net_result_t proven_sys_net_wait(proven_sys_net_wait_t *items, proven_size_t count,
                                            void *scratch, int timeout_ms, proven_size_t *ready);

/**
 * A kernel-held set of sockets: epoll on Linux, kqueue on the BSDs and macOS. The set persists
 * between waits and a wait returns only the sockets that are ready, so its cost does not grow
 * with the number of idle sockets. Where neither exists (Windows, other systems)
 * proven_sys_net_selector_open answers PROVEN_SYS_NET_UNSUPPORTED and the portable layer
 * builds the same interface on proven_sys_net_wait.
 *
 * Level-triggered: a socket is reported for as long as it is ready.
 */
typedef struct {
    void *tag;          /**< what was registered with the socket */
    proven_u8 got;      /**< PROVEN_SYS_NET_READABLE / _WRITABLE / _FAILED */
} proven_sys_net_event_t;

typedef enum {
    PROVEN_SYS_NET_SELECTOR_NONE = 0,
    PROVEN_SYS_NET_SELECTOR_EPOLL,
    PROVEN_SYS_NET_SELECTOR_KQUEUE
} proven_sys_net_selector_kind_t;

/** Which kernel facility this build uses; NONE when it has neither. */
proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void);

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out);
void proven_sys_net_selector_close(proven_uintptr_t selector);

/** Register `sock` (`add` true) or change what is asked of it. EXISTS when added twice,
 *  NOT_FOUND when changed without being there. */
proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add);

/** Take `sock` out of the set. A socket that is closed leaves the set by itself. */
proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock);

/**
 * Wait until something in the set is ready or `timeout_ms` passes (negative: no limit), and
 * report at most `cap` of them. OK with `count == 0` means the time passed or the wait was
 * interrupted. A hang-up is reported as readable and writable, since which was asked for is
 * not known here; with kqueue a socket ready both ways appears as two events.
 */
proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count);

/**
 * Resolve a host name to addresses for stream sockets, blocking, through the system resolver.
 * `host` is NUL-terminated. At most `cap` addresses are written; `count` says how many.
 * NOT_FOUND when the name has no address.
 */
proven_sys_net_result_t proven_sys_net_resolve(const char *host, proven_u16 port,
                                               proven_sys_net_addr_t *out, proven_size_t cap,
                                               proven_size_t *count);

#endif /* PROVEN_PLATFORM_SYS_NET_H */
