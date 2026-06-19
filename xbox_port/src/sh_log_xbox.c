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
        /* Unbuffered: the render loop never exits to fclose, and nxdk does not
         * reliably commit line-buffered writes to the HDD mid-loop, so log lines
         * written during the loop were lost. _IONBF forces each line to disk.
         * (Revert to _IOLBF for perf once the renderer is stable.) */
        setvbuf(g_ShDebugLog, NULL, _IONBF, 0);
    }
}
