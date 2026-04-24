#include "proven_sys_fs.h"
#include <sys/stat.h>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <stdlib.h>
#endif

// Internal helper for UTF-8 to Wide conversion on Windows
#if defined(_WIN32) || defined(_WIN64)
static void utf8_to_wide(const char *src, wchar_t *dst, size_t dst_cap) {
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_cap);
}
#endif

proven_sys_file_handle_t proven_sys_fs_open(const char *path, const char *mode_str) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);

    DWORD access = 0;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD disposition = 0;

    // Simplified mapping for "rb", "wb+", "ab+"
    if (mode_str[0] == 'r') {
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        if (mode_str[1] == '+') access |= GENERIC_WRITE;
    } else if (mode_str[0] == 'w') {
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
    } else if (mode_str[0] == 'a') {
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
    }

    HANDLE h = CreateFileW(wpath, access, share, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return (proven_sys_file_handle_t){ .internal = NULL };
    
    if (mode_str[0] == 'a') {
        SetFilePointer(h, 0, NULL, FILE_END);
    }

    return (proven_sys_file_handle_t){ .internal = (void*)h };
#else
    int flags = 0;
    if (mode_str[0] == 'r') {
        flags = O_RDONLY;
        if (mode_str[1] == '+') flags = O_RDWR;
    } else if (mode_str[0] == 'w') {
        flags = O_CREAT | O_TRUNC | O_RDWR;
    } else if (mode_str[0] == 'a') {
        flags = O_CREAT | O_APPEND | O_RDWR;
    }
    
    int fd = open(path, flags, 0666);
    if (fd < 0) return (proven_sys_file_handle_t){ .internal = NULL };
    // We cast the integer fd to void* by casting it through intptr_t
    return (proven_sys_file_handle_t){ .internal = (void*)(intptr_t)fd };
#endif
}

void proven_sys_fs_close(proven_sys_file_handle_t handle) {
    if (!handle.internal) return;
#if defined(_WIN32) || defined(_WIN64)
    CloseHandle((HANDLE)handle.internal);
#else
    close((int)(intptr_t)handle.internal);
#endif
}

size_t proven_sys_fs_read(proven_sys_file_handle_t handle, void *buf, size_t size) {
    if (!handle.internal) return 0;
#if defined(_WIN32) || defined(_WIN64)
    DWORD read = 0;
    if (ReadFile((HANDLE)handle.internal, buf, (DWORD)size, &read, NULL)) {
        return (size_t)read;
    }
    return 0;
#else
    ssize_t res = read((int)(intptr_t)handle.internal, buf, size);
    return res > 0 ? (size_t)res : 0;
#endif
}

size_t proven_sys_fs_write(proven_sys_file_handle_t handle, const void *buf, size_t size) {
    if (!handle.internal) return 0;
#if defined(_WIN32) || defined(_WIN64)
    DWORD written = 0;
    if (WriteFile((HANDLE)handle.internal, buf, (DWORD)size, &written, NULL)) {
        return (size_t)written;
    }
    return 0;
#else
    ssize_t res = write((int)(intptr_t)handle.internal, buf, size);
    return res > 0 ? (size_t)res : 0;
#endif
}

size_t proven_sys_fs_size(proven_sys_file_handle_t handle) {
    if (!handle.internal) return 0;
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER li;
    if (GetFileSizeEx((HANDLE)handle.internal, &li)) {
        return (size_t)li.QuadPart;
    }
    return 0;
#else
    struct stat st;
    if (fstat((int)(intptr_t)handle.internal, &st) == 0) {
        return (size_t)st.st_size;
    }
    return 0;
#endif
}

bool proven_sys_fs_rename(const char *src, const char *dest) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wsrc[MAX_PATH], wdest[MAX_PATH];
    utf8_to_wide(src, wsrc, MAX_PATH);
    utf8_to_wide(dest, wdest, MAX_PATH);
    return MoveFileW(wsrc, wdest) != 0;
#else
    return rename(src, dest) == 0;
#endif
}

bool proven_sys_fs_remove(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    return DeleteFileW(wpath) != 0;
#else
    return remove(path) == 0;
#endif
}

bool proven_sys_fs_mkdir(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    return CreateDirectoryW(wpath, NULL) != 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

bool proven_sys_fs_rmdir(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    return RemoveDirectoryW(wpath) != 0;
#else
    return rmdir(path) == 0;
#endif
}

proven_sys_dir_handle_t proven_sys_fs_dir_open(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    lstrcatW(wpath, L"\\*");

    WIN32_FIND_DATAW *fd = HeapAlloc(GetProcessHeap(), 0, sizeof(WIN32_FIND_DATAW));
    HANDLE h = FindFirstFileW(wpath, fd);
    if (h == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, fd);
        return (proven_sys_dir_handle_t){ .internal = NULL };
    }
    // We store the handle and the first result in a tiny struct for next calls
    struct win_dir { HANDLE h; WIN32_FIND_DATAW fd; bool first; };
    struct win_dir *wd = HeapAlloc(GetProcessHeap(), 0, sizeof(struct win_dir));
    wd->h = h;
    wd->fd = *fd;
    wd->first = true;
    HeapFree(GetProcessHeap(), 0, fd);
    return (proven_sys_dir_handle_t){ .internal = (void*)wd };
#else
    DIR *d = opendir(path);
    return (proven_sys_dir_handle_t){ .internal = (void*)d };
#endif
}

void proven_sys_fs_dir_close(proven_sys_dir_handle_t handle) {
    if (!handle.internal) return;
#if defined(_WIN32) || defined(_WIN64)
    struct win_dir { HANDLE h; WIN32_FIND_DATAW fd; bool first; } *wd = handle.internal;
    FindClose(wd->h);
    HeapFree(GetProcessHeap(), 0, wd);
#else
    closedir((DIR*)handle.internal);
#endif
}

bool proven_sys_fs_dir_next(proven_sys_dir_handle_t handle, proven_sys_dir_entry_t *out_entry) {
    if (!handle.internal) return false;
#if defined(_WIN32) || defined(_WIN64)
    struct win_dir { HANDLE h; WIN32_FIND_DATAW fd; bool first; } *wd = handle.internal;
    if (!wd->first) {
        if (!FindNextFileW(wd->h, &wd->fd)) return false;
    }
    wd->first = false;

    // Skip . and ..
    while (wd->fd.cFileName[0] == L'.') {
        if (wd->fd.cFileName[1] == L'\0' || (wd->fd.cFileName[1] == L'.' && wd->fd.cFileName[2] == L'\0')) {
            if (!FindNextFileW(wd->h, &wd->fd)) return false;
            continue;
        }
        break;
    }

    // Convert back from Wide to UTF-8 (static buffer for name return as per PAL style)
    static char name_buf[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, wd->fd.cFileName, -1, name_buf, MAX_PATH, NULL, NULL);
    out_entry->name = name_buf;
    out_entry->is_dir = (wd->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    out_entry->size = ((size_t)wd->fd.nFileSizeHigh << 32) | wd->fd.nFileSizeLow;
    return true;
#else
    DIR *d = (DIR*)handle.internal;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            if (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')) continue;
        }
        out_entry->name = entry->d_name;
        out_entry->is_dir = (entry->d_type == DT_DIR);
        out_entry->size = 0; 
        return true;
    }
    return false;
#endif
}

bool proven_sys_fs_chmod(const char *path, unsigned int perms) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    
    // Logic: If Owner-W (bit 7) is missing, set ReadOnly
    if (!(perms & 0200)) attr |= FILE_ATTRIBUTE_READONLY;
    else attr &= ~FILE_ATTRIBUTE_READONLY;
    
    return SetFileAttributesW(wpath, attr) != 0;
#else
    return chmod(path, perms) == 0;
#endif
}

bool proven_sys_fs_lock(proven_sys_file_handle_t handle, int type, bool wait) {
    if (!handle.internal) return false;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = (HANDLE)handle.internal;
    OVERLAPPED ov = {0};
    DWORD flags = (type == 1) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!wait) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    
    if (type == 2) return UnlockFileEx(h, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov) != 0;
    return LockFileEx(h, flags, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov) != 0;
#else
    int fd = (int)(intptr_t)handle.internal;
    struct flock fl = {0};
    if (type == 0) fl.l_type = F_RDLCK;
    else if (type == 1) fl.l_type = F_WRLCK;
    else fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    int cmd = wait ? F_SETLKW : F_SETLK;
    return fcntl(fd, cmd, &fl) == 0;
#endif
}

bool proven_sys_fs_stat(const char *path, proven_sys_fs_stat_t *out_stat) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &data)) return false;
    out_stat->size = ((size_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    out_stat->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    out_stat->mode = out_stat->is_dir ? 0755 : 0644;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) out_stat->mode &= ~0222;
    
    // Convert FILETIME to unix timestamp
    ULARGE_INTEGER ull;
    ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
    out_stat->mtime = (ull.QuadPart - 116444736000000000ULL) / 10000000ULL;
    return true;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    out_stat->size = (size_t)st.st_size;
    out_stat->is_dir = S_ISDIR(st.st_mode);
    out_stat->mode = (unsigned int)st.st_mode;
    out_stat->mtime = (long long)st.st_mtime;
    return true;
#endif
}

bool proven_sys_fs_link(const char *oldpath, const char *newpath) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wold[MAX_PATH], wnew[MAX_PATH];
    utf8_to_wide(oldpath, wold, MAX_PATH);
    utf8_to_wide(newpath, wnew, MAX_PATH);
    return CreateHardLinkW(wnew, wold, NULL) != 0;
#else
    return link(oldpath, newpath) == 0;
#endif
}

bool proven_sys_fs_symlink(const char *target, const char *linkpath) {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wtarget[MAX_PATH], wlink[MAX_PATH];
    utf8_to_wide(target, wtarget, MAX_PATH);
    utf8_to_wide(linkpath, wlink, MAX_PATH);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    // Note: requires checking if target is a directory for the flag
    return CreateSymbolicLinkW(wlink, wtarget, flags) != 0;
#else
    return symlink(target, linkpath) == 0;
#endif
}

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/mman.h>
#endif

proven_sys_mmap_res_t proven_sys_fs_create(proven_sys_file_handle_t handle, size_t offset, size_t size, int prot, int flags) {
    proven_sys_mmap_res_t res = { .ptr = NULL, .internal_handle = NULL };
#if defined(_WIN32) || defined(_WIN64)
    HANDLE hFile = (HANDLE)handle.internal;
    DWORD flProtect = 0;
    DWORD dwAccess = 0;

    if (prot & 0x02) { // Write
        flProtect = PAGE_READWRITE;
        dwAccess = FILE_MAP_WRITE;
    } else {
        flProtect = PAGE_READONLY;
        dwAccess = FILE_MAP_READ;
    }

    if (flags & 0x01) { // Private (Copy on write)
        flProtect = PAGE_WRITECOPY;
        dwAccess = FILE_MAP_COPY;
    }

    // Windows requires a separate Mapping object
    HANDLE hMap = CreateFileMappingW(hFile, NULL, flProtect, 0, 0, NULL);
    if (!hMap) return res;

    void *ptr = MapViewOfFile(hMap, dwAccess, (DWORD)(offset >> 32), (DWORD)(offset & 0xFFFFFFFF), size);
    if (!ptr) {
        CloseHandle(hMap);
        return res;
    }

    res.ptr = ptr;
    res.internal_handle = (void*)hMap;
    return res;
#else
    int fd = (int)(intptr_t)handle.internal;
    int p = 0;
    if (prot & 0x01) p |= PROT_READ;
    if (prot & 0x02) p |= PROT_WRITE;
    if (prot & 0x04) p |= PROT_EXEC;

    int f = 0;
    if (flags & 0x01) f |= MAP_PRIVATE;
    if (flags & 0x02) f |= MAP_SHARED;

    void *ptr = mmap(NULL, size, p, f, fd, (off_t)offset);
    if (ptr == MAP_FAILED) return res;

    res.ptr = ptr;
    return res;
#endif
}

bool proven_sys_fs_destroy(void *ptr, size_t size, void *internal_handle) {
#if defined(_WIN32) || defined(_WIN64)
    UnmapViewOfFile(ptr);
    if (internal_handle) CloseHandle((HANDLE)internal_handle);
    return true;
#else
    (void)internal_handle;
    return munmap(ptr, size) == 0;
#endif
}

bool proven_sys_fs_sync(void *ptr, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    return FlushViewOfFile(ptr, size) != 0;
#else
    return msync(ptr, size, MS_SYNC) == 0;
#endif
}
