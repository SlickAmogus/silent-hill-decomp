/* Center crosshair drawn while aiming in TPS / OTS, gated by the `crosshair`
 * config option. A small translucent white "+" 2D overlay, modeled on the
 * cutscene-border draw (centered screen coords -> AddPrim onto the g_OtTags0
 * overlay layer that OT2 draws on top of the world). Translucency = a DR_MODE
 * with ABR=0 (0.5*src + 0.5*dst) + setSemiTrans, exactly like the border bars. */
#include "game.h"
#include "pc_config.h"
#include "control_style.h"

#include <libetc.h>
#include <libgs.h>

#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"

void Pc_CrosshairDraw(void)
{
    #define CH_LEN   6   /* arm half-length from center */
    #define CH_THICK 1   /* arm half-thickness (2px bar) */

    static POLY_G4 s_poly[4];   /* [0]/[1] = horizontal bar, [2]/[3] = vertical (per double-buffer) */
    static DR_MODE s_drMode[2];
    static int     s_inited = 0;

    extern u16 g_Player_IsAttacking;

    GsOT*    ot;
    POLY_G4* poly;
    DR_MODE* drMode;
    int      i;

    if (!g_PcConfig.crosshair) return;
    if (g_ControlStyle != ControlStyle_Tps && g_ControlStyle != ControlStyle_Ots) return;
    if (g_GameWork.gameState != GameState_InGame) return;
    if (!(g_SysWork.playerCombat.isAiming || g_Player_IsAttacking)) return;

    if (!s_inited)
    {
        s_inited = 1;
        for (i = 0; i < 2; i++)
        {
            setcode(&s_drMode[i], 0xE1);
            setlen(&s_drMode[i], 1);
            s_drMode[i].code[0] = 0xE1000200; /* tpage ABR=0 -> 0.5*src + 0.5*dst (translucent) */
        }
        for (i = 0; i < 4; i++)
        {
            setPolyG4(&s_poly[i]);
            setSemiTrans(&s_poly[i], 1);
        }
        /* Center on screen. The bars were authored around (0,0) (top-left corner);
         * the crosshair shares OT2's UI-ortho coordinate space (same as subtitles),
         * so offset by half the logical screen. */
        #define CH_CX (SCREEN_WIDTH  / 2)
        #define CH_CY (SCREEN_HEIGHT / 2)
        for (i = 0; i < 2; i++)
        {
            setXY4(&s_poly[i],     CH_CX - CH_LEN,   CH_CY - CH_THICK,  CH_CX + CH_LEN,   CH_CY - CH_THICK,  CH_CX - CH_LEN,   CH_CY + CH_THICK,  CH_CX + CH_LEN,   CH_CY + CH_THICK);
            setXY4(&s_poly[i + 2], CH_CX - CH_THICK, CH_CY - CH_LEN,    CH_CX + CH_THICK, CH_CY - CH_LEN,    CH_CX - CH_THICK, CH_CY + CH_LEN,    CH_CX + CH_THICK, CH_CY + CH_LEN);
        }
        #undef CH_CX
        #undef CH_CY
    }

    for (i = 0; i < 4; i++)
    {
        s_poly[i].r0 = s_poly[i].r1 = s_poly[i].r2 = s_poly[i].r3 = 255;
        s_poly[i].g0 = s_poly[i].g1 = s_poly[i].g2 = s_poly[i].g3 = 255;
        s_poly[i].b0 = s_poly[i].b1 = s_poly[i].b2 = s_poly[i].b3 = 255;
    }

    poly   = &s_poly[g_ActiveBufferIdx];
    drMode = &s_drMode[g_ActiveBufferIdx];
    ot     = &g_OtTags0[g_ActiveBufferIdx][4];
    AddPrim(ot, poly);       /* horizontal bar */
    AddPrim(ot, &poly[2]);   /* vertical bar */
    AddPrim(ot, drMode);     /* set ABR=0 (translucent) before the bars draw */

    #undef CH_LEN
    #undef CH_THICK
}
