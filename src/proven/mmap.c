#include "proven/mmap.h"
#include "../../platform/proven_sys_fs.h"

proven_result_mmap_t proven_mmap_create(proven_fs_handle_t file, proven_size_t offset, proven_size_t size, proven_mmap_prot_t prot, proven_mmap_flags_t flags) {
    // If size is 0, we need to know the file size
    if (size == 0) {
        size = (proven_size_t)proven_sys_fs_size((proven_sys_file_handle_t){.internal = file.internal});
    }

    proven_sys_mmap_res_t res = proven_sys_fs_create((proven_sys_file_handle_t){.internal = file.internal}, (size_t)offset, (size_t)size, (int)prot, (int)flags);
    
    if (res.ptr == NULL) {
        return (proven_result_mmap_t){ .err = PROVEN_ERR_IO, .value = {0} };
    }

    proven_mmap_t m = {
        .ptr = res.ptr,
        .size = size,
        .file = file,
        .internal_handle = res.internal_handle
    };

    return (proven_result_mmap_t){ .err = PROVEN_OK, .value = m };
}

proven_err_t proven_mmap_destroy(proven_mmap_t *mmap) {
    if (!mmap || !mmap->ptr) return PROVEN_ERR_INVALID_ARG;

    if (!proven_sys_fs_destroy(mmap->ptr, (size_t)mmap->size, mmap->internal_handle)) {
        return PROVEN_ERR_IO;
    }

    mmap->ptr = NULL;
    mmap->size = 0;
    return PROVEN_OK;
}

proven_err_t proven_mmap_sync(proven_mmap_t *mmap) {
    if (!mmap || !mmap->ptr) return PROVEN_ERR_INVALID_ARG;

    if (!proven_sys_fs_sync(mmap->ptr, (size_t)mmap->size)) {
        return PROVEN_ERR_IO;
    }

    return PROVEN_OK;
}
