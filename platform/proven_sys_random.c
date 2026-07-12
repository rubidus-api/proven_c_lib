#include "proven_sys_random.h"

/* Contract first, implementation next (docs/TESTING.md §5.1). Stub: always fails, so the
 * contract test - which asserts strong bytes on a hosted platform - lands red. */
bool proven_sys_random_bytes(void *buf, size_t len) {
    (void)buf; (void)len;
    return false;
}
