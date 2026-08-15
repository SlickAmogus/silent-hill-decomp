/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_log_xbox360.c - Xbox 360 implementation of the shared sh_log.h contract.
 *
 * Same shape as xbox_port/src/sh_log_xbox.c: the game and the reused pc_port
 * files call SH_DBG(...), which writes to the FILE* g_ShDebugLog. Here that
 * handle lands on the USB stick (uda:) so a run can be read back on a PC
 * without a hard drive or a network path -- BadUpdate boots from USB anyway,
 * so the stick is always present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* Log-volume gate, same rationale and same list as the Xbox port: the per-frame
 * diagnostic probes flooded a multi-day session to 136 MB. It matters more here
 * than on Xbox -- this writes to a USB stick, not an internal disk. */
int g_XboxLogDiag = 0;

int Sh_LogAllow(const char* fmt)
{
    static const char* const GATED[] = {
        "[SH_AUDIO]", "[SH_BGM]", "[UIDIAG]", "[UPD]", "[UPD2]", "[POST]",
        "[FOGST]", "[FOGPAD]", "[ABR]", "[WALLSTOP]",
        "[WALL-HIT]", "[RAIN]", "[FSQ]", "[SS]", "[FXDROP]", "[BATCH]", "[STORE]",
        "[BIGPRIM]", "[ZETA]", "[ITEMZ]", "[FLEX]", "[FONTDUMP]", "[TXTPG]", "[TXSPR]",
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
    snprintf(path, sizeof(path), "%sshlog_idx.txt", dir);
    f = fopen(path, "r");
    if (f) { if (fgets(buf, sizeof(buf), f)) idx = atoi(buf); fclose(f); }
    if (idx < 0) idx = 0;
    if (idx < SH_LOG_KEEP)
        idx += SH_LOG_KEEP;
    f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", idx + 1); fclose(f); }
    if (idx >= SH_LOG_KEEP) {
        char oldp[64];
        snprintf(oldp, sizeof(oldp), "%ssilenthill_%03d.log", dir, idx - SH_LOG_KEEP);
        remove(oldp);
    }
    return idx;
}

void SH_DebugLogInit(void)
{
    /* uda: is the USB stick (libfat's devoptab name on libXenon) and is the only
     * device guaranteed present, since BadUpdate boots off it. sda: is the
     * internal drive when one is fitted and formatted. */
    static const char* const dirs[] = { "uda:/", "sda:/", "" };
    static char s_pathStore[64];
    int i;

    if (g_ShDebugLog)
        return;

    for (i = 0; i < 3 && !g_ShDebugLog; i++) {
        int n = ShLog_NextIndex(dirs[i]);
        snprintf(s_pathStore, sizeof(s_pathStore), "%ssilenthill_%03d.log", dirs[i], n);
        g_ShDebugLog = fopen(s_pathStore, "w");
        if (g_ShDebugLog)
            g_ShLogPath = s_pathStore;
    }
    if (g_ShDebugLog)
        setvbuf(g_ShDebugLog, s_logBuf, _IOFBF, sizeof(s_logBuf));
}

void SH_DebugLogFlush(void)
{
    if (g_ShDebugLog)
        fflush(g_ShDebugLog);
}

/* Heap headroom. The Xbox version reads free PHYSICAL pages via MmQueryStatistics;
 * libXenon has no equivalent, so this reports free bytes inside the newlib heap
 * instead. Different quantity, same use: watching it trend downward across chunk
 * streaming is what catches a leak. */
unsigned Xbox_MemFreeKB(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned)(mi.fordblks / 1024);
}

void Xbox_MemReport(const char* tag)
{
    unsigned freeKB = Xbox_MemFreeKB();
    SH_DBG("[MEM] %s: heap-free=%uKB", tag ? tag : "", freeKB);
}
