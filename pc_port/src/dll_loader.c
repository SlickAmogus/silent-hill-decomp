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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char s_errorBuf[256] = {0};

DllHandle DllLoader_Open(const char* path)
{
    /* Static lint before any code from the file can run (DllMain executes
     * inside LoadLibrary). Covers every open -- map overlays, chara pool,
     * plugins -- so a mod-installed maps/*.dll gets the same import screen
     * as a plugin. requirePluginExports=0: map DLLs export their own set;
     * the plugin loader re-audits with the contract check on top. */
    {
        char reason[256] = {0};
        if (DllSecurity_AuditPlugin(path, 0, reason, sizeof(reason)) != DLL_SECURITY_OK)
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

    h = FindFirstFileA("plugins\*.dll", &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < maxCount)
        {
            snprintf(paths[n], 260, "plugins\%s", fd.cFileName);
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

const char* DllLoader_GetError(void)
{
    const char* e = dlerror();
    return e ? e : "";
}

int DllLoader_ListPlugins(char paths[][260], int maxCount)
{
    (void)paths; (void)maxCount;
    return 0; /* plugin scan is Windows-only for now */
}

#endif
