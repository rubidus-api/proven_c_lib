#include "proven_sys_env.h"

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdlib.h>
#include <string.h>
#endif

bool proven_sys_env_get(const char *name, char *out_buf, size_t buf_cap, size_t *out_len) {
    if (!name || !out_buf || buf_cap == 0) return false;

#if defined(_WIN32) || defined(_WIN64)
    wchar_t wname[256];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256) == 0) return false;

    // Use an intermediate wide buffer for Windows Environment lookup
    wchar_t wbuf[1024];
    DWORD res = GetEnvironmentVariableW(wname, wbuf, 1024);
    if (res == 0 || res >= 1024) return false;

    int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out_buf, (int)buf_cap, NULL, NULL);
    if (bytes == 0) return false;

    if (out_len) *out_len = (size_t)(bytes - 1); // exclude null-terminator
    return true;
#else
    char *val = getenv(name);
    if (!val) return false;

    size_t len = strlen(val);
    if (len >= buf_cap) return false; // Not enough capacity

    memcpy(out_buf, val, len + 1); // include null-terminator just in case

    if (out_len) *out_len = len;
    return true;
#endif
}
