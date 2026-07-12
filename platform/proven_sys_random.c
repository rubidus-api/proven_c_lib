#include "proven_sys_random.h"

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

bool proven_sys_random_bytes(void *buf, size_t len) {
    if (len == 0) return true;
    if (!buf) return false;
    /* The modern, DLL-free Windows CSPRNG. BCRYPT_USE_SYSTEM_PREFERRED_RNG means no
     * algorithm handle to open or close. */
    NTSTATUS s = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return s == 0; /* STATUS_SUCCESS */
}

#else

#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(__linux__)
/* <sys/random.h> declares getrandom() on glibc 2.25+ and defines this guard macro; older
 * systems without it fall through to the /dev/urandom path below. */
#include <sys/random.h>
#if defined(GRND_NONBLOCK)
#define PROVEN_HAVE_GETRANDOM 1
#endif
#endif

bool proven_sys_random_bytes(void *buf, size_t len) {
    if (len == 0) return true;
    if (!buf) return false;

    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

#if defined(PROVEN_HAVE_GETRANDOM)
    /* getrandom() is the right call: it needs no fd, cannot be exhausted by an fd-leak, and
     * blocks only once, very early, until the pool is seeded - which is exactly when you
     * want it to wait rather than hand back weak bytes. Loop for short reads and retry EINTR. */
    while (got < len) {
        ssize_t n = getrandom(p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;   /* fall through to the /dev/urandom fallback for old kernels */
        }
        got += (size_t)n;
    }
    if (got == len) return true;
#endif

    /* Fallback for kernels/systems without getrandom: read /dev/urandom to completion. This
     * is also the path getentropy uses underneath on the BSDs. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) { close(fd); return false; }   /* EOF from /dev/urandom: cannot happen, but do not spin */
        got += (size_t)n;
    }
    close(fd);
    return true;
}

#endif
