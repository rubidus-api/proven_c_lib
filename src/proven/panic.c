#include "proven/panic.h"

/*
 * The panic handler is dispatched through a function pointer rather than an
 * overridable weak symbol. The weak-symbol approach linked on ELF but not on
 * PE/COFF (mingw-w64): a weak function definition in a separate object failed
 * to satisfy references, breaking every Windows link. The registration model
 * below is portable across ELF and PE and remains freestanding-safe (the
 * pointer has a constant static initializer; no runtime startup is required).
 */

static void proven_panic_default(const char *msg) {
    (void)msg;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    while (1) {}
#endif
}

static proven_panic_handler_t g_panic_handler = proven_panic_default;

void proven_set_panic_handler(proven_panic_handler_t handler) {
    g_panic_handler = handler ? handler : proven_panic_default;
}

void proven_panic(const char *msg) {
    g_panic_handler(msg);
}
