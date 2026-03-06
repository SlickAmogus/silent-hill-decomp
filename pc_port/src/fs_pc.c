/*
 * fs_pc.c - PC File System implementation
 *
 * Provides file I/O that replaces PSX CD-ROM access.
 */
#include "fs_pc.h"

#include <stdio.h>
#include <string.h>

static char g_DataPath[512] = "./gamedata";

int FsPC_Init(const char* dataPath)
{
    if (dataPath)
    {
        strncpy(g_DataPath, dataPath, sizeof(g_DataPath) - 1);
        g_DataPath[sizeof(g_DataPath) - 1] = '\0';
    }
    return 0;
}

static void BuildFullPath(char* out, size_t outSize, const char* relativePath)
{
    snprintf(out, outSize, "%s/%s", g_DataPath, relativePath);

    /* Convert backslashes to forward slashes */
    for (char* p = out; *p; p++)
    {
        if (*p == '\\') *p = '/';
    }
}

int FsPC_ReadFile(const char* path, void* dest, size_t maxSize)
{
    char fullPath[1024];
    FILE* f;
    size_t bytesRead;

    BuildFullPath(fullPath, sizeof(fullPath), path);

    f = fopen(fullPath, "rb");
    if (!f)
    {
        fprintf(stderr, "FsPC: Failed to open: %s\n", fullPath);
        return -1;
    }

    bytesRead = fread(dest, 1, maxSize, f);
    fclose(f);

    return (int)bytesRead;
}

size_t FsPC_GetFileSize(const char* path)
{
    char fullPath[1024];
    FILE* f;
    long size;

    BuildFullPath(fullPath, sizeof(fullPath), path);

    f = fopen(fullPath, "rb");
    if (!f)
    {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);

    return (size > 0) ? (size_t)size : 0;
}

int FsPC_FileExists(const char* path)
{
    char fullPath[1024];
    FILE* f;

    BuildFullPath(fullPath, sizeof(fullPath), path);

    f = fopen(fullPath, "rb");
    if (f)
    {
        fclose(f);
        return 1;
    }
    return 0;
}
