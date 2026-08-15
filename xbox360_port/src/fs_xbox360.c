/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fs_xbox360.c - filesystem setup for the Xbox 360 port.
 *
 * The Xbox version of this file is almost entirely nxdk drive mounting (forcing
 * D: to the XBE directory, mounting E:, resolving E:\UDATA). libXenon needs none
 * of that: fatInitDefault() registers every FAT volume it finds under a devoptab
 * name, so the job here reduces to picking WHICH volume holds the game data and
 * making sure the save directory exists.
 *
 * Volume names come from libfat on libXenon: "uda:" is USB mass storage, "sda:"
 * the internal drive. USB is searched first because BadUpdate boots from a stick,
 * so one is always attached.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libfat/fat.h>

#include "sh_log.h"
#include "fs_xbox360.h"

static char s_dataRoot[SH360_PATH_MAX] = "";
static char s_binName[SH360_PATH_MAX]  = "";
static int  s_fatOk = 0;

const char* Sh360Fs_DataRoot(void) { return s_dataRoot; }
const char* Sh360Fs_BinName(void)  { return s_binName;  }

static int EndsWithNoCase(const char* s, const char* suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return 0;
    s += ls - lx;
    while (*s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*suffix))
            return 0;
        s++; suffix++;
    }
    return 1;
}

/* A directory qualifies as the data root if it holds a .bin. The disc image is
 * named by whoever ripped it, so scan rather than guess at filenames. */
static int DirHasBin(const char* dir, char* binOut, int binOutSize)
{
    DIR*           d = opendir(dir);
    struct dirent* e;
    int            found = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (EndsWithNoCase(e->d_name, ".bin")) {
            snprintf(binOut, binOutSize, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int Sh360Fs_Init(void)
{
    static const char* const kRoots[] = {
        "uda:/silenthill/", "uda:/", "sda:/silenthill/", "sda:/"
    };
    unsigned i;

    if (s_dataRoot[0])
        return 1;

    s_fatOk = fatInitDefault() ? 1 : 0;
    if (!s_fatOk) {
        SH_DBG("[FS] fatInitDefault FAILED - no volumes mounted");
        return 0;
    }

    for (i = 0; i < sizeof(kRoots) / sizeof(kRoots[0]); i++) {
        if (DirHasBin(kRoots[i], s_binName, sizeof(s_binName))) {
            snprintf(s_dataRoot, sizeof(s_dataRoot), "%s", kRoots[i]);
            SH_DBG("[FS] data root '%s' bin '%s'", s_dataRoot, s_binName);
            return 1;
        }
        SH_DBG("[FS] no .bin in '%s'", kRoots[i]);
    }

    /* No disc image. Still pick a writable root so logs and config work and the
     * failure shows up as "no BIN" rather than as a dead filesystem. */
    snprintf(s_dataRoot, sizeof(s_dataRoot), "uda:/");
    SH_DBG("[FS] NO .bin FOUND - defaulting root to '%s'", s_dataRoot);
    return 0;
}

static int EnsureDir(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 1;
    return mkdir(path, 0777) == 0;
}

/* Consumed by the reused mcard_xbox.c, which is why it keeps the XboxFs_ name. */
int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    char dir[SH360_PATH_MAX];

    Sh360Fs_Init();
    snprintf(dir, sizeof(dir), "%ssave", s_dataRoot);
    if (!EnsureDir(dir)) {
        SH_DBG("[MCRD] could not create '%s'", dir);
        return 0;
    }
    snprintf(out, outSize, "%s", dir);
    return 1;
}

/* MUST be called after writing any file this port cares about keeping.
 *
 * fflush only pushes stdio's buffer into libfat; the FAT directory entry, which
 * carries the file SIZE, is not rewritten until fsync or close. A console
 * session ends by pulling the power, so there is never a clean fclose -- the
 * first hardware boot produced a log file of size zero for exactly this reason.
 * Saves and config have the same exposure, not just the log. */
void Sh360Fs_Sync(FILE* f)
{
    if (!f)
        return;
    fflush(f);
    fsync(fileno(f));
}
