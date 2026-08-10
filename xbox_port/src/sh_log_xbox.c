/*
 * sh_log_xbox.c - Xbox implementation of the shared sh_log.h logging contract.
 *
 * The game and the reused pc_port files call SH_DBG(...) (pc_port/include/sh_log.h),
 * which writes to the FILE* g_ShDebugLog. On PC that handle is SilentHill.log; here
 * it is D:\silenthill.log (nxdk maps the launch directory to D:\, matching the
 * Star Fox port's D:\starship.log convention). This keeps SH_DBG working on Xbox
 * with zero changes to the shared header.
 */
#include <stdio.h>
#include <stdlib.h>   /* atoi (per-run log index) */
#include <xboxkrnl/xboxkrnl.h>

#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;

/* Log-volume gate (config log_diag; 0 = quiet). The per-frame + periodic
 * diagnostic probes flooded a multi-day session to 136 MB (1.88 M lines) and
 * cost render-loop HDD flushes. Sh_LogAllow drops lines whose fmt starts with a
 * gated prefix; fmt is a compile-time literal so a dropped line costs one prefix
 * scan and NO vsnprintf/fwrite. Set log_diag=1 in the config to restore the full
 * diagnostic stream when a problem needs it. Essential boot/CD/FS/error prefixes
 * are deliberately NOT in the list, so they always log. */
int g_XboxLogDiag = 0;

int Sh_LogAllow(const char* fmt)
{
    static const char* const GATED[] = {
        "[SH_AUDIO]", "[SH_BGM]", "[UIDIAG]", "[UPD]", "[UPD2]", "[POST]",
        "[FOGST]", "[FOGPAD]", "[ABR]", "[WALLSTOP]",
        "[WALL-HIT]", "[RAIN]", "[FSQ]", "[SS]", "[FXDROP]", "[BATCH]", "[STORE]",
        "[BIGPRIM]", "[ZETA]", "[ITEMZ]", "[FLEX]", "[FONTDUMP]", "[TXTPG]", "[TXSPR]",
        /* NOT gated: [MEM] (RAM tick every ~10s -- the leak/creep diagnostic;
         * gating it hid the exact data needed to chase a progressive slowdown).
         *
         * NOT gated: [FT]. It was gated, and that is why four sessions of
         * slowdown reports carried no frame timing at all -- every perf theory
         * had to be inferred from side effects. It is one line per 120 frames
         * (~4/minute); the flood it was grouped with was per-frame probes.
         *
         * GATED: [BIGPRIM]. It is rate-limited 1/32 and still produced 11725 of
         * log 042's 16731 lines -- 70% of the file -- crowding out the data that
         * mattered. It has already told us what it can (the cafe draws ~6400
         * screen-sized prims); re-enable with log_diag=1 if needed again.
         *
         * NOT gated: [OTT] / [OTS]. The cycle-accurate split of the render frame
         * (walk / texture / submit / parse) was gated for the same reason [FT]
         * was, and left the second half of drawMs unexplained. Both are
         * per-window summaries, not per-frame floods. */
    };
    int i;
    if (g_XboxLogDiag)          return 1;   /* diag on: keep everything */
    if (!fmt || fmt[0] != '[')  return 1;   /* non-tagged line: keep */
    for (i = 0; i < (int)(sizeof(GATED) / sizeof(GATED[0])); i++) {
        const char* g = GATED[i];
        const char* f = fmt;
        while (*g && *g == *f) { g++; f++; }
        if (*g == '\0') return 0;           /* fmt begins with a gated prefix -> drop */
    }
    return 1;
}

/* RAM staging buffer for the log. Unbuffered (_IONBF) flushed every SH_DBG to the
 * HDD, which on the 733MHz/HDD path is a real per-line cost in the render loop.
 * A large full buffer (_IOFBF) makes SH_DBG a cheap memcpy; the loop flushes
 * periodically (SH_DebugLogFlush, called ~1/sec from VSync) so nothing is lost
 * mid-loop the way line-buffering dropped it. */
static char s_logBuf[256 * 1024];

/* Where the log actually opened (for the on-screen boot banner). */
const char* g_ShLogPath = "(none)";

/* Per-run log index (PC uses a timestamped filename; the Xbox RTC is often
 * unset/dead so a timestamp would collide or read 2000-01-01). A small counter
 * file on the same drive advances each boot so a new run does not overwrite the
 * previous log; SH_LOG_KEEP files are recycled. */
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
    /* MONOTONIC index (no wrap): the newest log is ALWAYS the highest-numbered
     * file. The old `idx %= 20` wrapped after silenthill_019 back to _000, so the
     * latest run was impossible to spot by name. Keep only a recent window by
     * deleting the log SH_LOG_KEEP behind. (One-time bump past any leftover
     * wrapped 0..19 counter so the number visibly climbs instead of re-using an
     * existing low file first.) */
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
    /* D: is the launch dir — READ-ONLY when booting from a disc/ISO (silently
     * gave "no log"). Fall back to E: (the writable data partition) and finally a
     * relative path. Whichever opens first wins; its counter file lives there. */
    static const char* const dirs[] = { "D:\\", "E:\\", "" };
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

/* Commit the RAM buffer to the HDD. The render loop never exits to fclose, so the
 * VSync wait path calls this every ~60 vblanks to bound log loss on a hang to ~1s. */
void SH_DebugLogFlush(void)
{
    if (g_ShDebugLog)
        fflush(g_ShDebugLog);
}

/* Heap-headroom probe. The reformat leak fixes should keep free RAM roughly flat
 * across chunk streaming / cutscenes; a monotonic drop means a leak survives.
 * AvailablePages is 4KB pages of free physical RAM (the pool malloc + the NV2A
 * contiguous allocator both draw from). Integer-only (nxdk printf drops %f). */
unsigned Xbox_MemFreeKB(void)
{
    MM_STATISTICS st;
    st.Length = sizeof(st);
    if (MmQueryStatistics(&st) != 0 /* STATUS_SUCCESS */)
        return 0;
    return (unsigned)st.AvailablePages * 4u;               /* pages -> KB */
}

void Xbox_MemReport(const char* tag)
{
    unsigned freeKB = Xbox_MemFreeKB();
    SH_DBG("[MEM] %s: free=%uKB (%u pages)", tag ? tag : "", freeKB, freeKB / 4u);
}
