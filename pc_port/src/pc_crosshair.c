/* Center crosshair drawn while aiming in TPS / OTS (always in FPS), gated by the
 * `crosshair` config option; `crosshair_style` selects the shape. A small
 * translucent white 2D overlay, modeled on the cutscene-border draw: centered
 * screen coords -> AddPrim onto the g_OtTags0 overlay layer that OT2 draws on
 * top of the world. Translucency = a DR_MODE with ABR=0 (0.5*src + 0.5*dst) +
 * setSemiTrans, exactly like the border bars.
 *
 * The g_OtTags0[][4] overlay is CENTER-ORIGIN — the cutscene-border bars that
 * share it are authored around (0,0) = screen center — so every reticle shape
 * is built around (0,0). These 2D prims are added directly (no GTE/nclip), so
 * there is no backface culling: quad winding is irrelevant. */
#include "game.h"
#include "pc_config.h"
#include "control_style.h"

#include <libetc.h>
#include <libgs.h>

#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"

enum { CH_CROSS = 0, CH_DOT = 1, CH_CIRCLE = 2, CH_DASH = 3 };

#define CH_MAX_PRIMS 12  /* worst case = the circle's 8 ring quads */

/* Axis-aligned filled rectangle (POLY_G4 quad) at [x0,x1] x [y0,y1]. */
static void Ch_Rect(POLY_G4* q, int x0, int y0, int x1, int y1)
{
    setXY4(q, x0, y0, x1, y0, x0, y1, x1, y1);
}

/* Build the geometry for `style` into p[0..]; returns the prim count used. */
static int Pc_CrosshairBuild(POLY_G4* p, int style)
{
    switch (style)
    {
        case CH_DOT:
            Ch_Rect(&p[0], -2, -2, 2, 2); /* ~4px filled dot */
            return 1;

        case CH_CIRCLE:
        {
            /* Octagon ring: 8 annulus quads between an outer (r~6) and inner
             * (r~4) octagon, sampled at 0/45/90/.../315 degrees. */
            static const short OX[8] = {  6,  4,  0, -4, -6, -4,  0,  4 };
            static const short OY[8] = {  0,  4,  6,  4,  0, -4, -6, -4 };
            static const short IX[8] = {  4,  3,  0, -3, -4, -3,  0,  3 };
            static const short IY[8] = {  0,  3,  4,  3,  0, -3, -4, -3 };
            int i;
            for (i = 0; i < 8; i++)
            {
                int j = (i + 1) & 7;
                setXY4(&p[i], OX[i], OY[i], OX[j], OY[j], IX[i], IY[i], IX[j], IY[j]);
            }
            return 8;
        }

        case CH_DASH:
        {
            /* Gap cross: four short bars set off from centre by a gap (the "- -"
             * dashes with the target in between, plus vertical marks). */
            const int g = 3, d = 4, t = 1; /* gap / dash length / half-thickness */
            Ch_Rect(&p[0], -g - d, -t, -g,     t); /* left  */
            Ch_Rect(&p[1],  g,     -t,  g + d,  t); /* right */
            Ch_Rect(&p[2], -t, -g - d,  t, -g);     /* up    */
            Ch_Rect(&p[3], -t,  g,      t,  g + d); /* down  */
            return 4;
        }

        case CH_CROSS:
        default:
        {
            const int l = 6, t = 1; /* arm half-length / half-thickness */
            Ch_Rect(&p[0], -l, -t, l, t); /* horizontal bar */
            Ch_Rect(&p[1], -t, -l, t, l); /* vertical bar   */
            return 2;
        }
    }
}

void Pc_CrosshairDraw(void)
{
    static POLY_G4 s_pool[2][CH_MAX_PRIMS]; /* per double-buffer */
    static DR_MODE s_drMode[2];
    static int     s_inited = 0;

    POLY_G4* p;
    GsOT*    ot;
    int      buf, n, i;

    if (!g_PcConfig.crosshair) return;
    if (g_ControlStyle != ControlStyle_Tps && g_ControlStyle != ControlStyle_Ots && g_ControlStyle != ControlStyle_Fps) return;
    if (g_GameWork.gameState != GameState_InGame) return;
    /* Settled gameplay only — no reticle over cutscenes, menus, or the map. */
    if (g_SysWork.sysState != SysState_Gameplay) return;
    /* Aim-only in TPS/OTS; always shown in first-person (the view IS the aim). */
    if (g_ControlStyle != ControlStyle_Fps && !g_SysWork.playerCombat.isAiming) return;

    if (!s_inited)
    {
        s_inited = 1;
        for (i = 0; i < 2; i++)
        {
            setcode(&s_drMode[i], 0xE1);
            setlen(&s_drMode[i], 1);
            s_drMode[i].code[0] = 0xE1000200; /* tpage ABR=0 -> 0.5*src + 0.5*dst (translucent) */
        }
        for (i = 0; i < 2 * CH_MAX_PRIMS; i++)
        {
            POLY_G4* q = (POLY_G4*)&s_pool[0][0] + i;
            setPolyG4(q);
            setSemiTrans(q, 1);
        }
    }

    /* Build into the ACTIVE buffer's pool only (the other is being displayed),
     * so re-authoring coords per frame can't race the GPU. setPolyG4/setSemiTrans
     * from init persist; setXY4 only rewrites coords. */
    buf = g_ActiveBufferIdx;
    p   = s_pool[buf];
    n   = Pc_CrosshairBuild(p, g_PcConfig.crosshairStyle);

    for (i = 0; i < n; i++)
    {
        p[i].r0 = p[i].r1 = p[i].r2 = p[i].r3 = 255;
        p[i].g0 = p[i].g1 = p[i].g2 = p[i].g3 = 255;
        p[i].b0 = p[i].b1 = p[i].b2 = p[i].b3 = 255;
    }

    ot = &g_OtTags0[buf][4];
    for (i = 0; i < n; i++)
        AddPrim(ot, &p[i]);
    AddPrim(ot, &s_drMode[buf]); /* set ABR=0 (translucent) before the shapes draw */
}
