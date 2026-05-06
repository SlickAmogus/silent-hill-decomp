/* warning_screen.c — PC-port "violent and disturbing images" warning.
 *
 * Renders the 2ZANKO_E warning image full-screen with a fade-in, mirroring
 * the PSX main()'s boot screen. Implementation follows the same Screen_Init
 * + centered-coord-system pattern as GameState_KonamiLogo_Update so the
 * image actually lands centered in the visible 4:3 area; the previous
 * approach of pushing an explicit 320×240 disp/draw env without calling
 * Screen_Init left activeDispEnv/drawenv in a hybrid state with PsyCross
 * (gs_screen_w/h still at the static defaults from libgs_stub.c init)
 * and the image rendered shrunk into one quadrant.
 *
 * Skipped if skip_intros = 1 in config.cfg.
 */
#include "common.h"
#include "game.h"
#include <psyq/libetc.h>
#include <psyq/libapi.h>
#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "screens/b_konami/b_konami.h"
#include "sh_log.h"
#include "pc_config.h"

extern void PsyX_EndScene(void);

/* TIM lives at VRAM tpage row 0 columns 13/14/15, CLUT at (768, 480). */
static s_FsImageDesc s_WarnImg = {
    .tPage = { 1, 13 },
    .u     = 32,
    .v     = 0,
    .clutX = 768,
    .clutY = 480
};

/* Draw the three 128×224-px SPRTs that make up the 2ZANKO image. After
 * Screen_Init(SCREEN_WIDTH * 2, true) the drawenv ofs is at the screen
 * center (320, 240) — same as the Konami logo path. We position the
 * image centered on (0, 0) in this coord system, so it lands centered
 * in the visible 4:3 area. The image is 384 px wide visually (3 tpages
 * × 128 px each, 4-bpp); we sample exactly the visible 128 px region
 * of each tpage (UV 0..127, V 0..223). Three SPRTs at sprtX = -192,
 * -64, 64 stitch the image symmetrically around 0, the same pattern
 * BootScreen_KonamiScreenDraw uses for the Konami logo.
 */
static void Warn_DrawImage(void)
{
    s32 i;
    s32 sprtX;
    /* g_OtTags0[][0xF] is the back (z=15) of g_OrderingTable2 (length=4
     * → 16 buckets). Image goes here; fade tile follows in the same
     * bucket and gets pushed to the head of the list, ending up in
     * front of the image at draw time. Same pattern as
     * BootScreen_KonamiScreenDraw. */
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0xF];

    for (i = 0, sprtX = -192; i < 3; sprtX += 128, i++)
    {
        SPRT* sp = (SPRT*)GsOUT_PACKET_P;
        DR_TPAGE* tp;

        addPrimFast(addr, sp, 4);
        setSprt(sp);
        setRGB0(sp, 0x80, 0x80, 0x80);
        setWH(sp, 128, 224);
        setXY0(sp, sprtX, -112);
        setUV0(sp, 0, 0);
        setClut(sp, s_WarnImg.clutX, s_WarnImg.clutY);

        tp = (DR_TPAGE*)((u8*)sp + sizeof(SPRT));
        setDrawTPage(tp, 0, 1, getTPageN(1, 0, 13 + i, 0));
        AddPrim(addr, tp);

        GsOUT_PACKET_P = (PACKET*)((u8*)tp + sizeof(DR_TPAGE));
    }
}

/* Subtractive-blend full-screen fade tile. Goes in front of the image,
 * so insert it after the SPRTs in the same OT bucket. */
static void Warn_DrawFadeTile(s32 fade)
{
    /* Last bucket (0xF) of OT2 — same as Konami fade tile. */
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0xF];
    DR_TPAGE* tp = (DR_TPAGE*)GsOUT_PACKET_P;
    TILE* tile;

    /* DR_TPAGE first to switch GPU into subtractive-blend (abr=2). */
    setDrawTPage(tp, 0, 1, getTPageN(0, 2, 0, 0));
    AddPrim(addr, tp);

    tile = (TILE*)((u8*)tp + sizeof(DR_TPAGE));
    addPrimFast(addr, tile, 3);
    setTile(tile);
    setSemiTrans(tile, 1);
    setRGB0(tile, fade, fade, fade);
    /* Cover the entire 640×480 framebuffer (centered drawenv has ofs
     * at (320,240); start at (-SCREEN_WIDTH, -SCREEN_HEIGHT) covers
     * (-320,-240) to (320,240) which is the whole fb). */
    setWH(tile, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
    setXY0(tile, -SCREEN_WIDTH, -SCREEN_HEIGHT);

    GsOUT_PACKET_P = (PACKET*)((u8*)tile + sizeof(TILE));
}

static void Warn_SwapAndDraw(void)
{
    VSync(SyncMode_Wait);
    GsSwapDispBuff();
    /* Draw via OT2 (where g_OtTags0 lives) — matches the OT bucket the
     * SPRTs and fade tile were pushed into. KonamiLogo uses the same OT. */
    GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
    PsyX_EndScene();

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
    GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);
}

void Pc_PlayWarningScreen(void)
{
    s32 fade;
    s32 holdFrame;

    if (g_PcConfig.skipIntros)
    {
        SH_DBG("[WARNSCR] skipped (skip_intros=1)");
        return;
    }

    SH_DBG("[WARNSCR] enter — initializing screen");

    /* Match the Konami-logo screen setup exactly: 640×480 interlaced
     * progressive-after-Screen_Init clip (drawenv.clip.h forced to 224).
     * Screen_Init internally calls GsInitGraph2 + GsDefDispBuff2 which
     * sets gs_screen_w/h to (640,480) and drawenv ofs to (320,240). */
    Screen_Init(SCREEN_WIDTH * 2, true);

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
    GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

    SH_DBG("[WARNSCR] queueing TIM load");
    Fs_QueueStartReadTim(FILE_1ST_2ZANKO_E_TIM, FS_BUFFER_0, &s_WarnImg);
    while (Fs_QueueGetLength() > 0)
    {
        Fs_QueueUpdate();
        VSync(SyncMode_Wait);
    }
    SH_DBG("[WARNSCR] TIM uploaded; starting fade-in");

    SetDispMask(1);

    /* 64-frame fade-in: fade goes 255 → -1 in steps of 4. */
    fade = 255;
    while (fade >= 0)
    {
        Warn_DrawImage();
        Warn_DrawFadeTile(fade);
        Warn_SwapAndDraw();
        fade -= 4;
    }

    SH_DBG("[WARNSCR] fade-in done; holding for 60 frames");

    /* Hold the fully-faded-in image so the player can read it. */
    for (holdFrame = 0; holdFrame < 60; holdFrame++)
    {
        Warn_DrawImage();
        Warn_SwapAndDraw();
    }

    SH_DBG("[WARNSCR] done");
}
