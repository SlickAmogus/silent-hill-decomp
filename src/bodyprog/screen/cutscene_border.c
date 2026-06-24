#include "game.h"
#ifdef SH_PC_PORT
#include "pc_config.h"
#endif

#include <libetc.h>
#include <libgs.h>

#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/view/structs.h"
#include "bodyprog/view/vc_main.h"
#include "bodyprog/view/vc_util.h"
#include "bodyprog/math/math.h"

// ========================================
// STATIC VARIABLES
// ========================================

#ifdef SH_PC_PORT
/* The positional initializers in the #else branch assume the PSX 1-long P_TAG.
 * On 64-bit PC, DECLARE_P_ADDR is 3 longs (8-byte addr + len/pgxp_index), so a
 * positional init shifts every subsequent field and the bars draw at garbage
 * coords off-screen. Init at runtime via the prim macros, exactly like the
 * screen-fade statics in screen_fade.c (Screen_FadeInitStatics). */
static DR_MODE D_800A8E98[2];
static POLY_G4 D_800A8EB0[4];
static int     s_borderStaticsInited = 0;

static void Screen_BorderInitStatics(void)
{
    /* Real drawable size from PsyCross (SDL window / desktop in borderless),
     * NOT g_PcConfig.windowWidth/Height — those hold the *configured* values
     * (4:3 default 640x480), so borderless-fullscreen on a 16:9 display kept
     * horScale=1.0 and the bars only spanned the 4:3 width, leaving the
     * widescreen side margins uncovered = black corner squares at the bar
     * height (user report). */
    extern int g_windowWidth;
    extern int g_windowHeight;

    s32 i;
    s16 halfW;

    /* One-time prim-type setup. Must run before setXY4 below, since setPolyG4
     * clears the primitive (including coords). */
    if (!s_borderStaticsInited)
    {
        s_borderStaticsInited = 1;

        for (i = 0; i < 2; i++)
        {
            setcode(&D_800A8E98[i], 0xE1);  /* P_TAG code → ParsePrimitive 0xE0 → tpage */
            setlen(&D_800A8E98[i], 1);
            D_800A8E98[i].code[0] = 0xE1000240; /* tpage=64 → ABR=2 → BM_SUBTRACT */
        }

        for (i = 0; i < 4; i++)
        {
            setPolyG4(&D_800A8EB0[i]);
            setSemiTrans(&D_800A8EB0[i], true);
        }
    }

    {
        const float psxAspect = 320.0f / 240.0f;
        const int   rw = g_windowWidth  > 0 ? g_windowWidth  : g_PcConfig.windowWidth;
        const int   rh = g_windowHeight > 0 ? g_windowHeight : g_PcConfig.windowHeight;
        const float winAspect = rh > 0 ? (float)rw / (float)rh : psxAspect;
        const float horScale = winAspect / psxAspect;
        halfW = (s16)(160.0f * horScale + 10.0f);
    }

    /* Width recomputed every call so a window resize is reflected.
     * [0]/[1] = top bar (per double-buffer), [2]/[3] = bottom bar.
     * Outer/inner bar Y are runtime-tunable (console BARTOP/BARBOT) while we pin
     * down the interlaced-buffer mapping; defaults are the PSX ±112/±96. */
    {
        extern int g_PsxBarOuter, g_PsxBarInner;
        const s16 o = (s16)g_PsxBarOuter, in = (s16)g_PsxBarInner;
        setXY4(&D_800A8EB0[0], -halfW, -o, halfW, -o, -halfW, -in, halfW, -in);
        setXY4(&D_800A8EB0[1], -halfW, -o, halfW, -o, -halfW, -in, halfW, -in);
        setXY4(&D_800A8EB0[2], -halfW,  o, halfW,  o, -halfW,  in, halfW,  in);
        setXY4(&D_800A8EB0[3], -halfW,  o, halfW,  o, -halfW,  in, halfW,  in);
    }
}
#else
static DR_MODE D_800A8E98[] = {
    { 0x3000000, { 0xE1000240, 0x0 } },
    { 0x3000000, { 0xE1000240, 0x0 } }
};

// TODO: Make a macro?
static POLY_G4 D_800A8EB0[] = {
    {
        0x8000000,
        0x0, 0x0, 0x0, 0x3A,
        -160, -112,
        0x0, 0x0, 0x0, 0x0,
        160, -112,
        0x0, 0x0, 0x0, 0x0,
        -160, -96,
        0x0, 0x0, 0x0, 0x0,
        160, -96
    },
    {
        0x8000000,
        0x0, 0x0, 0x0, 0x3A,
        -160, -112,
        0x0, 0x0, 0x0, 0x0,
        160, -112,
        0x0, 0x0, 0x0, 0x0,
        -160, -96,
        0x0, 0x0, 0x0, 0x0,
        160, -96
    },
    {
        0x8000000,
        0x0, 0x0, 0x0, 0x3A,
        -160, 112,
        0x0, 0x0, 0x0, 0x0,
        160, 112,
        0x0, 0x0, 0x0, 0x0,
        -160, 96,
        0x0, 0x0, 0x0, 0x0,
        160, 96
    },
    {
        0x8000000,
        0x0, 0x0, 0x0, 0x3A,
        -160, 112,
        0x0, 0x0, 0x0, 0x0,
        160, 112,
        0x0, 0x0, 0x0, 0x0,
        -160, 96,
        0x0, 0x0, 0x0, 0x0,
        160, 96
    }
};
#endif

static q19_12 g_BlackBorderShade = Q12(0.0f);

// ========================================
// CUTSCENE BORDERS
// ========================================

void Screen_CutsceneCameraStateUpdate(void) // 0x80032904
{
    void Screen_BlackBorderDraw(POLY_G4* poly, s32 color)
    {
        s32 i;
        s32 color0;
        s32 color1;

        color0 = color >> 4;
        color1 = color >> 5;

        if (color == Q12_CLAMPED(1.0f))
        {
            color1 = Q8_CLAMPED(1.0f);
        }

        for (i = 0; i < 2; i++)
        {
            poly[i * 2].r0 = color0;
            poly[i * 2].g0 = color0;
            poly[i * 2].b0 = color0;
            poly[i * 2].r1 = color0;
            poly[i * 2].g1 = color0;
            poly[i * 2].b1 = color0;
            poly[i * 2].r2 = color1;
            poly[i * 2].g2 = color1;
            poly[i * 2].b2 = color1;
            poly[i * 2].r3 = color1;
            poly[i * 2].g3 = color1;
            poly[i * 2].b3 = color1;
        }
    }

    GsOT*    ot;
    POLY_G4* poly;
    DR_MODE* drMode;

#ifdef SH_PC_PORT
    Screen_BorderInitStatics();
#endif

    drMode = &D_800A8E98[g_ActiveBufferIdx];
    poly   = &D_800A8EB0[g_ActiveBufferIdx];

    vcSetEvCamRate(g_BlackBorderShade);

    if (g_SysWork.bgmStatusFlags & BgmStatusFlag_Pause)
    {
        return;
    }

    switch (g_SysWork.cutsceneBorderState)
    {
        case CutsceneBorderState_FadeInStart:
            g_SysWork.cutsceneBorderState++;

        case CutsceneBorderState_FadingIn:
            g_BlackBorderShade += Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 1.0f);
            if (g_BlackBorderShade >= Q12_CLAMPED(1.0f))
            {
                g_BlackBorderShade = Q12_CLAMPED(1.0f);
                g_SysWork.cutsceneBorderState++;
            }

            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case CutsceneBorderState_ForceShow:
        case CutsceneBorderState_FadeOutStart:
            g_BlackBorderShade = Q12_CLAMPED(1.0f);
            g_SysWork.cutsceneBorderState++;

        case CutsceneBorderState_Shown:
            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case CutsceneBorderState_FadingOut:
            g_BlackBorderShade -= Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 1.0f);
            if (g_BlackBorderShade <= Q12(0.0f))
            {
                g_BlackBorderShade = Q12(0.0f);
                CutsceneBorder_Reset();
                return;
            }

            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case CutsceneBorderState_Reset:
            g_BlackBorderShade            = Q12(0.0f);
            g_SysWork.cutsceneBorderState = CutsceneBorderState_None;
            g_SysWork.sysFlags           &= ~SysFlag_CutsceneActive;
            return;

        case CutsceneBorderState_None:
            return;
    }

#ifdef SH_PC_PORT
    /* Fixed-camera cinematic border (NOT a full subtitled cutscene): draw the
     * letterbox bars in OT2 (the 2D-UI pass) instead of OT0. OT2 renders at FULL
     * vertical ortho (vscale 1.0), so the bars' ±112 lands on the true 224-line
     * screen edges. In OT0 they rode the 3D-world vertical crop (g_PsxWorldVScale
     * 0.872), which pulled them inward and left the scene visible in the gap at the
     * very top = the ghost half-bar. PsyCross already documents the bars as part of
     * the OT2 pass (the move that was never completed). Full cutscenes keep OT0 so
     * the bars stay behind their subtitles. */
    extern GsOT g_OrderingTable2[2];
    if (!(g_SysWork.sysFlags & SysFlag_CutsceneActive))
        ot = &g_OrderingTable2[g_ActiveBufferIdx];
    else
        ot = &g_OtTags0[g_ActiveBufferIdx][4];
#else
    ot = &g_OtTags0[g_ActiveBufferIdx][4];
#endif
    AddPrim(ot, poly);
    AddPrim(ot, &poly[2]);
    AddPrim(ot, drMode);

    if (!(g_SysWork.sysFlags & SysFlag_CutsceneActive))
    {
        vcChangeProjectionValue(g_GameWork.gsScreenHeight + Q12_MULT(377 - g_GameWork.gsScreenHeight, g_BlackBorderShade));
    }
}