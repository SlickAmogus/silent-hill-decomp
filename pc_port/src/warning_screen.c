/* warning_screen.c — PC-port "violent and disturbing images" warning.
 *
 * The original PSX game shows this in main() (src/main/main.c lines
 * 109-204) before any other state runs. It uses immediate-mode DrawPrim
 * with a custom DispEnv setup, which doesn't present cleanly through
 * PsyCross because PsyCross expects the OT-based GsClearOt / addPrim /
 * GsDrawOt / GsSwapDispBuff pipeline that the rest of the game uses.
 *
 * This rewrite uses that OT pipeline so the warning actually displays.
 * It mirrors the structure of GameState_KonamiLogo_Update — same
 * Screen_Init -> per-frame OT build -> GsSwapDispBuff -> GsDrawOt
 * sequence — just standalone, called from main_pc.c before MainLoop.
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
extern void BootScreen_ImageSegmentDraw(s_FsImageDesc* image, s32 otz,
                                         s32 vramX, s32 vramY,
                                         s32 w, s32 h, s32 x, s32 y);

static s_FsImageDesc s_WarnImg = {
    .tPage = { 1, 13 },
    .u     = 32,
    .v     = 0,
    .clutX = 768,
    .clutY = 480
};

/* Build the warning-image draw list into the active OT.
 * Mirrors the PSX main()'s 3×256x256 SPRT layout (src/main/main.c:175).
 * The image was loaded into VRAM at tpages 13/14/15 of texture row 0;
 * each 256x256 segment samples one tpage. The PC port doesn't apply
 * hor+ here so the layout matches PSX: -64, 64, 192. */
static void Warn_DrawImage(void)
{
    s32 i;
    s32 sprtX;
    for (i = 0, sprtX = -64; i < 3; sprtX += 128, i++)
    {
        /* Each segment is its own tpage column, so we synthesize
         * three image descs that point at tpages 13, 14, 15. */
        s_FsImageDesc seg = s_WarnImg;
        seg.tPage[1] = 13 + i;
        BootScreen_ImageSegmentDraw(&seg, 0, 0, 0, 256, 256, sprtX, -8);
    }
}

/* Add a fullscreen darken tile to the OT at otz=0 with intensity `fade`
 * (0..255). The tile uses semi-transparency in subtractive-blend mode so
 * `fade` directly reads as how dark the result is — fade=255 → black,
 * fade=0 → image emerges. PSX path used a separate DR_TPAGE+TILE pair
 * to switch to subtractive blend; here we just set the prim's tpage
 * abr bits directly. */
static void Warn_DrawFadeTile(s32 fade)
{
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0];
    TILE* tile = (TILE*)GsOUT_PACKET_P;
    DR_TPAGE* tp;

    /* Switch to subtractive blend (abr=2) by pushing a DR_TPAGE first. */
    tp = (DR_TPAGE*)tile;
    setDrawTPage(tp, 0, 1, getTPageN(0, 2, 0, 0));
    AddPrim(addr, tp);
    tile = (TILE*)((u8*)tp + sizeof(DR_TPAGE));

    addPrimFast(addr, tile, 3);
    setCodeWord(tile, PRIM_RECT | RECT_BLEND, 0);
    setRGB0(tile, fade, fade, fade);
    setWHFast(tile, SCREEN_WIDTH * 2, SCREEN_HEIGHT);
    setXY0Fast(tile, 0, 0);

    GsOUT_PACKET_P = (PACKET*)((u8*)tile + sizeof(TILE));
}

/* Per-frame swap+draw — mirrors the tail of GameState_KonamiLogo_Update.
 * Two OTs because the screen-fade subsystem uses OT2 for overlays; we
 * keep that habit even though this function only writes OT0. */
static void Warn_SwapAndDraw(void)
{
    VSync(SyncMode_Wait);
    GsSwapDispBuff();
    GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
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

    /* Same screen setup the Konami state does (interlaced, double-wide
     * framebuffer for the PSX 320x240→320x224 letterbox). Idempotent —
     * MainLoop's first state will call Screen_Init again with the same
     * parameters. */
    Screen_Init(SCREEN_WIDTH * 2, true);

    /* Initialize the OT pipeline state for the first frame. */
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

    /* 64-frame fade-in: fade goes 255 → -1 in steps of 4. The image
     * SPRTs are redrawn every frame because the framebuffer is cleared
     * each swap. */
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
