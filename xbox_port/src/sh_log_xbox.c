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

void SH_DebugLogInit(void)
{
    if (g_ShDebugLog)
        return;

    g_ShDebugLog = fopen("D:\\silenthill.log", "w");
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
void Xbox_MemReport(const char* tag)
{
    MM_STATISTICS st;
    st.Length = sizeof(st);
    if (MmQueryStatistics(&st) == 0 /* STATUS_SUCCESS */)
    {
        unsigned freeKB = (unsigned)st.AvailablePages * 4u;   /* pages -> KB */
        SH_DBG("[MEM] %s: free=%uKB (%u pages)", tag ? tag : "", freeKB, (unsigned)st.AvailablePages);
    }
}
