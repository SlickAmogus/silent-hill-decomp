/*
 * dll_loader.c — OS-specific shared library loading.
 *
 * This file ONLY includes OS headers. It must NOT include any game or
 * decomp headers, because <windows.h> conflicts with PSX type definitions
 * (EnterCriticalSection macro, 'byte' typedef, etc.).
 */
#include "dll_loader.h"
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char s_errorBuf[256] = {0};

DllHandle DllLoader_Open(const char* path)
{
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

int DllLoader_ListPlugins(char fileNames[][260], int maxFiles)
{
    int count = 0;
    WIN32_FIND_DATAA fd;

    /* 1. Direct plugins/ directory */
    HANDLE h = FindFirstFileA("plugins\\*.dll", &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < maxFiles)
            {
                snprintf(fileNames[count], 260, "plugins\\%s", fd.cFileName);
                count++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    /* 2. Mod folder layout: mods/<mod_name>/plugins/*.dll */
    WIN32_FIND_DATAA modFd;
    HANDLE modH = FindFirstFileA("mods\\*", &modFd);
    if (modH != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((modFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(modFd.cFileName, ".") != 0 && strcmp(modFd.cFileName, "..") != 0)
            {
                char subPluginSearch[260];
                snprintf(subPluginSearch, sizeof(subPluginSearch), "mods\\%s\\plugins\\*.dll", modFd.cFileName);
                WIN32_FIND_DATAA subFd;
                HANDLE subH = FindFirstFileA(subPluginSearch, &subFd);
                if (subH != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (!(subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < maxFiles)
                        {
                            char subPluginPath[260];
                            snprintf(subPluginPath, sizeof(subPluginPath), "mods\\%s\\plugins\\%s", modFd.cFileName, subFd.cFileName);

                            /* Check if already added */
                            int alreadyAdded = 0;
                            for (int i = 0; i < count; i++)
                            {
                                if (strcmp(fileNames[i], subPluginPath) == 0 ||
                                    strstr(fileNames[i], subFd.cFileName) != NULL)
                                {
                                    alreadyAdded = 1;
                                    break;
                                }
                            }
                            if (!alreadyAdded)
                            {
                                strncpy(fileNames[count], subPluginPath, 259);
                                fileNames[count][259] = '\0';
                                count++;
                            }
                        }
                    } while (FindNextFileA(subH, &subFd));
                    FindClose(subH);
                }
            }
        } while (FindNextFileA(modH, &modFd));
        FindClose(modH);
    }

    return count;
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
