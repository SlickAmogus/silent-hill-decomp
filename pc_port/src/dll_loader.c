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

DllHandle DllLoader_Open(const char* path)
{
    /* Static lint before any code from the file can run (DllMain executes
     * inside LoadLibrary). Covers every open -- map overlays, chara pool,
     * plugins -- so a mod-installed maps/*.dll gets the same import screen
     * as a plugin. requirePluginExports=0: map DLLs export their own set;
     * the plugin loader re-audits with the contract check on top. */
    {
        char reason[256] = {0};
        /* Everything the game opens is a map/chara overlay = edited game
         * code, held to the strict fingerprint. (The runtime plugin channel
         * was reviewed and cut; DLL_AUDIT_PLUGIN exists only as a reserved
         * audit mode.) */
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

#endif
