/* warning_screen.c - PC-port "violent and disturbing images" warning.
 *
 * The original PSX game shows this in main() (src/main/main.c, before
 * Konami logo). On PC the entire main() is replaced by main_pc.c so the
 * PSX code never runs; this function reproduces just the warning step.
 * Mirrors src/main/main.c lines 106-177.
 *
 * Skipped if skip_intros = 1 in config.cfg.
 */
#include "common.h"
#include "game.h"
#include <psyq/libetc.h>
#include <psyq/libapi.h>
#include "main/fsqueue.h"
#include "sh_log.h"
#include "pc_config.h"

extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);

static s_FsImageDesc s_WarnImg = {
    .tPage = { 1, 13 },
    .u     = 32,
    .v     = 0,
    .clutX = 768,
    .clutY = 480
};

void Pc_PlayWarningScreen(void)
{
    s32 fade;
    s32 i;
    s32 sprtX;
    u8  primBuf[256];
    u8* prim;
    s32 holdFrame;

    if (g_PcConfig.skipIntros)
    {
        SH_DBG("[WARNSCR] skipped (skip_intros=1)");
        return;
    }

    SH_DBG("[WARNSCR] enter");

    Fs_QueueStartReadTim(FILE_1ST_2ZANKO_E_TIM, FS_BUFFER_0, &s_WarnImg);
    while (Fs_QueueGetLength() > 0)
    {
        Fs_QueueUpdate();
        VSync(SyncMode_Wait);
    }
    SH_DBG("[WARNSCR] TIM upload done; entering 64-frame fade");

    SetDispMask(1);

    /* 64-frame fade-in via subtractive-blend TILE on top of the static
     * SPRT image. fade goes 255 -> 0; framebuffer is cleared to black
     * each PsyX_BeginScene so we have to redraw the SPRTs every frame. */
    fade = 255;
    while (fade >= 0)
    {
        PsyX_BeginScene();

        prim = primBuf;

        /* Three 256x256 SPRTs covering screen with the loaded TIM. */
        for (i = 0, sprtX = -64; i < 3; sprtX += 128, i++)
        {
            DR_TPAGE* tp = (DR_TPAGE*)prim;
            setDrawTPage(tp, 0, 1, getTPageN(1, 0, i + 13, 0));
            DrawPrim(tp);
            {
                SPRT* sp = (SPRT*)prim;
                setSprt(sp);
                setRGB0(sp, 0x80, 0x80, 0x80);
                setWH(sp, 256, 256);
                setXY0(sp, sprtX, -8);
                setUV0(sp, 0, 0);
                setClut(sp, s_WarnImg.clutX, s_WarnImg.clutY);
                DrawPrim(sp);
            }
        }

        /* Switch tpage to subtractive-blend, then draw fullscreen
         * darken-tile. As fade decreases, less darkening = image emerges. */
        {
            DR_TPAGE* tp = (DR_TPAGE*)prim;
            setDrawTPage(tp, 0, 1, getTPageN(0, 2, 0, 0));
            DrawPrim(tp);
        }
        {
            TILE* tile = (TILE*)prim;
            setTile(tile);
            setSemiTrans(tile, 1);
            setRGB0(tile, fade, fade, fade);
            setWH(tile, SCREEN_WIDTH, SCREEN_HEIGHT);
            setXY0(tile, 0, 0);
            DrawPrim(tile);
        }

        PsyX_EndScene();
        VSync(SyncMode_Wait);
        fade -= 4;
    }

    /* Hold final image for ~60 frames so player can read it. */
    for (holdFrame = 0; holdFrame < 60; holdFrame++)
    {
        PsyX_BeginScene();

        prim = primBuf;
        for (i = 0, sprtX = -64; i < 3; sprtX += 128, i++)
        {
            DR_TPAGE* tp = (DR_TPAGE*)prim;
            setDrawTPage(tp, 0, 1, getTPageN(1, 0, i + 13, 0));
            DrawPrim(tp);
            {
                SPRT* sp = (SPRT*)prim;
                setSprt(sp);
                setRGB0(sp, 0x80, 0x80, 0x80);
                setWH(sp, 256, 256);
                setXY0(sp, sprtX, -8);
                setUV0(sp, 0, 0);
                setClut(sp, s_WarnImg.clutX, s_WarnImg.clutY);
                DrawPrim(sp);
            }
        }

        PsyX_EndScene();
        VSync(SyncMode_Wait);
    }

    SH_DBG("[WARNSCR] done");
}
