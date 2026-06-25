/* Camera / control-style system — see control_style.h.
 *
 * Replaces the old debug-only Numpad-2 TPS toggle. The active style comes from
 * config (control_style), is toggled in-game by the rebindable Change-Camera
 * action (key_change_cam / pad_change_cam) without allow_debug_controls, and is
 * persisted back to config. The registry is published (control_styles) on boot
 * so the launcher lists exactly what this build supports. */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "sh_log.h"
#include "pc_config.h"
#include "control_style.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int g_ControlStyle = ControlStyle_Classic;
int g_OtsSide      = 1; /* +1 = right shoulder, -1 = left */

/* Set when entering TPS/OTS from classic so the orbit camera reseeds behind Harry
 * (read + cleared in Pc_TpsCamera_Apply) instead of popping from a stale pose. */
int g_TpsCamNeedsReset = 0;

/* Runtime "TPS camera active" flag the camera + player_control code already
 * read. Driven (mirrored) by g_ControlStyle so those read sites stay unchanged.
 * Set for both TPS and OTS (OTS is TPS with a lateral offset). */
extern int g_DebugThirdPersonCam;

static const struct { const char* id; const char* label; } g_ControlStyles[] = {
    { "classic", "Classic (Default)" },
    { "tps",     "Thirdperson Shooter" },
    { "ots",     "Over the Shoulder" },
};

int Pc_ControlStyleCount(void)
{
    return (int)(sizeof(g_ControlStyles) / sizeof(g_ControlStyles[0]));
}

const char* Pc_ControlStyleId(int idx)
{
    if (idx < 0 || idx >= Pc_ControlStyleCount()) return "";
    return g_ControlStyles[idx].id;
}

const char* Pc_ControlStyleLabel(int idx)
{
    if (idx < 0 || idx >= Pc_ControlStyleCount()) return "";
    return g_ControlStyles[idx].label;
}

void Pc_ControlStyleSet(int style)
{
    int wasTps = g_DebugThirdPersonCam;

    if (style < 0 || style >= Pc_ControlStyleCount())
        style = ControlStyle_Classic;

    g_ControlStyle        = style;
    g_DebugThirdPersonCam = (style == ControlStyle_Tps || style == ControlStyle_Ots);

    /* Swap the active control scheme (classic vs alternate-camera binds) to match
     * the new camera mode; the input layer picks up the new mappings next frame. */
    {
        extern void Pc_ApplyActiveControlScheme(void);
        Pc_ApplyActiveControlScheme();
    }

    /* Entering TPS/OTS from classic: reseed the orbit camera (yaw/pitch/zoom/
     * shoulder) so it starts behind Harry rather than snapping to a stale pose.
     * Not flagged on tps<->ots so cycling between them doesn't jerk the view. */
    if (!wasTps && g_DebugThirdPersonCam)
        g_TpsCamNeedsReset = 1;

    /* Persist so the choice survives a restart and the launcher reflects it.
     * Mouse capture is managed per-frame in Pc_ControlStyleUpdate. */
    PcConfig_SaveKeyValue("control_style", Pc_ControlStyleId(style));
}

/* Publish "id:Label|id:Label|..." so the launcher dropdown lists this build's
 * styles. */
static void Pc_ControlStylePublish(void)
{
    char list[256];
    int  i, n, len = 0;

    n = Pc_ControlStyleCount();
    list[0] = '\0';
    for (i = 0; i < n && len < (int)sizeof(list) - 1; i++)
    {
        int w = snprintf(list + len, sizeof(list) - (size_t)len, "%s%s:%s",
                         (i > 0) ? "|" : "", g_ControlStyles[i].id, g_ControlStyles[i].label);
        if (w < 0) break;
        len += w;
    }
    if (len > (int)sizeof(list) - 1) len = (int)sizeof(list) - 1;
    list[len] = '\0';

    PcConfig_SaveKeyValue("control_styles", list);
}

void Pc_ControlStyleInit(void)
{
    int style = g_PcConfig.controlStyle;
    if (style < 0 || style >= Pc_ControlStyleCount())
        style = ControlStyle_Classic;

    g_ControlStyle        = style;
    g_DebugThirdPersonCam = (style == ControlStyle_Tps || style == ControlStyle_Ots);

    /* Apply the control scheme (classic vs alternate-camera) matching the saved
     * camera style now that g_DebugThirdPersonCam is known. */
    {
        extern void Pc_ApplyActiveControlScheme(void);
        Pc_ApplyActiveControlScheme();
    }

    Pc_ControlStylePublish();

    SH_DBG("[CTRLSTYLE] active=%s, %d styles published",
           Pc_ControlStyleId(style), Pc_ControlStyleCount());
}

/* SDL game-controller name -> PSX held-button flag, via the default pad map.
 * Covers the rebindable defaults; a fully generic raw-SDL-button read is part
 * of the input-layer pass. */
static int ChangeCam_PadFlag(const char* name)
{
    if (!name)                          return 0;
    if (!strcmp(name, "rightstick"))    return ControllerFlag_R3;
    if (!strcmp(name, "leftstick"))     return ControllerFlag_L3;
    if (!strcmp(name, "leftshoulder"))  return ControllerFlag_L1;
    if (!strcmp(name, "rightshoulder")) return ControllerFlag_R1;
    if (!strcmp(name, "back"))          return ControllerFlag_Select;
    if (!strcmp(name, "start"))         return ControllerFlag_Start;
    if (!strcmp(name, "a"))             return ControllerFlag_Cross;
    if (!strcmp(name, "b"))             return ControllerFlag_Circle;
    if (!strcmp(name, "x"))             return ControllerFlag_Square;
    if (!strcmp(name, "y"))             return ControllerFlag_Triangle;
    return 0;
}

/* "Mouse1".."Mouse5" -> SDL mouse button number (else 0 = it's a key). */
static int SwapShoulder_MouseButton(const char* name)
{
    if (!name) return 0;
    if ((name[0] == 'M' || name[0] == 'm') && (name[1] == 'o' || name[1] == 'O') &&
        (name[2] == 'u' || name[2] == 'U') && (name[3] == 's' || name[3] == 'S') &&
        (name[4] == 'e' || name[4] == 'E'))
    {
        switch (atoi(name + 5))
        {
            case 1: return SDL_BUTTON_LEFT;
            case 2: return SDL_BUTTON_RIGHT;
            case 3: return SDL_BUTTON_MIDDLE;
            case 4: return SDL_BUTTON_X1;
            case 5: return SDL_BUTTON_X2;
        }
    }
    return 0;
}

void Pc_ControlStyleUpdate(void)
{
    static SDL_Scancode scCam    = SDL_SCANCODE_UNKNOWN;
    static int          padFlag  = 0;
    static int          resolved = 0;
    static int          prevKey  = 0;
    static int          prevPad  = 0;

    extern int g_PcConsoleInputActive;

    const Uint8* keys;
    int          curKey, curPad;
    int          inGameplay;
    int          wantCapture;

    if (!resolved)
    {
        scCam    = SDL_GetScancodeFromName(g_PcConfig.keyChangeCam);
        padFlag  = ChangeCam_PadFlag(g_PcConfig.padChangeCam);
        resolved = 1;
    }

    inGameplay = (g_GameWork.gameState == GameState_InGame &&
                  g_SysWork.sysState   == SysState_Gameplay &&
                  !g_PcConsoleInputActive);

    keys   = SDL_GetKeyboardState(NULL);
    curKey = (keys && scCam != SDL_SCANCODE_UNKNOWN) ? keys[scCam] : 0;
    curPad = (padFlag && g_Controller0) ? ((g_Controller0->heldBtnFlags & padFlag) != 0) : 0;

    /* Edge-toggle the active style — gameplay only. */
    if (inGameplay && ((curKey && !prevKey) || (curPad && !prevPad)))
    {
        int next = (g_ControlStyle + 1) % Pc_ControlStyleCount();
        Pc_ControlStyleSet(next);
        SH_DBG_ECHO("[CTRLSTYLE] Change Camera -> %s", Pc_ControlStyleId(next));
    }
    prevKey = curKey;
    prevPad = curPad;

    /* Menus always use the classic control scheme. An alternate camera's binds
     * (mouse-look, remapped confirm/cancel) would otherwise apply to menu /
     * pause / inventory navigation. Swap to classic when leaving gameplay and
     * restore the camera-matched scheme on return. No-op in classic mode. */
    {
        static int s_forcedClassic = 0;
        int wantForceClassic = (!inGameplay && g_DebugThirdPersonCam) ? 1 : 0;
        if (wantForceClassic != s_forcedClassic)
        {
            extern void Pc_ApplyClassicControlScheme(void);
            extern void Pc_ApplyActiveControlScheme(void);
            if (wantForceClassic)
                Pc_ApplyClassicControlScheme();
            else
                Pc_ApplyActiveControlScheme();
            s_forcedClassic = wantForceClassic;
        }
    }

    /* Swap the OTS shoulder side (default middle mouse) — gameplay + OTS only. */
    {
        static SDL_Scancode scSwap   = SDL_SCANCODE_UNKNOWN;
        static int          mbSwap   = 0;
        static int          swapRes  = 0;
        static int          prevSwap = 0;
        int                 curSwap  = 0;

        if (!swapRes)
        {
            mbSwap = SwapShoulder_MouseButton(g_PcConfig.keySwapShoulder);
            if (mbSwap == 0) scSwap = SDL_GetScancodeFromName(g_PcConfig.keySwapShoulder);
            swapRes = 1;
        }

        if (mbSwap != 0)
            curSwap = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(mbSwap)) != 0;
        else if (keys && scSwap != SDL_SCANCODE_UNKNOWN)
            curSwap = keys[scSwap];

        if (inGameplay && g_ControlStyle == ControlStyle_Ots && curSwap && !prevSwap)
        {
            g_OtsSide = -g_OtsSide;
            SH_DBG_ECHO("[CTRLSTYLE] OTS shoulder: %s", g_OtsSide > 0 ? "right" : "left");
        }
        prevSwap = curSwap;
    }

    /* Capture the mouse only while actually playing in TPS/OTS — free cursor in
     * menus / pause / console. */
    wantCapture = (g_DebugThirdPersonCam && inGameplay);
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != (wantCapture != 0))
    {
        SDL_SetRelativeMouseMode(wantCapture ? SDL_TRUE : SDL_FALSE);
        /* Drain any accumulated relative motion on the capture transition so the
         * first TPS/OTS frame doesn't jerk the camera by the cursor's menu travel. */
        SDL_GetRelativeMouseState(NULL, NULL);
    }
}
