#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/text/text_draw.h"
#include "main/fsqueue.h"
#ifdef SH_PC_PORT
#include "main/fileinfo.h" /* g_GameRegion — PAL FONT16 reload */
#endif

void GameFs_TitleGfxSeek(void) // 0x80032BD0
{
#ifdef SH_PC_PORT
    /* NTSC-J title art = TIM/TITLE.TIM (same 8bpp 320x480 shape + desc as the
     * US TITLE_E, different artwork: black bg + scratched logo + copyright —
     * all baked pixels). Runtime mirror of the retail NTSCJ branch below;
     * everything downstream (draw, menu, fog) is region-identical. */
    if (g_GameRegion == Region_JPN)
    {
        Fs_QueueStartSeek(FILE_TIM_TITLE_TIM);
        return;
    }
#endif
#if VERSION_REGION_IS(NTSC)
    Fs_QueueStartSeek(FILE_TIM_TITLE_E_TIM);
#elif VERSION_REGION_IS(NTSCJ)
    Fs_QueueStartSeek(FILE_TIM_TITLE_TIM);
#endif
}

void GameFs_TitleGfxLoad(void) // 0x80032BF0
{
#ifdef SH_PC_PORT
    if (g_GameRegion == Region_JPN)
    {
        Fs_QueueStartReadTim(FILE_TIM_TITLE_TIM, FS_BUFFER_3, &g_TitleImg);
    }
    else
#endif
    {
#if VERSION_REGION_IS(NTSC)
    Fs_QueueStartReadTim(FILE_TIM_TITLE_E_TIM, FS_BUFFER_3, &g_TitleImg);
#elif VERSION_REGION_IS(NTSCJ)
    Fs_QueueStartReadTim(FILE_TIM_TITLE_TIM, FS_BUFFER_3, &g_TitleImg);
#endif
    }

#ifdef SH_PC_PORT
    /* PAL: the boot-time FONT16 upload at (768,128) is stomped by the Konami
     * logo (768,0..383) and BG_ETC (768,0..127) queued right after it — retail
     * SLES reloads the font from the B_KONAMI overlay for the same reason.
     * This is the universal pre-title chokepoint (normal boot, no-memcard,
     * skip_intros, warm reboot); FIFO queue order makes the font upload land
     * last, and every caller drains the queue before the menu draws. */
    if (g_GameRegion == Region_EUR)
    {
        Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
    }
#endif
}

void GameFs_StreamBinSeek(void) // 0x80032C20
{
    Fs_QueueStartSeek(FILE_VIN_STREAM_BIN);
}

void GameFs_StreamBinLoad(void) // 0x80032C40
{
    Fs_QueueStartRead(FILE_VIN_STREAM_BIN, FS_BUFFER_1);
}

void GameFs_OptionBinLoad(void) // 0x80032C68
{
    Fs_QueueStartReadTim(FILE_TIM_OPTION_TIM, FS_BUFFER_1, &g_ItemInspectionImg);
    Fs_QueueStartRead(FILE_VIN_OPTION_BIN, FS_BUFFER_1);
}

void GameFs_SaveLoadBinLoad(void) // 0x80032CA8
{
    Fs_QueueStartReadTim(FILE_TIM_SAVELOAD_TIM, FS_BUFFER_1, &g_ItemInspectionImg);
    Fs_QueueStartRead(FILE_VIN_SAVELOAD_BIN, FS_BUFFER_1);
}

void func_80032CE8(void) // 0x80032CE8
{
    Gfx_StringSetPosition(SCREEN_POSITION_X(33.75f), SCREEN_POSITION_Y(43.5f));
    Gfx_StringDraw("\x7Now_loading.", 100);
}
