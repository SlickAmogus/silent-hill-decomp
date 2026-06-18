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

#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char*) = NULL;

void SH_DebugLogInit(void)
{
    if (g_ShDebugLog)
        return;

    g_ShDebugLog = fopen("D:\\silenthill.log", "w");
    if (g_ShDebugLog)
    {
        /* Line-buffered so a crash mid-frame still flushes complete lines, and
         * to avoid the per-call fflush cost the PC port documented (combat can
         * emit thousands of SH_DBG lines per frame). */
        setvbuf(g_ShDebugLog, NULL, _IOLBF, 0);
    }
}
