/*
 * xbox_combat_fx.c - the PC port's low-health glow, which is plain PSX prims.
 *
 * pc_combat.c is excluded on Xbox for its SDL keyboard/controller binds (quick
 * heal, weapon cycle, manual reload, quick turn), but this effect never touched
 * either: it is four additive POLY_G4 bands in OT2 bucket 4, the same path the
 * screen fades take, so the NV2A draws it exactly as PsyCross does. The body is
 * pc_combat.c's Pc_LowHealthGlowUpdate minus the Hor+ widening (the Xbox frame
 * is always the 4:3 PSX one). Config: low_health_glow, Xbox Options page 4.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_config.h"
#include "bodyprog/text/text_draw.h"
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>   /* KeTickCount */

/* PC's GR_ImageAdjustActive with DbgOverlay_Render's scene-frame rule: the
 * grade applies to frames that render the 3D world (InGame / MapEvent) and to
 * the Brightness screen (g_cfg_calibBar), so its preview responds; never the
 * menus, paper map, save screen or FMVs. image_adjust_scope=0 grades all. */
int Xbox_ImageAdjustInScope(void)
{
    extern int g_cfg_calibBar;

    if (g_cfg_calibBar || !g_PcConfig.imageAdjustScope)
        return 1;
    /* In-game movies play under GameState_InGame through the real VSync. */
    if (g_SysWork.sysState == SysState_Fmv)
        return 0;
    return g_GameWork.gameState == GameState_InGame ||
           g_GameWork.gameState == GameState_MapEvent;
}

void Pc_LowHealthGlowUpdate(void)
{
    #define GLOW_HP_MAX   20
    #define GLOW_PERIOD   Q12(1.6f)
    #define GLOW_R_MIN    36
    #define GLOW_R_MAX    112
    static POLY_G4  s_band[2][4];
    static DR_TPAGE s_tp[2];
    static s32      s_phase;
    extern int      g_PsxCutsceneActive;
    extern int      g_PsxPresentLastFrame;
    s32 hp, pulse, strength, r, halfW, halfH, dx, dy, buf, i;

    if (!g_PcConfig.lowHealthGlow)
        return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
        return;
    if (g_PsxCutsceneActive || g_PsxPresentLastFrame)
        return;

    hp = g_SysWork.playerWork.player.health >> Q12_SHIFT;
    if (hp <= 0 || hp >= GLOW_HP_MAX)
        return;

    s_phase += g_DeltaTime;
    while (s_phase >= GLOW_PERIOD)
        s_phase -= GLOW_PERIOD;
    pulse    = (Math_Sin((s32)(((s64)s_phase * 4096) / GLOW_PERIOD)) + Q12(1.0f)) >> 1; /* 0..4096 */
    strength = ((GLOW_HP_MAX - hp) * Q12(1.0f)) / GLOW_HP_MAX;                           /* 0..4096 by deficit */
    r        = GLOW_R_MIN + (((GLOW_R_MAX - GLOW_R_MIN) * pulse) >> 12);
    r        = (r * (Q12(0.5f) + (strength >> 1))) >> 12;                                 /* half at 19 hp, full at 0 */
    if (r <= 0)
        return;

    halfH = SCREEN_HEIGHT / 2;
    halfW = SCREEN_WIDTH / 2;
    dy    = (halfH * 27) / 100;
    dx    = (halfW * 20) / 100;
    buf   = g_ActiveBufferIdx;

    for (i = 0; i < 4; i++)
    {
        POLY_G4* q = &s_band[buf][i];
        setPolyG4(q);
        setSemiTrans(q, 1);
        switch (i)
        {
            case 0: /* top: red along the top edge, black at the inner edge */
                setXY4(q, -halfW, -halfH, halfW, -halfH, -halfW, -halfH + dy, halfW, -halfH + dy);
                setRGB0(q, (u8)r, 0, 0); setRGB1(q, (u8)r, 0, 0); setRGB2(q, 0, 0, 0); setRGB3(q, 0, 0, 0);
                break;
            case 1: /* bottom */
                setXY4(q, -halfW, halfH - dy, halfW, halfH - dy, -halfW, halfH, halfW, halfH);
                setRGB0(q, 0, 0, 0); setRGB1(q, 0, 0, 0); setRGB2(q, (u8)r, 0, 0); setRGB3(q, (u8)r, 0, 0);
                break;
            case 2: /* left */
                setXY4(q, -halfW, -halfH, -halfW + dx, -halfH, -halfW, halfH, -halfW + dx, halfH);
                setRGB0(q, (u8)r, 0, 0); setRGB1(q, 0, 0, 0); setRGB2(q, (u8)r, 0, 0); setRGB3(q, 0, 0, 0);
                break;
            default: /* right */
                setXY4(q, halfW - dx, -halfH, halfW, -halfH, halfW - dx, halfH, halfW, halfH);
                setRGB0(q, 0, 0, 0); setRGB1(q, (u8)r, 0, 0); setRGB2(q, 0, 0, 0); setRGB3(q, (u8)r, 0, 0);
                break;
        }
        AddPrim(&g_OtTags0[buf][4], q);
    }
    /* Added last so it is drawn first: additive blend for the bands above. */
    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 1, 0, 0));
    AddPrim(&g_OtTags0[buf][4], &s_tp[buf]);

    #undef GLOW_HP_MAX
    #undef GLOW_PERIOD
    #undef GLOW_R_MIN
    #undef GLOW_R_MAX
}

/* PC's FPS Display (fps_display: 0 off, 1-4 = top-left, top-right, bottom-left,
 * bottom-right), drawn with the game font instead of the PC's GL overlay.
 * Counts the frames this hook sees -- one per presented frame -- over
 * half-second windows, so the number is what is actually reaching the TV.
 * Called from DbgOverlay_XboxRender (main menu onward). */
void Xbox_FpsCounterRender(void)
{
    extern int g_PsxPresentLastFrame;
    extern s16 Gfx_StringColorGet(void);
    static u32 s_winStartMs;
    static s32 s_winFrames;
    static s32 s_fps;
    char       buf[12];
    s32        corner = g_PcConfig.fpsDisplay;
    u32        now    = (u32)KeTickCount;
    s32        x, y, n, v;

    if (s_winStartMs == 0)
        s_winStartMs = now;
    s_winFrames++;
    if (now - s_winStartMs >= 500u)
    {
        s_fps        = (s_winFrames * 1000 + (s32)(now - s_winStartMs) / 2) / (s32)(now - s_winStartMs);
        s_winFrames  = 0;
        s_winStartMs = now;
    }

    if (corner < 1 || corner > 4)
        return;
    /* A held frame (pause, pickup, map message) re-presents the last live one,
     * counter included: a live reading over it would show two, so the frozen
     * one stands until play resumes. */
    if (g_PsxPresentLastFrame)
        return;

    v = (s_fps < 0) ? 0 : (s_fps > 999 ? 999 : s_fps);
    n = 0;
    if (v >= 100) buf[n++] = (char)('0' + v / 100);
    if (v >= 10)  buf[n++] = (char)('0' + (v / 10) % 10);
    buf[n++] = (char)('0' + v % 10);
    memcpy(buf + n, "_FPS", 5);

    /* Text y spans the 224-line frame and a glyph is 16 tall: SCREEN_HEIGHT (240)
     * put the bottom corners' digits below the screen. */
    x = (corner == 1 || corner == 3) ? 8 : SCREEN_WIDTH - 8 - 58;
    y = (corner == 1 || corner == 2) ? 4 : FRAMEBUFFER_HEIGHT_PROGRESSIVE - 4 - 16;

    /* Bucket 0 of OT2 is walked last, so the counter sits on top of the minimap
     * and the screen fade, like the PC overlay. The 448-line screens (title,
     * inventory, paper map) take the hi-res glyph path, which doubles y into
     * their frame. The game's text state is put back afterwards. */
    {
        const s16 color = Gfx_StringColorGet();
        const int hiRes = g_SysWork.enableHighResGlyphs;

        Gfx_StringSetColor(StringColorId_Green);
        Gfx_StringSetPosition(x, y);
        Gfx_Strings2dLayerIdxSet(0);
        g_SysWork.enableHighResGlyphs = (g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED);
        Gfx_StringDraw(buf, 12);
        g_SysWork.enableHighResGlyphs = hiRes;
        Gfx_StringsReset2dLayerIdx();
        Gfx_StringSetColor(color);
    }
}
