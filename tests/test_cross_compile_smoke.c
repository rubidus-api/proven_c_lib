#include <proven.h>

_Static_assert(PROVEN_VERSION_NUM >= 260516, "version macro must be available");
_Static_assert(sizeof(proven_byte_t) == 1, "byte type must be one byte");
_Static_assert(sizeof(proven_mem_view_t) >= sizeof(void *) + sizeof(proven_size_t), "memory view must expose pointer and size storage");

int proven_cross_compile_smoke(void) {
    proven_err_t err = PROVEN_OK;
    proven_mem_t mem = { .ptr = (proven_byte_t *)0, .size = 0 };
    proven_mem_view_t view = proven_mem_view_from_owned(mem);
    return (err == PROVEN_OK && view.ptr == (const proven_byte_t *)0 && view.size == 0) ? 0 : 1;
}
