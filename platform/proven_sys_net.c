#if !defined(_WIN32) && !defined(_WIN64)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* accept4: a connection that is non-blocking and close-on-exec from birth */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "proven_sys_net.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
/* ============================================================================
 * Winsock
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <afunix.h>
#include <windows.h>

typedef SOCKET native_socket_t;
typedef int native_len_t;
typedef WSAPOLLFD native_pollfd_t;
#define NATIVE_INVALID INVALID_SOCKET
#define native_close closesocket

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static INIT_ONCE g_net_once = INIT_ONCE_STATIC_INIT;
static bool g_net_started;

static BOOL CALLBACK net_start_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    WSADATA data;
    g_net_started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return TRUE;
}

/* Winsock refuses every call, name resolution included, until WSAStartup has run. Once per
 * process, on first use; never undone - there is no moment at which the library knows the last
 * socket in the process is gone. */
static bool net_ready(void) {
    InitOnceExecuteOnce(&g_net_once, net_start_once, NULL, NULL);
    return g_net_started;
}

static int net_last_error(void) { return WSAGetLastError(); }

static proven_sys_net_result_t net_reason(int e) {
    switch (e) {
        case WSAEWOULDBLOCK:      return PROVEN_SYS_NET_WOULD_BLOCK;
        case WSAEINPROGRESS:
        case WSAEALREADY:         return PROVEN_SYS_NET_IN_PROGRESS;
        case WSAECONNREFUSED:     return PROVEN_SYS_NET_REFUSED;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENETRESET:
        case WSAESHUTDOWN:        return PROVEN_SYS_NET_RESET;
        case WSAETIMEDOUT:        return PROVEN_SYS_NET_TIMEOUT;
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAENETDOWN:
        case WSAEHOSTDOWN:        return PROVEN_SYS_NET_UNREACHABLE;
        case WSAEADDRINUSE:       return PROVEN_SYS_NET_ADDR_IN_USE;
        case WSAEADDRNOTAVAIL:    return PROVEN_SYS_NET_ADDR_INVALID;
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAESOCKTNOSUPPORT:
        case WSAEPFNOSUPPORT:     return PROVEN_SYS_NET_UNSUPPORTED;
        case WSAEACCES:           return PROVEN_SYS_NET_DENIED;
        case WSAEMSGSIZE:         return PROVEN_SYS_NET_TOO_BIG;
        case WSAEMFILE:
        case WSAENOBUFS:
        case WSA_NOT_ENOUGH_MEMORY: return PROVEN_SYS_NET_LIMIT;
        default:                  return PROVEN_SYS_NET_ERROR;
    }
}

static bool net_interrupted(int e) { return e == WSAEINTR; }

static native_socket_t native_open(int af, int type) {
    native_socket_t s = WSASocketW(af, type, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return s;
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

/* An accepted socket does not reliably inherit either property. */
static bool native_prepare_accepted(native_socket_t s) {
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) return false;
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return true;
}

static void native_listener_options(native_socket_t s) {
    /* Not SO_REUSEADDR: on Windows that lets another process bind the same port and take the
     * connections. Exclusive use is the option that means what SO_REUSEADDR means elsewhere. */
    BOOL on = TRUE;
    (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
}

static void native_datagram_options(native_socket_t s) {
    /* Without this, a datagram sent to a port nobody listens on makes a LATER receive on this
     * socket fail with a connection reset - an error about some other packet. */
    BOOL off = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &returned, NULL, NULL);
}

#define NATIVE_SEND_FLAGS 0
#define NATIVE_SHUT_WR SD_SEND
#define NATIVE_POLL_IN POLLRDNORM
#define NATIVE_POLL_OUT POLLWRNORM

#else
/* ============================================================================
 * POSIX sockets
 * ========================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

typedef int native_socket_t;
typedef socklen_t native_len_t;
typedef struct pollfd native_pollfd_t;
#define NATIVE_INVALID (-1)
#define native_close close

static bool net_ready(void) { return true; }
static int net_last_error(void) { return errno; }

static proven_sys_net_result_t net_reason(int e) {
    switch (e) {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
                             return PROVEN_SYS_NET_WOULD_BLOCK;
        case EINPROGRESS:
        case EALREADY:       return PROVEN_SYS_NET_IN_PROGRESS;
        case ECONNREFUSED:   return PROVEN_SYS_NET_REFUSED;
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:          return PROVEN_SYS_NET_RESET;
        case ETIMEDOUT:      return PROVEN_SYS_NET_TIMEOUT;
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ENETDOWN:
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
                             return PROVEN_SYS_NET_UNREACHABLE;
        case EADDRINUSE:     return PROVEN_SYS_NET_ADDR_IN_USE;
        case EADDRNOTAVAIL:  return PROVEN_SYS_NET_ADDR_INVALID;
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
#ifdef ESOCKTNOSUPPORT
        case ESOCKTNOSUPPORT:
#endif
                             return PROVEN_SYS_NET_UNSUPPORTED;
        case EACCES:
        case EPERM:          return PROVEN_SYS_NET_DENIED;
        case ENOENT:
        case ENOTDIR:        return PROVEN_SYS_NET_NOT_FOUND;
        case EMSGSIZE:       return PROVEN_SYS_NET_TOO_BIG;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:         return PROVEN_SYS_NET_LIMIT;
        default:             return PROVEN_SYS_NET_ERROR;
    }
}

static bool net_interrupted(int e) { return e == EINTR; }

/* Linux makes a socket non-blocking and close-on-exec in the call that creates it; elsewhere
 * the two flags are set right after. */
#if !defined(__linux__)
static bool native_set_flags(native_socket_t s) {
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) != 0) return false;
    int fd_flags = fcntl(s, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(s, F_SETFD, fd_flags | FD_CLOEXEC) != 0) return false;
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return true;
}
#endif

static native_socket_t native_open(int af, int type) {
#if defined(__linux__)
    native_socket_t s = socket(af, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return s;
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return s;
#else
    native_socket_t s = socket(af, type, 0);
    if (s < 0) return s;
    if (!native_set_flags(s)) { close(s); return -1; }
    return s;
#endif
}

static bool native_prepare_accepted(native_socket_t s) {
#if defined(__linux__)
    (void)s;
    return true;            /* accept4 already set both flags */
#else
    return native_set_flags(s);
#endif
}

static void native_listener_options(native_socket_t s) {
    /* A restarted server may bind while connections of the previous run are still in TIME_WAIT. */
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
}

static void native_datagram_options(native_socket_t s) { (void)s; }

#ifdef MSG_NOSIGNAL
#define NATIVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define NATIVE_SEND_FLAGS 0   /* SO_NOSIGPIPE was set when the socket was made */
#endif
#define NATIVE_SHUT_WR SHUT_WR
#define NATIVE_POLL_IN POLLIN
#define NATIVE_POLL_OUT POLLOUT

#endif

/* ============================================================================
 * Shared by both
 * ========================================================================== */

static proven_sys_net_result_t net_fail(void) {
    return net_reason(net_last_error());
}

static int native_family(proven_sys_net_family_t family) {
    switch (family) {
        case PROVEN_SYS_NET_FAMILY_IPV4: return AF_INET;
        case PROVEN_SYS_NET_FAMILY_IPV6: return AF_INET6;
        case PROVEN_SYS_NET_FAMILY_UNIX: return AF_UNIX;
        default: return -1;
    }
}

/* Build the native address. Returns its length, or 0 when the address cannot be expressed. */
static native_len_t to_native(const proven_sys_net_addr_t *a, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (a->family == PROVEN_SYS_NET_FAMILY_IPV4) {
        struct sockaddr_in *in = (struct sockaddr_in *)out;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->ip, 4);
        return (native_len_t)sizeof *in;
    }
    if (a->family == PROVEN_SYS_NET_FAMILY_IPV6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)out;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        memcpy(&in6->sin6_addr, a->ip, 16);
        in6->sin6_scope_id = a->scope_id;
        return (native_len_t)sizeof *in6;
    }
    if (a->family == PROVEN_SYS_NET_FAMILY_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        if (a->path_len == 0 || a->path_len >= sizeof un->sun_path) return 0;
        un->sun_family = AF_UNIX;
        memcpy(un->sun_path, a->path, a->path_len);
        un->sun_path[a->path_len] = '\0';
        return (native_len_t)sizeof *un;
    }
    return 0;
}

static void from_native(const struct sockaddr_storage *in, native_len_t len, proven_sys_net_addr_t *out) {
    memset(out, 0, sizeof *out);
    if (in->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)in;
        out->family = PROVEN_SYS_NET_FAMILY_IPV4;
        out->port = ntohs(v4->sin_port);
        memcpy(out->ip, &v4->sin_addr, 4);
    } else if (in->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)in;
        out->family = PROVEN_SYS_NET_FAMILY_IPV6;
        out->port = ntohs(v6->sin6_port);
        memcpy(out->ip, &v6->sin6_addr, 16);
        out->scope_id = (proven_u32)v6->sin6_scope_id;
    } else if (in->ss_family == AF_UNIX) {
        /* The peer of a Unix-domain connection is usually unnamed: the family is known and the
         * path is empty. The length the OS reported says how much of sun_path is meaningful. */
        const struct sockaddr_un *un = (const struct sockaddr_un *)in;
        out->family = PROVEN_SYS_NET_FAMILY_UNIX;
        proven_size_t head = (proven_size_t)((const char *)un->sun_path - (const char *)un);
        proven_size_t avail = (proven_size_t)len > head ? (proven_size_t)len - head : 0;
        if (avail > sizeof un->sun_path) avail = sizeof un->sun_path;
        proven_size_t n = 0;
        while (n < avail && n < PROVEN_SYS_NET_UNIX_PATH_MAX && un->sun_path[n] != '\0') n++;
        memcpy(out->path, un->sun_path, n);
        out->path[n] = '\0';
        out->path_len = (proven_u8)n;
    }
}

static proven_sys_net_result_t open_bound(const proven_sys_net_addr_t *addr, int type, native_socket_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    int af = native_family(addr->family);
    struct sockaddr_storage ss;
    native_len_t len = to_native(addr, &ss);
    if (af < 0 || len == 0) return PROVEN_SYS_NET_ADDR_INVALID;

    native_socket_t s = native_open(af, type);
    if (s == NATIVE_INVALID) return net_fail();
    if (af == AF_INET6) {
        int on = 1;
        (void)setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&on, sizeof on);
    }
    if (type == SOCK_STREAM && af != AF_UNIX) native_listener_options(s);
    if (type == SOCK_DGRAM) native_datagram_options(s);

    if (bind(s, (const struct sockaddr *)&ss, len) != 0) {
        proven_sys_net_result_t r = net_fail();
#if defined(_WIN32) || defined(_WIN64)
        /* A port another socket holds exclusively is reported as "access denied" here, and
         * Windows has no privileged ports for that to be confused with. */
        if (r == PROVEN_SYS_NET_DENIED && af != AF_UNIX) r = PROVEN_SYS_NET_ADDR_IN_USE;
#endif
        native_close(s);
        return r;
    }
    *out = s;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_open_listener(const proven_sys_net_addr_t *addr, int backlog,
                                                     proven_sys_socket_t *out) {
    native_socket_t s = NATIVE_INVALID;
    proven_sys_net_result_t r = open_bound(addr, SOCK_STREAM, &s);
    if (r != PROVEN_SYS_NET_OK) return r;
    if (listen(s, backlog > 0 ? backlog : SOMAXCONN) != 0) {
        r = net_fail();
        native_close(s);
        return r;
    }
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_open_datagram(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    if (addr->family != PROVEN_SYS_NET_FAMILY_IPV4 && addr->family != PROVEN_SYS_NET_FAMILY_IPV6) {
        return PROVEN_SYS_NET_UNSUPPORTED;
    }
    native_socket_t s = NATIVE_INVALID;
    proven_sys_net_result_t r = open_bound(addr, SOCK_DGRAM, &s);
    if (r != PROVEN_SYS_NET_OK) return r;
    *out = (proven_sys_socket_t)s;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_connect_start(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    int af = native_family(addr->family);
    struct sockaddr_storage ss;
    native_len_t len = to_native(addr, &ss);
    if (af < 0 || len == 0) return PROVEN_SYS_NET_ADDR_INVALID;

    native_socket_t s = native_open(af, SOCK_STREAM);
    if (s == NATIVE_INVALID) return net_fail();

    for (;;) {
        if (connect(s, (const struct sockaddr *)&ss, len) == 0) {
            *out = (proven_sys_socket_t)s;
            return PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        proven_sys_net_result_t r = net_reason(e);
        /* A non-blocking connect that has merely started says "would block" on Windows and
         * "in progress" on POSIX. A Unix-domain connect to a full backlog says "would block"
         * on Linux and means the same thing to the caller: not connected, try again later. */
        if (r == PROVEN_SYS_NET_WOULD_BLOCK || r == PROVEN_SYS_NET_IN_PROGRESS) {
            *out = (proven_sys_socket_t)s;
            return PROVEN_SYS_NET_IN_PROGRESS;
        }
        native_close(s);
#if defined(_WIN32) || defined(_WIN64)
        /* Winsock answers a Unix-domain connect to a path that does not exist the way it
         * answers one that nobody listens on. POSIX tells the two apart, and a caller needs to:
         * one means "start the server", the other "the server is not accepting". */
        if (af == AF_UNIX) {
            WCHAR wide[PROVEN_SYS_NET_UNIX_PATH_MAX + 1];
            int n = MultiByteToWideChar(CP_UTF8, 0, addr->path, (int)addr->path_len, wide, PROVEN_SYS_NET_UNIX_PATH_MAX);
            if (n > 0) {
                wide[n] = 0;
                if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) {
                    DWORD fe = GetLastError();
                    if (fe == ERROR_FILE_NOT_FOUND || fe == ERROR_PATH_NOT_FOUND) return PROVEN_SYS_NET_NOT_FOUND;
                }
            }
        }
#endif
        return r;
    }
}

proven_sys_net_result_t proven_sys_net_pair(proven_sys_socket_t *a, proven_sys_socket_t *b) {
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
#if defined(_WIN32) || defined(_WIN64)
    /* No socketpair here: listen on a loopback port the system chooses, connect to it, and
     * accept. The listener exists for a moment; the accepted socket is checked to be the one
     * that connected, so that another local process racing for that port cannot end up as one
     * end of the pair. */
    native_socket_t listener = native_open(AF_INET, SOCK_STREAM);
    if (listener == NATIVE_INVALID) return net_fail();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    native_len_t len = (native_len_t)sizeof addr;
    native_socket_t client = NATIVE_INVALID, server = NATIVE_INVALID;
    proven_sys_net_result_t r = PROVEN_SYS_NET_ERROR;
    if (bind(listener, (const struct sockaddr *)&addr, len) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0) { r = net_fail(); goto done; }
    client = native_open(AF_INET, SOCK_STREAM);
    if (client == NATIVE_INVALID) { r = net_fail(); goto done; }
    if (connect(client, (const struct sockaddr *)&addr, len) != 0) {
        proven_sys_net_result_t cr = net_fail();
        if (cr != PROVEN_SYS_NET_WOULD_BLOCK && cr != PROVEN_SYS_NET_IN_PROGRESS) { r = cr; goto done; }
    }
    for (int tries = 0; tries < 200 && server == NATIVE_INVALID; ++tries) {
        struct sockaddr_in from;
        native_len_t fl = (native_len_t)sizeof from;
        native_socket_t s = accept(listener, (struct sockaddr *)&from, &fl);
        if (s == NATIVE_INVALID) {
            if (net_reason(net_last_error()) != PROVEN_SYS_NET_WOULD_BLOCK) { r = net_fail(); goto done; }
            Sleep(5);
            continue;
        }
        struct sockaddr_in mine;
        native_len_t ml = (native_len_t)sizeof mine;
        if (getsockname(client, (struct sockaddr *)&mine, &ml) == 0 && mine.sin_port == from.sin_port &&
            from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            server = s;
        } else {
            closesocket(s);                     /* somebody else's connection */
        }
    }
    if (server == NATIVE_INVALID) { r = PROVEN_SYS_NET_TIMEOUT; goto done; }
    if (!native_prepare_accepted(server)) { r = PROVEN_SYS_NET_ERROR; goto done; }
    closesocket(listener);
    *a = (proven_sys_socket_t)client;
    *b = (proven_sys_socket_t)server;
    return PROVEN_SYS_NET_OK;
done:
    if (listener != NATIVE_INVALID) closesocket(listener);
    if (client != NATIVE_INVALID) closesocket(client);
    if (server != NATIVE_INVALID) closesocket(server);
    return r;
#else
    int fds[2];
#if defined(__linux__)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) return net_fail();
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return net_fail();
    if (!native_set_flags(fds[0]) || !native_set_flags(fds[1])) { close(fds[0]); close(fds[1]); return PROVEN_SYS_NET_ERROR; }
#endif
    *a = (proven_sys_socket_t)fds[0];
    *b = (proven_sys_socket_t)fds[1];
    return PROVEN_SYS_NET_OK;
#endif
}

proven_sys_net_result_t proven_sys_net_connect_result(proven_sys_socket_t sock) {
    int err = 0;
    native_len_t len = (native_len_t)sizeof err;
    if (getsockopt((native_socket_t)sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return net_fail();
    if (err != 0) return net_reason(err);
    /* No error recorded and yet not connected happens when the wait woke for another reason:
     * the peer address is the test that cannot be fooled. */
    struct sockaddr_storage ss;
    native_len_t sl = (native_len_t)sizeof ss;
    if (getpeername((native_socket_t)sock, (struct sockaddr *)&ss, &sl) != 0) return PROVEN_SYS_NET_IN_PROGRESS;
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_accept(proven_sys_socket_t listener, proven_sys_socket_t *out,
                                              proven_sys_net_addr_t *peer) {
    for (;;) {
        struct sockaddr_storage ss;
        native_len_t len = (native_len_t)sizeof ss;
#if defined(__linux__)
        native_socket_t s = accept4((native_socket_t)listener, (struct sockaddr *)&ss, &len,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        native_socket_t s = accept((native_socket_t)listener, (struct sockaddr *)&ss, &len);
#endif
        if (s == NATIVE_INVALID) {
            int e = net_last_error();
            if (net_interrupted(e)) continue;
            proven_sys_net_result_t r = net_reason(e);
            /* A connection that was reset while it waited in the backlog is not a failure of
             * the listener: skip it and take the next one. */
            if (r == PROVEN_SYS_NET_RESET) continue;
            return r;
        }
        if (!native_prepare_accepted(s)) {
            native_close(s);
            return PROVEN_SYS_NET_ERROR;
        }
        if (peer) from_native(&ss, len, peer);
        *out = (proven_sys_socket_t)s;
        return PROVEN_SYS_NET_OK;
    }
}

/* One call may not be asked for more than the platform's int-sized length. */
static int clamp_len(proven_size_t n) {
    return n > 0x40000000u ? 0x40000000 : (int)n;
}

/* How much one send may offer. Winsock accepts a buffer of any size in one non-blocking send
 * and queues all of it, whatever SO_SNDBUF says - so a 48 MiB write to a peer that reads
 * nothing "succeeds", and neither a deadline nor back-pressure ever comes into play. Offered in
 * pieces of this size, the socket refuses once its buffer is full, as it does everywhere else. */
static int clamp_send(proven_size_t n) {
#if defined(_WIN32) || defined(_WIN64)
    return n > 65536u ? 65536 : (int)n;
#else
    return clamp_len(n);
#endif
}

proven_sys_net_result_t proven_sys_net_recv(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                            proven_size_t *received) {
    *received = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = recv((native_socket_t)sock, (char *)buf, clamp_len(cap), 0);
#else
        ssize_t n = recv((native_socket_t)sock, buf, (size_t)clamp_len(cap), 0);
#endif
        if (n > 0) { *received = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        if (n == 0) return PROVEN_SYS_NET_EOF;
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_send(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                            proven_size_t *sent) {
    *sent = 0;
    if (len == 0) return PROVEN_SYS_NET_OK;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = send((native_socket_t)sock, (const char *)buf, clamp_send(len), NATIVE_SEND_FLAGS);
#else
        ssize_t n = send((native_socket_t)sock, buf, (size_t)clamp_send(len), NATIVE_SEND_FLAGS);
#endif
        if (n >= 0) { *sent = (proven_size_t)n; return PROVEN_SYS_NET_OK; }
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_recv_from(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                 proven_size_t *received, proven_sys_net_addr_t *from) {
    *received = 0;
    for (;;) {
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
#if defined(_WIN32) || defined(_WIN64)
        native_len_t sl = (native_len_t)sizeof ss;
        int n = recvfrom((native_socket_t)sock, (char *)buf, clamp_len(cap), 0, (struct sockaddr *)&ss, &sl);
        if (n >= 0) {
            *received = (proven_size_t)n;
            if (from) from_native(&ss, sl, from);
            return PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
        if (e == WSAEMSGSIZE) {
            /* The buffer was filled and the rest of the datagram discarded. */
            *received = (proven_size_t)clamp_len(cap);
            if (from) from_native(&ss, sl, from);
            return PROVEN_SYS_NET_TRUNCATED;
        }
#else
        /* recvmsg, not recvfrom: only msg_flags says the datagram was cut. */
        struct iovec iov = { .iov_base = buf, .iov_len = (size_t)clamp_len(cap) };
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_name = &ss;
        msg.msg_namelen = (socklen_t)sizeof ss;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t n = recvmsg((native_socket_t)sock, &msg, 0);
        if (n >= 0) {
            *received = (proven_size_t)n;
            if (from) from_native(&ss, msg.msg_namelen, from);
            return (msg.msg_flags & MSG_TRUNC) ? PROVEN_SYS_NET_TRUNCATED : PROVEN_SYS_NET_OK;
        }
        int e = net_last_error();
#endif
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_send_to(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                               const proven_sys_net_addr_t *to) {
    struct sockaddr_storage ss;
    native_len_t sl = to_native(to, &ss);
    if (sl == 0) return PROVEN_SYS_NET_ADDR_INVALID;
    if (len > 0x40000000u) return PROVEN_SYS_NET_TOO_BIG;
    for (;;) {
#if defined(_WIN32) || defined(_WIN64)
        int n = sendto((native_socket_t)sock, (const char *)buf, (int)len, NATIVE_SEND_FLAGS,
                       (const struct sockaddr *)&ss, sl);
#else
        ssize_t n = sendto((native_socket_t)sock, buf, len, NATIVE_SEND_FLAGS,
                           (const struct sockaddr *)&ss, sl);
#endif
        if (n >= 0) return (proven_size_t)n == len ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_ERROR;
        int e = net_last_error();
        if (net_interrupted(e)) continue;
        return net_reason(e);
    }
}

proven_sys_net_result_t proven_sys_net_shutdown_write(proven_sys_socket_t sock) {
    if (shutdown((native_socket_t)sock, NATIVE_SHUT_WR) != 0) return net_fail();
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_close(proven_sys_socket_t sock) {
    /* Not retried on EINTR: on Linux the descriptor is already gone, and closing it again could
     * close one that another thread has just been given. */
    if (native_close((native_socket_t)sock) != 0) {
        int e = net_last_error();
        return net_interrupted(e) ? PROVEN_SYS_NET_OK : net_reason(e);
    }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_local_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    native_len_t len = (native_len_t)sizeof ss;
    if (getsockname((native_socket_t)sock, (struct sockaddr *)&ss, &len) != 0) return net_fail();
    from_native(&ss, len, out);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_peer_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    native_len_t len = (native_len_t)sizeof ss;
    if (getpeername((native_socket_t)sock, (struct sockaddr *)&ss, &len) != 0) return net_fail();
    from_native(&ss, len, out);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_set_nodelay(proven_sys_socket_t sock, bool on) {
    int v = on ? 1 : 0;
    if (setsockopt((native_socket_t)sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&v, sizeof v) != 0) return net_fail();
    return PROVEN_SYS_NET_OK;
}

proven_size_t proven_sys_net_wait_scratch_size(proven_size_t count) {
    if (count > PROVEN_SIZE_MAX / sizeof(native_pollfd_t)) return PROVEN_SIZE_MAX;
    return count * sizeof(native_pollfd_t);
}

proven_sys_net_result_t proven_sys_net_wait(proven_sys_net_wait_t *items, proven_size_t count,
                                            void *scratch, int timeout_ms, proven_size_t *ready) {
    *ready = 0;
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    native_pollfd_t *fds = scratch;
    for (proven_size_t i = 0; i < count; ++i) {
        fds[i].fd = (native_socket_t)items[i].sock;
        fds[i].events = 0;
        fds[i].revents = 0;
        if (items[i].want & PROVEN_SYS_NET_READABLE) fds[i].events |= NATIVE_POLL_IN;
        if (items[i].want & PROVEN_SYS_NET_WRITABLE) fds[i].events |= NATIVE_POLL_OUT;
        items[i].got = 0;
    }
#if defined(_WIN32) || defined(_WIN64)
    if (count == 0) {
        /* WSAPoll refuses an empty set; an empty wait is a sleep. */
        if (timeout_ms != 0) Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        return PROVEN_SYS_NET_OK;
    }
    int n = WSAPoll(fds, (ULONG)count, timeout_ms);
#else
    int n = poll(fds, (nfds_t)count, timeout_ms);
#endif
    if (n < 0) {
        int e = net_last_error();
        return net_interrupted(e) ? PROVEN_SYS_NET_OK : net_reason(e);
    }
    for (proven_size_t i = 0; i < count; ++i) {
        short r = fds[i].revents;
        proven_u8 got = 0;
        if (r & NATIVE_POLL_IN) got |= PROVEN_SYS_NET_READABLE;
        if (r & NATIVE_POLL_OUT) got |= PROVEN_SYS_NET_WRITABLE;
        /* A hang-up means the call that was asked about will not wait: a read returns the end
         * of input, a write its error. Windows reports a peer's orderly close as a hang-up
         * alone, where Linux reports it as readable; both must read the same to the caller. */
        if (r & POLLHUP) got |= (proven_u8)(items[i].want & (PROVEN_SYS_NET_READABLE | PROVEN_SYS_NET_WRITABLE));
        if (r & (POLLERR | POLLNVAL)) got |= PROVEN_SYS_NET_FAILED;
        /* Whatever else the OS reported, something happened: an item that woke the wait must
         * not look idle, or the caller would wait again and spin. */
        if (r != 0 && got == 0) got = PROVEN_SYS_NET_FAILED;
        items[i].got = got;
        if (got) (*ready)++;
    }
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_resolve(const char *host, proven_u16 port,
                                               proven_sys_net_addr_t *out, proven_size_t cap,
                                               proven_size_t *count) {
    *count = 0;
    if (!net_ready()) return PROVEN_SYS_NET_ERROR;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &list);
    if (rc != 0) {
        if (list) freeaddrinfo(list);
        switch (rc) {
            case EAI_NONAME:
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
            case EAI_NODATA:
#endif
                return PROVEN_SYS_NET_NOT_FOUND;
            case EAI_AGAIN:  return PROVEN_SYS_NET_TIMEOUT;   /* the name server did not answer */
            case EAI_MEMORY: return PROVEN_SYS_NET_LIMIT;
            default:         return PROVEN_SYS_NET_ERROR;
        }
    }
    for (struct addrinfo *ai = list; ai && *count < cap; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        proven_size_t n = (proven_size_t)ai->ai_addrlen < sizeof ss ? (proven_size_t)ai->ai_addrlen : sizeof ss;
        memcpy(&ss, ai->ai_addr, n);
        from_native(&ss, (native_len_t)n, &out[*count]);
        out[*count].port = port;
        (*count)++;
    }
    freeaddrinfo(list);
    return *count > 0 ? PROVEN_SYS_NET_OK : PROVEN_SYS_NET_NOT_FOUND;
}

// -----------------------------------------------------------------------------
// Selector: epoll, kqueue, or none
// -----------------------------------------------------------------------------

#if defined(__linux__)
#include <sys/epoll.h>

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_EPOLL; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) return net_reason(errno);
    *out = (proven_uintptr_t)fd;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) {
    (void)close((int)selector);
}

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (want & PROVEN_SYS_NET_READABLE) ev.events |= EPOLLIN;
    if (want & PROVEN_SYS_NET_WRITABLE) ev.events |= EPOLLOUT;
    ev.data.ptr = tag;
    if (epoll_ctl((int)selector, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, (int)sock, &ev) == 0) return PROVEN_SYS_NET_OK;
    if (errno == EEXIST) return PROVEN_SYS_NET_EXISTS;
    if (errno == ENOENT) return PROVEN_SYS_NET_NOT_FOUND;
    if (errno == ENOSPC || errno == ENOMEM) return PROVEN_SYS_NET_LIMIT;
    return net_reason(errno);
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (epoll_ctl((int)selector, EPOLL_CTL_DEL, (int)sock, &ev) == 0) return PROVEN_SYS_NET_OK;
    return errno == ENOENT ? PROVEN_SYS_NET_NOT_FOUND : net_reason(errno);
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    struct epoll_event got[64];
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    int want = cap < 64 ? (int)cap : 64;
    int n = epoll_wait((int)selector, got, want, timeout_ms);
    if (n < 0) return errno == EINTR ? PROVEN_SYS_NET_OK : net_reason(errno);
    for (int i = 0; i < n; ++i) {
        proven_u8 g = 0;
        if (got[i].events & EPOLLIN) g |= PROVEN_SYS_NET_READABLE;
        if (got[i].events & EPOLLOUT) g |= PROVEN_SYS_NET_WRITABLE;
        /* A hang-up means neither a read nor a write will wait; which was asked is not known
         * here, so both are reported and the call that follows says what happened. */
        if (got[i].events & EPOLLHUP) g |= PROVEN_SYS_NET_READABLE | PROVEN_SYS_NET_WRITABLE;
        if (got[i].events & EPOLLERR) g |= PROVEN_SYS_NET_FAILED;
        if (g == 0) g = PROVEN_SYS_NET_FAILED;
        events[i].tag = got[i].data.ptr;
        events[i].got = g;
    }
    *count = (proven_size_t)n;
    return PROVEN_SYS_NET_OK;
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>

/* Written from the manual pages. Like every BSD path in this unit it has not been compiled or
 * run by this project: no such host is available to it. */

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_KQUEUE; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    int fd = kqueue();
    if (fd < 0) return net_reason(errno);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    *out = (proven_uintptr_t)fd;
    return PROVEN_SYS_NET_OK;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) {
    (void)close((int)selector);
}

/* kqueue keeps one entry per (socket, filter). Each change is applied by itself so that
 * deleting a filter that was never added - which is not an error for this interface - can be
 * told from a real failure. */
static int kq_change(int kq, int sock, int filter, int flags, void *tag) {
    struct kevent ch;
    EV_SET(&ch, (uintptr_t)sock, filter, flags, 0, 0, tag);
    return kevent(kq, &ch, 1, NULL, 0, NULL) == 0 ? 0 : errno;
}

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    (void)add;      /* kqueue has no separate "modify": adding an existing filter replaces it */
    int kq = (int)selector;
    int e1 = kq_change(kq, (int)sock, EVFILT_READ, (want & PROVEN_SYS_NET_READABLE) ? EV_ADD : EV_DELETE, tag);
    int e2 = kq_change(kq, (int)sock, EVFILT_WRITE, (want & PROVEN_SYS_NET_WRITABLE) ? EV_ADD : EV_DELETE, tag);
    if (e1 != 0 && e1 != ENOENT) return net_reason(e1);
    if (e2 != 0 && e2 != ENOENT) return net_reason(e2);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    int kq = (int)selector;
    int e1 = kq_change(kq, (int)sock, EVFILT_READ, EV_DELETE, NULL);
    int e2 = kq_change(kq, (int)sock, EVFILT_WRITE, EV_DELETE, NULL);
    if (e1 != 0 && e1 != ENOENT) return net_reason(e1);
    if (e2 != 0 && e2 != ENOENT) return net_reason(e2);
    return PROVEN_SYS_NET_OK;
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    struct kevent got[64];
    struct timespec ts, *tp = NULL;
    *count = 0;
    if (cap == 0) return PROVEN_SYS_NET_OK;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent((int)selector, NULL, 0, got, cap < 64 ? (int)cap : 64, tp);
    if (n < 0) return errno == EINTR ? PROVEN_SYS_NET_OK : net_reason(errno);
    for (int i = 0; i < n; ++i) {
        proven_u8 g = 0;
        if (got[i].flags & EV_ERROR) g = PROVEN_SYS_NET_FAILED;
        else if (got[i].filter == EVFILT_READ) g = PROVEN_SYS_NET_READABLE;
        else if (got[i].filter == EVFILT_WRITE) g = PROVEN_SYS_NET_WRITABLE;
        else g = PROVEN_SYS_NET_FAILED;
        events[i].tag = got[i].udata;
        events[i].got = g;
    }
    *count = (proven_size_t)n;
    return PROVEN_SYS_NET_OK;
}

#else

proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void) { return PROVEN_SYS_NET_SELECTOR_NONE; }

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out) {
    (void)out;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

void proven_sys_net_selector_close(proven_uintptr_t selector) { (void)selector; }

proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add) {
    (void)selector; (void)sock; (void)want; (void)tag; (void)add;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock) {
    (void)selector; (void)sock;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count) {
    (void)selector; (void)events; (void)cap; (void)timeout_ms;
    *count = 0;
    return PROVEN_SYS_NET_UNSUPPORTED;
}

#endif

#else
/* A translation unit must not be empty. */
typedef int proven_sys_net_unused_t;
#endif
