/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_log_ps3.c - PS3 implementation of the shared sh_log.h contract.
 *
 * Same shape as xbox_port/src/sh_log_xbox.c and the 360's version: the game and
 * the reused pc_port files call SH_DBG(...), which writes to the FILE*
 * g_ShDebugLog. Here that lands next to the game data on /dev_hdd0 or
 * /dev_usb000, so a run can be read back by pulling the stick or over FTP.
 *
 * Under RPCS3 the same path resolves inside dev_hdd0/, which is how the desktop
 * loop reads the log without a console attached.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include "sh_log.h"
#include "fs_ps3.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;
void (*g_ShOverlayToastLine)(const char*) = NULL;

/* Log-volume gate, same rationale and same list as the Xbox and 360 ports: the
 * per-frame diagnostic probes flooded a multi-day session to 136 MB. */
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
    char  path[SH_PS3_PATH_MAX * 2], buf[16];
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
        char oldp[SH_PS3_PATH_MAX * 2];
        snprintf(oldp, sizeof(oldp), "%ssilenthill_%03d.log", dir, idx - SH_LOG_KEEP);
        remove(oldp);
    }
    return idx;
}

void SH_DebugLogInit(void)
{
    static char s_pathStore[SH_PS3_PATH_MAX * 2];

    if (g_ShDebugLog)
        return;

    /* Beside the game data, so the log and the disc image travel together and a
     * bug report is one directory. Falls back to the hdd root if the data root
     * was never resolved (no BIN found), which is exactly the case where the
     * log matters most. */
    {
        const char* root = Sh3Fs_DataRoot();
        if (!root || !root[0])
            root = "/dev_hdd0/";
        {
            int n = ShLog_NextIndex(root);
            snprintf(s_pathStore, sizeof(s_pathStore), "%ssilenthill_%03d.log", root, n);
            g_ShDebugLog = fopen(s_pathStore, "w");
            if (g_ShDebugLog)
                g_ShLogPath = s_pathStore;
        }
    }
    if (g_ShDebugLog) {
        /* LINE buffered, not fully buffered, and that is a bring-up decision
         * rather than the end state.
         *
         * A fault does not unwind on this platform -- RPCS3 freezes the thread
         * and lv2 kills the process -- so with a 256 KB full buffer every line
         * after the last explicit flush is lost, which is precisely the run-up
         * to the crash you need. The first PS3 boot ended in DrawOTag with a
         * log that stopped at "entering MainLoop" for exactly this reason.
         *
         * The buffer stays large so a line still costs one write and not a
         * malloc. Once the boot is stable this should go back to _IOFBF with a
         * crash handler doing the flush, the way the PC port does it. */
        setvbuf(g_ShDebugLog, s_logBuf, _IOLBF, sizeof(s_logBuf));
    }
}

void SH_DebugLogFlush(void)
{
    if (!g_ShDebugLog)
        return;
    /* fflush alone left a ZERO-BYTE log on the 360's first hardware boot: it
     * only pushes stdio's buffer into the filesystem layer, and the directory
     * entry that holds the file SIZE is not rewritten until fsync or close. A
     * console test session ends by powering off, so there is never a clean
     * fclose. The same is true of lv2's FAT-backed volumes. */
    fflush(g_ShDebugLog);
    fsync(fileno(g_ShDebugLog));
}

/* Heap use, reported as bytes IN USE rather than "free".
 *
 * The 360 learned this the hard way: returning mallinfo().fordblks -- free space
 * inside the current arena -- read a constant 6KB from the first tick of the
 * first boot and looked alarming. It was meaningless, because newlib grows the
 * arena on demand via sbrk, so free-inside-arena stays near zero however much
 * memory is available. Bytes in use is what actually answers the question this
 * probe exists for, which is whether something leaks across chunk streaming.
 *
 * The names are the Xbox port's because the shared callers use them. */
unsigned Xbox_MemFreeKB(void)
{
    struct mallinfo mi = mallinfo();
    return (unsigned)(mi.uordblks / 1024);
}

void Xbox_MemReport(const char* tag)
{
    struct mallinfo mi = mallinfo();
    SH_DBG("[MEM] %s: used=%uKB arena=%uKB", tag ? tag : "",
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
}
