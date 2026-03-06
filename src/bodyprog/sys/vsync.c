#include "game.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#endif

#include "bodyprog/demo.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sys/game_main.h"

void Screen_VSyncCallback(void) // 0x80032B80
{
#ifdef SH_PC_PORT
    {
        static int cbDbg = 0;
        if (cbDbg < 3) {
            printf("[SH] VSyncCallback called! counter0=%d\n", g_SysWork.counters_1C[0]);
            cbDbg++;
        }
    }
#endif
    g_Demo_FrameCount++;
    g_WarmBootTimer++;

    g_SysWork.counters_1C[0]++;
    g_SysWork.counters_1C[1]++;
    g_SysWork.counters_1C[2]++;
}
