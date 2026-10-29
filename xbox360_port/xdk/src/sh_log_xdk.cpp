/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_log_xdk.cpp - the shared sh_log.h contract for the XDK build.
 *
 * Same shape as sh_log_xbox360.c (libXenon): SH_DBG writes to the FILE*
 * g_ShDebugLog. Here the file lands in game:\, the directory the .xex was
 * launched from, so a run on hardware reads back from the same stick or
 * folder the game sits in, and a run under Xenia reads back from the PC
 * folder directly.
 *
 * C++ only because the XDK's own headers are; every symbol is exported with
 * C linkage for the C tree.
 */
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>

extern "C" {
#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* Log-volume gate, same list and rationale as the other console ports: the
 * per-frame diagnostic probes flooded a multi-day session to 136 MB. */
int g_XboxLogDiag = 0;

int Sh_LogAllow(const char* fmt)
{
    static const char* const GATED[] = {
        "[SH_AUDIO]", "[SH_BGM]", "[UIDIAG]", "[UPD]", "[UPD2]", "[POST]",
        "[FOGST]", "[WALLSTOP]",
        "[WALL-HIT]", "[RAIN]", "[FSQ]", "[SS]", "[FXDROP]", "[BATCH]", "[STORE]",
        "[BIGPRIM]", "[ZETA]", "[ITEMZ]", "[FLEX]", "[FONTDUMP]", "[TXTPG]", "[TXSPR]",
        /* the 360 bring-up probes: periodic, ~10k lines a session on hardware
         * (run 027), each a USB write the render loop waits on */
        "[IPD-DRAW]", "[WMAT]", "[MESHDRAW]", "[ALTCULL]", "[A900]", "[POS]",
        "[CAMCHAIN]", "[RTPT]", "[VWMAT]", "[WEDGE]", "[TEXW]", "[RGB]", "[PTYPE]",
        "[PCODE]", "[OTS]", "[FOGPAD]", "[FOGCOL]", "[ABR]", "[TEXVRAM]", "[CHARA-TEX]",
        "[WOBJ]", "[VCOPY]", "[MESH]", "[CHNK]", "[WSMAT]", "[MELEEDMG]", "[MELEE-SS0]",
        "[MUZZLE]", "[ATTACK-DONE]", "[BOOT0/TIM]", "[TITLELOAD]", "[TITLEPAL]",
        "[TITLEPAGE]", "[TITLESPR]", "[TITLEQ]", "[INVSLOT]", "[IPD-INST]", "[LM-SWAP]",
        "[DMS-RAW]", "[EFFECTS]", "[DPCL]", "[GP-STATE-TXN]", "[PAD] buttons",
    };
    int i;
    if (g_XboxLogDiag)          return 1;
    if (!fmt || fmt[0] != '[')  return 1;
    for (i = 0; i < (int)(sizeof(GATED) / sizeof(GATED[0])); i++) {
        const char* g = GATED[i];
        const char* f = fmt;
        while (*g && *g == *f) { g++; f++; }
        if (*g == '\0') return 0;
    }
    return 1;
}

static char s_logBuf[256 * 1024];

const char* g_ShLogPath = "(none)";

#define SH_LOG_KEEP 20

static int ShLog_NextIndex(const char* dir)
{
    char  path[64], buf[16];
    int   idx = 0;
    FILE* f;
    _snprintf(path, sizeof(path), "%sshlog_idx.txt", dir);
    f = fopen(path, "r");
    if (f) { if (fgets(buf, sizeof(buf), f)) idx = atoi(buf); fclose(f); }
    if (idx < 0) idx = 0;
    if (idx < SH_LOG_KEEP)
        idx += SH_LOG_KEEP;
    f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", idx + 1); fclose(f); }
    if (idx >= SH_LOG_KEEP) {
        char oldp[64];
        _snprintf(oldp, sizeof(oldp), "%ssilenthill_%03d.log", dir, idx - SH_LOG_KEEP);
        remove(oldp);
    }
    return idx;
}

/* game:\ first: on a stick or HDD that is the game's own folder. Xenia mounts
 * a bare .xex's folder READ-ONLY, so there everything the port writes falls
 * through to the emulator's writable cache mount. */
static const char* const s_writeDirs[] = { "game:\\", "cache:\\", "hdd:\\" };

const char* Sh360Fs_WritableRoot(void)
{
    static const char* s_root;
    unsigned i;
    if (s_root)
        return s_root;
    for (i = 0; i < sizeof(s_writeDirs) / sizeof(s_writeDirs[0]); i++) {
        char  path[64];
        FILE* f;
        _snprintf(path, sizeof(path), "%ssh_write_probe.txt", s_writeDirs[i]);
        f = fopen(path, "w");
        if (f) {
            fclose(f);
            remove(path);
            s_root = s_writeDirs[i];
            return s_root;
        }
    }
    s_root = s_writeDirs[0];
    return s_root;
}

void SH_DebugLogInit(void)
{
    static char s_pathStore[64];
    const char* dir;
    int n;

    if (g_ShDebugLog)
        return;

    dir = Sh360Fs_WritableRoot();
    n = ShLog_NextIndex(dir);
    _snprintf(s_pathStore, sizeof(s_pathStore), "%ssilenthill_%03d.log", dir, n);
    g_ShDebugLog = fopen(s_pathStore, "w");
    if (g_ShDebugLog) {
        g_ShLogPath = s_pathStore;
        setvbuf(g_ShDebugLog, s_logBuf, _IOFBF, sizeof(s_logBuf));
    }
}

void SH_DebugLogFlush(void)
{
    if (!g_ShDebugLog)
        return;
    /* fflush alone leaves the data in the OS cache; a session on hardware ends
     * with the power button, so push it to the medium as well. Same lesson
     * the libXenon build learned with an empty first log. */
    fflush(g_ShDebugLog);
    /* Committing to the medium is a synchronous FAT update on a USB stick --
     * tens of ms -- and this runs from VSync once a second: a steady hitch
     * (run 027). Commit every 10th call (~10 s) instead. Boot, crash and quit
     * paths flush through their own fflush; the worst case on power-off is
     * the last ~10 s of log. */
    {
        static unsigned s_calls;
        if ((s_calls++ % 10) == 0)
            FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_ShDebugLog)));
    }
}

unsigned Xbox_MemFreeKB(void)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (unsigned)(ms.dwAvailPhys / 1024);
}

unsigned Xbox_MemUsedKB(void)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (unsigned)((ms.dwTotalPhys - ms.dwAvailPhys) / 1024);
}

void Xbox_MemReport(const char* tag)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    SH_DBG("[MEM] %s: free=%uKB total=%uKB", tag ? tag : "",
           (unsigned)(ms.dwAvailPhys / 1024), (unsigned)(ms.dwTotalPhys / 1024));
}

} /* extern "C" */
