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
#include <unistd.h>   /* fsync -- libxenon provides it, newlib does not */

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
    if (!g_ShDebugLog)
        return;
    /* fflush alone produced a log file of size ZERO on the first hardware boot.
     * It only pushes the stdio buffer down to libfat; the FAT directory entry,
     * which is where the file's SIZE lives, is not rewritten until fsync or
     * close. A test session always ends by powering the console off, so there is
     * never a clean fclose -- without the fsync every run reads back empty. */
    fflush(g_ShDebugLog);
    fsync(fileno(g_ShDebugLog));
}

/* Heap use. The Xbox version reads free PHYSICAL pages via MmQueryStatistics;
 * libXenon has no equivalent.
 *
 * This reports bytes IN USE, not "free". The first version returned
 * mallinfo().fordblks -- free space in the current arena -- which read a
 * constant 6KB from the first tick of the first boot and looked alarming. It was
 * meaningless: newlib grows the arena on demand via sbrk, so free-inside-arena
 * stays near zero no matter how much memory is available. Bytes in use is what
 * actually answers the question this probe exists for, which is whether
 * something leaks across chunk streaming. */
unsigned Xbox_MemUsedKB(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned)(mi.uordblks / 1024);
}

unsigned Xbox_MemFreeKB(void)
{
    return Xbox_MemUsedKB();   /* name kept for the shared callers */
}

void Xbox_MemReport(const char* tag)
{
    struct mallinfo mi = mallinfo();
    SH_DBG("[MEM] %s: used=%uKB arena=%uKB", tag ? tag : "",
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
}
