/* SPDX-License-Identifier: GPL-3.0-or-later */
/* <dirent.h> for the UWP build: the MSVC runtime has no POSIX directory API,
 * and the desktop Windows build gets this header from MinGW. */
#ifndef SH_UWP_DIRENT_H
#define SH_UWP_DIRENT_H

#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent
{
    char d_name[260];
};

typedef struct
{
    intptr_t              handle;
    struct __finddata64_t data;
    int                   pending;
    struct dirent         entry;
} DIR;

static inline DIR* opendir(const char* path)
{
    char pattern[1024];
    DIR* dir;

    if (snprintf(pattern, sizeof(pattern), "%s/*", path) >= (int)sizeof(pattern))
        return NULL;
    dir = (DIR*)calloc(1, sizeof(DIR));
    if (dir == NULL)
        return NULL;
    dir->handle = _findfirst64(pattern, &dir->data);
    if (dir->handle == -1)
    {
        free(dir);
        return NULL;
    }
    dir->pending = 1;
    return dir;
}

static inline struct dirent* readdir(DIR* dir)
{
    if (!dir->pending && _findnext64(dir->handle, &dir->data) != 0)
        return NULL;
    dir->pending = 0;
    strncpy(dir->entry.d_name, dir->data.name, sizeof(dir->entry.d_name) - 1);
    dir->entry.d_name[sizeof(dir->entry.d_name) - 1] = '\0';
    return &dir->entry;
}

static inline int closedir(DIR* dir)
{
    _findclose(dir->handle);
    free(dir);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
