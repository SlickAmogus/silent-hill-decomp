/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pc_file_ops.h. Nothing from the game is included here: <windows.h> and
 * the PSX headers cannot share a translation unit (see pc_ra_http.c). */

#include <stdio.h>
#include <string.h>

#include "pc_file_ops.h"

#if defined(_WIN32)

#include <windows.h>

/* 1601-01-01, where FILETIME starts, in .NET ticks. */
#define PCFILE_TICKS_1601 504911232000000000LL

int PcFile_Stat(const char* path, long long* size, long long* ticks)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d) ||
        (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    if (size)
        *size = ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
    if (ticks)
        *ticks = (((long long)d.ftLastWriteTime.dwHighDateTime << 32) |
                  (long long)d.ftLastWriteTime.dwLowDateTime) + PCFILE_TICKS_1601;
    return 1;
}

int PcFile_CopyKeepTime(const char* from, const char* to)
{
    /* A read-only target (a mod author's flag, kept by an earlier copy) would
     * refuse the overwrite. */
    DWORD attr = GetFileAttributesA(to);

    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesA(to, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return CopyFileA(from, to, FALSE) ? 1 : 0;
}

int PcFile_MoveReplace(const char* from, const char* to)
{
    /* MOVEFILE_REPLACE_EXISTING is documented as not for folders. */
    DWORD attr = GetFileAttributesA(from);

    if (attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    return MoveFileExA(from, to, (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}

#else

#include <sys/stat.h>
#include <sys/time.h>

/* 1970-01-01 in seconds from 0001-01-01. */
#define PCFILE_SECS_1970 62135596800LL

int PcFile_Stat(const char* path, long long* size, long long* ticks)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (size)
        *size = (long long)st.st_size;
    if (ticks)
        *ticks = ((long long)st.st_mtime + PCFILE_SECS_1970) * 10000000LL;
    return 1;
}

int PcFile_CopyKeepTime(const char* from, const char* to)
{
    struct stat    st;
    struct timeval tv[2];
    char           buf[65536];
    size_t         n;
    int            ok = 1;
    FILE*          in;
    FILE*          out;

    if (stat(from, &st) != 0 || (in = fopen(from, "rb")) == NULL)
        return 0;
    if ((out = fopen(to, "wb")) == NULL)
    {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
        {
            ok = 0;
            break;
        }
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    if (!ok)
    {
        remove(to);
        return 0;
    }
    tv[0].tv_sec  = st.st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec  = st.st_mtime;
    tv[1].tv_usec = 0;
    utimes(to, tv);
    return 1;
}

/* rename() in this image is PsyCross's memory-card call (libapi.c), which
 * answers success for an ordinary path and moves nothing. */
int PcFile_MoveReplace(const char* from, const char* to)
{
    (void)from;
    (void)to;
    return 0;
}

#endif
