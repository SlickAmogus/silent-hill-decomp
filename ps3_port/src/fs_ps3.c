/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fs_ps3.c - filesystem setup for the PS3 port.
 *
 * The Xbox version of this file is almost entirely nxdk drive mounting and the
 * 360's is libfat devoptab probing. lv2 needs neither: GameOS mounts its volumes
 * itself and exposes them at fixed absolute paths, so the job here reduces to
 * picking WHICH directory holds the game data and making sure the save
 * directory exists.
 *
 * Search order is USB first, then the game's own USRDIR, then the hdd. USB
 * leads because a BIN is large and users keep it on a stick; USRDIR is second
 * because that is where a packaged .pkg install would put it.
 *
 * Newlib stdio works directly on these paths under PSL1GHT, so no lv2 sysFs
 * calls are needed here -- which conveniently keeps this TU clear of
 * <ppu-types.h> and lets it use SH_DBG.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "sh_log.h"
#include "fs_ps3.h"

static char s_dataRoot[SH_PS3_PATH_MAX] = "";
static char s_binName[SH_PS3_PATH_MAX]  = "";
static char s_saveDir[SH_PS3_PATH_MAX]  = "";
static int  s_done = 0;

const char* Sh3Fs_DataRoot(void) { return s_dataRoot; }
const char* Sh3Fs_BinName(void)  { return s_binName;  }

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

int Sh3Fs_Init(void)
{
    /* dev_usb000..007 are the USB ports; GameOS numbers them by insertion
     * order, so all eight are probed rather than assuming the first. */
    static const char* const CANDIDATES[] = {
        "/dev_usb000/silenthill/", "/dev_usb000/",
        "/dev_usb001/silenthill/", "/dev_usb001/",
        "/dev_usb002/silenthill/", "/dev_usb002/",
        "/dev_usb003/silenthill/", "/dev_usb003/",
        "/dev_hdd0/game/SHPS30001/USRDIR/",
        "/dev_hdd0/silenthill/",
        "/dev_hdd0/",
    };
    unsigned i;

    if (s_done)
        return s_binName[0] != '\0';
    s_done = 1;

    for (i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        char bin[SH_PS3_PATH_MAX];
        if (DirHasBin(CANDIDATES[i], bin, sizeof(bin))) {
            snprintf(s_dataRoot, sizeof(s_dataRoot), "%s", CANDIDATES[i]);
            snprintf(s_binName,  sizeof(s_binName),  "%s", bin);
            break;
        }
    }

    /* Saves go to the hdd even when the data is on USB: a stick can be pulled
     * mid-session, and losing a save to that is worse than the indirection. */
    snprintf(s_saveDir, sizeof(s_saveDir), "/dev_hdd0/game/SHPS30001/USRDIR/");
    mkdir("/dev_hdd0/game", 0777);
    mkdir("/dev_hdd0/game/SHPS30001", 0777);
    mkdir("/dev_hdd0/game/SHPS30001/USRDIR", 0777);

    return s_binName[0] != '\0';
}

const char* Sh3Fs_SaveDir(void)
{
    Sh3Fs_Init();
    return s_saveDir;
}

/* mcard_xbox.c calls this to find where memory-card images live. Same directory
 * as the rest of the save data. */
const char* XboxFs_ResolveSaveDir(void)
{
    return Sh3Fs_SaveDir();
}
