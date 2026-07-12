#include "proven/random.h"
#include "../../platform/proven_sys_random.h"

bool proven_random_bytes(void *buf, proven_size_t len) {
    return proven_sys_random_bytes(buf, len);
}

proven_u64 proven_random_u64(void) {
    proven_u64 v = 0;
    if (!proven_random_bytes(&v, sizeof v)) return 0;
    return v;
}
