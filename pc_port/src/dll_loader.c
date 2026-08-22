/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dll_loader.c — OS-specific shared library loading.
 *
 * This file ONLY includes OS headers. It must NOT include any game or
 * decomp headers, because <windows.h> conflicts with PSX type definitions
 * (EnterCriticalSection macro, 'byte' typedef, etc.).
 */
#include "dll_loader.h"
#include "dll_security.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char s_errorBuf[256] = {0};

/* True only for a mod DLL under maps/ -- the sole channel the security audit
 * screens. The game also opens first-party bundled runtime DLLs through here
 * (FFmpeg: avformat/avcodec/... by bare name from the exe dir), which
 * legitimately import system/networking libraries the game itself does not;
 * auditing those as "edited game code" wrongly rejected them and killed
 * mp4/mkv FMV overrides. Matches "maps/" or "maps\" anywhere in the path. */
static int DllLoader_IsModPath(const char* path)
{
    const char* p;
    if (path == NULL) return 0;
    for (p = path; p[0] && p[1] && p[2] && p[3] && p[4]; p++)
    {
        if ((p[0] | 0x20) == 'm' && (p[1] | 0x20) == 'a' &&
            (p[2] | 0x20) == 'p' && (p[3] | 0x20) == 's' &&
            (p[4] == '/' || p[4] == '\\'))
        {
            return 1;
        }
    }
    return 0;
}

DllHandle DllLoader_Open(const char* path)
{
    /* Screen ONLY the mod channel (maps overlays). Those are user-supplied edited
     * game code and must pass the import fingerprint before any of their code
     * runs (DllMain executes inside LoadLibrary). The game's own bundled DLLs
     * -- FFmpeg above all -- load unscreened. */
    if (DllLoader_IsModPath(path))
    {
        char reason[256] = {0};
        if (DllSecurity_AuditPlugin(path, DLL_AUDIT_MAP, reason, sizeof(reason)) != DLL_SECURITY_OK)
        {
            snprintf(s_errorBuf, sizeof(s_errorBuf), "blocked by static check: %s", reason);
            return NULL;
        }
    }

    HMODULE h = LoadLibraryA(path);
    if (!h)
    {
        snprintf(s_errorBuf, sizeof(s_errorBuf), "LoadLibrary error %lu", GetLastError());
    }
    return (DllHandle)h;
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    FARPROC p = GetProcAddress((HMODULE)handle, name);
    if (!p)
    {
        snprintf(s_errorBuf, sizeof(s_errorBuf), "GetProcAddress error %lu", GetLastError());
    }
    return (void*)p;
}

void DllLoader_Close(DllHandle handle)
{
    if (handle)
        FreeLibrary((HMODULE)handle);
}

int DllLoader_ListPlugins(char paths[][260], int maxCount)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              n = 0;

    h = FindFirstFileA("plugins\\*.dll", &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < maxCount)
        {
            snprintf(paths[n], 260, "plugins\\%s", fd.cFileName);
            n++;
        }
    } while (n < maxCount && FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

const char* DllLoader_GetError(void)
{
    return s_errorBuf;
}

#else /* POSIX */
#include <dlfcn.h>

DllHandle DllLoader_Open(const char* path)
{
    return dlopen(path, RTLD_NOW);
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    return dlsym(handle, name);
}

void DllLoader_Close(DllHandle handle)
{
    if (handle)
        dlclose(handle);
}

int DllLoader_ListPlugins(char paths[][260], int maxCount)
{
    (void)paths; (void)maxCount;
    return 0; /* plugin scan is Windows-only for now */
}

const char* DllLoader_GetError(void)
{
    const char* e = dlerror();
    return e ? e : "";
}

#endif
