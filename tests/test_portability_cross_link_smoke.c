/*
 * Cross link smoke.
 *
 * The cross matrix is otherwise compile-only, which misses link-time symbol
 * resolution differences between object formats. PE/COFF (mingw-w64) does not
 * resolve a weak function definition in a separate object the way ELF does, so
 * a compile-only check let the weak `proven_panic_handler` default pass while
 * every Windows link failed (fixed in v26.06.16x).
 *
 * This translation unit provides a `main` and exercises an `_or_panic` path so
 * the panic symbol must resolve at link time when the full proven object set is
 * linked. It is compiled and linked but never executed (cross targets are not
 * run on the build host).
 */

#include "proven/arena.h"

int proven_cross_compile_smoke(void);

int main(void) {
    proven_u8 buf[64];
    proven_mem_mut_t backing = { .ptr = buf, .size = sizeof buf };
    proven_arena_t arena = proven_arena_create(backing);
    proven_mem_mut_t m = proven_arena_alloc_or_panic(&arena, 16);
    (void)m;
    return proven_cross_compile_smoke();
}
