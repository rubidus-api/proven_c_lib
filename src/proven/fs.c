#include "proven/fs.h"
#include "../../platform/proven_sys_fs.h"
#include "proven/array.h"
#include "proven/algorithm.h"

typedef struct {
    proven_err_t err;
    char *value;
} internal_result_cstr_t;

static bool view_has_nul(proven_u8str_view_t view) {
    if (view.size > 0 && !view.ptr) return true;
    for (proven_size_t i = 0; i < view.size; ++i) {
        if (view.ptr[i] == 0) return true;
    }
    return false;
}

static internal_result_cstr_t internal_view_to_cstr(proven_allocator_t scratch, proven_u8str_view_t view) {
    if (!proven_alloc_is_valid(scratch)) return (internal_result_cstr_t){.err = PROVEN_ERR_INVALID_ARG};
    if (view_has_nul(view)) return (internal_result_cstr_t){.err = PROVEN_ERR_INVALID_ARG};
    proven_size_t cap;
    if (PROVEN_CKD_ADD(&cap, view.size, 1)) return (internal_result_cstr_t){.err = PROVEN_ERR_OVERFLOW};
    proven_result_mem_mut_t m_res = scratch.alloc_fn(scratch.ctx, cap, 1);
    if (!proven_is_ok(m_res.err)) return (internal_result_cstr_t){.err = m_res.err};
    char *buf = (char *)m_res.value.ptr;
    for (proven_size_t i = 0; i < view.size; ++i) buf[i] = (char)view.ptr[i];
    buf[view.size] = '\0';
    return (internal_result_cstr_t){.err = PROVEN_OK, .value = buf};
}

static void internal_cstr_free(proven_allocator_t scratch, char *buf) {
    if (buf && proven_alloc_is_valid(scratch)) {
        scratch.free_fn(scratch.ctx, buf);
    }
}

proven_result_file_t proven_fs_open(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_mode_t mode) {
    proven_result_file_t res = {0};
    internal_result_cstr_t path_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(path_res.err)) {
        res.err = path_res.err;
        return res;
    }
    char *path_buf = path_res.value;

    int pal_flags = (int)mode;
    const int supported_flags = PROVEN_FS_READ | PROVEN_FS_WRITE | PROVEN_FS_APPEND | PROVEN_FS_CREATE | PROVEN_FS_TRUNC | PROVEN_FS_CREATE_NEW;
    if (pal_flags & ~supported_flags) {
        internal_cstr_free(scratch, path_buf);
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    if ((pal_flags & PROVEN_FS_APPEND) && (pal_flags & PROVEN_FS_TRUNC)) {
        internal_cstr_free(scratch, path_buf);
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    if ((pal_flags & PROVEN_FS_TRUNC) && !(pal_flags & (PROVEN_FS_WRITE | PROVEN_FS_APPEND))) {
        internal_cstr_free(scratch, path_buf);
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    // If no access bits are set, default to READ
    if (!(pal_flags & (PROVEN_FS_READ | PROVEN_FS_WRITE | PROVEN_FS_APPEND))) {
        pal_flags |= PROVEN_FS_READ;
    }

    proven_sys_file_handle_t sh = proven_sys_fs_open(path_buf, pal_flags);
    internal_cstr_free(scratch, path_buf);

#if defined(_WIN32) || defined(_WIN64)
    if (!sh.handle) {
        res.err = PROVEN_ERR_IO;
        return res;
    }
    res.value.internal.ptr = sh.handle;
#else
    if (sh.fd < 0) {
        res.err = PROVEN_ERR_IO;
        return res;
    }
    res.value.internal.fd = sh.fd;
#endif

    res.err = PROVEN_OK;
    return res;
}

void proven_fs_close(proven_file_t file) {
#if defined(_WIN32) || defined(_WIN64)
    proven_sys_fs_close((proven_sys_file_handle_t){ .handle = file.internal.ptr });
#else
    proven_sys_fs_close((proven_sys_file_handle_t){ .fd = file.internal.fd });
#endif
}

proven_result_size_t proven_fs_read(proven_file_t file, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
#if defined(_WIN32) || defined(_WIN64)
    if (!file.internal.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sr = proven_sys_fs_read((proven_sys_file_handle_t){ .handle = file.internal.ptr }, dest.ptr, dest.size);
#else
    if (file.internal.fd < 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sr = proven_sys_fs_read((proven_sys_file_handle_t){ .fd = file.internal.fd }, dest.ptr, dest.size);
#endif
    res.err = sr.err;
    res.value = sr.value;
    return res;
}

proven_result_size_t proven_fs_write(proven_file_t file, proven_mem_view_t src) {
    proven_result_size_t res = {0};
#if defined(_WIN32) || defined(_WIN64)
    if (!file.internal.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sw = proven_sys_fs_write((proven_sys_file_handle_t){ .handle = file.internal.ptr }, src.ptr, src.size);
#else
    if (file.internal.fd < 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sw = proven_sys_fs_write((proven_sys_file_handle_t){ .fd = file.internal.fd }, src.ptr, src.size);
#endif
    res.err = sw.err;
    res.value = sw.value;
    return res;
}

proven_err_t proven_fs_write_all(proven_file_t file, proven_mem_view_t src) {
#if defined(_WIN32) || defined(_WIN64)
    if (!file.internal.ptr) return PROVEN_ERR_INVALID_ARG;
#else
    if (file.internal.fd < 0) return PROVEN_ERR_INVALID_ARG;
#endif
    if (src.size > 0 && !src.ptr) return PROVEN_ERR_INVALID_ARG;
    
    proven_size_t total_written = 0;
    while (total_written < src.size) {
        proven_mem_view_t slice = { .ptr = src.ptr + total_written, .size = src.size - total_written };
        proven_result_size_t r = proven_fs_write(file, slice);
        if (!proven_is_ok(r.err)) return r.err;
        if (r.value == 0) return PROVEN_ERR_IO;
        total_written += r.value;
    }
    return PROVEN_OK;
}

proven_result_size_t proven_fs_size(proven_file_t file) {
    proven_result_size_t res = {0};
#if defined(_WIN32) || defined(_WIN64)
    if (!file.internal.ptr) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sr = proven_sys_fs_size((proven_sys_file_handle_t){ .handle = file.internal.ptr });
#else
    if (file.internal.fd < 0) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    proven_sys_result_size_t sr = proven_sys_fs_size((proven_sys_file_handle_t){ .fd = file.internal.fd });
#endif
    res.err = sr.err;
    res.value = sr.value;
    return res;
}

proven_err_t proven_fs_rename(proven_allocator_t scratch, proven_u8str_view_t src, proven_u8str_view_t dest) {
    internal_result_cstr_t s_res = internal_view_to_cstr(scratch, src);
    if (!proven_is_ok(s_res.err)) return s_res.err;
    
    internal_result_cstr_t d_res = internal_view_to_cstr(scratch, dest);
    if (!proven_is_ok(d_res.err)) {
        internal_cstr_free(scratch, s_res.value);
        return d_res.err;
    }

    bool success = proven_sys_fs_rename(s_res.value, d_res.value);
    internal_cstr_free(scratch, s_res.value);
    internal_cstr_free(scratch, d_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_remove(proven_allocator_t scratch, proven_u8str_view_t path) {
    internal_result_cstr_t p_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(p_res.err)) return p_res.err;
    
    bool success = proven_sys_fs_remove(p_res.value);
    internal_cstr_free(scratch, p_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_mkdir(proven_allocator_t scratch, proven_u8str_view_t path) {
    internal_result_cstr_t p_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(p_res.err)) return p_res.err;
    
    bool success = proven_sys_fs_mkdir(p_res.value);
    internal_cstr_free(scratch, p_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_rmdir(proven_allocator_t scratch, proven_u8str_view_t path) {
    internal_result_cstr_t p_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(p_res.err)) return p_res.err;
    
    bool success = proven_sys_fs_rmdir(p_res.value);
    internal_cstr_free(scratch, p_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_copy(proven_allocator_t temp_alloc, proven_u8str_view_t src, proven_u8str_view_t dest) {
    if (!proven_alloc_is_valid(temp_alloc)) return PROVEN_ERR_INVALID_ARG;

    // Prevent copying a file to itself (identity check via stat)
    {
        proven_fs_stat_t s_stat, d_stat;
        proven_err_t s_err = proven_fs_stat(temp_alloc, src, &s_stat);
        proven_err_t d_err = proven_fs_stat(temp_alloc, dest, &d_stat);
        
        // If both exist and point to the same physical file (same dev and inode/fileindex)
        if (proven_is_ok(s_err) && proven_is_ok(d_err)) {
            if (s_stat.dev == d_stat.dev && s_stat.ino == d_stat.ino) {
                // If they are the same file, copying is a no-op that risks truncation if we proceed.
                // We return PROVEN_OK if they are identical as requested by standard behavior (cp fails, but no-op is often acceptable)
                // Actually most 'cp' tools return error if src and dest are same.
                return PROVEN_ERR_INVALID_ARG;
            }
        }
    }

    proven_result_file_t r_res = proven_fs_open(temp_alloc, src, PROVEN_FS_READ);
    if (!proven_is_ok(r_res.err)) return r_res.err;
    
    proven_result_file_t w_res = proven_fs_open(temp_alloc, dest, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    if (!proven_is_ok(w_res.err)) {
        proven_fs_close(r_res.value);
        return w_res.err;
    }

    proven_size_t buf_size = 4096 * 16;
    proven_result_mem_mut_t m_res = temp_alloc.alloc_fn(temp_alloc.ctx, buf_size, 8);
    if (!proven_is_ok(m_res.err)) {
        proven_fs_close(r_res.value);
        proven_fs_close(w_res.value);
        return m_res.err;
    }

    proven_err_t final_err = PROVEN_OK;
    while (true) {
        proven_result_size_t read_bytes = proven_fs_read(r_res.value, m_res.value);
        if (read_bytes.err == PROVEN_ERR_EOF) {
            break;
        }
        if (!proven_is_ok(read_bytes.err) || read_bytes.value == 0) {
            final_err = read_bytes.err;
            break;
        }
        proven_mem_view_t v = { .ptr = m_res.value.ptr, .size = read_bytes.value };
        proven_err_t werr = proven_fs_write_all(w_res.value, v);
        if (!proven_is_ok(werr)) {
            final_err = werr;
            break;
        }
    }

    temp_alloc.free_fn(temp_alloc.ctx, m_res.value.ptr);
    proven_fs_close(r_res.value);
    proven_fs_close(w_res.value);
    return final_err;
}

static int compare_fs_entries(const void *a, const void *b) {
    const proven_fs_entry_t *ea = a;
    const proven_fs_entry_t *eb = b;
    // Dirs first, then name
    if (ea->type != eb->type) {
        if (ea->type == PROVEN_FS_TYPE_DIR) return -1;
        if (eb->type == PROVEN_FS_TYPE_DIR) return 1;
    }
    proven_u8str_view_t va = proven_u8str_as_view(&ea->name);
    proven_u8str_view_t vb = proven_u8str_as_view(&eb->name);
    
    proven_size_t min_len = va.size < vb.size ? va.size : vb.size;
    for (proven_size_t i = 0; i < min_len; ++i) {
        if (va.ptr[i] < vb.ptr[i]) return -1;
        if (va.ptr[i] > vb.ptr[i]) return 1;
    }
    if (va.size < vb.size) return -1;
    if (va.size > vb.size) return 1;
    return 0;
}

proven_result_array_t proven_fs_list(proven_allocator_t alloc, proven_u8str_view_t path) {
    proven_result_array_t res = {0};
    internal_result_cstr_t p_res = internal_view_to_cstr(alloc, path);
    if (!proven_is_ok(p_res.err)) {
        res.err = p_res.err;
        return res;
    }

    proven_sys_dir_handle_t dh = proven_sys_fs_dir_open(p_res.value);
    internal_cstr_free(alloc, p_res.value);
    
    if (!dh.internal) {
        res.err = PROVEN_ERR_IO;
        return res;
    }

    proven_result_array_t a_res = PROVEN_ARRAY_INIT(alloc, proven_fs_entry_t, 16);
    if (!proven_is_ok(a_res.err)) {
        proven_sys_fs_dir_close(dh);
        return a_res;
    }

    proven_sys_dir_entry_t se;
    while (proven_sys_fs_dir_next(dh, &se)) {
        proven_fs_entry_t entry = {0};
        entry.type = se.is_dir ? PROVEN_FS_TYPE_DIR : PROVEN_FS_TYPE_FILE;
        entry.size = se.size;
        
        proven_u8str_view_t name_view = { .ptr = (const proven_u8*)se.name, .size = 0 };
        while (se.name[name_view.size]) name_view.size++;
        
        proven_result_u8str_t s_res = proven_u8str_create_from_view(alloc, name_view);
        if (!proven_is_ok(s_res.err)) {
            proven_sys_fs_dir_close(dh);
            proven_fs_list_destroy(alloc, &a_res.value);
            res.err = s_res.err;
            return res;
        }
        entry.name = s_res.value;
        
        proven_err_t push_err = proven_array_push(&a_res.value, &entry);
        if (!proven_is_ok(push_err)) {
            proven_u8str_destroy(alloc, &entry.name);
            proven_sys_fs_dir_close(dh);
            proven_fs_list_destroy(alloc, &a_res.value);
            res.err = push_err;
            return res;
        }
    }
    proven_sys_fs_dir_close(dh);

    // Sort entries: directories first, then alphabetical
    proven_array_sort(&a_res.value, compare_fs_entries);

    res.err = PROVEN_OK;
    res.value = a_res.value;
    return res;
}

void proven_fs_list_destroy(proven_allocator_t alloc, proven_array_t *list) {
    if (!list || !list->data) return;
    proven_size_t count = list->len;
    proven_fs_entry_t *entries = (proven_fs_entry_t*)list->data;
    for (proven_size_t i = 0; i < count; ++i) {
        proven_u8str_destroy(alloc, &entries[i].name);
    }
    proven_array_destroy(list);
}

proven_result_mem_mut_t proven_fs_read_all(proven_allocator_t alloc, proven_u8str_view_t path) {
    proven_result_mem_mut_t res = {0};
    if (!proven_alloc_is_valid(alloc)) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    
    proven_result_file_t f_res = proven_fs_open(alloc, path, PROVEN_FS_READ);
    if (!proven_is_ok(f_res.err)) {
        res.err = f_res.err;
        return res;
    }
    
    proven_file_t f = f_res.value;
    proven_result_size_t s_res = proven_fs_size(f);
    if (!proven_is_ok(s_res.err)) {
        proven_fs_close(f);
        res.err = s_res.err;
        return res;
    }
    
    proven_size_t size = s_res.value;
    if (size == 0) {
        res.err = PROVEN_OK;
        res.value = (proven_mem_mut_t){ .ptr = (proven_byte_t*)0, .size = 0 };
        proven_fs_close(f);
        return res;
    }
    
    proven_result_mem_mut_t m_res = alloc.alloc_fn(alloc.ctx, size, 1);
    if (!proven_is_ok(m_res.err)) {
        proven_fs_close(f);
        res.err = m_res.err;
        return res;
    }
    
    proven_size_t total_read = 0;
    while (total_read < size) {
        proven_mem_mut_t slice = { .ptr = m_res.value.ptr + total_read, .size = size - total_read };
        proven_result_size_t r = proven_fs_read(f, slice);
        if (r.err == PROVEN_ERR_EOF) {
            break;
        }
        if (!proven_is_ok(r.err)) {
            proven_fs_close(f);
            alloc.free_fn(alloc.ctx, m_res.value.ptr);
            res.err = r.err;
            return res;
        }
        if (r.value == 0) {
            break;
        }
        total_read += r.value;
    }
    proven_fs_close(f);
    
    // Shrink allocation if partial read happened
    if (total_read < size && alloc.realloc_fn) {
        proven_result_mem_mut_t shrink = alloc.realloc_fn(alloc.ctx, m_res.value.ptr, size, total_read, 1);
        if (proven_is_ok(shrink.err)) {
            m_res.value = shrink.value;
        }
    }
    
    res.err = PROVEN_OK;
    res.value = m_res.value;
    res.value.size = total_read;
    return res;
}

proven_err_t proven_fs_chmod(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_perms_t perms) {
    internal_result_cstr_t p_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(p_res.err)) return p_res.err;
    
    bool success = proven_sys_fs_chmod(p_res.value, (unsigned int)perms);
    internal_cstr_free(scratch, p_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_lock(proven_file_t file, proven_fs_lock_type_t type, bool wait) {
    int t = 0;
    if (type == PROVEN_FS_LOCK_SHARED) {
        t = 0;
    } else if (type == PROVEN_FS_LOCK_EXCLUSIVE) {
        t = 1;
    } else if (type == PROVEN_FS_LOCK_UNLOCK) {
        t = 2;
    } else {
        return PROVEN_ERR_INVALID_ARG;
    }
    
#if defined(_WIN32) || defined(_WIN64)
    return proven_sys_fs_lock((proven_sys_file_handle_t){ .handle = file.internal.ptr }, t, wait) ? PROVEN_OK : PROVEN_ERR_IO;
#else
    return proven_sys_fs_lock((proven_sys_file_handle_t){ .fd = file.internal.fd }, t, wait) ? PROVEN_OK : PROVEN_ERR_IO;
#endif
}

proven_err_t proven_fs_stat(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_stat_t *out_stat) {
    if (!out_stat) return PROVEN_ERR_INVALID_ARG;
    
    internal_result_cstr_t p_res = internal_view_to_cstr(scratch, path);
    if (!proven_is_ok(p_res.err)) return p_res.err;
    
    proven_sys_fs_stat_t se;
    bool stat_ok = proven_sys_fs_stat(p_res.value, &se);
    internal_cstr_free(scratch, p_res.value);
    
    if (!stat_ok) return PROVEN_ERR_IO;
    
    out_stat->size = se.size;
    out_stat->type = se.is_dir ? PROVEN_FS_TYPE_DIR : PROVEN_FS_TYPE_FILE;
    out_stat->perms = (proven_fs_perms_t)se.mode;
    out_stat->modified_at = se.mtime;
    out_stat->created_at = 0; // standard stat doesn't provide birth time on all posix
    out_stat->dev = se.dev;
    out_stat->ino = se.ino;
    
    return PROVEN_OK;
}

proven_err_t proven_fs_symlink(proven_allocator_t scratch, proven_u8str_view_t target, proven_u8str_view_t linkpath) {
    internal_result_cstr_t t_res = internal_view_to_cstr(scratch, target);
    if (!proven_is_ok(t_res.err)) return t_res.err;

    internal_result_cstr_t l_res = internal_view_to_cstr(scratch, linkpath);
    if (!proven_is_ok(l_res.err)) {
        internal_cstr_free(scratch, t_res.value);
        return l_res.err;
    }
    
    bool success = proven_sys_fs_symlink(t_res.value, l_res.value);
    internal_cstr_free(scratch, t_res.value);
    internal_cstr_free(scratch, l_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_link(proven_allocator_t scratch, proven_u8str_view_t oldpath, proven_u8str_view_t newpath) {
    internal_result_cstr_t o_res = internal_view_to_cstr(scratch, oldpath);
    if (!proven_is_ok(o_res.err)) return o_res.err;

    internal_result_cstr_t n_res = internal_view_to_cstr(scratch, newpath);
    if (!proven_is_ok(n_res.err)) {
        internal_cstr_free(scratch, o_res.value);
        return n_res.err;
    }

    bool success = proven_sys_fs_link(o_res.value, n_res.value);
    internal_cstr_free(scratch, o_res.value);
    internal_cstr_free(scratch, n_res.value);
    return success ? PROVEN_OK : PROVEN_ERR_IO;
}

bool proven_fs_is_absolute(proven_u8str_view_t path) {
    if (path.size == 0 || !path.ptr) return false;
#define PROVEN_FS_IS_SEP(c) ((c) == '/' || (c) == '\\')
    // POSIX root
    if (path.ptr[0] == '/') return true;
    // Windows UNC, device, and extended prefixes.
    if (path.size >= 2 && PROVEN_FS_IS_SEP(path.ptr[0]) && PROVEN_FS_IS_SEP(path.ptr[1])) return true;
    // Windows drive root (e.g., C:/)
    if (path.size >= 3 && 
        ((path.ptr[0] >= 'a' && path.ptr[0] <= 'z') || (path.ptr[0] >= 'A' && path.ptr[0] <= 'Z')) &&
        path.ptr[1] == ':' &&
        PROVEN_FS_IS_SEP(path.ptr[2])) {
        return true;
    }
#undef PROVEN_FS_IS_SEP
    return false;
}
