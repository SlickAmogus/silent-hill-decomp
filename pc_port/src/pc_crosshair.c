/* Center crosshair drawn while aiming in TPS / OTS, gated by the `crosshair`
 * config option. A simple white "+" 2D overlay, modeled on the cutscene-border
 * draw (centered screen coords -> AddPrim onto the g_OtTags0 overlay layer that
 * OT2 draws on top of the world). */
#include "game.h"
#include "pc_config.h"
#include "control_style.h"

#include <libetc.h>
#include <libgs.h>

#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"

void Pc_CrosshairDraw(void)
{
    #define CH_LEN   8   /* arm half-length from center */
    #define CH_THICK 1   /* arm half-thickness */

    static POLY_G4 s_poly[4];   /* [0]/[1] = horizontal bar, [2]/[3] = vertical (per double-buffer) */
    static int     s_inited = 0;

    extern u16 g_Player_IsAttacking;

    GsOT*    ot;
    POLY_G4* poly;
    int      i;

    if (!g_PcConfig.crosshair) return;
    if (g_ControlStyle != ControlStyle_Tps && g_ControlStyle != ControlStyle_Ots) return;
    if (g_GameWork.gameState != GameState_InGame) return;
    if (!(g_SysWork.playerCombat.isAiming || g_Player_IsAttacking)) return;

    if (!s_inited)
    {
        s_inited = 1;
        for (i = 0; i < 4; i++)
            setPolyG4(&s_poly[i]);
        for (i = 0; i < 2; i++)
        {
            setXY4(&s_poly[i],     -CH_LEN, -CH_THICK,  CH_LEN, -CH_THICK,  -CH_LEN, CH_THICK,  CH_LEN, CH_THICK);
            setXY4(&s_poly[i + 2], -CH_THICK, -CH_LEN,  CH_THICK, -CH_LEN,  -CH_THICK, CH_LEN,  CH_THICK, CH_LEN);
        }
    }

    for (i = 0; i < 4; i++)
    {
        s_poly[i].r0 = s_poly[i].r1 = s_poly[i].r2 = s_poly[i].r3 = 255;
        s_poly[i].g0 = s_poly[i].g1 = s_poly[i].g2 = s_poly[i].g3 = 255;
        s_poly[i].b0 = s_poly[i].b1 = s_poly[i].b2 = s_poly[i].b3 = 255;
    }

    poly = &s_poly[g_ActiveBufferIdx];
    ot   = &g_OtTags0[g_ActiveBufferIdx][4];
    AddPrim(ot, poly);       /* horizontal bar */
    AddPrim(ot, &poly[2]);   /* vertical bar */

    #undef CH_LEN
    #undef CH_THICK
}
