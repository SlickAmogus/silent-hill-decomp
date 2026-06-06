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
    s32 i;
    s16 halfW;

    if (s_borderStaticsInited)
    {
        return;
    }
    s_borderStaticsInited = 1;

    {
        const float psxAspect = 320.0f / 240.0f;
        const float winAspect = g_PcConfig.windowHeight > 0
            ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight
            : psxAspect;
        const float horScale = winAspect / psxAspect;
        halfW = (s16)(160.0f * horScale + 10.0f);
    }

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
    /* [0]/[1] = top bar (per double-buffer), [2]/[3] = bottom bar. */
    setXY4(&D_800A8EB0[0], -halfW, -112, halfW, -112, -halfW, -96, halfW, -96);
    setXY4(&D_800A8EB0[1], -halfW, -112, halfW, -112, -halfW, -96, halfW, -96);
    setXY4(&D_800A8EB0[2], -halfW,  112, halfW,  112, -halfW,  96, halfW,  96);
    setXY4(&D_800A8EB0[3], -halfW,  112, halfW,  112, -halfW,  96, halfW,  96);
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

    switch (g_SysWork.field_30)
    {
        case 18:
            g_SysWork.field_30++;

        case 19:
            g_BlackBorderShade += Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 1.0f);
            if (g_BlackBorderShade >= Q12_CLAMPED(1.0f))
            {
                g_BlackBorderShade = Q12_CLAMPED(1.0f);
                g_SysWork.field_30++;
            }

            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case 20:
        case 22:
            g_BlackBorderShade = Q12_CLAMPED(1.0f);
            g_SysWork.field_30++;

        case 21:
            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case 23:
            g_BlackBorderShade -= Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 1.0f);
            if (g_BlackBorderShade <= Q12(0.0f))
            {
                g_BlackBorderShade = Q12(0.0f);
                g_SysWork.field_30 = 0;
                return;
            }

            Screen_BlackBorderDraw(poly, g_BlackBorderShade);
            break;

        case 0:
            g_BlackBorderShade    = Q12(0.0f);
            g_SysWork.field_30    = 1;
            g_SysWork.flags_22A4 &= ~UnkSysFlag_3;
            return;

        case 1:
            return;
    }

    ot = &g_OtTags0[g_ActiveBufferIdx][4];
    AddPrim(ot, poly);
    AddPrim(ot, &poly[2]);
    AddPrim(ot, drMode);

    if (!(g_SysWork.flags_22A4 & UnkSysFlag_3))
    {
        vcChangeProjectionValue(g_GameWork.gsScreenHeight + Q12_MULT(377 - g_GameWork.gsScreenHeight, g_BlackBorderShade));
    }
}