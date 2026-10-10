#include "proven/net.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "../../platform/proven_sys_net.h"

/*
 * The portable half of the socket layer. Everything the operating system does is in
 * platform/proven_sys_net.c; what is here is the part that is the same everywhere: address
 * text, the mapping of reasons to proven_err_t, and the loop that turns a non-blocking socket
 * and a deadline into a call that waits exactly as long as it was told to.
 */

// -----------------------------------------------------------------------------
// Reasons to errors
// -----------------------------------------------------------------------------

static proven_err_t internal_err_from_reason(proven_sys_net_result_t r) {
    switch (r) {
        case PROVEN_SYS_NET_OK:           return PROVEN_OK;
        case PROVEN_SYS_NET_WOULD_BLOCK:
        case PROVEN_SYS_NET_IN_PROGRESS:  return PROVEN_ERR_AGAIN;
        case PROVEN_SYS_NET_EOF:          return PROVEN_ERR_EOF;
        case PROVEN_SYS_NET_TIMEOUT:      return PROVEN_ERR_TIMEOUT;
        case PROVEN_SYS_NET_REFUSED:      return PROVEN_ERR_REFUSED;
        case PROVEN_SYS_NET_RESET:        return PROVEN_ERR_RESET;
        case PROVEN_SYS_NET_UNREACHABLE:  return PROVEN_ERR_UNREACHABLE;
        case PROVEN_SYS_NET_ADDR_IN_USE:  return PROVEN_ERR_BUSY;
        case PROVEN_SYS_NET_ADDR_INVALID: return PROVEN_ERR_INVALID_ARG;
        case PROVEN_SYS_NET_NOT_FOUND:    return PROVEN_ERR_NOT_FOUND;
        case PROVEN_SYS_NET_DENIED:       return PROVEN_ERR_PERMISSION;
        case PROVEN_SYS_NET_UNSUPPORTED:  return PROVEN_ERR_UNSUPPORTED;
        case PROVEN_SYS_NET_TRUNCATED:
        case PROVEN_SYS_NET_TOO_BIG:      return PROVEN_ERR_OUT_OF_BOUNDS;
        /* Out of descriptors or buffers: nothing is wrong with the call, and it may work once
         * something else is closed. "Back off" is the right response, which is what BUSY asks. */
        case PROVEN_SYS_NET_LIMIT:        return PROVEN_ERR_BUSY;
        case PROVEN_SYS_NET_EXISTS:       return PROVEN_ERR_EXISTS;
        default:                          return PROVEN_ERR_IO;
    }
}

// -----------------------------------------------------------------------------
// Addresses
// -----------------------------------------------------------------------------

static void internal_addr_to_sys(const proven_net_addr_t *a, proven_sys_net_addr_t *s) {
    *s = (proven_sys_net_addr_t){0};
    switch (a->family) {
        case PROVEN_NET_FAMILY_IPV4: s->family = PROVEN_SYS_NET_FAMILY_IPV4; break;
        case PROVEN_NET_FAMILY_IPV6: s->family = PROVEN_SYS_NET_FAMILY_IPV6; break;
        case PROVEN_NET_FAMILY_UNIX: s->family = PROVEN_SYS_NET_FAMILY_UNIX; break;
        default:                     s->family = PROVEN_SYS_NET_FAMILY_NONE; break;
    }
    s->port = a->port;
    s->scope_id = a->scope_id;
    for (int i = 0; i < 16; ++i) s->ip[i] = a->ip[i];
    proven_size_t n = a->path_len;
    if (n > PROVEN_SYS_NET_UNIX_PATH_MAX) n = PROVEN_SYS_NET_UNIX_PATH_MAX;
    for (proven_size_t i = 0; i < n; ++i) s->path[i] = a->path[i];
    s->path[n] = '\0';
    s->path_len = (proven_u8)n;
}

static void internal_addr_from_sys(const proven_sys_net_addr_t *s, proven_net_addr_t *a) {
    *a = (proven_net_addr_t){0};
    switch (s->family) {
        case PROVEN_SYS_NET_FAMILY_IPV4: a->family = PROVEN_NET_FAMILY_IPV4; break;
        case PROVEN_SYS_NET_FAMILY_IPV6: a->family = PROVEN_NET_FAMILY_IPV6; break;
        case PROVEN_SYS_NET_FAMILY_UNIX: a->family = PROVEN_NET_FAMILY_UNIX; break;
        default:                         a->family = PROVEN_NET_FAMILY_NONE; break;
    }
    a->port = s->port;
    a->scope_id = s->scope_id;
    for (int i = 0; i < 16; ++i) a->ip[i] = s->ip[i];
    proven_size_t n = s->path_len;
    for (proven_size_t i = 0; i < n; ++i) a->path[i] = s->path[i];
    a->path[n] = '\0';
    a->path_len = (proven_u8)n;
}

proven_net_addr_t proven_net_addr_ipv4(proven_u8 a, proven_u8 b, proven_u8 c, proven_u8 d, proven_u16 port) {
    proven_net_addr_t out = {0};
    out.family = PROVEN_NET_FAMILY_IPV4;
    out.port = port;
    out.ip[0] = a; out.ip[1] = b; out.ip[2] = c; out.ip[3] = d;
    return out;
}

proven_net_addr_t proven_net_addr_ipv6(const proven_byte_t ip[16], proven_u16 port, proven_u32 scope_id) {
    proven_net_addr_t out = {0};
    if (!ip) return out;
    out.family = PROVEN_NET_FAMILY_IPV6;
    out.port = port;
    out.scope_id = scope_id;
    for (int i = 0; i < 16; ++i) out.ip[i] = ip[i];
    return out;
}

proven_net_addr_t proven_net_addr_loopback(proven_net_family_t family, proven_u16 port) {
    proven_net_addr_t out = {0};
    if (family == PROVEN_NET_FAMILY_IPV4) return proven_net_addr_ipv4(127, 0, 0, 1, port);
    if (family == PROVEN_NET_FAMILY_IPV6) {
        out.family = PROVEN_NET_FAMILY_IPV6;
        out.port = port;
        out.ip[15] = 1;
    }
    return out;
}

proven_net_addr_t proven_net_addr_any(proven_net_family_t family, proven_u16 port) {
    proven_net_addr_t out = {0};
    if (family == PROVEN_NET_FAMILY_IPV4 || family == PROVEN_NET_FAMILY_IPV6) {
        out.family = family;
        out.port = port;
    }
    return out;
}

proven_err_t proven_net_addr_unix(proven_u8str_view_t path, proven_net_addr_t *out) {
    if (!out || (path.size > 0 && !path.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (path.size == 0) return PROVEN_ERR_INVALID_ARG;
    if (path.size > PROVEN_NET_UNIX_PATH_MAX) return PROVEN_ERR_OUT_OF_BOUNDS;
    for (proven_size_t i = 0; i < path.size; ++i) {
        if (path.ptr[i] == 0) return PROVEN_ERR_INVALID_ARG;
    }
    proven_net_addr_t a = {0};
    a.family = PROVEN_NET_FAMILY_UNIX;
    for (proven_size_t i = 0; i < path.size; ++i) a.path[i] = (char)path.ptr[i];
    a.path[path.size] = '\0';
    a.path_len = (proven_u8)path.size;
    *out = a;
    return PROVEN_OK;
}

/* Four decimal numbers, 0-255, separated by dots; no leading zeros, because inet_aton reads
 * "010" as octal eight and a reader of the text does not. */
static bool internal_parse_ipv4(const proven_byte_t *p, proven_size_t n, proven_byte_t out[4]) {
    proven_size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= n || p[i] != '.') return false;
            i++;
        }
        proven_size_t start = i;
        unsigned v = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            if (i - start >= 3) return false;
            v = v * 10u + (unsigned)(p[i] - '0');
            i++;
        }
        proven_size_t digits = i - start;
        if (digits == 0 || v > 255u) return false;
        if (digits > 1 && p[start] == '0') return false;
        out[part] = (proven_byte_t)v;
    }
    return i == n;
}

static int internal_hex_digit(proven_byte_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* RFC 4291 section 2.2: eight groups of one to four hex digits, at most one "::" standing for
 * one or more zero groups, and optionally a dotted IPv4 address as the last 32 bits. */
static bool internal_parse_ipv6(const proven_byte_t *p, proven_size_t n, proven_byte_t out[16]) {
    proven_u16 groups[8] = {0};
    int count = 0;
    int compress = -1;
    proven_size_t pos = 0;

    if (n >= 2 && p[0] == ':' && p[1] == ':') {
        compress = 0;
        pos = 2;
    } else if (n >= 1 && p[0] == ':') {
        return false;
    }

    while (pos < n) {
        proven_size_t end = pos;
        bool dotted = false;
        while (end < n && p[end] != ':') {
            if (p[end] == '.') dotted = true;
            end++;
        }
        if (dotted) {
            /* The IPv4 tail: it must be the last thing, and it fills two groups. */
            proven_byte_t v4[4];
            if (end != n || count > 6) return false;
            if (!internal_parse_ipv4(p + pos, n - pos, v4)) return false;
            groups[count++] = (proven_u16)(((proven_u16)v4[0] << 8) | v4[1]);
            groups[count++] = (proven_u16)(((proven_u16)v4[2] << 8) | v4[3]);
            pos = n;
            break;
        }
        proven_size_t digits = end - pos;
        if (digits == 0 || digits > 4 || count >= 8) return false;
        unsigned v = 0;
        for (proven_size_t i = pos; i < end; ++i) {
            int d = internal_hex_digit(p[i]);
            if (d < 0) return false;
            v = (v << 4) | (unsigned)d;
        }
        groups[count++] = (proven_u16)v;
        pos = end;
        if (pos == n) break;
        /* p[pos] is ':' */
        if (pos + 1 < n && p[pos + 1] == ':') {
            if (compress >= 0) return false;       /* a second "::" */
            compress = count;
            pos += 2;
        } else {
            pos += 1;
            if (pos == n) return false;            /* a single trailing ':' */
        }
    }

    proven_u16 full[8] = {0};
    if (compress < 0) {
        if (count != 8) return false;
        for (int i = 0; i < 8; ++i) full[i] = groups[i];
    } else {
        if (count > 7) return false;               /* "::" stands for at least one group */
        int tail = count - compress;
        for (int i = 0; i < compress; ++i) full[i] = groups[i];
        for (int i = 0; i < tail; ++i) full[8 - tail + i] = groups[compress + i];
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 2] = (proven_byte_t)(full[i] >> 8);
        out[i * 2 + 1] = (proven_byte_t)(full[i] & 0xff);
    }
    return true;
}

proven_err_t proven_net_addr_parse(proven_u8str_view_t text, proven_u16 port, proven_net_addr_t *out) {
    if (!out || (text.size > 0 && !text.ptr)) return PROVEN_ERR_INVALID_ARG;
    const proven_byte_t *p = text.ptr;
    proven_size_t n = text.size;
    if (n == 0) return PROVEN_ERR_INVALID_FORMAT;

    proven_byte_t v4[4];
    if (internal_parse_ipv4(p, n, v4)) {
        *out = proven_net_addr_ipv4(v4[0], v4[1], v4[2], v4[3], port);
        return PROVEN_OK;
    }

    if (p[0] == '[') {
        if (n < 2 || p[n - 1] != ']') return PROVEN_ERR_INVALID_FORMAT;
        p += 1;
        n -= 2;
    }
    /* An optional zone: '%' and a decimal interface index. Names ("%eth0") are a lookup, which
     * a parser that never touches the system cannot do. */
    proven_u32 scope = 0;
    for (proven_size_t i = 0; i < n; ++i) {
        if (p[i] != '%') continue;
        proven_size_t digits = n - i - 1;
        if (digits == 0 || digits > 10) return PROVEN_ERR_INVALID_FORMAT;
        proven_u64 v = 0;
        for (proven_size_t k = i + 1; k < n; ++k) {
            if (p[k] < '0' || p[k] > '9') return PROVEN_ERR_INVALID_FORMAT;
            v = v * 10u + (proven_u64)(p[k] - '0');
        }
        if (v > 0xffffffffull) return PROVEN_ERR_INVALID_FORMAT;
        scope = (proven_u32)v;
        n = i;
        break;
    }
    proven_byte_t v6[16];
    if (!internal_parse_ipv6(p, n, v6)) return PROVEN_ERR_INVALID_FORMAT;
    *out = proven_net_addr_ipv6(v6, port, scope);
    return PROVEN_OK;
}

typedef struct {
    proven_byte_t *ptr;
    proven_size_t cap;
    proven_size_t len;
    bool overflow;
} internal_text_t;

static void internal_put(internal_text_t *t, char c) {
    if (t->len < t->cap) t->ptr[t->len++] = (proven_byte_t)c;
    else t->overflow = true;
}

static void internal_put_dec(internal_text_t *t, proven_u32 v) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n > 0) internal_put(t, tmp[--n]);
}

static void internal_put_hex16(internal_text_t *t, proven_u16 v) {
    static const char d[] = "0123456789abcdef";
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        unsigned nib = (v >> shift) & 0xfu;
        if (nib || started || shift == 0) { internal_put(t, d[nib]); started = true; }
    }
}

static void internal_put_ipv4(internal_text_t *t, const proven_byte_t ip[4]) {
    for (int i = 0; i < 4; ++i) {
        if (i) internal_put(t, '.');
        internal_put_dec(t, ip[i]);
    }
}

/* RFC 5952: lowercase, no leading zeros, and the longest run of two or more zero groups
 * written as "::" - the first such run when two are equally long. */
static void internal_put_ipv6(internal_text_t *t, const proven_byte_t ip[16]) {
    proven_u16 g[8];
    for (int i = 0; i < 8; ++i) g[i] = (proven_u16)(((proven_u16)ip[i * 2] << 8) | ip[i * 2 + 1]);

    /* ::ffff:a.b.c.d - an IPv4 address carried in IPv6 - reads better with its dotted tail. */
    bool mapped = g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xffffu;
    int limit = mapped ? 6 : 8;

    int best = -1, best_len = 0;
    for (int i = 0; i < limit;) {
        if (g[i] != 0) { i++; continue; }
        int j = i;
        while (j < limit && g[j] == 0) j++;
        if (j - i >= 2 && j - i > best_len) { best = i; best_len = j - i; }
        i = j;
    }
    for (int i = 0; i < limit;) {
        if (i == best) {
            internal_put(t, ':');
            internal_put(t, ':');
            i += best_len;
            continue;
        }
        if (i > 0 && i != best + best_len) internal_put(t, ':');
        internal_put_hex16(t, g[i]);
        i++;
    }
    if (mapped) {
        if (best + best_len != limit) internal_put(t, ':');
        internal_put_ipv4(t, ip + 12);
    }
}

proven_err_t proven_net_addr_format(const proven_net_addr_t *addr, proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!addr || !written || (out.size > 0 && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    internal_text_t t = { .ptr = out.ptr, .cap = out.size, .len = 0, .overflow = false };
    switch (addr->family) {
        case PROVEN_NET_FAMILY_IPV4:
            internal_put_ipv4(&t, addr->ip);
            internal_put(&t, ':');
            internal_put_dec(&t, addr->port);
            break;
        case PROVEN_NET_FAMILY_IPV6:
            internal_put(&t, '[');
            internal_put_ipv6(&t, addr->ip);
            if (addr->scope_id != 0) {
                internal_put(&t, '%');
                internal_put_dec(&t, addr->scope_id);
            }
            internal_put(&t, ']');
            internal_put(&t, ':');
            internal_put_dec(&t, addr->port);
            break;
        case PROVEN_NET_FAMILY_UNIX:
            for (proven_size_t i = 0; i < addr->path_len; ++i) internal_put(&t, addr->path[i]);
            break;
        default:
            return PROVEN_ERR_INVALID_ARG;
    }
    if (t.overflow) return PROVEN_ERR_OUT_OF_BOUNDS;
    *written = t.len;
    return PROVEN_OK;
}

bool proven_net_addr_eq(const proven_net_addr_t *a, const proven_net_addr_t *b) {
    if (!a || !b || a->family != b->family) return false;
    switch (a->family) {
        case PROVEN_NET_FAMILY_IPV4:
            if (a->port != b->port) return false;
            for (int i = 0; i < 4; ++i) if (a->ip[i] != b->ip[i]) return false;
            return true;
        case PROVEN_NET_FAMILY_IPV6:
            if (a->port != b->port || a->scope_id != b->scope_id) return false;
            for (int i = 0; i < 16; ++i) if (a->ip[i] != b->ip[i]) return false;
            return true;
        case PROVEN_NET_FAMILY_UNIX:
            if (a->path_len != b->path_len) return false;
            for (proven_size_t i = 0; i < a->path_len; ++i) if (a->path[i] != b->path[i]) return false;
            return true;
        default:
            return true;    /* two addresses of no family */
    }
}

proven_err_t proven_net_resolve(proven_u8str_view_t host, proven_u16 port,
                                proven_net_addr_t *out, proven_size_t cap, proven_size_t *count) {
    if (count) *count = 0;
    if (!count || (cap > 0 && !out) || (host.size > 0 && !host.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (host.size == 0 || host.size > 253) return PROVEN_ERR_INVALID_ARG;
    if (cap == 0) return PROVEN_ERR_OUT_OF_BOUNDS;

    /* A literal needs no resolver, and must not depend on one being reachable. */
    proven_net_addr_t literal;
    if (proven_net_addr_parse(host, port, &literal) == PROVEN_OK) {
        out[0] = literal;
        *count = 1;
        return PROVEN_OK;
    }

    char name[254];
    for (proven_size_t i = 0; i < host.size; ++i) {
        if (host.ptr[i] == 0) return PROVEN_ERR_INVALID_ARG;
        name[i] = (char)host.ptr[i];
    }
    name[host.size] = '\0';

    /* The resolver may return more than the caller has room for; a fixed chunk on the stack
     * bounds this call without allocating. */
    proven_sys_net_addr_t found[16];
    proven_size_t want = cap < 16 ? cap : 16;
    proven_size_t n = 0;
    proven_sys_net_result_t r = proven_sys_net_resolve(name, port, found, want, &n);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    for (proven_size_t i = 0; i < n; ++i) internal_addr_from_sys(&found[i], &out[i]);
    *count = n;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Deadlines and the wait
// -----------------------------------------------------------------------------

proven_net_deadline_t proven_net_deadline_in(proven_u32 ms) {
    return proven_time_monotonic_now() + (proven_time_t)ms * 1000000;
}

/* Milliseconds left until `until`: -1 for no deadline, 0 when it has passed. Rounded up, so a
 * wait never returns a millisecond early and spins. */
static int internal_remaining_ms(proven_net_deadline_t until, bool *expired) {
    *expired = false;
    if (until == PROVEN_NET_NO_DEADLINE) return -1;
    if (until == PROVEN_NET_DONT_WAIT) { *expired = true; return 0; }
    proven_time_t now = proven_time_monotonic_now();
    if (now >= until) { *expired = true; return 0; }
    proven_i64 left = until - now;
    proven_i64 ms = left / 1000000 + ((left % 1000000) != 0 ? 1 : 0);
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    return (int)ms;
}

/* Wait for one socket. PROVEN_OK: something happened on it - try the operation again, which
 * will say what. PROVEN_ERR_TIMEOUT: the deadline passed. */
static proven_err_t internal_wait_one(proven_uintptr_t handle, proven_u8 want, proven_net_deadline_t until) {
    for (;;) {
        bool expired = false;
        int ms = internal_remaining_ms(until, &expired);
        if (expired) return PROVEN_ERR_TIMEOUT;
        proven_sys_net_wait_t item = { .sock = (proven_sys_socket_t)handle, .want = want, .got = 0 };
        alignas(16) proven_byte_t scratch[32];
        proven_size_t ready = 0;
        proven_sys_net_result_t r = proven_sys_net_wait(&item, 1, scratch, ms, &ready);
        if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
        if (ready > 0) return PROVEN_OK;
        /* Nothing yet: the time ran out, or the wait was interrupted. The loop re-reads the
         * clock and decides which. */
    }
}

// -----------------------------------------------------------------------------
// Stream sockets
// -----------------------------------------------------------------------------

proven_err_t proven_net_listen(proven_net_addr_t at, int backlog,
                               proven_net_listener_t *out, proven_net_addr_t *bound) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_listener_t){0};
    if (at.family == PROVEN_NET_FAMILY_NONE) return PROVEN_ERR_INVALID_ARG;

    proven_sys_net_addr_t sys;
    internal_addr_to_sys(&at, &sys);
    proven_sys_socket_t s = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_open_listener(&sys, backlog, &s);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);

    if (bound) {
        proven_sys_net_addr_t actual;
        r = proven_sys_net_local_addr(s, &actual);
        if (r != PROVEN_SYS_NET_OK) {
            (void)proven_sys_net_close(s);
            return internal_err_from_reason(r);
        }
        internal_addr_from_sys(&actual, bound);
    }
    out->internal.handle = (proven_uintptr_t)s;
    out->internal.family = (proven_u8)at.family;
    out->internal.open = true;
    return PROVEN_OK;
}

proven_err_t proven_net_accept(proven_net_listener_t *listener, proven_net_deadline_t until,
                               proven_net_conn_t *out, proven_net_addr_t *peer) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_conn_t){0};
    if (!proven_net_listener_is_open(listener)) return PROVEN_ERR_INVALID_STATE;
    for (;;) {
        proven_sys_socket_t s = PROVEN_SYS_SOCKET_INVALID;
        proven_sys_net_addr_t from;
        proven_sys_net_result_t r = proven_sys_net_accept((proven_sys_socket_t)listener->internal.handle, &s, &from);
        if (r == PROVEN_SYS_NET_OK) {
            out->internal.handle = (proven_uintptr_t)s;
            out->internal.family = listener->internal.family;
            out->internal.open = true;
            if (peer) internal_addr_from_sys(&from, peer);
            return PROVEN_OK;
        }
        if (r != PROVEN_SYS_NET_WOULD_BLOCK) return internal_err_from_reason(r);
        proven_err_t w = internal_wait_one(listener->internal.handle, PROVEN_SYS_NET_READABLE, until);
        if (w != PROVEN_OK) return w;
    }
}

proven_err_t proven_net_listener_close(proven_net_listener_t *listener) {
    if (!listener) return PROVEN_ERR_INVALID_ARG;
    if (!listener->internal.open) return PROVEN_OK;
    proven_sys_socket_t s = (proven_sys_socket_t)listener->internal.handle;
    *listener = (proven_net_listener_t){0};
    return internal_err_from_reason(proven_sys_net_close(s));
}

proven_err_t proven_net_listener_addr(const proven_net_listener_t *listener, proven_net_addr_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    if (!proven_net_listener_is_open(listener)) return PROVEN_ERR_INVALID_STATE;
    proven_sys_net_addr_t sys;
    proven_sys_net_result_t r = proven_sys_net_local_addr((proven_sys_socket_t)listener->internal.handle, &sys);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    internal_addr_from_sys(&sys, out);
    return PROVEN_OK;
}

proven_err_t proven_net_connect(proven_net_addr_t to, proven_net_deadline_t until, proven_net_conn_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_conn_t){0};
    if (to.family == PROVEN_NET_FAMILY_NONE) return PROVEN_ERR_INVALID_ARG;

    proven_sys_net_addr_t sys;
    internal_addr_to_sys(&to, &sys);
    proven_sys_socket_t s = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_connect_start(&sys, &s);
    while (r == PROVEN_SYS_NET_IN_PROGRESS) {
        proven_err_t w = internal_wait_one((proven_uintptr_t)s, PROVEN_SYS_NET_WRITABLE, until);
        if (w != PROVEN_OK) {
            (void)proven_sys_net_close(s);
            return w;
        }
        r = proven_sys_net_connect_result(s);
        if (r != PROVEN_SYS_NET_OK && r != PROVEN_SYS_NET_IN_PROGRESS) (void)proven_sys_net_close(s);
    }
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);

    out->internal.handle = (proven_uintptr_t)s;
    out->internal.family = (proven_u8)to.family;
    out->internal.open = true;
    return PROVEN_OK;
}

proven_err_t proven_net_connect_start(proven_net_addr_t to, proven_net_conn_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_conn_t){0};
    if (to.family == PROVEN_NET_FAMILY_NONE) return PROVEN_ERR_INVALID_ARG;

    proven_sys_net_addr_t sys;
    internal_addr_to_sys(&to, &sys);
    proven_sys_socket_t s = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_connect_start(&sys, &s);
    if (r != PROVEN_SYS_NET_OK && r != PROVEN_SYS_NET_IN_PROGRESS) return internal_err_from_reason(r);

    out->internal.handle = (proven_uintptr_t)s;
    out->internal.family = (proven_u8)to.family;
    out->internal.open = true;
    return r == PROVEN_SYS_NET_OK ? PROVEN_OK : PROVEN_ERR_AGAIN;
}

proven_err_t proven_net_connect_finish(proven_net_conn_t *conn) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (!conn->internal.open) return PROVEN_ERR_INVALID_STATE;
    proven_sys_socket_t s = (proven_sys_socket_t)conn->internal.handle;
    proven_sys_net_result_t r = proven_sys_net_connect_result(s);
    if (r == PROVEN_SYS_NET_OK) return PROVEN_OK;
    if (r == PROVEN_SYS_NET_IN_PROGRESS) return PROVEN_ERR_AGAIN;
    (void)proven_sys_net_close(s);
    *conn = (proven_net_conn_t){0};
    return internal_err_from_reason(r);
}

proven_result_size_t proven_net_read(proven_net_conn_t *conn, proven_mem_mut_t dest, proven_net_deadline_t until) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!conn || (dest.size > 0 && !dest.ptr)) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    if (!conn->internal.open) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    if (dest.size == 0) return res;
    for (;;) {
        proven_size_t n = 0;
        proven_sys_net_result_t r = proven_sys_net_recv((proven_sys_socket_t)conn->internal.handle, dest.ptr, dest.size, &n);
        if (r == PROVEN_SYS_NET_OK) { res.value = n; return res; }
        if (r != PROVEN_SYS_NET_WOULD_BLOCK) { res.err = internal_err_from_reason(r); return res; }
        proven_err_t w = internal_wait_one(conn->internal.handle, PROVEN_SYS_NET_READABLE, until);
        if (w != PROVEN_OK) { res.err = w; return res; }
    }
}

static proven_result_size_t internal_write(proven_net_conn_t *conn, proven_mem_view_t src,
                                           proven_net_deadline_t until, bool all) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!conn || (src.size > 0 && !src.ptr)) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    if (!conn->internal.open) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    while (res.value < src.size) {
        proven_size_t n = 0;
        proven_sys_net_result_t r = proven_sys_net_send((proven_sys_socket_t)conn->internal.handle,
                                                        src.ptr + res.value, src.size - res.value, &n);
        if (r == PROVEN_SYS_NET_OK) {
            res.value += n;
            if (!all && n > 0) return res;
            if (n > 0) continue;
            r = PROVEN_SYS_NET_WOULD_BLOCK;     /* accepted nothing: wait for room */
        }
        if (r != PROVEN_SYS_NET_WOULD_BLOCK) { res.err = internal_err_from_reason(r); return res; }
        proven_err_t w = internal_wait_one(conn->internal.handle, PROVEN_SYS_NET_WRITABLE, until);
        if (w != PROVEN_OK) { res.err = w; return res; }
    }
    return res;
}

proven_result_size_t proven_net_write(proven_net_conn_t *conn, proven_mem_view_t src, proven_net_deadline_t until) {
    return internal_write(conn, src, until, false);
}

proven_result_size_t proven_net_write_all(proven_net_conn_t *conn, proven_mem_view_t src, proven_net_deadline_t until) {
    return internal_write(conn, src, until, true);
}

proven_err_t proven_net_shutdown_write(proven_net_conn_t *conn) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (!conn->internal.open) return PROVEN_ERR_INVALID_STATE;
    return internal_err_from_reason(proven_sys_net_shutdown_write((proven_sys_socket_t)conn->internal.handle));
}

proven_err_t proven_net_close(proven_net_conn_t *conn) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (!conn->internal.open) return PROVEN_OK;
    proven_sys_socket_t s = (proven_sys_socket_t)conn->internal.handle;
    *conn = (proven_net_conn_t){0};
    return internal_err_from_reason(proven_sys_net_close(s));
}

proven_err_t proven_net_conn_local_addr(const proven_net_conn_t *conn, proven_net_addr_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    if (!proven_net_conn_is_open(conn)) return PROVEN_ERR_INVALID_STATE;
    proven_sys_net_addr_t sys;
    proven_sys_net_result_t r = proven_sys_net_local_addr((proven_sys_socket_t)conn->internal.handle, &sys);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    internal_addr_from_sys(&sys, out);
    return PROVEN_OK;
}

proven_err_t proven_net_conn_peer_addr(const proven_net_conn_t *conn, proven_net_addr_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    if (!proven_net_conn_is_open(conn)) return PROVEN_ERR_INVALID_STATE;
    proven_sys_net_addr_t sys;
    proven_sys_net_result_t r = proven_sys_net_peer_addr((proven_sys_socket_t)conn->internal.handle, &sys);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    internal_addr_from_sys(&sys, out);
    return PROVEN_OK;
}

proven_err_t proven_net_conn_set_nodelay(proven_net_conn_t *conn, bool on) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (!conn->internal.open) return PROVEN_ERR_INVALID_STATE;
    if (conn->internal.family == PROVEN_NET_FAMILY_UNIX) return PROVEN_ERR_UNSUPPORTED;
    return internal_err_from_reason(proven_sys_net_set_nodelay((proven_sys_socket_t)conn->internal.handle, on));
}

proven_err_t proven_net_pair(proven_net_conn_t *a, proven_net_conn_t *b) {
    if (!a || !b) return PROVEN_ERR_INVALID_ARG;
    *a = (proven_net_conn_t){0};
    *b = (proven_net_conn_t){0};
    proven_sys_socket_t sa = PROVEN_SYS_SOCKET_INVALID, sb = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_pair(&sa, &sb);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    /* The family decides only whether TCP_NODELAY applies; the pair is addressless either way. */
#if defined(_WIN32) || defined(_WIN64)
    proven_u8 family = (proven_u8)PROVEN_NET_FAMILY_IPV4;
#else
    proven_u8 family = (proven_u8)PROVEN_NET_FAMILY_UNIX;
#endif
    a->internal.handle = (proven_uintptr_t)sa; a->internal.family = family; a->internal.open = true;
    b->internal.handle = (proven_uintptr_t)sb; b->internal.family = family; b->internal.open = true;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Datagram sockets
// -----------------------------------------------------------------------------

proven_err_t proven_net_udp_open(proven_net_addr_t at, proven_net_udp_t *out, proven_net_addr_t *bound) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_udp_t){0};
    if (at.family == PROVEN_NET_FAMILY_NONE) return PROVEN_ERR_INVALID_ARG;

    proven_sys_net_addr_t sys;
    internal_addr_to_sys(&at, &sys);
    proven_sys_socket_t s = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_open_datagram(&sys, &s);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    if (bound) {
        proven_sys_net_addr_t actual;
        r = proven_sys_net_local_addr(s, &actual);
        if (r != PROVEN_SYS_NET_OK) {
            (void)proven_sys_net_close(s);
            return internal_err_from_reason(r);
        }
        internal_addr_from_sys(&actual, bound);
    }
    out->internal.handle = (proven_uintptr_t)s;
    out->internal.family = (proven_u8)at.family;
    out->internal.open = true;
    return PROVEN_OK;
}

proven_err_t proven_net_udp_send_to(proven_net_udp_t *udp, proven_net_addr_t to, proven_mem_view_t data,
                                    proven_net_deadline_t until) {
    if (!udp || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (!udp->internal.open) return PROVEN_ERR_INVALID_STATE;
    proven_sys_net_addr_t sys;
    internal_addr_to_sys(&to, &sys);
    for (;;) {
        proven_sys_net_result_t r = proven_sys_net_send_to((proven_sys_socket_t)udp->internal.handle,
                                                           data.ptr, data.size, &sys);
        if (r != PROVEN_SYS_NET_WOULD_BLOCK) return internal_err_from_reason(r);
        proven_err_t w = internal_wait_one(udp->internal.handle, PROVEN_SYS_NET_WRITABLE, until);
        if (w != PROVEN_OK) return w;
    }
}

proven_result_size_t proven_net_udp_recv_from(proven_net_udp_t *udp, proven_mem_mut_t dest,
                                              proven_net_addr_t *from, proven_net_deadline_t until) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!udp || (dest.size > 0 && !dest.ptr)) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    if (!udp->internal.open) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    for (;;) {
        proven_size_t n = 0;
        proven_sys_net_addr_t sys;
        proven_sys_net_result_t r = proven_sys_net_recv_from((proven_sys_socket_t)udp->internal.handle,
                                                             dest.ptr, dest.size, &n, &sys);
        if (r == PROVEN_SYS_NET_OK || r == PROVEN_SYS_NET_TRUNCATED) {
            res.value = n;
            res.err = internal_err_from_reason(r);
            if (from) internal_addr_from_sys(&sys, from);
            return res;
        }
        if (r != PROVEN_SYS_NET_WOULD_BLOCK) { res.err = internal_err_from_reason(r); return res; }
        proven_err_t w = internal_wait_one(udp->internal.handle, PROVEN_SYS_NET_READABLE, until);
        if (w != PROVEN_OK) { res.err = w; return res; }
    }
}

proven_err_t proven_net_udp_addr(const proven_net_udp_t *udp, proven_net_addr_t *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    if (!proven_net_udp_is_open(udp)) return PROVEN_ERR_INVALID_STATE;
    proven_sys_net_addr_t sys;
    proven_sys_net_result_t r = proven_sys_net_local_addr((proven_sys_socket_t)udp->internal.handle, &sys);
    if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
    internal_addr_from_sys(&sys, out);
    return PROVEN_OK;
}

proven_err_t proven_net_udp_close(proven_net_udp_t *udp) {
    if (!udp) return PROVEN_ERR_INVALID_ARG;
    if (!udp->internal.open) return PROVEN_OK;
    proven_sys_socket_t s = (proven_sys_socket_t)udp->internal.handle;
    *udp = (proven_net_udp_t){0};
    return internal_err_from_reason(proven_sys_net_close(s));
}

// -----------------------------------------------------------------------------
// Readiness
// -----------------------------------------------------------------------------

proven_net_handle_t proven_net_listener_handle(const proven_net_listener_t *listener) {
    proven_net_handle_t h = {0};
    if (proven_net_listener_is_open(listener)) { h.raw = listener->internal.handle; h.valid = true; }
    return h;
}

proven_net_handle_t proven_net_conn_handle(const proven_net_conn_t *conn) {
    proven_net_handle_t h = {0};
    if (proven_net_conn_is_open(conn)) { h.raw = conn->internal.handle; h.valid = true; }
    return h;
}

proven_net_handle_t proven_net_udp_handle(const proven_net_udp_t *udp) {
    proven_net_handle_t h = {0};
    if (proven_net_udp_is_open(udp)) { h.raw = udp->internal.handle; h.valid = true; }
    return h;
}

proven_err_t proven_net_waker_open(proven_net_waker_t *waker) {
    if (!waker) return PROVEN_ERR_INVALID_ARG;
    *waker = (proven_net_waker_t){0};
    return proven_net_pair(&waker->reader, &waker->writer);
}

void proven_net_waker_wake(proven_net_waker_t *waker) {
    if (!waker || !waker->writer.internal.open) return;
    /* One byte, without waiting. If the pipe is full the reader has plenty to wake up for
     * already, so "would block" is success. The platform send is safe to call from several
     * threads on one socket. */
    static const proven_byte_t one = 1;
    proven_size_t sent = 0;
    (void)proven_sys_net_send((proven_sys_socket_t)waker->writer.internal.handle, &one, 1, &sent);
}

void proven_net_waker_drain(proven_net_waker_t *waker) {
    if (!waker || !waker->reader.internal.open) return;
    proven_byte_t sink[64];
    for (;;) {
        proven_size_t n = 0;
        proven_sys_net_result_t r = proven_sys_net_recv((proven_sys_socket_t)waker->reader.internal.handle, sink, sizeof sink, &n);
        if (r != PROVEN_SYS_NET_OK || n < sizeof sink) return;
    }
}

proven_net_handle_t proven_net_waker_handle(const proven_net_waker_t *waker) {
    proven_net_handle_t none = {0};
    return waker ? proven_net_conn_handle(&waker->reader) : none;
}

void proven_net_waker_close(proven_net_waker_t *waker) {
    if (!waker) return;
    (void)proven_net_close(&waker->reader);
    (void)proven_net_close(&waker->writer);
}

/* The scratch holds two arrays: this layer's list of sockets for the platform unit, and the
 * platform unit's own array for the OS. 16 bytes of slack let any pointer be aligned. */
#define INTERNAL_POLL_ALIGN ((proven_size_t)16)

proven_size_t proven_net_poll_scratch_size(proven_size_t count) {
    proven_size_t native = proven_sys_net_wait_scratch_size(count);
    if (native == PROVEN_SIZE_MAX) return PROVEN_SIZE_MAX;
    if (count > (PROVEN_SIZE_MAX - native - 2 * INTERNAL_POLL_ALIGN) / sizeof(proven_sys_net_wait_t)) {
        return PROVEN_SIZE_MAX;
    }
    return count * sizeof(proven_sys_net_wait_t) + native + 2 * INTERNAL_POLL_ALIGN;
}

static proven_byte_t *internal_align_up(proven_byte_t *p) {
    proven_uintptr_t v = (proven_uintptr_t)p;
    proven_uintptr_t pad = (INTERNAL_POLL_ALIGN - (v % INTERNAL_POLL_ALIGN)) % INTERNAL_POLL_ALIGN;
    return p + pad;
}

proven_err_t proven_net_poll_with(proven_mem_mut_t scratch, proven_net_poll_item_t *items, proven_size_t count,
                                  proven_net_deadline_t until, proven_size_t *ready) {
    if (ready) *ready = 0;
    if (!ready || (count > 0 && !items)) return PROVEN_ERR_INVALID_ARG;
    proven_size_t need = proven_net_poll_scratch_size(count);
    if (need == PROVEN_SIZE_MAX || scratch.size < need || (need > 0 && !scratch.ptr)) return PROVEN_ERR_OUT_OF_BOUNDS;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!items[i].handle.valid) return PROVEN_ERR_INVALID_ARG;
        items[i].got = 0;
    }

    proven_byte_t *first = internal_align_up(scratch.ptr);
    proven_sys_net_wait_t *list = (proven_sys_net_wait_t *)(void *)first;
    proven_byte_t *native = internal_align_up(first + count * sizeof(proven_sys_net_wait_t));

    for (;;) {
        bool expired = false;
        int ms = internal_remaining_ms(until, &expired);
        /* An expired deadline still gets one look that does not wait: "is anything ready right
         * now" is a fair question to ask with PROVEN_NET_DONT_WAIT. */
        if (expired) ms = 0;
        for (proven_size_t i = 0; i < count; ++i) {
            list[i].sock = (proven_sys_socket_t)items[i].handle.raw;
            list[i].want = (proven_u8)(items[i].want & (PROVEN_NET_READABLE | PROVEN_NET_WRITABLE));
            list[i].got = 0;
        }
        proven_size_t n = 0;
        proven_sys_net_result_t r = proven_sys_net_wait(list, count, native, ms, &n);
        if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
        if (n > 0) {
            for (proven_size_t i = 0; i < count; ++i) items[i].got = list[i].got;
            *ready = n;
            return PROVEN_OK;
        }
        if (expired) return PROVEN_ERR_TIMEOUT;
    }
}

proven_err_t proven_net_poll(proven_net_poll_item_t *items, proven_size_t count,
                             proven_net_deadline_t until, proven_size_t *ready) {
    if (ready) *ready = 0;
    if (count > PROVEN_NET_POLL_INLINE_MAX) return PROVEN_ERR_OUT_OF_BOUNDS;
    /* Room for PROVEN_NET_POLL_INLINE_MAX items on every platform this builds for; the size is
     * checked against the real requirement by proven_net_poll_with. */
    alignas(16) proven_byte_t scratch[64 * 48 + 32];
    return proven_net_poll_with((proven_mem_mut_t){ .ptr = scratch, .size = sizeof scratch }, items, count, until, ready);
}

// -----------------------------------------------------------------------------
// Transport
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Selector
// -----------------------------------------------------------------------------

/* One socket of a poll-kind selector. */
typedef struct {
    proven_uintptr_t raw;
    void *tag;
    proven_u8 want;
} sel_item_t;

struct proven_net_selector {
    proven_allocator_t alloc;
    proven_size_t count;
    proven_uintptr_t sys;               /* the kernel object, when native */
    bool native;
    /* The poll kind: the list, a hash from socket to its place in the list, and the memory
     * the wait needs. All three grow together. */
    sel_item_t *items;
    proven_u32 *slots;                  /* place + 1; 0 is empty */
    proven_byte_t *scratch;
    proven_size_t cap;                  /* items */
    proven_size_t slot_count;           /* a power of two, at least twice cap */
    proven_size_t scratch_size;
    proven_size_t next;                 /* where the last report stopped, so nobody is starved */
};

static void *sel_alloc(proven_allocator_t a, proven_size_t size) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, size ? size : 1, 16);
    return proven_is_ok(m.err) ? (void *)m.value.ptr : (void *)0;
}

static proven_size_t sel_hash(proven_uintptr_t raw, proven_size_t slot_count) {
    proven_u64 h = (proven_u64)raw * 0x9E3779B97F4A7C15ull;
    return (proven_size_t)(h >> 32) & (slot_count - 1);
}

/* The slot that holds `raw`, or the empty slot where it would go. */
static proven_size_t sel_find(const proven_net_selector_t *s, proven_uintptr_t raw, bool *found) {
    proven_size_t i = sel_hash(raw, s->slot_count);
    for (;;) {
        proven_u32 v = s->slots[i];
        if (v == 0) { *found = false; return i; }
        if (s->items[v - 1].raw == raw) { *found = true; return i; }
        i = (i + 1) & (s->slot_count - 1);
    }
}

static proven_err_t sel_grow(proven_net_selector_t *s) {
    proven_size_t cap = s->cap ? s->cap * 2 : 64;
    proven_size_t slot_count = cap * 2;
    proven_size_t wait_bytes = proven_net_poll_scratch_size(cap);
    if (cap > 0x7fffffffu || wait_bytes == PROVEN_SIZE_MAX) return PROVEN_ERR_OVERFLOW;
    sel_item_t *items = sel_alloc(s->alloc, cap * sizeof *items);
    proven_u32 *slots = sel_alloc(s->alloc, slot_count * sizeof *slots);
    proven_byte_t *scratch = sel_alloc(s->alloc, wait_bytes);
    if (!items || !slots || !scratch) {
        if (items) s->alloc.free_fn(s->alloc.ctx, items);
        if (slots) s->alloc.free_fn(s->alloc.ctx, slots);
        if (scratch) s->alloc.free_fn(s->alloc.ctx, scratch);
        return PROVEN_ERR_NOMEM;
    }
    for (proven_size_t i = 0; i < s->count; ++i) items[i] = s->items[i];
    for (proven_size_t i = 0; i < slot_count; ++i) slots[i] = 0;
    if (s->items) s->alloc.free_fn(s->alloc.ctx, s->items);
    if (s->slots) s->alloc.free_fn(s->alloc.ctx, s->slots);
    if (s->scratch) s->alloc.free_fn(s->alloc.ctx, s->scratch);
    s->items = items;
    s->slots = slots;
    s->scratch = scratch;
    s->cap = cap;
    s->slot_count = slot_count;
    s->scratch_size = wait_bytes;
    for (proven_size_t i = 0; i < s->count; ++i) {
        bool found;
        proven_size_t at = sel_find(s, items[i].raw, &found);
        s->slots[at] = (proven_u32)(i + 1);
    }
    return PROVEN_OK;
}

proven_err_t proven_net_selector_create_poll(proven_allocator_t alloc, proven_net_selector_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc)) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = sel_alloc(alloc, sizeof *s);
    if (!s) return PROVEN_ERR_NOMEM;
    *s = (proven_net_selector_t){0};
    s->alloc = alloc;
    *out = s;
    return PROVEN_OK;
}

proven_err_t proven_net_selector_create(proven_allocator_t alloc, proven_net_selector_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc)) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = sel_alloc(alloc, sizeof *s);
    if (!s) return PROVEN_ERR_NOMEM;
    *s = (proven_net_selector_t){0};
    s->alloc = alloc;
    proven_sys_net_result_t r = proven_sys_net_selector_open(&s->sys);
    if (r == PROVEN_SYS_NET_OK) {
        s->native = true;
    } else if (r != PROVEN_SYS_NET_UNSUPPORTED) {
        alloc.free_fn(alloc.ctx, s);
        return internal_err_from_reason(r);
    }
    *out = s;
    return PROVEN_OK;
}

void proven_net_selector_destroy(proven_net_selector_t *selector) {
    if (!selector) return;
    proven_allocator_t a = selector->alloc;
    if (selector->native) proven_sys_net_selector_close(selector->sys);
    if (selector->items) a.free_fn(a.ctx, selector->items);
    if (selector->slots) a.free_fn(a.ctx, selector->slots);
    if (selector->scratch) a.free_fn(a.ctx, selector->scratch);
    a.free_fn(a.ctx, selector);
}

proven_net_selector_kind_t proven_net_selector_kind(const proven_net_selector_t *selector) {
    if (!selector || !selector->native) return PROVEN_NET_SELECTOR_POLL;
    return proven_sys_net_selector_kind() == PROVEN_SYS_NET_SELECTOR_KQUEUE ? PROVEN_NET_SELECTOR_KQUEUE : PROVEN_NET_SELECTOR_EPOLL;
}

proven_size_t proven_net_selector_count(const proven_net_selector_t *selector) {
    return selector ? selector->count : 0;
}

proven_err_t proven_net_selector_add(proven_net_selector_t *selector, proven_net_handle_t handle, proven_u8 want, void *tag) {
    if (!selector || !handle.valid) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = selector;
    want &= (proven_u8)(PROVEN_NET_READABLE | PROVEN_NET_WRITABLE);
    if (s->native) {
        proven_sys_net_result_t r = proven_sys_net_selector_set(s->sys, (proven_sys_socket_t)handle.raw, want, tag, true);
        if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
        s->count++;
        return PROVEN_OK;
    }
    if (s->count == s->cap) {
        proven_err_t e = sel_grow(s);
        if (e != PROVEN_OK) return e;
    }
    bool found;
    proven_size_t at = sel_find(s, handle.raw, &found);
    if (found) return PROVEN_ERR_EXISTS;
    s->items[s->count] = (sel_item_t){ .raw = handle.raw, .tag = tag, .want = want };
    s->slots[at] = (proven_u32)(s->count + 1);
    s->count++;
    return PROVEN_OK;
}

proven_err_t proven_net_selector_modify(proven_net_selector_t *selector, proven_net_handle_t handle, proven_u8 want, void *tag) {
    if (!selector || !handle.valid) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = selector;
    want &= (proven_u8)(PROVEN_NET_READABLE | PROVEN_NET_WRITABLE);
    if (s->native) return internal_err_from_reason(proven_sys_net_selector_set(s->sys, (proven_sys_socket_t)handle.raw, want, tag, false));
    if (s->count == 0) return PROVEN_ERR_NOT_FOUND;
    bool found;
    proven_size_t at = sel_find(s, handle.raw, &found);
    if (!found) return PROVEN_ERR_NOT_FOUND;
    sel_item_t *it = &s->items[s->slots[at] - 1];
    it->want = want;
    it->tag = tag;
    return PROVEN_OK;
}

proven_err_t proven_net_selector_remove(proven_net_selector_t *selector, proven_net_handle_t handle) {
    if (!selector || !handle.valid) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = selector;
    if (s->native) {
        proven_sys_net_result_t r = proven_sys_net_selector_remove(s->sys, (proven_sys_socket_t)handle.raw);
        if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
        if (s->count > 0) s->count--;
        return PROVEN_OK;
    }
    if (s->count == 0) return PROVEN_ERR_NOT_FOUND;
    bool found;
    proven_size_t at = sel_find(s, handle.raw, &found);
    if (!found) return PROVEN_ERR_NOT_FOUND;
    proven_size_t place = s->slots[at] - 1;
    proven_size_t mask = s->slot_count - 1;

    /* Take the entry out of the hash and close the gap it leaves in its probe run. */
    proven_size_t hole = at;
    for (proven_size_t j = (at + 1) & mask; s->slots[j] != 0; j = (j + 1) & mask) {
        proven_size_t home = sel_hash(s->items[s->slots[j] - 1].raw, s->slot_count);
        /* Move j back into the hole unless its home lies strictly between the hole and j. */
        bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (!between) { s->slots[hole] = s->slots[j]; hole = j; }
    }
    s->slots[hole] = 0;

    /* The last item of the list takes the freed place. */
    proven_size_t last = s->count - 1;
    if (place != last) {
        s->items[place] = s->items[last];
        proven_size_t moved = sel_find(s, s->items[place].raw, &found);
        s->slots[moved] = (proven_u32)(place + 1);
    }
    s->count--;
    return PROVEN_OK;
}

proven_err_t proven_net_selector_wait(proven_net_selector_t *selector, proven_net_ready_t *events, proven_size_t cap,
                                      proven_net_deadline_t until, proven_size_t *count) {
    if (count) *count = 0;
    if (!selector || !events || !count || cap == 0) return PROVEN_ERR_INVALID_ARG;
    proven_net_selector_t *s = selector;
    for (;;) {
        bool expired = false;
        int ms = internal_remaining_ms(until, &expired);
        if (expired) ms = 0;

        if (s->native) {
            proven_sys_net_event_t got[64];
            proven_size_t n = 0;
            proven_sys_net_result_t r = proven_sys_net_selector_wait(s->sys, got, cap < 64 ? cap : 64, ms, &n);
            if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
            if (n > 0) {
                for (proven_size_t i = 0; i < n; ++i) events[i] = (proven_net_ready_t){ .tag = got[i].tag, .got = got[i].got };
                *count = n;
                return PROVEN_OK;
            }
        } else {
            proven_byte_t *first = s->count ? internal_align_up(s->scratch) : (void *)0;
            proven_sys_net_wait_t *list = (proven_sys_net_wait_t *)(void *)first;
            proven_byte_t *native = s->count ? internal_align_up(first + s->count * sizeof(proven_sys_net_wait_t)) : (void *)0;
            for (proven_size_t i = 0; i < s->count; ++i) {
                list[i].sock = (proven_sys_socket_t)s->items[i].raw;
                list[i].want = s->items[i].want;
                list[i].got = 0;
            }
            proven_size_t n = 0;
            proven_sys_net_result_t r = proven_sys_net_wait(list, s->count, native, ms, &n);
            if (r != PROVEN_SYS_NET_OK) return internal_err_from_reason(r);
            if (n > 0) {
                /* Start where the last report stopped: with more ready sockets than `cap`, the
                 * ones at the front of the list must not be the only ones ever reported. */
                proven_size_t start = s->next < s->count ? s->next : 0;
                proven_size_t out = 0, i = start;
                do {
                    if (list[i].got != 0) {
                        events[out++] = (proven_net_ready_t){ .tag = s->items[i].tag, .got = list[i].got };
                    }
                    i = i + 1 == s->count ? 0 : i + 1;
                } while (i != start && out < cap);
                s->next = i;
                *count = out;
                return PROVEN_OK;
            }
        }
        if (expired) return PROVEN_ERR_TIMEOUT;
    }
}

static proven_result_size_t internal_conn_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    return proven_net_read((proven_net_conn_t *)ctx, dest, until);
}

static proven_result_size_t internal_conn_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    return proven_net_write((proven_net_conn_t *)ctx, src, until);
}

static proven_err_t internal_conn_shutdown(void *ctx) {
    return proven_net_shutdown_write((proven_net_conn_t *)ctx);
}

static proven_err_t internal_conn_close(void *ctx) {
    return proven_net_close((proven_net_conn_t *)ctx);
}

proven_transport_t proven_net_conn_transport(proven_net_conn_t *conn) {
    proven_transport_t t = {0};
    if (!conn) return t;
    t.ctx = conn;
    t.read_fn = internal_conn_read;
    t.write_fn = internal_conn_write;
    t.shutdown_fn = internal_conn_shutdown;
    t.close_fn = internal_conn_close;
    return t;
}

proven_result_size_t proven_transport_read(proven_transport_t t, proven_mem_mut_t dest, proven_net_deadline_t until) {
    if (!proven_transport_is_valid(t)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    return t.read_fn(t.ctx, dest, until);
}

proven_result_size_t proven_transport_write(proven_transport_t t, proven_mem_view_t src, proven_net_deadline_t until) {
    if (!proven_transport_is_valid(t)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    return t.write_fn(t.ctx, src, until);
}

proven_result_size_t proven_transport_write_all(proven_transport_t t, proven_mem_view_t src, proven_net_deadline_t until) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!proven_transport_is_valid(t) || (src.size > 0 && !src.ptr)) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    while (res.value < src.size) {
        proven_mem_view_t rest = { .ptr = src.ptr + res.value, .size = src.size - res.value };
        proven_result_size_t r = t.write_fn(t.ctx, rest, until);
        res.value += r.value;
        if (!proven_is_ok(r.err)) { res.err = r.err; return res; }
        /* A transport that takes nothing and reports no error would loop here for ever. */
        if (r.value == 0) { res.err = PROVEN_ERR_IO; return res; }
    }
    return res;
}

proven_err_t proven_transport_shutdown(proven_transport_t t) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;
    return t.shutdown_fn ? t.shutdown_fn(t.ctx) : PROVEN_OK;
}

proven_err_t proven_transport_close(proven_transport_t t) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;
    return t.close_fn ? t.close_fn(t.ctx) : PROVEN_OK;
}

static proven_net_deadline_t internal_stream_deadline(const proven_transport_stream_t *s) {
    return s->timeout_ms == 0 ? PROVEN_NET_NO_DEADLINE : proven_net_deadline_in(s->timeout_ms);
}

static proven_result_size_t internal_stream_read(void *ctx, proven_mem_mut_t dest) {
    proven_transport_stream_t *s = ctx;
    return proven_transport_read(s->transport, dest, internal_stream_deadline(s));
}

static proven_result_size_t internal_stream_write(void *ctx, proven_mem_view_t chunk) {
    proven_transport_stream_t *s = ctx;
    return proven_transport_write(s->transport, chunk, internal_stream_deadline(s));
}

proven_reader_t proven_transport_reader(proven_transport_stream_t *state, proven_transport_t t, proven_u32 timeout_ms) {
    proven_reader_t r = {0};
    if (!state || !proven_transport_is_valid(t)) return r;
    state->transport = t;
    state->timeout_ms = timeout_ms;
    r.ctx = state;
    r.read_fn = internal_stream_read;
    return r;
}

proven_writer_t proven_transport_writer(proven_transport_stream_t *state, proven_transport_t t, proven_u32 timeout_ms) {
    proven_writer_t w = {0};
    if (!state || !proven_transport_is_valid(t)) return w;
    state->transport = t;
    state->timeout_ms = timeout_ms;
    w.ctx = state;
    w.write_fn = internal_stream_write;
    w.flush_fn = (void *)0;
    return w;
}

#else
/* A translation unit must not be empty. */
typedef int proven_net_unused_t;
#endif
