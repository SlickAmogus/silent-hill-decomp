/* warning_screen.c — PC-port "violent and disturbing images" warning.
 *
 * Mirrors the structure of GameState_KonamiLogo_Update — Screen_Init →
 * per-frame OT build → GsSwapDispBuff → GsDrawOt → PsyX_EndScene — but
 * with an explicit DrawEnv/DispEnv setup so the prim coordinates are
 * predictable. Without explicit setup, GsDefDispBuff2's choice of
 * buffer position leaves drawenv.ofs at non-zero values and the image
 * lands in the bottom-right quadrant of the framebuffer.
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

/* Explicit DispEnv / DrawEnv mirroring src/main/main.c lines 57-66.
 * 320×224 visible area, drawn into the 320×240 framebuffer at (0,0). */
static DISPENV s_WarnDispEnv = {
    .disp   = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT },
    .screen = { 0, 8, 256, 224 }
};
static DRAWENV s_WarnDrawEnv = {
    .clip = { 0, 0, SCREEN_WIDTH, 224 },
    .ofs  = { 0, 0 },
    .dtd  = 1,
    .isbg = 1,
    .r0 = 0, .g0 = 0, .b0 = 0
};

static void Warn_DrawImage(void)
{
    /* Three 256-wide SPRT segments. Each samples one of the three
     * vertically-adjacent texture pages (13, 14, 15) the TIM was
     * uploaded into. PSX coords: x = -64, 64, 192 with width 256
     * means the three segments overlap 50% with each other. The
     * actual image data is 384 px wide (3 tpages × 128 px) and the
     * draw rects exceed that to the sides — the off-image areas
     * sample CLUT index 0 (transparent, drops via setSemiTrans). */
    s32 i;
    s32 sprtX;
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0];

    for (i = 0, sprtX = -64; i < 3; sprtX += 128, i++)
    {
        SPRT* sp = (SPRT*)GsOUT_PACKET_P;
        DR_TPAGE* tp;

        addPrimFast(addr, sp, 4);
        setSprt(sp);
        setRGB0(sp, 0x80, 0x80, 0x80);
        setWH(sp, 256, 256);
        setXY0(sp, sprtX, -8);
        setUV0(sp, 0, 0);
        setClut(sp, s_WarnImg.clutX, s_WarnImg.clutY);

        tp = (DR_TPAGE*)((u8*)sp + sizeof(SPRT));
        setDrawTPage(tp, 0, 1, getTPageN(1, 0, 13 + i, 0));
        AddPrim(addr, tp);

        GsOUT_PACKET_P = (PACKET*)((u8*)tp + sizeof(DR_TPAGE));
    }
}

static void Warn_DrawFadeTile(s32 fade)
{
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0];
    DR_TPAGE* tp = (DR_TPAGE*)GsOUT_PACKET_P;
    TILE* tile;

    /* Push DR_TPAGE first to switch to subtractive-blend (abr=2). */
    setDrawTPage(tp, 0, 1, getTPageN(0, 2, 0, 0));
    AddPrim(addr, tp);

    tile = (TILE*)((u8*)tp + sizeof(DR_TPAGE));
    addPrimFast(addr, tile, 3);
    setTile(tile);
    setSemiTrans(tile, 1);
    setRGB0(tile, fade, fade, fade);
    setWH(tile, SCREEN_WIDTH, SCREEN_HEIGHT);
    setXY0(tile, 0, 0);

    GsOUT_PACKET_P = (PACKET*)((u8*)tile + sizeof(TILE));
}

static void Warn_SwapAndDraw(void)
{
    VSync(SyncMode_Wait);
    GsSwapDispBuff();
    PutDispEnv(&s_WarnDispEnv);
    PutDrawEnv(&s_WarnDrawEnv);
    GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
    PsyX_EndScene();

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
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

    /* Set up GPU work area at 320×240 progressive — same dimensions PSX
     * main() used. ResetGraph(0) was called in main_pc.c init, so we just
     * need to set the disp/draw envs. */
    PutDispEnv(&s_WarnDispEnv);
    PutDrawEnv(&s_WarnDrawEnv);

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);

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
