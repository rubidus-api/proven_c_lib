#include "proven/panic.h"

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void proven_panic_handler(const char *msg) {
    (void)msg;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    while (1) {}
#endif
}
