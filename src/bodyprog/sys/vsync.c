#include "game.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include "bodyprog/libsd.h"
#endif

#include "bodyprog/demo.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sys/game_main.h"

void Screen_VSyncCallback(void) // 0x80032B80
{
    g_Demo_FrameCount++;
    g_WarmBootTimer++;

    g_SysWork.counters_1C[0]++;
    g_SysWork.counters_1C[1]++;
    g_SysWork.counters_1C[2]++;

#ifdef SH_PC_PORT
    /* On PSX, SsSetTickMode configures libsnd to call SsSeqCalledTbyT
     * from the VSync interrupt. Since libsnd is stubbed, we call the
     * Konami wrapper directly to pump the MIDI sequencer each frame. */
    SdSeqCalledTbyT();
#endif
}
