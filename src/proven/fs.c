#include "proven/fs.h"
#include "../../platform/proven_sys_fs.h"
#include "proven/array.h"
#include "proven/algorithm.h"

static bool internal_view_to_cstr(proven_u8str_view_t view, char *buf, size_t buf_size) {
    if (view.size >= buf_size) return false;
    for (proven_size_t i = 0; i < view.size; ++i) buf[i] = (char)view.ptr[i];
    buf[view.size] = '\0';
    return true;
}

proven_result_file_t proven_fs_open(proven_u8str_view_t path, proven_fs_mode_t mode) {
    proven_result_file_t res = {0};
    char path_buf[512];
    if (!internal_view_to_cstr(path, path_buf, sizeof(path_buf))) {
        res.err = PROVEN_ERR_OUT_OF_BOUNDS;
        return res;
    }

    const char *m = "rb";
    if (mode & PROVEN_FS_WRITE) {
        if (mode & PROVEN_FS_APPEND) m = "ab+";
        else m = "wb+";
    }

    proven_sys_file_handle_t sh = proven_sys_fs_open(path_buf, m);
    if (!sh.internal) {
        res.err = PROVEN_ERR_IO;
        return res;
    }

    res.err = PROVEN_OK;
    res.value.internal = sh.internal;
    return res;
}

void proven_fs_close(proven_file_t file) {
    proven_sys_fs_close((proven_sys_file_handle_t){ .internal = file.internal });
}

proven_result_size_t proven_fs_read(proven_file_t file, proven_mem_mut_t dest) {
    proven_result_size_t res = {0};
    if (!file.internal) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    size_t r = proven_sys_fs_read((proven_sys_file_handle_t){ .internal = file.internal }, dest.ptr, dest.size);
    res.err = PROVEN_OK;
    res.value = (proven_size_t)r;
    return res;
}

proven_result_size_t proven_fs_write(proven_file_t file, proven_mem_view_t src) {
    proven_result_size_t res = {0};
    if (!file.internal) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    size_t w = proven_sys_fs_write((proven_sys_file_handle_t){ .internal = file.internal }, src.ptr, src.size);
    res.err = PROVEN_OK;
    res.value = (proven_size_t)w;
    return res;
}

proven_result_size_t proven_fs_size(proven_file_t file) {
    proven_result_size_t res = {0};
    if (!file.internal) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }
    res.err = PROVEN_OK;
    res.value = (proven_size_t)proven_sys_fs_size((proven_sys_file_handle_t){ .internal = file.internal });
    return res;
}

proven_err_t proven_fs_rename(proven_u8str_view_t src, proven_u8str_view_t dest) {
    char s_buf[512], d_buf[512];
    if (!internal_view_to_cstr(src, s_buf, sizeof(s_buf)) || !internal_view_to_cstr(dest, d_buf, sizeof(d_buf))) {
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    return proven_sys_fs_rename(s_buf, d_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_remove(proven_u8str_view_t path) {
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_remove(p_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_mkdir(proven_u8str_view_t path) {
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_mkdir(p_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_rmdir(proven_u8str_view_t path) {
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_rmdir(p_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_copy(proven_allocator_t temp_alloc, proven_u8str_view_t src, proven_u8str_view_t dest) {
    proven_result_file_t r_res = proven_fs_open(src, PROVEN_FS_READ);
    if (!proven_is_ok(r_res.err)) return r_res.err;
    
    proven_result_file_t w_res = proven_fs_open(dest, PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    if (!proven_is_ok(w_res.err)) {
        proven_fs_close(r_res.value);
        return w_res.err;
    }

    proven_size_t buf_size = 4096 * 16;
    proven_result_mem_mut_t m_res = temp_alloc.alloc_fn(temp_alloc.ctx, buf_size, 1);
    if (!proven_is_ok(m_res.err)) {
        proven_fs_close(r_res.value);
        proven_fs_close(w_res.value);
        return m_res.err;
    }

    proven_err_t final_err = PROVEN_OK;
    while (true) {
        proven_result_size_t read_bytes = proven_fs_read(r_res.value, m_res.value);
        if (!proven_is_ok(read_bytes.err) || read_bytes.value == 0) {
            final_err = read_bytes.err;
            break;
        }
        proven_mem_view_t v = { .ptr = m_res.value.ptr, .size = read_bytes.value };
        proven_result_size_t written = proven_fs_write(w_res.value, v);
        if (!proven_is_ok(written.err) || written.value != read_bytes.value) {
            final_err = PROVEN_ERR_IO;
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
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) {
        res.err = PROVEN_ERR_OUT_OF_BOUNDS;
        return res;
    }

    proven_sys_dir_handle_t dh = proven_sys_fs_dir_open(p_buf);
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
        if (!proven_is_ok(s_res.err)) continue;
        entry.name = s_res.value;
        
        proven_err_t push_err = proven_array_push(&a_res.value, &entry);
        if (!proven_is_ok(push_err)) break; // or handle more aggressively
    }
    proven_sys_fs_dir_close(dh);

    // Sort entries: directories first, then alphabetical
    proven_array_sort(&a_res.value, compare_fs_entries);

    res.err = PROVEN_OK;
    res.value = a_res.value;
    return res;
}

proven_result_mem_mut_t proven_fs_read_all(proven_allocator_t alloc, proven_u8str_view_t path) {
    proven_result_mem_mut_t res = {0};
    
    proven_result_file_t f_res = proven_fs_open(path, PROVEN_FS_READ);
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
    proven_result_mem_mut_t m_res = alloc.alloc_fn(alloc.ctx, size, 1);
    if (!proven_is_ok(m_res.err)) {
        proven_fs_close(f);
        res.err = m_res.err;
        return res;
    }
    
    (void)proven_fs_read(f, m_res.value);
    proven_fs_close(f);
    
    res.err = PROVEN_OK;
    res.value = m_res.value;
    return res;
}

proven_err_t proven_fs_chmod(proven_u8str_view_t path, proven_fs_perms_t perms) {
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_chmod(p_buf, (unsigned int)perms) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_lock(proven_file_t file, proven_fs_lock_type_t type, bool wait) {
    int t = 0;
    if (type == PROVEN_FS_LOCK_SHARED) t = 0;
    else if (type == PROVEN_FS_LOCK_EXCLUSIVE) t = 1;
    else t = 2;
    
    return proven_sys_fs_lock((proven_sys_file_handle_t){ .internal = file.internal }, t, wait) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_stat(proven_u8str_view_t path, proven_fs_stat_t *out_stat) {
    char p_buf[512];
    if (!internal_view_to_cstr(path, p_buf, sizeof(p_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    
    proven_sys_fs_stat_t se;
    if (!proven_sys_fs_stat(p_buf, &se)) return PROVEN_ERR_IO;
    
    out_stat->size = se.size;
    out_stat->type = se.is_dir ? PROVEN_FS_TYPE_DIR : PROVEN_FS_TYPE_FILE;
    out_stat->perms = (proven_fs_perms_t)se.mode;
    out_stat->modified_at = se.mtime;
    out_stat->created_at = 0; // standard stat doesn't provide birth time on all posix
    
    return PROVEN_OK;
}

proven_err_t proven_fs_symlink(proven_u8str_view_t target, proven_u8str_view_t linkpath) {
    char t_buf[512], l_buf[512];
    if (!internal_view_to_cstr(target, t_buf, sizeof(t_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (!internal_view_to_cstr(linkpath, l_buf, sizeof(l_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_symlink(t_buf, l_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

proven_err_t proven_fs_link(proven_u8str_view_t oldpath, proven_u8str_view_t newpath) {
    char o_buf[512], n_buf[512];
    if (!internal_view_to_cstr(oldpath, o_buf, sizeof(o_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (!internal_view_to_cstr(newpath, n_buf, sizeof(n_buf))) return PROVEN_ERR_OUT_OF_BOUNDS;
    return proven_sys_fs_link(o_buf, n_buf) ? PROVEN_OK : PROVEN_ERR_IO;
}

bool proven_fs_is_absolute(proven_u8str_view_t path) {
    if (path.size == 0) return false;
    // POSIX root
    if (path.ptr[0] == '/') return true;
    // Windows drive root (e.g., C:/)
    if (path.size >= 3 && 
        ((path.ptr[0] >= 'a' && path.ptr[0] <= 'z') || (path.ptr[0] >= 'A' && path.ptr[0] <= 'Z')) &&
        path.ptr[1] == ':' &&
        (path.ptr[2] == '/' || path.ptr[2] == '\\')) {
        return true;
    }
    return false;
}
