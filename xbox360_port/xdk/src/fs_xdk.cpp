/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fs_xdk.cpp - filesystem setup for the XDK build.
 *
 * libXenon mounted FAT volumes by hand and searched uda:/ and sda:/. Under the
 * 360's own kernel there is nothing to mount: game:\ is always the directory
 * the .xex was launched from, on whatever medium that is (USB stick, HDD,
 * or a PC folder under Xenia). So the disc image lives next to the .xex, in
 * silenthill\ or the root, and everything the port writes goes there too.
 *
 * Paths use backslashes throughout: the 360 kernel does not treat '/' as a
 * separator.
 */
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <io.h>

extern "C" {
#include "sh_log.h"
#include "fs_xbox360.h"
}

static char s_dataRoot[SH360_PATH_MAX] = "";
static char s_binName[SH360_PATH_MAX]  = "";

extern "C" const char* Sh360Fs_DataRoot(void) { return s_dataRoot; }
extern "C" const char* Sh360Fs_BinName(void)  { return s_binName;  }

/* A directory qualifies as the data root if it holds a .bin. The disc image is
 * named by whoever ripped it, so scan rather than guess at filenames. */
static int DirHasBin(const char* dir, char* binOut, int binOutSize)
{
    char            pat[SH360_PATH_MAX];
    WIN32_FIND_DATA fd;
    HANDLE          h;

    _snprintf(pat, sizeof(pat), "%s*.bin", dir);
    pat[sizeof(pat) - 1] = '\0';
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    _snprintf(binOut, binOutSize, "%s", fd.cFileName);
    binOut[binOutSize - 1] = '\0';
    FindClose(h);
    return 1;
}

extern "C" int Sh360Fs_Init(void)
{
    static const char* const kRoots[] = { "game:\\silenthill\\", "game:\\" };
    unsigned i;

    if (s_dataRoot[0])
        return 1;

    for (i = 0; i < sizeof(kRoots) / sizeof(kRoots[0]); i++) {
        if (DirHasBin(kRoots[i], s_binName, sizeof(s_binName))) {
            _snprintf(s_dataRoot, sizeof(s_dataRoot), "%s", kRoots[i]);
            SH_DBG("[FS] data root '%s' bin '%s'", s_dataRoot, s_binName);
            return 1;
        }
        SH_DBG("[FS] no .bin in '%s'", kRoots[i]);
    }

    /* No disc image. Still pick a writable root so logs and config work and the
     * failure shows up as "no BIN" rather than as a dead filesystem. */
    _snprintf(s_dataRoot, sizeof(s_dataRoot), "game:\\");
    SH_DBG("[FS] NO .bin FOUND - defaulting root to '%s'", s_dataRoot);
    return 0;
}

static int EnsureDir(const char* path)
{
    if (CreateDirectory(path, NULL))
        return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Consumed by the reused mcard_xbox.c, which is why it keeps the XboxFs_ name. */
extern "C" int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    char dir[SH360_PATH_MAX];

    Sh360Fs_Init();
    _snprintf(dir, sizeof(dir), "%ssave", s_dataRoot);
    dir[sizeof(dir) - 1] = '\0';
    if (!EnsureDir(dir)) {
        /* Read-only game folder (Xenia): keep the card beside the log. */
        extern const char* Sh360Fs_WritableRoot(void);   /* sh_log_xdk.cpp */
        _snprintf(dir, sizeof(dir), "%ssilenthill_save", Sh360Fs_WritableRoot());
        dir[sizeof(dir) - 1] = '\0';
        if (!EnsureDir(dir)) {
            SH_DBG("[MCRD] could not create '%s'", dir);
            return 0;
        }
    }
    _snprintf(out, outSize, "%s", dir);
    out[outSize - 1] = '\0';
    return 1;
}

extern "C" void Sh360Fs_Sync(FILE* f)
{
    if (!f)
        return;
    fflush(f);
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
}
