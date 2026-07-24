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
    idx %= SH_LOG_KEEP;
    f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", (idx + 1) % SH_LOG_KEEP); fclose(f); }
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
