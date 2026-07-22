#include "game.h"

#include <memory.h>
#include <psyq/libetc.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/radio.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_debug_draw.h"
#include "bodyprog/text/text_draw.h"
#include "screens/options.h"
#include "screens/stream/stream.h"

#define LINE_CURSOR_TIMER_MAX 8
#ifdef SH_PC_PORT
#include <stdio.h>
#include <string.h>
#include "sh_log.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"
#include "map_registry.h"
#include "lang_text.h" /* PAL Language row (title-screen options) */
#include "lang_pack.h" /* PC-side pack language label (Polish) */
#define LAYER_24   PSX_OT_OFS(24)
#define LAYER_40   PSX_OT_OFS(40)
#define LAYER_36   PSX_OT_OFS(36)
#define LAYER_8148 PSX_OT_OFS(8148)
#else
#define LAYER_24   24
#define LAYER_40   40
#define LAYER_36   36
#define LAYER_8148 8148
#endif

s32  g_MainOptionsMenu_SelectedEntry      = 0;
s32  g_ExtraOptionsMenu_SelectedEntry     = 0;
s32  g_MainOptionsMenu_PrevSelectedEntry  = 0;
s32  g_ExtraOptionsMenu_PrevSelectedEntry = 0;
bool g_ScreenPosMenu_InvertBackgroundFade = false;
bool g_ControllerMenu_IsOnActionsPane     = false;

/** @brief Tracks movement time of the cursor highlight. */
static s32 g_Options_SelectionHighlightTimer;

/** @brief Number of options to show in the extra options screen. Shows extra unlockable settings if they are unlocked. */
static s32 g_ExtraOptionsMenu_EntryCount;

static s32 g_ExtraOptionsMenu_SelectedBloodColorEntry;

static s32 g_ExtraOptionsMenu_BulletMultMax;

// ========================================
// MAIN AND EXTRA OPTION SCREEN
// ========================================

#ifdef SH_PC_PORT
/* ============================================================================
 *  PC OPTIONS MENU  (repurposes the "Screen Position" main-menu entry)
 *
 *  Two pages mirroring the launcher. Each setting row cycles its config value
 *  with Left/Right, writes the config key and echoes to the console; realtime
 *  rows also push their live render global so the change is immediate, while
 *  restart-only rows print a "takes effect next launch" notice. Action rows
 *  (Next/Prev page, Back) activate with Confirm; Cancel always backs out to the
 *  main options menu (like every other submenu). Modeled on Extra Options. */

/* Live render globals mirrored for realtime settings (defined in PsyCross /
 * main_pc.c — the same ones main_pc.c seeds from g_PcConfig at boot). */
extern int g_cfg_psxDither;
extern int g_cfg_bilinearFiltering;
extern int g_PsxUsePgxp;
extern int g_cfg_postProcess;
extern int g_cfg_tonemap;
extern int g_PsyX_UsePerPixelFlashlight;
extern int g_PsyX_UseFlashlightShadows;

/* PsyCross live-apply helpers for the window-level settings (resolution, window
 * mode, vsync) so they take effect immediately instead of next launch. */
extern void PsyX_ApplyWindowState(int width, int height, int fullscreen);
extern void PsyX_ApplyVsync(int vsync);

/* Per-pixel flashlight beam live floats (PsyCross); mirrored by the sliders. */
extern float g_PsyX_FlashlightIntensity;
extern float g_PsyX_FlashlightSize;

/* FMV movie (SDL PCM) live volume, 0..1; mirrored by the FMV Movie slider.
 * The Sound menu's Voice slider drives g_PcXaVolume separately. */
extern float g_PcFmvVolume;

s32 g_PcOptionsMenu_SelectedEntry     = 0;
s32 g_PcOptionsMenu_PrevSelectedEntry = 0;
static s32 g_PcOptionsMenu_Page       = 0; /* 0 = Graphics, 1 = System, 2 = Controls, 3 = Camera */

/* Mouse hover moves the selection WITHOUT resetting g_Options_SelectionHighlightTimer:
 * a reset re-arms the LINE_CURSOR_TIMER_MAX gate, which then swallows the click that
 * follows the hover a fraction of a second later. But the underline's endpoints are
 * statics inside the highlight draws, recomputed ONLY when that timer is 0 — so the
 * bullet followed the mouse while the underline stayed where the keyboard left it.
 *
 * This asks the next highlight draw to recompute its endpoints once. Prev == Selected
 * at that point, so from == to and the underline snaps to the row instead of sliding —
 * which is the right behaviour for a pointer that is already there. */
s32 g_PcOptions_HighlightSnap = 0;

enum { PCK_INT, PCK_RES, PCK_FILTER, PCK_WINMODE, PCK_VSYNC, PCK_SLIDER, PCK_MAP, PCK_FLMODE, PCK_NEXT, PCK_PREV, PCK_BACK };

/* PC-options row origin. The heading sits at y=20 and the rows used to start at 56,
 * leaving a full empty row beneath it while the pages ran off the BOTTOM of the
 * 240-line screen. Starting at 40 reclaims that row for every page. Everything that
 * is positioned per-row moves with it: the two string draws, the mouse hit-test in
 * Options_PcOptionsMenu_Control, and — in the centred quad space the highlight and
 * bullets are authored in — HIGHLIGHT_OFFSET_Y and the bullet quad Y's. */
#define PCOPT_LINE_BASE_Y 40

typedef struct {
    const char*        name;     /* row label (underscores render as spaces) */
    int*               field;    /* &g_PcConfig.X (NULL for resolution + actions) */
    const char*        key;      /* config.cfg key (NULL for actions) */
    const int*         vals;     /* allowed values, in cycle order */
    int                nVals;
    const char* const* labels;   /* value labels parallel to vals (NULL = numeric) */
    int*               live;     /* single live global to mirror (NULL = none) */
    int                realtime; /* 1 = applies now, 0 = needs restart */
    int                kind;     /* PCK_* */
    float*             ffield;   /* PCK_SLIDER: config float this row adjusts */
    float*             flive;    /* PCK_SLIDER: live global to mirror (NULL = none) */
    float              fmin;     /* PCK_SLIDER value range + step */
    float              fmax;
    float              fstep;
} s_PcOpt;

static const int VAL_WIN[]   = { 0, 1, 2 };
static const int VAL_VSYNC[] = { 0, 1 };
static const int VAL_FILT[]  = { 0, 1, 2 };
static const int VAL_ONOFF[] = { 0, 1 };
static const int VAL_AA[]    = { 0, 2, 4, 8 };
static const int VAL_POST[]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
static const int VAL_TONE[]  = { 0, 1, 2, 3 };
static const int VAL_CON[]   = { 0, 1, 2, 3 };
static const int VAL_FPS[]   = { 0, 30, 60, 120, 240 };
static const int VAL_FLMODE[] = { 0, 1, 2, 3 };
static const int VAL_MMCNR[]  = { 0, 1, 2, 3 };
static const int VAL_MMSHP[]  = { 0, 1 };

static const char* const LBL_WIN[]   = { "Windowed", "Fullscreen", "Borderless" };
static const char* const LBL_VSYNC[] = { "Off", "On" };
static const char* const LBL_FILT[]  = { "Off", "Dither", "Bilinear" };
static const char* const LBL_ONOFF[] = { "Off", "On" };
static const char* const LBL_AA[]    = { "Off", "2x", "4x", "8x" };
static const char* const LBL_POST[]  = { "Off", "CRT", "Scanlines", "Vignette", "Color_Grade", "Film_Grain", "Sharpen", "PSX_Retro", "Cinematic" };
static const char* const LBL_TONE[]  = { "Off", "Reinhard", "ACES", "Filmic" };
static const char* const LBL_CON[]   = { "Off", "External", "In_Game", "Both" };
static const char* const LBL_FPS[]   = { "Off", "30", "60", "120", "240" };
/* Short enough to fit the value column at every language/width ("Modern_Shadows" clipped). */
static const char* const LBL_FLMODE[] = { "Classic", "C_+_Shadows", "Modern", "M_+_Shadows" };
static const char* const LBL_MMCNR[]  = { "Top_Left", "Top_Right", "Bottom_Left", "Bottom_Right" };
static const char* const LBL_MMSHP[]  = { "Square", "Circle" };

static const int RES_W[] = { 640, 1280, 1366, 1600, 1920, 2560, 3840 };
static const int RES_H[] = { 480,  720,  768,  900, 1080, 1440, 2160 };
#define PC_RES_COUNT ((int)(sizeof(RES_W) / sizeof(RES_W[0])))

static const s_PcOpt PCOPT_G[] = {
    { "Resolution",     NULL,                           NULL,                   NULL,      0, NULL,      NULL,                          0, PCK_RES    },
    { "Window_Mode",    &g_PcConfig.fullscreen,         "fullscreen",           VAL_WIN,   3, LBL_WIN,   NULL,                          1, PCK_WINMODE },
    { "VSync",          &g_PcConfig.vsync,              "vsync",                VAL_VSYNC, 2, LBL_VSYNC, NULL,                          1, PCK_VSYNC   },
    { "Texture_Filter", &g_PcConfig.psxDither,          "psx_dither",           VAL_FILT,  3, LBL_FILT,  NULL,                          1, PCK_FILTER },
    { "PGXP",           &g_PcConfig.usePgxp,            "use_pgxp",             VAL_ONOFF, 2, LBL_ONOFF, &g_PsxUsePgxp,                 1, PCK_INT    },
    { "Antialiasing",   &g_PcConfig.msaaSamples,        "msaa",                 VAL_AA,    4, LBL_AA,    NULL,                          0, PCK_INT    },
    { "Post_Process",   &g_PcConfig.postProcess,        "post_process",         VAL_POST,  9, LBL_POST,  &g_cfg_postProcess,            1, PCK_INT    },
    { "Tone_Mapping",   &g_PcConfig.tonemap,            "tonemap",              VAL_TONE,  4, LBL_TONE,  &g_cfg_tonemap,                1, PCK_INT    },
    /* New-Game start map. Moved here from the Camera page, which had run to 12
     * rows (the practical maximum) while this page had room to spare. */
    { "Map",            NULL,                           "map",                  NULL,      0, NULL,      NULL,                          1, PCK_MAP    },
    { "Next_Page",      NULL,                           NULL,                   NULL,      0, NULL,      NULL,                          0, PCK_NEXT   },
    { "Back",           NULL,                           NULL,                   NULL,      0, NULL,      NULL,                          0, PCK_BACK   },
};

static const s_PcOpt PCOPT_S[] = {
    /* One row for the whole flashlight look: Classic (PSX per-vertex),
     * Classic_Shadows (per-pixel, PSX-calibrated + shadows), Modern (stylized
     * per-pixel spotlight), Modern_Shadows. PCK_FLMODE routes the change
     * through Pc_FlashlightModeApply, which derives the per-pixel/style/shadow
     * globals and the per-style beam defaults. */
    { "Flashlight",       &g_PcConfig.flashlightMode,     "flashlight_mode",      VAL_FLMODE, 4, LBL_FLMODE, NULL,                        1, PCK_FLMODE },
    { "Beam_Intensity",   NULL, "flashlight_intensity", NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.flashlightIntensity, &g_PsyX_FlashlightIntensity, 0.0f, 3.0f, 0.1f },
    { "Beam_Size",        NULL, "flashlight_size",      NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.flashlightSize,      &g_PsyX_FlashlightSize,      0.0f, 3.0f, 0.1f },
    { "Disable_Culling",  &g_PcConfig.disableCulling, "disable_culling",  VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT  },
    { "Preload_Chunks",   &g_PcConfig.preloadChunks,  "preload_chunks",   VAL_ONOFF, 2, LBL_ONOFF, NULL, 0, PCK_INT  },
    { "FPS_Limit",        &g_PcConfig.fpsCap,         "fps_cap",          VAL_FPS,   5, LBL_FPS,   NULL, 1, PCK_INT  },
    { "FMV_Movie_Vol",    NULL, "fmv_volume",           NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.fmvVolume,           &g_PcFmvVolume,             0.0f, 1.0f, 0.05f },
    /* Moved here from the Camera page for the same reason as Map above. */
    { "Crosshair",        &g_PcConfig.crosshair,      "crosshair",        VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT  },
    { "Prev_Page",        NULL,                       NULL,               NULL,      0, NULL,      NULL, 0, PCK_PREV },
    { "Next_Page",        NULL,                       NULL,               NULL,      0, NULL,      NULL, 0, PCK_NEXT },
    { "Back",             NULL,                       NULL,               NULL,      0, NULL,      NULL, 0, PCK_BACK },
};

/* Page 3 (Controls): the 2D screen-relative control toggles + look sensitivities
 * and the invert toggles. (The New-Game start Map row now lives on page 4.) */
static const s_PcOpt PCOPT_C[] = {
    { "2D_Controls",       &g_PcConfig.control2d,        "control_2d",             VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "2D_Snap",           &g_PcConfig.control2dSnap,    "control_2d_snap",        VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Mouse_Sensitivity", NULL, "mouse_sensitivity",      NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.mouseSensitivity,      NULL, 0.1f, 4.0f, 0.1f },
    { "Pad_Sensitivity",   NULL, "controller_sensitivity", NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.controllerSensitivity, NULL, 0.1f, 4.0f, 0.1f },
    { "First_Person_FOV",  NULL, "fps_fov",                NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.fpsFov,                NULL, 55.0f, 110.0f, 1.0f },
    { "Third_Person_FOV",  NULL, "tps_fov",                NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.tpsFov,                NULL, 55.0f, 110.0f, 1.0f },
    { "Invert_Mouse_Y",    &g_PcConfig.invertMouseY,      "invert_mouse_y",         VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Invert_Pad_Y",      &g_PcConfig.invertControllerY, "invert_controller_y",    VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Prev_Page",         NULL,                          NULL,                     NULL,      0, NULL,      NULL, 0, PCK_PREV },
    { "Next_Page",         NULL,                          NULL,                     NULL,      0, NULL,      NULL, 0, PCK_NEXT },
    { "Back",              NULL,                          NULL,                     NULL,      0, NULL,      NULL, 0, PCK_BACK },
};

/* Page 4 (Camera): the aiming + alternate-camera options. A page fits ~12 lines
 * (PCOPT_LINE_BASE_Y 40 + 16/row on a 240-line screen) and this one had reached
 * that limit, so Crosshair moved to the System page and the New-Game start Map
 * row to the Graphics page — both of which had spare rows. Third_Person_FOV
 * lives with First_Person_FOV on the Controls page. */
static const s_PcOpt PCOPT_T[] = {
    { "Minimap",           &g_PcConfig.minimap,            "minimap",               VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Minimap_Shape",     &g_PcConfig.minimapShape,       "minimap_shape",         VAL_MMSHP, 2, LBL_MMSHP, NULL, 1, PCK_INT },
    { "Minimap_Corner",    &g_PcConfig.minimapCorner,      "minimap_corner",        VAL_MMCNR, 4, LBL_MMCNR, NULL, 1, PCK_INT },
    { "Minimap_Opacity",   NULL, "minimap_opacity",        NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.minimapOpacity, NULL, 0.0f, 100.0f, 5.0f },
    { "Aim_Assist",        &g_PcConfig.aimAssist,          "aim_assist",            VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    /* 0..200 to match the config loader, the TPSAIMZOOM console command and the
     * pc_config.h contract — 100 is the original full zoom, 200 a deeper 2x.
     * The slider alone was capped at 100, so the top half was unreachable. */
    { "Aim_Zoom",          NULL, "tps_aim_zoom_amount",    NULL, 0, NULL, NULL, 1, PCK_SLIDER, &g_PcConfig.tpsAimZoom,  NULL, 0.0f, 200.0f, 5.0f },
    { "OTS_Aim_In_TPS",    &g_PcConfig.tpsOtsAim,          "tps_ots_aim",           VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Camera_Collision",  &g_PcConfig.tpsCameraCollision, "tps_camera_collision",  VAL_ONOFF, 2, LBL_ONOFF, NULL, 1, PCK_INT },
    { "Prev_Page",         NULL,                           NULL,                    NULL,      0, NULL,      NULL, 0, PCK_PREV },
    { "Back",              NULL,                           NULL,                    NULL,      0, NULL,      NULL, 0, PCK_BACK },
};

static void Options_PcOptionsMenu_EntryStringsDraw(void);
static void Options_PcOptionsMenu_ConfigDraw(void);
static void Options_PcOptionsMenu_SelectionHighlightDraw(void);

/* ---- Mouse support: hover selects, click acts, wheel adjusts values ----
 * Injections write the same controller bits the stock input code reads, so
 * every screen keeps its own step/clamp/SFX/state logic. */

static void PcMouse_InjectEnter(void)
{
    g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.enter;
}

static void PcMouse_InjectCancel(void)
{
    g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.cancel;
}

/* A value-cycle step: toggle rows read clickedBtnFlags, sliders/volumes read
 * pulsedBtnFlags — set both, like a fresh physical press does. */
static void PcMouse_InjectDir(int dir)
{
    u32 flag = (dir > 0) ? ControllerFlag_LStickRight : ControllerFlag_LStickLeft;

    g_Controller0->clickedBtnFlags |= flag;
    g_Controller0->pulsedBtnFlags  |= flag;
}

static const s_PcOpt* PcOpt_Page(int* count)
{
    if (g_PcOptionsMenu_Page == 0) { *count = (int)(sizeof(PCOPT_G) / sizeof(PCOPT_G[0])); return PCOPT_G; }
    if (g_PcOptionsMenu_Page == 1) { *count = (int)(sizeof(PCOPT_S) / sizeof(PCOPT_S[0])); return PCOPT_S; }
    if (g_PcOptionsMenu_Page == 2) { *count = (int)(sizeof(PCOPT_C) / sizeof(PCOPT_C[0])); return PCOPT_C; }
    *count = (int)(sizeof(PCOPT_T) / sizeof(PCOPT_T[0]));
    return PCOPT_T;
}

static int PcOpt_ValIndex(const s_PcOpt* e)
{
    /* Prefer the live render global when the row mirrors one: it's the ground
     * truth, so the menu reflects toggles made via hotkeys/console (F1 PGXP,
     * F2 post-process, etc.) rather than a stale g_PcConfig copy. */
    int i, v = (e->live) ? *e->live : *e->field;
    for (i = 0; i < e->nVals; i++)
        if (e->vals[i] == v) return i;
    return 0;
}

static const char* PcOpt_ValueLabel(const s_PcOpt* e, char* buf, int bufsz)
{
    if (e->kind == PCK_RES) {
        snprintf(buf, bufsz, "%dx%d", g_PcConfig.windowWidth, g_PcConfig.windowHeight);
        return buf;
    }
    if (e->kind == PCK_SLIDER) {
        snprintf(buf, bufsz, "%.2f", e->ffield ? *e->ffield : 0.0f);
        return buf;
    }
    if (e->kind == PCK_MAP) {
        return g_PcConfig.mapName[0] ? g_PcConfig.mapName : "map0_s00";
    }
    if (e->field == NULL)
        return "";
    {
        int idx = PcOpt_ValIndex(e);
        if (e->labels) return e->labels[idx];
        snprintf(buf, bufsz, "%d", e->vals[idx]);
        return buf;
    }
}

/* Click-and-drag on a slider row. PcOpt_Adjust writes config.cfg on every call,
 * so driving it once per drag step would rewrite the file dozens of times a
 * second; instead move the value live here and let the caller save once when the
 * button is released. */
static void PcOpt_SliderDragApply(const s_PcOpt* e, int steps)
{
    float v;

    if (e->kind != PCK_SLIDER || !e->ffield || steps == 0)
        return;

    v = *e->ffield + (float)steps * e->fstep;
    if (v < e->fmin) v = e->fmin;
    if (v > e->fmax) v = e->fmax;

    *e->ffield = v;
    if (e->flive) *e->flive = v;
}

static void PcOpt_SliderDragCommit(const s_PcOpt* e)
{
    char fb[16];

    if (e->kind != PCK_SLIDER || !e->ffield || !e->key)
        return;

    snprintf(fb, sizeof(fb), "%.3f", *e->ffield);
    PcConfig_SaveKeyValue(e->key, fb);
    SH_DBG_ECHO("%s: %.2f", e->name, *e->ffield);
}

static void PcOpt_Adjust(const s_PcOpt* e, int dir)
{
    char buf[24];
    char vbuf[24];

    if (e->kind == PCK_RES) {
        int i, idx = 0;
        for (i = 0; i < PC_RES_COUNT; i++)
            if (RES_W[i] == g_PcConfig.windowWidth && RES_H[i] == g_PcConfig.windowHeight) { idx = i; break; }
        idx = (idx + dir + PC_RES_COUNT) % PC_RES_COUNT;
        g_PcConfig.windowWidth  = RES_W[idx];
        g_PcConfig.windowHeight = RES_H[idx];
        snprintf(buf, sizeof(buf), "%d", g_PcConfig.windowWidth);  PcConfig_SaveKeyValue("width", buf);
        snprintf(buf, sizeof(buf), "%d", g_PcConfig.windowHeight); PcConfig_SaveKeyValue("height", buf);
        PsyX_ApplyWindowState(g_PcConfig.windowWidth, g_PcConfig.windowHeight, g_PcConfig.fullscreen);
        SH_DBG_ECHO("Resolution: %dx%d", g_PcConfig.windowWidth, g_PcConfig.windowHeight);
        return;
    }
    if (e->kind == PCK_SLIDER) {
        float v = (e->ffield ? *e->ffield : 0.0f) + (float)dir * e->fstep;
        if (v < e->fmin) v = e->fmin;
        if (v > e->fmax) v = e->fmax;
        if (e->ffield) *e->ffield = v;
        if (e->flive)  *e->flive  = v;
        if (e->key) { char fb[16]; snprintf(fb, sizeof(fb), "%.3f", v); PcConfig_SaveKeyValue(e->key, fb); }
        SH_DBG_ECHO("%s: %.2f", e->name, v);
        return;
    }
    if (e->kind == PCK_MAP) {
        /* Cycle the New-Game start map (same effect as the console MAP command
         * and the 4/5 debug keys): set g_PcConfig.mapName; the map loads on the
         * next New Game. MAP_NAMES is dense 0..Count-1 in story order. */
        int cnt = MapRegistry_Count();
        int idx = MapRegistry_FindByName(g_PcConfig.mapName);
        const char* nm;
        if (idx < 0) idx = 0;
        idx = (idx + dir + cnt) % cnt;
        nm  = MapRegistry_GetName((e_MapIdx)idx);
        strncpy(g_PcConfig.mapName, nm, sizeof(g_PcConfig.mapName) - 1);
        g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
        PcConfig_SaveMapName(nm);
        {
            const char* desc = MapRegistry_GetDescription((e_MapIdx)idx);
            SH_DBG_ECHO("Map: %s (%s)", nm, (desc && desc[0]) ? desc : nm);
        }
        return;
    }
    if (e->field == NULL)
        return;

    {
        int idx = (PcOpt_ValIndex(e) + dir + e->nVals) % e->nVals;
        *e->field = e->vals[idx];
        snprintf(buf, sizeof(buf), "%d", *e->field);
        if (e->key) PcConfig_SaveKeyValue(e->key, buf);

        if (e->kind == PCK_FILTER) {
            switch (*e->field) {
            case 1:  g_cfg_psxDither = 1; g_cfg_bilinearFiltering = 0; break;
            case 2:  g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 1; break;
            default: g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 0; break;
            }
        } else if (e->kind == PCK_WINMODE) {
            PsyX_ApplyWindowState(g_PcConfig.windowWidth, g_PcConfig.windowHeight, g_PcConfig.fullscreen);
        } else if (e->kind == PCK_VSYNC) {
            PsyX_ApplyVsync(g_PcConfig.vsync);
        } else if (e->kind == PCK_FLMODE) {
            Pc_FlashlightModeApply(*e->field, 1);
        } else if (e->live) {
            *e->live = *e->field;
        }

        SH_DBG_ECHO("%s: %s", e->name, PcOpt_ValueLabel(e, vbuf, sizeof(vbuf)));
        if (!e->realtime)
            SH_DBG_ECHO("This setting will take effect the next time the game is started.");
    }
}

void Options_PcOptionsMenu_Control(void)
{
    int            count;
    const s_PcOpt* tbl = PcOpt_Page(&count);

    Options_PcOptionsMenu_EntryStringsDraw();
    Options_PcOptionsMenu_ConfigDraw();
    Options_PcOptionsMenu_SelectionHighlightDraw();
    Options_Menu_VignetteDraw();
    Screen_BackgroundImgDraw(&g_ItemInspectionImg);
    Pc_MouseCursor_Draw();

    if (g_GameWork.gameStateSteps[0] != OptionsMenuState_PcOptions)
        return;

    if ((LINE_CURSOR_TIMER_MAX - 1) < g_Options_SelectionHighlightTimer)
        g_Options_SelectionHighlightTimer = LINE_CURSOR_TIMER_MAX;
    else
        g_Options_SelectionHighlightTimer++;

    if (g_Options_SelectionHighlightTimer == LINE_CURSOR_TIMER_MAX)
    {
        const s_PcOpt* sel;

        if (g_PcOptionsMenu_SelectedEntry >= count)
            g_PcOptionsMenu_SelectedEntry = 0;
        g_PcOptionsMenu_PrevSelectedEntry = g_PcOptionsMenu_SelectedEntry;

        /* Mouse: hover selects (snapping, so the click that follows isn't
         * swallowed by the highlight timer), click activates action rows /
         * cycles value rows, wheel steps the hovered value, right-click backs
         * out (via the cancel handler below). */
        {
            int mx, my;

            /* Slider drag state. Holding the button on a slider row grabs it:
             * the row is pinned (so the vertical wobble of a drag can't hover
             * onto a neighbour) and pointer travel is accumulated into value
             * steps. Right or up raises, left or down lowers, so either axis
             * feels natural. Committed once on release — see
             * PcOpt_SliderDragApply. */
            static int s_dragRow  = -1;
            static int s_dragX    = 0;
            static int s_dragY    = 0;
            static int s_dragMoved = 0;
            const int  PCOPT_DRAG_PX_PER_STEP = 4;

            if (Pc_MouseCursor_UiPos(&mx, &my))
            {
                int row = -1, i;

                for (i = 0; i < count; i++) {
                    int top = PCOPT_LINE_BASE_Y + (i * 16) - 3;
                    if (my >= top && my < top + 16) { row = i; break; }
                }

                /* Start a drag when the button goes down on a slider row. */
                if (s_dragRow < 0 && Pc_MouseCursor_LeftClicked() &&
                    row >= 0 && row == g_PcOptionsMenu_SelectedEntry &&
                    tbl[row].kind == PCK_SLIDER)
                {
                    s_dragRow   = row;
                    s_dragX     = mx;
                    s_dragY     = my;
                    s_dragMoved = 0;
                }

                if (s_dragRow >= 0 && Pc_MouseCursor_LeftHeld() && s_dragRow < count)
                {
                    int travel = (mx - s_dragX) - (my - s_dragY);
                    int steps  = travel / PCOPT_DRAG_PX_PER_STEP;

                    if (steps != 0) {
                        PcOpt_SliderDragApply(&tbl[s_dragRow], steps);
                        /* Keep the remainder so slow drags still accumulate. */
                        s_dragX += steps * PCOPT_DRAG_PX_PER_STEP;
                        s_dragY -= steps * PCOPT_DRAG_PX_PER_STEP;
                        s_dragMoved = 1;
                    }
                }
                else if (s_dragRow >= 0)
                {
                    /* Released. A drag saves once; a click that never moved
                     * keeps the old behaviour of nudging the value one step. */
                    if (s_dragRow < count) {
                        if (s_dragMoved) PcOpt_SliderDragCommit(&tbl[s_dragRow]);
                        else             PcOpt_Adjust(&tbl[s_dragRow], 1);
                    }
                    s_dragRow = -1;
                }

                if (s_dragRow < 0 &&
                    row >= 0 && Pc_MouseCursor_Moved() && row != g_PcOptionsMenu_SelectedEntry) {
                    g_PcOptionsMenu_SelectedEntry     = row;
                    g_PcOptionsMenu_PrevSelectedEntry = row;
                    g_PcOptions_HighlightSnap         = 1;
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                }
                if (s_dragRow < 0 && row >= 0 && row == g_PcOptionsMenu_SelectedEntry) {
                    int wheel = Pc_MouseCursor_WheelStep();

                    if (tbl[row].kind >= PCK_NEXT) {
                        if (Pc_MouseCursor_LeftClicked())
                            PcMouse_InjectEnter();
                    } else if (Pc_MouseCursor_LeftClicked() && tbl[row].kind != PCK_SLIDER) {
                        PcMouse_InjectDir(1);
                    } else if (wheel != 0) {
                        PcMouse_InjectDir(wheel);
                    }
                }
                if (Pc_MouseCursor_RightClicked())
                    PcMouse_InjectCancel();
            }
            else if (s_dragRow >= 0)
            {
                /* Pointer left the viewport mid-drag — commit and let go. */
                if (s_dragMoved && s_dragRow < count)
                    PcOpt_SliderDragCommit(&tbl[s_dragRow]);
                s_dragRow = -1;
            }
        }

        if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickUp) {
            Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            g_PcOptionsMenu_SelectedEntry = ((g_PcOptionsMenu_SelectedEntry - 1) + count) % count;
            g_Options_SelectionHighlightTimer = 0;
        }
        if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickDown) {
            Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            g_PcOptionsMenu_SelectedEntry = (g_PcOptionsMenu_SelectedEntry + 1) % count;
            g_Options_SelectionHighlightTimer = 0;
        }

        /* L1 / R1 (LB / RB) jump to the previous / next PC-options page (wraps).
         * clickedBtnFlags is the per-frame rising edge; keyboard L1=A / R1=D map to
         * the same PSX bits, so this works on keyboard too. */
        if (g_Controller0->clickedBtnFlags & ControllerFlag_R1) {
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            g_PcOptionsMenu_Page = (g_PcOptionsMenu_Page + 1) & 3; /* 4 pages */
            g_PcOptionsMenu_SelectedEntry     = 0;
            g_Options_SelectionHighlightTimer = 0;
        }
        if (g_Controller0->clickedBtnFlags & ControllerFlag_L1) {
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            g_PcOptionsMenu_Page = (g_PcOptionsMenu_Page + 3) & 3; /* -1 mod 4 */
            g_PcOptionsMenu_SelectedEntry     = 0;
            g_Options_SelectionHighlightTimer = 0;
        }

        sel = &tbl[g_PcOptionsMenu_SelectedEntry];

        /* Every value row (INT/RES/FILTER/WINMODE/VSYNC/SLIDER) adjusts on
         * left/right; only the action rows (NEXT/PREV/BACK) are excluded. */
        if (sel->kind < PCK_NEXT) {
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight) {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                PcOpt_Adjust(sel, +1);
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft) {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                PcOpt_Adjust(sel, -1);
            }
        }

        if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter) {
            if (sel->kind == PCK_NEXT) {
                Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
                g_PcOptionsMenu_Page++; /* Graphics -> System -> Controls -> Camera */
                g_PcOptionsMenu_SelectedEntry = 0;
                g_Options_SelectionHighlightTimer = 0;
            } else if (sel->kind == PCK_PREV) {
                Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
                g_PcOptionsMenu_Page--; /* Camera -> Controls -> System -> Graphics */
                g_PcOptionsMenu_SelectedEntry = 0;
                g_Options_SelectionHighlightTimer = 0;
            } else if (sel->kind == PCK_BACK) {
                Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
                ScreenFade_Start(true, false, false);
                g_GameWork.gameStateSteps[0] = OptionsMenuState_LeavePcOptions;
                g_SysWork.counters_1C[1]     = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
                return;
            }
        }
    }

    if ((g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel) &&
        g_GameWork.gameStateSteps[0] != OptionsMenuState_LeavePcOptions)
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        ScreenFade_Start(true, false, false);
        g_GameWork.gameStateSteps[0] = OptionsMenuState_LeavePcOptions;
        g_SysWork.counters_1C[1]     = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
    }
}

static void Options_PcOptionsMenu_EntryStringsDraw(void)
{
    #define LINE_BASE_X   64
    #define LINE_BASE_Y   PCOPT_LINE_BASE_Y
    #define LINE_OFFSET_Y 16

    int            count, i;
    const s_PcOpt* tbl = PcOpt_Page(&count);
    DVECTOR        strPos  = { 100, 20 };
    const char*    HEADING = "PC_Options";

    Gfx_StringSetColor(StringColorId_White);
    Gfx_StringSetPosition(strPos.vx, strPos.vy);
    Gfx_Strings2dLayerIdxSet(8);
    Gfx_StringDraw(HEADING, DEFAULT_MAP_MESSAGE_LENGTH);

    for (i = 0; i < count; i++) {
        Gfx_StringSetPosition(LINE_BASE_X, LINE_BASE_Y + (i * LINE_OFFSET_Y));
        Gfx_Strings2dLayerIdxSet(8);
        Gfx_StringDraw(tbl[i].name, DEFAULT_MAP_MESSAGE_LENGTH);
    }
    Gfx_StringsReset2dLayerIdx();

    #undef LINE_BASE_X
    #undef LINE_BASE_Y
    #undef LINE_OFFSET_Y
}

static void Options_PcOptionsMenu_ConfigDraw(void)
{
    #define LINE_BASE_X   64
    #define LINE_BASE_Y   PCOPT_LINE_BASE_Y
    #define LINE_OFFSET_Y 16

    int            count, i;
    const s_PcOpt* tbl = PcOpt_Page(&count);
    char           buf[24];
    /* System's and Controls'/Camera's labels run long ("Disable Culling",
     * "Mouse Sensitivity", "Camera Collision"), so push their value column right
     * to clear them — but no further: System's widest value ("C_+_Shadows",
     * 109px) must still end before the 320px clip (its labels end by ~196, so
     * 204 clears both ways). Controls and Camera hold only short values (numbers
     * and On/Off), so they can afford 240. */
    int            valX = (g_PcOptionsMenu_Page == 0) ? 196 : (g_PcOptionsMenu_Page == 1) ? 204 : 240;

    Gfx_StringSetColor(StringColorId_White);
    for (i = 0; i < count; i++) {
        const char* v = PcOpt_ValueLabel(&tbl[i], buf, sizeof(buf));
        if (v && v[0]) {
            Gfx_StringSetPosition(valX, LINE_BASE_Y + (i * LINE_OFFSET_Y));
            Gfx_Strings2dLayerIdxSet(8);
            Gfx_StringDraw(v, DEFAULT_MAP_MESSAGE_LENGTH);
        }
    }

    Gfx_StringsReset2dLayerIdx();

    #undef LINE_BASE_X
    #undef LINE_BASE_Y
    #undef LINE_OFFSET_Y
}

static void Options_PcOptionsMenu_SelectionHighlightDraw(void)
{
    #define LINE_OFFSET_Y      16
    #define HIGHLIGHT_OFFSET_X -121
    /* 58 + 16: the rows moved up one line (PCOPT_LINE_BASE_Y 56 -> 40), and this
     * space measures DOWN from the row, so it grows by the same 16. */
    #define HIGHLIGHT_OFFSET_Y 74
    #define HILITE_WIDTH       196

    int            count, i, j;
    s16            interpAlpha;
    s_Line2d       highlightLine;
    s_Quad2d       bulletQuads[2];
    DVECTOR*       quadVerts;
    static DVECTOR selectionHighlightFrom;
    static DVECTOR selectionHighlightTo;

    /* Bullet quads, likewise lifted 16 to follow PCOPT_LINE_BASE_Y. */
    const DVECTOR BULLET_QUAD_VERTS_FRONT[] = { { -120, -71 }, { -120, -59 }, { -108, -71 }, { -108, -59 } };
    const DVECTOR BULLET_QUAD_VERTS_BACK[]  = { { -121, -72 }, { -121, -58 }, { -107, -72 }, { -107, -58 } };

    const s_PcOpt* tbl = PcOpt_Page(&count);
    (void)tbl;

    if (g_Options_SelectionHighlightTimer == 0 || g_PcOptions_HighlightSnap) {
        g_PcOptions_HighlightSnap = 0;
        selectionHighlightFrom.vx = HILITE_WIDTH + (65536 + HIGHLIGHT_OFFSET_X);
        selectionHighlightFrom.vy = ((u16)g_PcOptionsMenu_PrevSelectedEntry * LINE_OFFSET_Y) - HIGHLIGHT_OFFSET_Y;
        selectionHighlightTo.vx   = HILITE_WIDTH + (65536 + HIGHLIGHT_OFFSET_X);
        selectionHighlightTo.vy   = ((u16)g_PcOptionsMenu_SelectedEntry * LINE_OFFSET_Y) - HIGHLIGHT_OFFSET_Y;
    }

    interpAlpha = Math_Sin(g_Options_SelectionHighlightTimer << 7);

    highlightLine.vertex0.vx = HIGHLIGHT_OFFSET_X;
    highlightLine.vertex1.vx = selectionHighlightFrom.vx +
                               FP_FROM((selectionHighlightTo.vx - selectionHighlightFrom.vx) * interpAlpha, Q12_SHIFT);
    highlightLine.vertex1.vy = selectionHighlightFrom.vy +
                               FP_FROM((selectionHighlightTo.vy - selectionHighlightFrom.vy) * interpAlpha, Q12_SHIFT) +
                               LINE_OFFSET_Y;
    highlightLine.vertex0.vy = highlightLine.vertex1.vy;
    Options_Selection_HighlightDraw(&highlightLine, true, false);

    for (i = 0; i < count; i++) {
        int line = i;
        quadVerts = (DVECTOR*)&bulletQuads;
        for (j = 0; j < RECT_VERT_COUNT; j++) {
            quadVerts[j].vx                   = BULLET_QUAD_VERTS_FRONT[j].vx;
            quadVerts[j].vy                   = BULLET_QUAD_VERTS_FRONT[j].vy + (line * LINE_OFFSET_Y);
            quadVerts[j + sizeof(DVECTOR)].vx = BULLET_QUAD_VERTS_BACK[j].vx;
            quadVerts[j + sizeof(DVECTOR)].vy = BULLET_QUAD_VERTS_BACK[j].vy + (line * LINE_OFFSET_Y);
        }
        if (i == g_PcOptionsMenu_SelectedEntry) {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, false);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  false);
        } else {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, true);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  true);
        }
    }

    #undef LINE_OFFSET_Y
    #undef HIGHLIGHT_OFFSET_X
    #undef HIGHLIGHT_OFFSET_Y
    #undef HILITE_WIDTH
}
#endif /* SH_PC_PORT */

void GameState_Options_Update(void) // 0x801E2D44
{
    s32 unlockedOptFlags;
    s32 i;

    if (g_GameWork.gameStatePrev == GameState_InGame)
    {
        func_800363D0();
    }

    if (g_GameWork.gameStatePrev != GameState_MainMenu)
    {
        Game_TimerUpdate();
    }

    // Handle options menu state.
    switch (g_GameWork.gameStateSteps[0])
    {
        case OptionsMenuState_EnterMainOptions:
            DrawSync(SyncMode_Wait);

            if (g_GameWork.gameStatePrev != GameState_InGame)
            {
                VSync(SyncMode_Wait8);
            }

            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;

            ScreenFade_Start(false, true, false);
            g_IntervalVBlanks   = 1;

            if (g_GameWork.gameStatePrev == GameState_InGame)
            {
                Game_RadioSoundStop();
            }

            g_MainOptionsMenu_SelectedEntry      = MainOptionsMenuEntry_Exit;
            g_MainOptionsMenu_PrevSelectedEntry  = 0;
            g_ExtraOptionsMenu_SelectedEntry     = 0;
            g_ExtraOptionsMenu_PrevSelectedEntry = 0;
            g_Options_SelectionHighlightTimer    = 0;
            g_ExtraOptionsMenu_BulletMultMax     = 1;
            unlockedOptFlags                     = g_GameWork.config.extraOptionsEnabled;

            // Set available bullet multiplier.
            for (i = 0; i < 5; i++)
            {
                if (unlockedOptFlags & (1 << i))
                {
                    g_ExtraOptionsMenu_BulletMultMax++;
                }
            }

            // Set selected blood color.
            switch (g_GameWork.config.extraBloodColor)
            {
                case BloodColor_Normal:
                    g_ExtraOptionsMenu_SelectedBloodColorEntry = BloodColorMenuEntry_Normal;
                    break;

                case BloodColor_Green:
                    g_ExtraOptionsMenu_SelectedBloodColorEntry = BloodColorMenuEntry_Green;
                    break;

                case BloodColor_Violet:
                    g_ExtraOptionsMenu_SelectedBloodColorEntry = BloodColorMenuEntry_Violet;
                    break;

                case BloodColor_Black:
                    g_ExtraOptionsMenu_SelectedBloodColorEntry = BloodColorMenuEntry_Black;
                    break;
            }

            g_ExtraOptionsMenu_EntryCount = (g_GameWork.config.extraOptionsEnabled) ? 8 : 6;
            g_GameWork.gameStateSteps[0]  = OptionsMenuState_MainOptions;
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateSteps[1]  = 0;
            g_GameWork.gameStateSteps[2]  = 0;
            break;

        case OptionsMenuState_LeaveScreenPos:
        case OptionsMenuState_LeaveBrightness:
        case OptionsMenuState_LeaveController:
            g_GameWork.gameStateSteps[0] = OptionsMenuState_MainOptions;
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateSteps[1] = 0;
            g_GameWork.gameStateSteps[2] = 0;
            break;

        case OptionsMenuState_EnterScreenPos:
            if (ScreenFade_IsFinished())
            {
                g_GameWork.gameStateSteps[0] = OptionsMenuState_ScreenPos;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case OptionsMenuState_ScreenPos:
            Options_ScreenPosMenu_Control();
            break;

        case OptionsMenuState_EnterBrightness:
            if (ScreenFade_IsFinished())
            {
                Fs_QueueWaitForEmpty();

                g_GameWork.gameStateSteps[0] = OptionsMenuState_Brightness;
                g_GameWork.gameStateSteps[0] = OptionsMenuState_Brightness;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case OptionsMenuState_Brightness:
            Options_BrightnessMenu_Control();
            break;

        case OptionsMenuState_EnterController:
            // Switch to controller binding menu.
            if (ScreenFade_IsFinished())
            {
                g_GameWork.gameStateSteps[0] = OptionsMenuState_Controller;
                g_GameWork.gameStateSteps[0] = OptionsMenuState_Controller;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case OptionsMenuState_Controller:
            Options_ControllerMenu_Control();
            break;

        case OptionsMenuState_Leave:
            ScreenFade_Start(true, false, false);
            g_GameWork.gameStateSteps[0] = OptionsMenuState_LeaveMainOptions;
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateSteps[1] = 0;
            g_GameWork.gameStateSteps[2] = 0;
            break;

        case OptionsMenuState_LeaveMainOptions:
            if (ScreenFade_IsFinished())
            {
                // TODO: Likely `Game_StateSetPrevious` inline, but `gameState`/`gameStatePrev` loads inside are switched?

                e_GameState prevGameState = g_GameWork.gameStatePrev;
                e_GameState gameState     = g_GameWork.gameState;

                g_SysWork.counters_1C[0]              = 0;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;

                SysWork_StateSetNext(SysState_Gameplay);

                g_GameWork.gameStateSteps[0] = gameState;
                g_GameWork.gameState         = prevGameState;
                g_GameWork.gameStatePrev     = gameState;
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterMainOptions;
            }

            break;

        case OptionsMenuState_EnterExtraOptions:
            if (ScreenFade_IsFinished())
            {
                g_GameWork.gameStateSteps[0] = OptionsMenuState_ExtraOptions;
                g_SysWork.counters_1C[1]                = 0;
                ScreenFade_Start(false, true, false);
                g_GameWork.gameStateSteps[1]      = 0;
                g_GameWork.gameStateSteps[2]      = 0;
                g_Options_SelectionHighlightTimer = 0;
            }
            break;

        case OptionsMenuState_LeaveExtraOptions:
            if (ScreenFade_IsFinished())
            {
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterMainOptions;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
                ScreenFade_Start(false, true, false);
            }
            break;

#ifdef SH_PC_PORT
        case OptionsMenuState_EnterPcOptions:
            if (ScreenFade_IsFinished())
            {
                g_PcOptionsMenu_Page              = 0;
                g_PcOptionsMenu_SelectedEntry     = 0;
                g_PcOptionsMenu_PrevSelectedEntry = 0;
                g_Options_SelectionHighlightTimer = 0;
                ScreenFade_Start(false, true, false);
                g_GameWork.gameStateSteps[0] = OptionsMenuState_PcOptions;
                g_SysWork.counters_1C[1]     = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case OptionsMenuState_LeavePcOptions:
            if (ScreenFade_IsFinished())
            {
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterMainOptions;
                g_SysWork.counters_1C[1]     = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
                ScreenFade_Start(false, true, false);
            }
            break;
#endif
    }

    // Handle menu state.
    switch (g_GameWork.gameStateSteps[0])
    {
        case OptionsMenuState_MainOptions:
        case OptionsMenuState_Leave:
        case OptionsMenuState_LeaveMainOptions:
        case OptionsMenuState_EnterScreenPos:
        case OptionsMenuState_EnterBrightness:
        case OptionsMenuState_EnterController:
        case OptionsMenuState_EnterExtraOptions:
#ifdef SH_PC_PORT
        case OptionsMenuState_EnterPcOptions:
#endif
            Options_MainOptionsMenu_Control();
            break;

        case OptionsMenuState_ExtraOptions:
        case OptionsMenuState_LeaveExtraOptions:
            Options_ExtraOptionsMenu_Control();
            break;

#ifdef SH_PC_PORT
        case OptionsMenuState_PcOptions:
        case OptionsMenuState_LeavePcOptions:
            Options_PcOptionsMenu_Control();
            break;
#endif
    }
}

void Options_ExtraOptionsMenu_Control(void) // 0x801E318C
{
    Options_ExtraOptionsMenu_EntryStringsDraw();
    Options_ExtraOptionsMenu_ConfigDraw();
    Options_ExtraOptionsMenu_SelectionHighlightDraw();
    Options_Menu_VignetteDraw();
    Screen_BackgroundImgDraw(&g_ItemInspectionImg);
#ifdef SH_PC_PORT
    Pc_MouseCursor_Draw();
#endif

    if (g_GameWork.gameStateSteps[0] != OptionsMenuState_ExtraOptions)
    {
        return;
    }

    // Increment line move timer.
    if ((LINE_CURSOR_TIMER_MAX - 1) < g_Options_SelectionHighlightTimer)
    {
        g_Options_SelectionHighlightTimer = LINE_CURSOR_TIMER_MAX;
    }
    else
    {
        g_Options_SelectionHighlightTimer++;
    }

    if (g_Options_SelectionHighlightTimer == LINE_CURSOR_TIMER_MAX)
    {
        g_ExtraOptionsMenu_PrevSelectedEntry = g_ExtraOptionsMenu_SelectedEntry;

#ifdef SH_PC_PORT
        /* Mouse: hover selects (snapping), click / wheel cycles the value
         * (every row here is a value row), right-click backs out via the
         * cancel handler at the bottom. Rows start at y=64 on this screen. */
        {
            int mx, my;

            if (Pc_MouseCursor_UiPos(&mx, &my))
            {
                s32 row = -1;
                s32 i;

                for (i = 0; i < g_ExtraOptionsMenu_EntryCount; i++)
                {
                    s32 top = 64 + (i * 16) - 3;
                    if (my >= top && my < top + 16) { row = i; break; }
                }

                if (row >= 0 && Pc_MouseCursor_Moved() && row != g_ExtraOptionsMenu_SelectedEntry)
                {
                    g_ExtraOptionsMenu_SelectedEntry     = row;
                    g_ExtraOptionsMenu_PrevSelectedEntry = row;
                    g_PcOptions_HighlightSnap            = 1;
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                }
                if (row >= 0 && row == g_ExtraOptionsMenu_SelectedEntry)
                {
                    int wheel = Pc_MouseCursor_WheelStep();

                    if (Pc_MouseCursor_LeftClicked())
                        PcMouse_InjectDir(1);
                    else if (wheel != 0)
                        PcMouse_InjectDir(wheel);
                }
                if (Pc_MouseCursor_RightClicked())
                    PcMouse_InjectCancel();
            }
        }
#endif

        // Leave to gameplay (if options menu was accessed with `Option` input action).
        if (g_GameWork.gameStatePrev == GameState_InGame &&
            !(g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter) &&
            (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.option))
        {
            Sd_PlaySfx(Sfx_MenuCancel, 0, 64);

            g_GameWork.gameStateSteps[0] = OptionsMenuState_Leave;
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateSteps[1] = 0;
            g_GameWork.gameStateSteps[2] = 0;
            return;
        }

        // Move selection cursor up/down.
        if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickUp)
        {
            s32 var = 1;
            Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            g_ExtraOptionsMenu_SelectedEntry  = ((g_ExtraOptionsMenu_SelectedEntry - var) + g_ExtraOptionsMenu_EntryCount) % g_ExtraOptionsMenu_EntryCount;
            g_Options_SelectionHighlightTimer = 0;
        }
        if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickDown)
        {
            Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            g_ExtraOptionsMenu_SelectedEntry++;
            g_ExtraOptionsMenu_SelectedEntry  = g_ExtraOptionsMenu_SelectedEntry % g_ExtraOptionsMenu_EntryCount;
            g_Options_SelectionHighlightTimer = 0;
        }

        // Handle config change.
        switch (g_ExtraOptionsMenu_SelectedEntry)
        {
            case ExtraOptionsMenuEntry_WeaponCtrl:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                    g_GameWork.config.extraWeaponCtrl = !g_GameWork.config.extraWeaponCtrl;
                }
                break;

            case ExtraOptionsMenuEntry_Blood:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                    g_ExtraOptionsMenu_SelectedBloodColorEntry++;
                }
                if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                    g_ExtraOptionsMenu_SelectedBloodColorEntry += 3;
                }

                // Set config.
                g_ExtraOptionsMenu_SelectedBloodColorEntry = g_ExtraOptionsMenu_SelectedBloodColorEntry % BloodColorMenuEntry_Count;
                switch (g_ExtraOptionsMenu_SelectedBloodColorEntry)
                {
                    case BloodColorMenuEntry_Normal:
                        g_GameWork.config.extraBloodColor = BloodColor_Normal;
                        break;

                    case BloodColorMenuEntry_Green:
                        g_GameWork.config.extraBloodColor = BloodColor_Green;
                        break;

                    case BloodColorMenuEntry_Violet:
                        g_GameWork.config.extraBloodColor = BloodColor_Violet;
                        break;

                    case BloodColorMenuEntry_Black:
                        g_GameWork.config.extraBloodColor = BloodColor_Black;
                        break;
                }
#ifdef SH_PC_PORT
                /* Mirror the legit blood-color choice so Map_EffectTexturesLoad
                 * can restore it after per-map corruption (#41). */
                {
                    extern unsigned char g_PcTrustedBloodColor;
                    g_PcTrustedBloodColor = (unsigned char)g_GameWork.config.extraBloodColor;
                }
#endif
                break;

            case ExtraOptionsMenuEntry_ViewCtrl:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraViewCtrl = !g_GameWork.config.extraViewCtrl;
                }
                break;

            case ExtraOptionsMenuEntry_RetreatTurn:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraRetreatTurn = (s8)g_GameWork.config.extraRetreatTurn == 0;
                }
                break;

            case ExtraOptionsMenuEntry_MovementCtrl:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraWalkRunCtrl = (s8)g_GameWork.config.extraWalkRunCtrl == 0;
                }
                break;

            case ExtraOptionsMenuEntry_AutoAiming:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraAutoAiming = (s8)g_GameWork.config.extraAutoAiming == 0;
                }
                break;

            case ExtraOptionsMenuEntry_ViewMode:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraViewMode = !g_GameWork.config.extraViewMode;
                }
                break;

            case ExtraOptionsMenuEntry_BulletMult:
                // Scroll left/right.
                if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraBulletAdjust++;
                }
                if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                    // Set config.
                    g_GameWork.config.extraBulletAdjust = g_GameWork.config.extraBulletAdjust + (g_ExtraOptionsMenu_BulletMultMax - 1);
                }
                g_GameWork.config.extraBulletAdjust = g_GameWork.config.extraBulletAdjust % g_ExtraOptionsMenu_BulletMultMax;
                break;
        }
    }

    // Leave menu.
    if ((g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.cancel |
                                          (ControllerFlag_L2 | ControllerFlag_R2 |
                                           ControllerFlag_L1 | ControllerFlag_R1))) &&
        g_GameWork.gameStateSteps[0] != OptionsMenuState_LeaveExtraOptions)
    {
        if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
        {
            Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        }
        else
        {
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        }

        ScreenFade_Start(true, false, false);
        g_GameWork.gameStateSteps[0] = OptionsMenuState_LeaveExtraOptions;
        g_SysWork.counters_1C[1]              = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
    }
}

void Options_MainOptionsMenu_Control(void) // 0x801E3770
{
    #define SOUND_VOL_STEP 8

    s32 audioType;
    s32 vol;

    // Draw graphics.
    Options_MainOptionsMenu_EntryStringsDraw();
    Options_MainOptionsMenu_ConfigDraw();
    Options_MainOptionsMenu_SelectionHighlightDraw();
    Options_Menu_VignetteDraw();
    Screen_BackgroundImgDraw(&g_ItemInspectionImg);
    Options_MainOptionsMenu_BgmVolumeBarDraw();
    Options_MainOptionsMenu_SfxVolumeBarDraw();
#ifdef SH_PC_PORT
    Options_MainOptionsMenu_FmvVolumeBarDraw();
    Pc_MouseCursor_Draw();
#endif

    if (g_GameWork.gameStateSteps[0] != OptionsMenuState_MainOptions)
    {
        return;
    }

    // Increment line move timer.
    if ((LINE_CURSOR_TIMER_MAX - 1) < g_Options_SelectionHighlightTimer)
    {
        g_Options_SelectionHighlightTimer = LINE_CURSOR_TIMER_MAX;
    }
    else
    {
        g_Options_SelectionHighlightTimer++;
    }

    if (g_Options_SelectionHighlightTimer != LINE_CURSOR_TIMER_MAX)
    {
        return;
    }

    g_MainOptionsMenu_PrevSelectedEntry = g_MainOptionsMenu_SelectedEntry;

#ifdef SH_PC_PORT
    /* Mouse: hover selects (snapping past the highlight timer so the follow-up
     * click isn't swallowed), click enters the submenu rows / cycles the value
     * rows, click-drag on a volume bar walks the volume to the pointed notch
     * (one stock step per frame, so the game's clamp + SFX stay in charge),
     * wheel steps the hovered value, right-click = cancel. */
    {
        int mx, my;

        if (Pc_MouseCursor_UiPos(&mx, &my))
        {
            s32 row = -1;
            s32 i;

            for (i = 0; i < MainOptionsMenuEntry_Count; i++)
            {
                s32 top = 56 + (i * 16) - 3;
                if (my >= top && my < top + 16) { row = i; break; }
            }

            if (row >= 0 && Pc_MouseCursor_Moved() && row != g_MainOptionsMenu_SelectedEntry)
            {
                g_MainOptionsMenu_SelectedEntry     = row;
                g_MainOptionsMenu_PrevSelectedEntry = row;
                g_PcOptions_HighlightSnap           = 1;
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            }

            if (row >= 0 && row == g_MainOptionsMenu_SelectedEntry)
            {
                int wheel = Pc_MouseCursor_WheelStep();

                if (row <= MainOptionsMenuEntry_ScreenPosition)
                {
                    /* Exit / Brightness / Controller / PC Options. */
                    if (Pc_MouseCursor_LeftClicked())
                        PcMouse_InjectEnter();
                }
                else if (row >= MainOptionsMenuEntry_BgmVolume)
                {
                    /* Volume bars: notches at authored x 184 + n*6, n = 0..15;
                     * just left of the bar drags the volume to zero. */
                    if (Pc_MouseCursor_LeftHeld() && mx >= 174)
                    {
                        extern float g_PcXaVolume;
                        s32 curVol =
                            (row == MainOptionsMenuEntry_BgmVolume) ? g_GameWork.config.volumeBgm :
                            (row == MainOptionsMenuEntry_SfxVolume) ? g_GameWork.config.volumeSe  :
                            CLAMP((s32)((g_PcXaVolume * (float)OPT_SOUND_VOLUME_MAX) + 0.5f), 0, OPT_SOUND_VOLUME_MAX);
                        s32 lit = (mx < 184) ? 0 : CLAMP(((mx - 184) / 6) + 1, 1, 16);

                        if (lit > curVol / 8)
                            PcMouse_InjectDir(1);
                        else if (lit < curVol / 8)
                            PcMouse_InjectDir(-1);
                    }
                    else if (wheel != 0)
                    {
                        PcMouse_InjectDir(wheel);
                    }
                }
                else if (Pc_MouseCursor_LeftClicked())
                {
                    /* Vibration / Auto Load (Language) / Sound toggles. */
                    PcMouse_InjectDir(1);
                }
                else if (wheel != 0)
                {
                    PcMouse_InjectDir(wheel);
                }
            }

            if (Pc_MouseCursor_RightClicked())
                PcMouse_InjectCancel();
        }
    }
#endif

    // Leave to gameplay (if options menu was accessed with `Option` input action).
    if (g_GameWork.gameStatePrev == GameState_InGame &&
        !(g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter) &&
        (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.option))
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);

        g_GameWork.gameStateSteps[0] = OptionsMenuState_Leave;
        g_SysWork.counters_1C[1]              = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
        return;
    }

    // Move selection cursor up/down.
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickUp)
    {
        Sd_PlaySfx(Sfx_MenuMove, 0, 64);

        g_Options_SelectionHighlightTimer = 0;
        g_MainOptionsMenu_SelectedEntry   = (g_MainOptionsMenu_SelectedEntry + (MainOptionsMenuEntry_Count - 1)) % MainOptionsMenuEntry_Count;
    }
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickDown)
    {
        Sd_PlaySfx(Sfx_MenuMove, 0, 64);

        g_Options_SelectionHighlightTimer = 0;
        g_MainOptionsMenu_SelectedEntry   = (g_MainOptionsMenu_SelectedEntry + 1) % MainOptionsMenuEntry_Count;
    }

    // Handle config change.
    switch (g_MainOptionsMenu_SelectedEntry)
    {
        case MainOptionsMenuEntry_Exit:
            // Exit menu to gameplay.
            if (g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.enter |
                                                 g_GameWorkPtr->config.controllerConfig.cancel))
            {
                Sd_PlaySfx(Sfx_MenuCancel, 0, 64);

                g_GameWork.gameStateSteps[0] = OptionsMenuState_Leave;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case MainOptionsMenuEntry_Controller:
            // Enter controller screen.
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
            {
                Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
                Fs_QueueStartReadTim(FILE_TIM_OPTION2_TIM, IMAGE_BUFFER_3, &g_ControllerButtonAtlasImg);

                ScreenFade_Start(true, false, false);
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterController;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case MainOptionsMenuEntry_ScreenPosition:
            // Enter screen position screen.
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
            {
                Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);

                ScreenFade_Start(true, false, false);
#ifdef SH_PC_PORT
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterPcOptions;
#else
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterScreenPos;
#endif
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case MainOptionsMenuEntry_Brightness:
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
            {
                Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
                if (g_GameWork.gameStatePrev == GameState_MainMenu)
                {
                    Fs_QueueStartReadTim(FILE_TIM_OP_BRT_E_TIM, IMAGE_BUFFER_3, &g_BrightnessScreenImg0);
                }
                else
                {
                    Fs_QueueStartReadTim(FILE_TIM_OP_BRT_E_TIM, IMAGE_BUFFER_3, &g_BrightnessScreenImg1);
                }

                ScreenFade_Start(true, false, false);
                g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterBrightness;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case MainOptionsMenuEntry_Vibration:
            if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
            {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                g_GameWork.config.vibrationEnabled = !g_GameWork.config.vibrationEnabled << 7;
            }
            break;

        case MainOptionsMenuEntry_AutoLoad:
#ifdef SH_PC_PORT
            /* PAL: this row is Language when the menu was entered from the
             * title screen (retail SLES had a front-end Language option). */
            if (Pc_LangMenuRowActive())
            {
                int langCount = Pc_LangSelectableCount();

                if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                    Pc_LangSetLanguage((g_PcConfig.language + 1) % langCount);
                }
                else if (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)
                {
                    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                    Pc_LangSetLanguage((g_PcConfig.language + langCount - 1) % langCount);
                }
                break;
            }
#endif
            if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
            {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                g_GameWork.config.autoLoad = (s8)g_GameWork.config.autoLoad == 0;
            }
            break;

        case MainOptionsMenuEntry_Sound:
            if (g_Controller0->clickedBtnFlags & (ControllerFlag_LStickRight | ControllerFlag_LStickLeft))
            {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);

                // Set config.
                audioType                           = AudioMode_Stereo;
                g_GameWork.config.soundType = !g_GameWork.config.soundType;
                if (g_GameWork.config.soundType)
                {
                    audioType = AudioMode_Mono;
                }
                SD_Call(audioType);
            }
            break;

        case MainOptionsMenuEntry_BgmVolume:
            vol = g_GameWork.config.volumeBgm;

            if ((vol < OPT_SOUND_VOLUME_MAX && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)) ||
                (vol > 0                    && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuMove);
            }
            if ((vol == OPT_SOUND_VOLUME_MAX && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)) ||
                (vol == 0                    && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuError);
            }

            // Scroll left/right.
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
            {
                vol = vol + SOUND_VOL_STEP;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
            {
                vol = vol - SOUND_VOL_STEP;
            }

            // Set config.
            vol = CLAMP(vol, 0, OPT_SOUND_VOLUME_MAX);
            Sd_SetVolume(OPT_SOUND_VOLUME_MAX, vol, g_GameWork.config.volumeSe);
            g_GameWork.config.volumeBgm = vol;
            break;

        case MainOptionsMenuEntry_SfxVolume:
            vol = g_GameWork.config.volumeSe;

            if ((vol < OPT_SOUND_VOLUME_MAX && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)) ||
                (vol > 0                    && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuMove);
            }
            if ((vol == OPT_SOUND_VOLUME_MAX && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)) ||
                (vol == 0                    && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuError);
            }

            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
            {
                vol = vol + SOUND_VOL_STEP;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
            {
                vol = vol - SOUND_VOL_STEP;
            }

            vol = CLAMP(vol, 0, OPT_SOUND_VOLUME_MAX);

            Sd_SetVolume(OPT_SOUND_VOLUME_MAX, vol, g_GameWork.config.volumeSe);
            g_GameWork.config.volumeSe = vol;
            break;

#ifdef SH_PC_PORT
        case MainOptionsMenuEntry_FmvVolume:
        {
            /* PC-only: XA cutscene-voice stream volume ("Voice"). Mirrors the
             * BGM/SE slider feel (16 notches, step 8 over 0..128) but drives
             * g_PcXaVolume in [0,1] and persists `xa_volume`. FMV movie audio is
             * a separate slider (FMV Movie Vol on the PC Options page). */
            extern float g_PcXaVolume;
            extern void  PcConfig_ApplyXaVolume(float norm);

            s32 fvol = (s32)((g_PcXaVolume * (float)OPT_SOUND_VOLUME_MAX) + 0.5f);
            s32 oldFvol;

            fvol    = CLAMP(fvol, 0, OPT_SOUND_VOLUME_MAX);
            oldFvol = fvol;

            if ((fvol < OPT_SOUND_VOLUME_MAX && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)) ||
                (fvol > 0                    && (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuMove);
            }
            if ((fvol == OPT_SOUND_VOLUME_MAX && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight)) ||
                (fvol == 0                    && (g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft)))
            {
                SD_Call(Sfx_MenuError);
            }

            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
            {
                fvol += SOUND_VOL_STEP;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
            {
                fvol -= SOUND_VOL_STEP;
            }
            fvol = CLAMP(fvol, 0, OPT_SOUND_VOLUME_MAX);

            if (fvol != oldFvol)
            {
                PcConfig_ApplyXaVolume((float)fvol / (float)OPT_SOUND_VOLUME_MAX);
            }
            break;
        }
#endif

        default:
            break;
    }

    vol = 0;

    if (g_Controller0->clickedBtnFlags & (ControllerFlag_L2 | ControllerFlag_R2 |
                                         ControllerFlag_L1 | ControllerFlag_R1))
    {
        if (g_GameWork.gameStateSteps[0] == OptionsMenuState_EnterExtraOptions)
        {
            return;
        }

        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);

        ScreenFade_Start(true, false, false);
        g_GameWork.gameStateSteps[0] = OptionsMenuState_EnterExtraOptions;
        g_SysWork.counters_1C[1]              = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
    }

    // Reset selection cursor.
    if (((g_GameWork.gameStateSteps[0] != OptionsMenuState_EnterExtraOptions && g_MainOptionsMenu_SelectedEntry != MainOptionsMenuEntry_Exit) &&
         !(g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)) &&
        (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel))
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);

        g_Options_SelectionHighlightTimer = 0;
        g_MainOptionsMenu_SelectedEntry         = MainOptionsMenuEntry_Exit;
    }

    #undef SOUND_VOL_STEP
}

void Options_MainOptionsMenu_BgmVolumeBarDraw(void) // 0x801E3F68
{
    Options_MainOptionsMenu_VolumeBarDraw(0, g_GameWork.config.volumeBgm);
}

void Options_MainOptionsMenu_SfxVolumeBarDraw(void) // 0x801E3F90
{
    Options_MainOptionsMenu_VolumeBarDraw(1, g_GameWork.config.volumeSe);
}

#ifdef SH_PC_PORT
void Options_MainOptionsMenu_FmvVolumeBarDraw(void)
{
    extern float g_PcXaVolume;
    s32 fvol = (s32)((g_PcXaVolume * (float)OPT_SOUND_VOLUME_MAX) + 0.5f);
    fvol = CLAMP(fvol, 0, OPT_SOUND_VOLUME_MAX);
    Options_MainOptionsMenu_VolumeBarDraw(2, (u8)fvol);
}
#endif

void Options_MainOptionsMenu_VolumeBarDraw(s32 row, u8 vol) // 0x801E3FB8
{
    #define STR_OFFSET_Y 16
    #define NOTCH_SIZE_X 5
    #define NOTCH_COUNT  16

    s32      x0Offset;
    s32      x0;
    s32      offset;
    s32      yOffset;
    s32      localVol;
    s32      i;
    s32      j;
    u32      colorComp;
    s32      xOffset;
    s32      color0;
    s32      color1;
    s32      color2;
    GsOT*    ot;
    POLY_F4* poly;

    ot       = &g_OrderingTable2[g_ActiveBufferIdx];
    localVol = vol;

    // Draw bar notches.
    for (i = 0; i < NOTCH_COUNT; i++)
    {
        colorComp = localVol & 0x7;
        colorComp = (colorComp * 12) + 64;

        for (j = 1; j >= 0; j--)
        {
            poly = (POLY_F4*)GsOUT_PACKET_P;
            setPolyF4(poly);

            if (i < (vol / 8))
            {
                color0 = 0xA0 + (0x40 * j);
                setRGBC0(poly, color0, color0, color0, PRIM_POLY | RECT_SIZE_1);
            }
            else if (i > (vol / 8))
            {
                color1 = 0x40 + (0x40 * j);
                setRGBC0(poly, color1, color1, color1, PRIM_POLY | RECT_SIZE_1);
            }
            else
            {
                color2 = colorComp + (0x40 * j);
                setRGBC0(poly, color2, color2, color2, PRIM_POLY | RECT_SIZE_1);
            }

            xOffset = 24 + (i * 6);
            offset  = -69;

            x0Offset = j + 24;
            x0       = (x0Offset + (i * 6)) & 0xFFFF;
            yOffset  = j + 56;

            setXY0Fast(poly, x0,                           (row * STR_OFFSET_Y) + yOffset);
            setXY1Fast(poly, x0,                           (row * STR_OFFSET_Y) - (j + offset));
            setXY2Fast(poly, (xOffset - j) + NOTCH_SIZE_X, (row * STR_OFFSET_Y) + yOffset);
            setXY3Fast(poly, (xOffset - j) + NOTCH_SIZE_X, (row * STR_OFFSET_Y) - (j + offset));
            addPrim((u8*)ot->org + LAYER_24, poly);
            GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F4);
        }
    }

    #undef STR_OFFSET_Y
    #undef NOTCH_SIZE_X
    #undef NOTCH_COUNT
}

void Options_ExtraOptionsMenu_EntryStringsDraw(void) // 0x801E416C
{
    #define LINE_BASE_X   64
    #define LINE_BASE_Y   64
    #define LINE_OFFSET_X 16
    #define LINE_OFFSET_Y 16

    s32            i;
    static DVECTOR selectionHighlightFromUnused;
    static DVECTOR selectionHighlightToUnused;

    const DVECTOR STR_POS = { 86, 20 };

    const char* EXTRA_OPTIONS_STR = "EXTRA_OPTION_\x01\x01\x01\x01\x01S";

    const char* ENTRY_STRS[] = {
        "Weapon_Control",
        "Blood_Color",
        "View_Control",
        "Retreat_Turn",
        "\x01W"
        "\x01"
        "a\x01l\x01k/R\x01\x01u\x01n_\x01\x01\x01\x01"
        "Co\x01n\x01t\x01ro\x01l",
        "Auto_Aiming",
        "View_Mode",
        "Bullet_Adjust"
    };

    // @unused Likely an older implementation for active highlight selection position reference setup found in `Options_ExtraOptionsMenu_SelectionHighlightDraw`.
    if (g_Options_SelectionHighlightTimer == 0)
    {
        selectionHighlightFromUnused.vx = LINE_BASE_X - LINE_OFFSET_X;
        selectionHighlightToUnused.vx   = LINE_BASE_X;
        selectionHighlightFromUnused.vy = ((u16)g_MainOptionsMenu_PrevSelectedEntry * LINE_OFFSET_Y) + LINE_BASE_Y;
        selectionHighlightToUnused.vy   = ((u16)g_MainOptionsMenu_SelectedEntry     * LINE_OFFSET_Y) + LINE_BASE_Y;
    }
    Math_Sin(g_Options_SelectionHighlightTimer << 7);

    // Draw heading string.
    Gfx_StringSetColor(StringColorId_White);
    Gfx_StringSetPosition(STR_POS.vx, STR_POS.vy);
    Gfx_Strings2dLayerIdxSet(8);
    Gfx_StringDraw(EXTRA_OPTIONS_STR, DEFAULT_MAP_MESSAGE_LENGTH);

    // Draw entry strings.
    for (i = 0; i < g_ExtraOptionsMenu_EntryCount; i++)
    {
        Gfx_StringSetPosition(LINE_BASE_X, LINE_BASE_Y + (i * LINE_OFFSET_Y));
        Gfx_Strings2dLayerIdxSet(8);
        Gfx_StringDraw(ENTRY_STRS[i], DEFAULT_MAP_MESSAGE_LENGTH);
    }

    #undef LINE_BASE_X
    #undef LINE_BASE_Y
    #undef LINE_OFFSET_X
    #undef LINE_OFFSET_Y
}

void Options_MainOptionsMenu_EntryStringsDraw(void) // 0x801E42EC
{
    #define LINE_BASE_X   64
    #define LINE_BASE_Y   56
    #define LINE_OFFSET_X 16
    #define LINE_OFFSET_Y 16

    s32            i;
    static DVECTOR selectionHighlightFromUnused;
    static DVECTOR selectionHighlightToUnused;

    DVECTOR strPos = { 121, 20 };

    const char* OPTIONS_STR = "OPTION_\x01\x01\x01\x01\x01S";

    const char* ENTRY_STRS[] = {
        "Exit",
        "Brightness_Level",
        "Controller_Config",
#ifdef SH_PC_PORT
        "PC_Options",
#else
        "Screen_Position",
#endif
        "Vibration",
        "Auto_Load",
        "Sound",
        "BGM_Volume",
        "SE_Volume"
#ifdef SH_PC_PORT
        ,
        "Voice"
#endif
    };

#ifdef SH_PC_PORT
    if (Pc_LangMenuRowActive())
    {
        ENTRY_STRS[MainOptionsMenuEntry_AutoLoad] = "Language";
    }
#endif

    // @unused Likely an older implementation for active highlight selection position reference setup found in `Options_MainOptionsMenu_SelectionHighlightDraw`.
    if (g_Options_SelectionHighlightTimer == 0)
    {
        selectionHighlightFromUnused.vx = LINE_BASE_X - LINE_OFFSET_X;
        selectionHighlightToUnused.vx   = LINE_BASE_X;
        selectionHighlightFromUnused.vy = ((u16)g_MainOptionsMenu_PrevSelectedEntry * LINE_OFFSET_Y) + LINE_BASE_Y;
        selectionHighlightToUnused.vy   = ((u16)g_MainOptionsMenu_SelectedEntry     * LINE_OFFSET_Y) + LINE_BASE_Y;
    }
    Math_Sin(g_Options_SelectionHighlightTimer << 7);

    // Draw heading string.
    Gfx_StringSetColor(StringColorId_White);
    Gfx_StringSetPosition(strPos.vx, strPos.vy);
    Gfx_Strings2dLayerIdxSet(8);
    Gfx_StringDraw(OPTIONS_STR, DEFAULT_MAP_MESSAGE_LENGTH);

    // Draw entry strings.
    for (i = 0; i < MainOptionsMenuEntry_Count; i++)
    {
        Gfx_StringSetPosition(LINE_BASE_X, LINE_BASE_Y + (i * LINE_OFFSET_Y));
        Gfx_Strings2dLayerIdxSet(8);
        Gfx_StringDraw(ENTRY_STRS[i], DEFAULT_MAP_MESSAGE_LENGTH);
    }

    Gfx_StringsReset2dLayerIdx();

    #undef LINE_BASE_X
    #undef LINE_BASE_Y
    #undef LINE_OFFSET_X
    #undef LINE_OFFSET_Y
}

void Options_ExtraOptionsMenu_SelectionHighlightDraw(void) // 0x801E4450
{
    #define BULLET_QUAD_COUNT  2
    #define LINE_BASE_X        64
    #define LINE_BASE_Y        56
    #define LINE_OFFSET_X      16
    #define LINE_OFFSET_Y      16
    #define HIGHLIGHT_OFFSET_X -121
    #define HIGHLIGHT_OFFSET_Y 50

    s32            i;
    s32            j;
    q3_12          interpAlpha;
    s_Line2d       highlightLine;
    s_Quad2d       bulletQuads[BULLET_QUAD_COUNT];
    DVECTOR*       quadVerts;
    static DVECTOR selectionHighlightFrom;
    static DVECTOR selectionHighlightTo;

    const u8 SELECTION_HIGHLIGHT_WIDTHS[] = {
        157, 126, 135, 135, 157, 130, 112, 134
    };

    // 12x12 quad.
    const DVECTOR FRONT_BULLET_QUAD[] = {
        { -120, -47 },
        { -120, -35 },
        { -108, -47 },
        { -108, -35 }
    };

    // 14x14 quad.
    const DVECTOR BACK_BULLET_QUAD[] = {
        { -121, -48 },
        { -121, -34 },
        { -107, -48 },
        { -107, -34 }
    };

    // Set active selection highlight position references.
#ifdef SH_PC_PORT
    if (g_Options_SelectionHighlightTimer == 0 || g_PcOptions_HighlightSnap)
#else
    if (g_Options_SelectionHighlightTimer == 0)
#endif
    {
#ifdef SH_PC_PORT
        g_PcOptions_HighlightSnap = 0;
#endif
        selectionHighlightFrom.vx = SELECTION_HIGHLIGHT_WIDTHS[g_ExtraOptionsMenu_PrevSelectedEntry] + (65536 + HIGHLIGHT_OFFSET_X); // TODO
        selectionHighlightFrom.vy = ((u16)g_ExtraOptionsMenu_PrevSelectedEntry * LINE_OFFSET_Y)      - HIGHLIGHT_OFFSET_Y;
        selectionHighlightTo.vx   = SELECTION_HIGHLIGHT_WIDTHS[g_ExtraOptionsMenu_SelectedEntry]     + (65536 + HIGHLIGHT_OFFSET_X); // TODO
        selectionHighlightTo.vy   = ((u16)g_ExtraOptionsMenu_SelectedEntry * LINE_OFFSET_Y)          - HIGHLIGHT_OFFSET_Y;
    }

    // Compute sine-based interpolation alpha.
    interpAlpha = Math_Sin(g_Options_SelectionHighlightTimer << 7);

    // Draw active selection highlight.
    highlightLine.vertex0.vx = HIGHLIGHT_OFFSET_X;
    highlightLine.vertex1.vx = selectionHighlightFrom.vx +
                               Q12_MULT(selectionHighlightTo.vx - selectionHighlightFrom.vx, interpAlpha);
    highlightLine.vertex1.vy = selectionHighlightFrom.vy +
                               Q12_MULT(selectionHighlightTo.vy - selectionHighlightFrom.vy, interpAlpha) +
                               LINE_OFFSET_Y;
    highlightLine.vertex0.vy = highlightLine.vertex1.vy;
    Options_Selection_HighlightDraw(&highlightLine, true, false);

    // Draw selection bullet points.
    for (i = 0; i < g_ExtraOptionsMenu_EntryCount; i++)
    {
        // Set bullet quads.
        quadVerts = (DVECTOR*)&bulletQuads;
        for (j = 0; j < RECT_VERT_COUNT; j++)
        {
            quadVerts[j].vx                   = FRONT_BULLET_QUAD[j].vx;
            quadVerts[j].vy                   = FRONT_BULLET_QUAD[j].vy + (i * LINE_OFFSET_Y);
            quadVerts[j + sizeof(DVECTOR)].vx = BACK_BULLET_QUAD[j].vx;
            quadVerts[j + sizeof(DVECTOR)].vy = BACK_BULLET_QUAD[j].vy + (i * LINE_OFFSET_Y);
        }

        // Active selection bullet point.
        if (i == g_ExtraOptionsMenu_SelectedEntry)
        {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, false);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  false);
        }
        // Inactive selection bullet point.
        else
        {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, true);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  true);
        }
    }

    #undef BULLET_QUAD_COUNT
    #undef LINE_BASE_X
    #undef LINE_BASE_Y
    #undef LINE_OFFSET_X
    #undef LINE_OFFSET_Y
    #undef HIGHLIGHT_OFFSET_X
    #undef HIGHLIGHT_OFFSET_Y
}

void Options_MainOptionsMenu_SelectionHighlightDraw(void) // 0x801E472C
{
    #define LINE_OFFSET_Y      16
    #define HIGHLIGHT_OFFSET_X -121
    #define HIGHLIGHT_OFFSET_Y 58

    s32            i;
    s32            j;
    s16            interpAlpha;
    s_Line2d       highlightLine;
    s_Quad2d       bulletQuads[2];
    DVECTOR*       quadVerts;
    static DVECTOR selectionHighlightFrom;
    static DVECTOR selectionHighlightTo;

    const u8 SELECTION_HIGHLIGHT_WIDTHS[] = {
        59, 169, 174, 156, 104, 112, 75, 129, 112
#ifdef SH_PC_PORT
        , 75 /* Voice */
#endif
    };

    // 12x12 quad.
    const DVECTOR BULLET_QUAD_VERTS_FRONT[] = {
        { -120, -55 },
        { -120, -43 },
        { -108, -55 },
        { -108, -43 }
    };

    // 14x14 quad.
    const DVECTOR BULLET_QUAD_VERTS_BACK[] = {
        { -121, -56 },
        { -121, -42 },
        { -107, -56 },
        { -107, -42 }
    };

    // Set active selection highlight position references.
#ifdef SH_PC_PORT
    if (g_Options_SelectionHighlightTimer == 0 || g_PcOptions_HighlightSnap)
#else
    if (g_Options_SelectionHighlightTimer == 0)
#endif
    {
#ifdef SH_PC_PORT
        g_PcOptions_HighlightSnap = 0;
#endif
        selectionHighlightFrom.vx = SELECTION_HIGHLIGHT_WIDTHS[g_MainOptionsMenu_PrevSelectedEntry] + (65536 + HIGHLIGHT_OFFSET_X); // TODO
        selectionHighlightFrom.vy = ((u16)g_MainOptionsMenu_PrevSelectedEntry * LINE_OFFSET_Y)      - HIGHLIGHT_OFFSET_Y;
        selectionHighlightTo.vx   = SELECTION_HIGHLIGHT_WIDTHS[g_MainOptionsMenu_SelectedEntry]     + (65536 + HIGHLIGHT_OFFSET_X); // TODO
        selectionHighlightTo.vy   = ((u16)g_MainOptionsMenu_SelectedEntry * LINE_OFFSET_Y)          - HIGHLIGHT_OFFSET_Y;
    }

    // Compute sine-based interpolation alpha.
    interpAlpha = Math_Sin(g_Options_SelectionHighlightTimer << 7);

    // Draw active selection highlight.
    highlightLine.vertex0.vx = HIGHLIGHT_OFFSET_X;
    highlightLine.vertex1.vx = selectionHighlightFrom.vx +
                               FP_FROM((selectionHighlightTo.vx - selectionHighlightFrom.vx) * interpAlpha, Q12_SHIFT);
    highlightLine.vertex1.vy = selectionHighlightFrom.vy +
                               FP_FROM((selectionHighlightTo.vy - selectionHighlightFrom.vy) * interpAlpha, Q12_SHIFT) +
                               LINE_OFFSET_Y;
    highlightLine.vertex0.vy = highlightLine.vertex1.vy;
    Options_Selection_HighlightDraw(&highlightLine, true, false);

    // Draw selection bullet points.
    for (i = 0; i < MainOptionsMenuEntry_Count; i++)
    {
        // Set bullet quads.
        quadVerts = (DVECTOR*)&bulletQuads;
        for (j = 0; j < RECT_VERT_COUNT; j++)
        {
            quadVerts[j].vx                   = BULLET_QUAD_VERTS_FRONT[j].vx;
            quadVerts[j].vy                   = BULLET_QUAD_VERTS_FRONT[j].vy + (i * LINE_OFFSET_Y);
            quadVerts[j + sizeof(DVECTOR)].vx = BULLET_QUAD_VERTS_BACK[j].vx;
            quadVerts[j + sizeof(DVECTOR)].vy = BULLET_QUAD_VERTS_BACK[j].vy + (i * LINE_OFFSET_Y);
        }

        // Active selection bullet point.
        if (i == g_MainOptionsMenu_SelectedEntry)
        {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, false);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  false);
        }
        // Inactive selection bullet point.
        else
        {
            Options_Selection_BulletPointDraw(&bulletQuads[0], false, true);
            Options_Selection_BulletPointDraw(&bulletQuads[1], true,  true);
        }
    }

    #undef LINE_OFFSET_Y
    #undef HIGHLIGHT_OFFSET_X
    #undef HIGHLIGHT_OFFSET_Y
}

void Options_Menu_VignetteDraw(void) // 0x801E49F0
{
    s32      y0;
    s32      y1;
    s32      xy2;
    s32      xy3;
    s32      i;
    GsOT*    ot;
    POLY_G4* poly;

    ot = &g_OrderingTable0[g_ActiveBufferIdx];

    xy3 = 160 + (0xFFA0 << 16); // TODO: -96
    xy2 = 160 + (0xFF90 << 16); // TODO: -112
    y1  = 0xFFA0 << 16;         // TODO: -96
    y0  = 0xFF90 << 16;         // TODO: -112

    for (i = 0; i < 2; i++)
    {
        poly = (POLY_G4*)GsOUT_PACKET_P;

        setPolyG4(poly);
        setSemiTrans(poly, true);

        setRGB0(poly, 0x60, 0x60, 0x60);
        setRGB1(poly, 0, 0, 0);
        setRGB2(poly, 0x60, 0x60, 0x60);
        setRGB3(poly, 0, 0, 0);

        *(u32*)(&poly->x0) = 0xFF60 + (y0 + ((0xE0 * i) << 16)); // TODO: -160
        *(u32*)(&poly->x1) = 0xFF60 + (y1 + ((0xA8 * i) << 16)); // TODO: -160
        *(u32*)(&poly->x2) = xy2    + ((0xE0 * i) << 16);
        *(u32*)(&poly->x3) = xy3    + ((0xA8 * i) << 16);

        addPrim((u8*)ot->org + LAYER_8148, poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G4);
    }

    Gfx_Primitive2dTextureSet(0, 0, 2037, 6);
}

void Options_ExtraOptionsMenu_ConfigDraw(void) // 0x801E4B2C
{
    #define STR_BASE_Y   64
    #define STR_OFFSET_Y 16

    const s_Triangle2d FRONT_ARROWS[] = {
        { { 38,  -42 }, { 46,  -50 }, { 46,  -34 } },
        { { 120, -42 }, { 112, -50 }, { 112, -34 } },
        { { 38,  -26 }, { 46,  -34 }, { 46,  -18 } },
        { { 120, -26 }, { 112, -34 }, { 112, -18 } },
        { { 35,  -10 }, { 43,  -18 }, { 43,  -2  } },
        { { 123, -10 }, { 115, -18 }, { 115, -2  } },
        { { 35,   6  }, { 43,  -2  }, { 43,   14 } },
        { { 123,  6  }, { 115, -2  }, { 115,  14 } },
        { { 35,   22 }, { 43,   14 }, { 43,   30 } },
        { { 123,  22 }, { 115,  14 }, { 115,  30 } },
        { { 51,   38 }, { 59,   30 }, { 59,   46 } },
        { { 107,  38 }, { 99,   30 }, { 99,   46 } },
        { { 24,   54 }, { 32,   46 }, { 32,   62 } },
        { { 131,  54 }, { 123,  46 }, { 123,  62 } },
        { { 57,   70 }, { 65,   62 }, { 65,   78 } },
        { { 104,  70 }, { 96,   62 }, { 96,   78 } }
    };

    const s_Triangle2d BACK_ARROWS[] = {
        { { 37,  -42 }, { 47,  -52 }, { 47,  -32 } },
        { { 121, -42 }, { 111, -52 }, { 111, -32 } },
        { { 37,  -26 }, { 47,  -36 }, { 47,  -16 } },
        { { 121, -26 }, { 111, -36 }, { 111, -16 } },
        { { 34,  -10 }, { 44,  -20 }, { 44,   0  } },
        { { 124, -10 }, { 114, -20 }, { 114,  0  } },
        { { 34,   6  }, { 44,  -4  }, { 44,   16 } },
        { { 124,  6  }, { 114, -4  }, { 114,  16 } },
        { { 34,   22 }, { 44,   12 }, { 44,   32 } },
        { { 124,  22 }, { 114,  12 }, { 114,  32 } },
        { { 50,   38 }, { 60,   28 }, { 60,   48 } },
        { { 108,  38 }, { 98,   28 }, { 98,   48 } },
        { { 23,   54 }, { 33,   44 }, { 33,   64 } },
        { { 132,  54 }, { 122,  44 }, { 122,  64 } },
        { { 56,   70 }, { 66,   60 }, { 66,   80 } },
        { { 105,  70 }, { 95,   60 }, { 95,   80 } }
    };

    // TODO: Can this be split?
    const char* CONFIG_STRS[] = {
        "Press",
        "Switch",

        "Normal",
        "Green",
        "Violet",
        "Black",

        "_",

        "Normal",
        "Reverse",

        "On",
        "Off",

        "Normal",
        "Self_View",

        "x1",
        "x2",
        "x3",
        "x4",
        "x5",
        "x6"
    };

    s32 strPosX;
    s32 i;
    s32 j;

    Gfx_StringSetColor(StringColorId_White);

    // Draw left/right arrows for subset of options.
    if (g_ExtraOptionsMenu_SelectedEntry < (u32)ExtraOptionsMenuEntry_Count)
    {
        // Draw flashing left/right arrows.
        for (i = 0; i < 2; i++)
        {
            Options_Selection_ArrowDraw(&FRONT_ARROWS[(g_ExtraOptionsMenu_SelectedEntry * 2) + i], true, false);
        }

        // Draw border to highlight flashing left/right arrow corresponding to direction of UI navigation.
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickLeft)
        {
            Options_Selection_ArrowDraw(&BACK_ARROWS[g_ExtraOptionsMenu_SelectedEntry << 1], false, false);
        }
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickRight)
        {
            Options_Selection_ArrowDraw(&BACK_ARROWS[(g_ExtraOptionsMenu_SelectedEntry << 1) + 1], false, false);
        }
    }

    // Draw entry strings.
    for (j = 0; j < g_ExtraOptionsMenu_EntryCount; j++)
    {
        switch (j)
        {
            case ExtraOptionsMenuEntry_WeaponCtrl:
                strPosX = (g_GameWork.config.extraWeaponCtrl != 0) ? 217 : 212;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_WeaponCtrl));
                Gfx_StringDraw(CONFIG_STRS[!g_GameWork.config.extraWeaponCtrl], 10);
                break;

            case ExtraOptionsMenuEntry_Blood:
                switch (g_ExtraOptionsMenu_SelectedBloodColorEntry)
                {
                    case BloodColorMenuEntry_Normal:
                        Gfx_StringSetPosition(210, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_Blood));
                        break;

                    case BloodColorMenuEntry_Green:
                        Gfx_StringSetPosition(214, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_Blood));
                        break;

                    case BloodColorMenuEntry_Violet:
                        Gfx_StringSetPosition(214, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_Blood));
                        break;

                    case BloodColorMenuEntry_Black:
                        Gfx_StringSetPosition(217, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_Blood));
                        break;
                }

                Gfx_StringDraw(CONFIG_STRS[g_ExtraOptionsMenu_SelectedBloodColorEntry + 2], 10);
                break;

            case ExtraOptionsMenuEntry_ViewCtrl:
                strPosX = !g_GameWork.config.extraViewCtrl ? 210 : 206;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_ViewCtrl));
                Gfx_StringDraw(CONFIG_STRS[((g_GameWork.config.extraViewCtrl != 0) ? 32 : 28) >> 2], 10);
                break;

            case ExtraOptionsMenuEntry_RetreatTurn:
                strPosX = !g_GameWork.config.extraRetreatTurn ? 210 : 206;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_RetreatTurn));
                Gfx_StringDraw(CONFIG_STRS[((g_GameWork.config.extraRetreatTurn != 0) ? 32 : 28) >> 2], 10);
                break;

            case ExtraOptionsMenuEntry_MovementCtrl:
                strPosX = !g_GameWork.config.extraWalkRunCtrl ? 210 : 206;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_MovementCtrl));
                Gfx_StringDraw(CONFIG_STRS[((g_GameWork.config.extraWalkRunCtrl != 0) ? 32 : 28) >> 2], 10);
                break;

            case ExtraOptionsMenuEntry_AutoAiming:
                strPosX = !g_GameWork.config.extraAutoAiming ? 228 : 226;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_AutoAiming));
                Gfx_StringDraw(CONFIG_STRS[((g_GameWork.config.extraAutoAiming != 0) ? 40 : 36) >> 2], 10);
                break;

            case ExtraOptionsMenuEntry_ViewMode:
                strPosX = !g_GameWork.config.extraViewMode ? 210 : 200;
                Gfx_StringSetPosition(strPosX, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_ViewMode));
                Gfx_StringDraw(CONFIG_STRS[(g_GameWork.config.extraViewMode ? 48 : 44) >> 2], 10);
                break;

            case ExtraOptionsMenuEntry_BulletMult:
                Gfx_StringSetPosition(230, STR_BASE_Y + (STR_OFFSET_Y * ExtraOptionsMenuEntry_BulletMult));
                Gfx_StringDraw(CONFIG_STRS[g_GameWork.config.extraBulletAdjust + 13], 10);
                break;
        }
    }

    #undef STR_BASE_Y
    #undef STR_OFFSET_Y
}

void Options_MainOptionsMenu_ConfigDraw(void) // 0x801E4FFC
{
    const s_Triangle2d FRONT_ARROWS[] = {
        { { 40,  14 }, { 48,  6  }, { 48,  22 } },
        { { 96,  14 }, { 88,  6  }, { 88,  22 } },
        { { 40,  30 }, { 48,  22 }, { 48,  38 } },
        { { 96,  30 }, { 88,  22 }, { 88,  38 } },
        { { 19,  46 }, { 27,  38 }, { 27,  54 } },
        { { 124, 46 }, { 116, 38 }, { 116, 54 } },
        { { 12,  62 }, { 20,  54 }, { 20,  70 } },
        { { 131, 62 }, { 123, 54 }, { 123, 70 } },
        { { 12,  78 }, { 20,  70 }, { 20,  86 } },
        { { 131, 78 }, { 123, 70 }, { 123, 86 } }
#ifdef SH_PC_PORT
        ,
        { { 12,  94 }, { 20,  86 }, { 20,  102 } }, /* PC: FMV/voice left  */
        { { 131, 94 }, { 123, 86 }, { 123, 102 } }  /* PC: FMV/voice right */
#endif
    };

    const s_Triangle2d BACK_ARROWS[] = {
        { { 39,  14 }, { 49,  4  }, { 49,  24 } },
        { { 97,  14 }, { 87,  4  }, { 87,  24 } },
        { { 39,  30 }, { 49,  20 }, { 49,  40 } },
        { { 97,  30 }, { 87,  20 }, { 87,  40 } },
        { { 18,  46 }, { 28,  36 }, { 28,  56 } },
        { { 125, 46 }, { 115, 36 }, { 115, 56 } },
        { { 11,  62 }, { 21,  52 }, { 21,  72 } },
        { { 132, 62 }, { 122, 52 }, { 122, 72 } },
        { { 11,  78 }, { 21,  68 }, { 21,  88 } },
        { { 132, 78 }, { 122, 68 }, { 122, 88 } }
#ifdef SH_PC_PORT
        ,
        { { 11,  94 }, { 21,  84 }, { 21,  104 } }, /* PC: FMV/voice left  */
        { { 132, 94 }, { 122, 84 }, { 122, 104 } }  /* PC: FMV/voice right */
#endif
    };

    const char* CONFIG_STRS[] =
    {
        "On",
        "Off",
        "Stereo",
        "Monaural"
    };

    s32 strPosX;
    s32 strIdx;
    s32 i;

    Gfx_StringSetColor(StringColorId_White);

    // Draw left/right arrows for subset of options.
    if (g_MainOptionsMenu_SelectedEntry >= 4 && g_MainOptionsMenu_SelectedEntry < MainOptionsMenuEntry_Count)
    {
#ifdef SH_PC_PORT
        /* The Language row's value names are far wider than the On/Off the
         * arrow tables were sized for — pull its arrows outward so they
         * flank every language name instead of overlapping it. */
        s32 langDx = (g_MainOptionsMenu_SelectedEntry == MainOptionsMenuEntry_AutoLoad &&
                      Pc_LangMenuRowActive()) ? 26 : 0;

        #define ARROW_DRAW_DX(tris, idx, dx, flashing)              \
        {                                                           \
            s_Triangle2d shifted = (tris)[idx];                     \
            shifted.vertex0.vx  += (dx);                            \
            shifted.vertex1.vx  += (dx);                            \
            shifted.vertex2.vx  += (dx);                            \
            Options_Selection_ArrowDraw(&shifted, flashing, false); \
        }

        // Draw flashing left/right arrows.
        for (i = 0; i < 2; i++)
        {
            ARROW_DRAW_DX(FRONT_ARROWS, ((g_MainOptionsMenu_SelectedEntry - 4) * 2) + i,
                          (i == 0) ? -langDx : langDx, true);
        }

        // Draw border to highlight flashing left/right arrow corresponding to direction of UI navigation.
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickLeft)
        {
            ARROW_DRAW_DX(BACK_ARROWS, (g_MainOptionsMenu_SelectedEntry - 4) << 1, -langDx, false);
        }
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickRight)
        {
            ARROW_DRAW_DX(BACK_ARROWS, ((g_MainOptionsMenu_SelectedEntry - 4) << 1) + 1, langDx, false);
        }

        #undef ARROW_DRAW_DX
#else
        // Draw flashing left/right arrows.
        for (i = 0; i < 2; i++)
        {
            Options_Selection_ArrowDraw(&FRONT_ARROWS[(((g_MainOptionsMenu_SelectedEntry - 4) * 2) + i)], true, false);
        }

        // Draw border to highlight flashing left/right arrow corresponding to direction of UI navigation.
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickLeft)
        {
            Options_Selection_ArrowDraw(&BACK_ARROWS[(g_MainOptionsMenu_SelectedEntry - 4) << 1], false, false);
        }
        if (g_Controller0->heldBtnFlags & ControllerFlag_LStickRight)
        {
            Options_Selection_ArrowDraw(&BACK_ARROWS[((g_MainOptionsMenu_SelectedEntry - 4) << 1) + 1], false, false);
        }
#endif
    }

    for (i = 0; i < 3; i++)
    {
        switch (i)
        {
            case 0:
                strPosX = !g_GameWork.config.vibrationEnabled ? 214 : 216;
                Gfx_StringSetPosition(strPosX, 120);

                strIdx = !g_GameWork.config.vibrationEnabled;
                Gfx_StringDraw(CONFIG_STRS[strIdx], 10);
                break;

            case 1:
#ifdef SH_PC_PORT
                if (Pc_LangMenuRowActive())
                {
                    /* Names in the retail PAL option-menu order (= config
                     * language index). X nudged per word length like On/Off.
                     * Index 5+ are PC-side packs; their label is the pack's own
                     * `!menu` field (e.g. "POLISH"). */
                    static const char* const LANG_STRS[5]  = { "English", "German", "French", "Spanish", "Italian" };
                    static const u8          LANG_STR_X[5] = { 198, 204, 204, 198, 198 };
                    const char*              label;
                    int                      lx;

                    if (g_PcConfig.language >= LANG_PACK_FIRST)
                    {
                        label = Pc_LangPackName();
                        lx    = 200;
                    }
                    else
                    {
                        label = LANG_STRS[g_PcConfig.language];
                        lx    = LANG_STR_X[g_PcConfig.language];
                    }

                    Gfx_StringSetPosition(lx, 136);
                    Gfx_StringDraw(label, 10);
                    break;
                }
#endif
                strPosX = !g_GameWork.config.autoLoad ? 214 : 216;
                Gfx_StringSetPosition(strPosX, 136);

                strIdx = !g_GameWork.config.autoLoad;
                Gfx_StringDraw(CONFIG_STRS[strIdx], 10);
                break;

            case 2:
                strPosX = g_GameWork.config.soundType ? 194 : 206;
                Gfx_StringSetPosition(strPosX, 152);

                strIdx = g_GameWork.config.soundType + 2;
                Gfx_StringDraw(CONFIG_STRS[strIdx], 10);
                break;
        }
    }
}

// ========================================
// SCREEN POSITION OPTION SCREEN
// ========================================

void Options_ScreenPosMenu_Control(void) // 0x801E53A0
{
    #define OPT_SCREEN_POS_X_RANGE 11
    #define OPT_SCREEN_POS_Y_RANGE 8
    #define BG_FADE_STEP           16

    s32        i;
    s8         posX;
    TILE*      tile;
    PACKET*    packet;
    static s32 screenPosMenu_BackgroundFade;
    static s16 screenPosMenu_PositionX;
    static s16 screenPosMenu_PositionY;

    posX = g_GameWorkConst->config.screenPositionX;
    if (posX != screenPosMenu_PositionX || g_GameWorkConst->config.screenPositionY != screenPosMenu_PositionY)
    {
        Screen_XyPositionSet(posX, g_GameWorkConst->config.screenPositionY);
    }

    screenPosMenu_PositionX = g_GameWorkConst->config.screenPositionX;
    screenPosMenu_PositionY = g_GameWorkConst->config.screenPositionY;

    switch (g_GameWork.gameStateSteps[1])
    {
        case ScreenPosMenuState_0:
            ScreenFade_Start(true, true, false);
            screenPosMenu_BackgroundFade  = 255;
            g_GameWork.gameStateSteps[1] = ScreenPosMenuState_1;
            g_GameWork.gameStateSteps[2] = 0;

        case ScreenPosMenuState_1:
            g_GameWork.gameStateSteps[2] = 0;
            g_GameWork.gameStateSteps[1]++;
            break;

        case ScreenPosMenuState_2:
            // Set config.
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickUp)
            {
                g_GameWorkConst->config.screenPositionY--;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickDown)
            {
                g_GameWorkConst->config.screenPositionY++;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
            {
                g_GameWorkConst->config.screenPositionX--;
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
            {
                g_GameWorkConst->config.screenPositionX++;
            }
            g_GameWorkConst->config.screenPositionX = CLAMP(g_GameWorkConst->config.screenPositionX, -OPT_SCREEN_POS_X_RANGE, OPT_SCREEN_POS_X_RANGE);
            g_GameWorkConst->config.screenPositionY = CLAMP(g_GameWorkConst->config.screenPositionY, -OPT_SCREEN_POS_Y_RANGE, OPT_SCREEN_POS_Y_RANGE);

            // Play sound.
            if (g_GameWorkConst->config.screenPositionX != screenPosMenu_PositionX ||
                g_GameWorkConst->config.screenPositionY != screenPosMenu_PositionY)
            {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            }

            // Start background color fade.
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
            {
                if (screenPosMenu_BackgroundFade == 255)
                {
                    screenPosMenu_BackgroundFade       = 0;
                    g_ScreenPosMenu_InvertBackgroundFade = (g_ScreenPosMenu_InvertBackgroundFade + 1) & (1 << 0);
                }
            }

            // Leave menu.
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
            {
                Sd_PlaySfx(Sfx_MenuCancel, 0, 64);

                ScreenFade_Start(true, false, false);
                g_GameWork.gameStateSteps[2] = 0;
                g_GameWork.gameStateSteps[1]++;
            }
            break;

        case ScreenPosMenuState_Leave:
            // Switch to previous menu.
            if (ScreenFade_IsFinished())
            {
                ScreenFade_Start(true, true, false);
                g_GameWork.gameStateSteps[0]    = OptionsMenuState_LeaveScreenPos;
                g_SysWork.counters_1C[1]                 = 0;
                g_GameWork.gameStateSteps[1]    = 0;
                g_GameWork.gameStateSteps[2]    = 0;
                g_GameWork.background2dColor.r = 0;
                g_GameWork.background2dColor.g = 0;
                g_GameWork.background2dColor.b = 0;
                return;
            }
            break;
    }

    // Update background fade.
    screenPosMenu_BackgroundFade += BG_FADE_STEP;
    screenPosMenu_BackgroundFade  = CLAMP(screenPosMenu_BackgroundFade, 0, 255);
    switch (g_ScreenPosMenu_InvertBackgroundFade)
    {
        case false:
            g_GameWork.background2dColor.r = ~screenPosMenu_BackgroundFade;
            g_GameWork.background2dColor.g = ~screenPosMenu_BackgroundFade;
            g_GameWork.background2dColor.b = ~screenPosMenu_BackgroundFade;
            break;

        case true:
            g_GameWork.background2dColor.r = screenPosMenu_BackgroundFade;
            g_GameWork.background2dColor.g = screenPosMenu_BackgroundFade;
            g_GameWork.background2dColor.b = screenPosMenu_BackgroundFade;
            break;
    }

    packet = GsOUT_PACKET_P;

    for (i = 0; i < 6; i++)
    {
        tile = (TILE*)packet;

        setTile(tile);

        if (g_ScreenPosMenu_InvertBackgroundFade == 0)
        {
            setRGB0(tile, 0xFF, 0, 0);
        }
        else if (g_ScreenPosMenu_InvertBackgroundFade == 1)
        {
            setRGB0(tile, 0xFF, 0, 0);
        }

        if (i < 3)
        {
            setXY0(tile, -160, -97 + (96 * i));
            setWH(tile, SCREEN_WIDTH, 2);
        }
        else
        {
            setXY0(tile, -577 + (144 * i), -120);
            setWH(tile, 2, SCREEN_HEIGHT);
        }

        addPrim(g_OrderingTable0[g_ActiveBufferIdx].org, (TILE*)packet);
        packet += sizeof(TILE);
    }

    for (i = 0; i < 28; i++)
    {
        tile = (TILE*)packet;

        setTile(tile);

        switch (g_ScreenPosMenu_InvertBackgroundFade)
        {
            case 0:
                setRGB0(tile, screenPosMenu_BackgroundFade, screenPosMenu_BackgroundFade, screenPosMenu_BackgroundFade);
                break;

            case 1:
                setRGB0(tile, ~screenPosMenu_BackgroundFade, ~screenPosMenu_BackgroundFade, ~screenPosMenu_BackgroundFade);
                break;
        }

        if (i < 11)
        {
            setXY0(tile, -160, -81 + (16 * i));
            setWH(tile, SCREEN_WIDTH, 2);
        }
        else
        {
            setXY0(tile, -305 + (16 * i), -120);
            setWH(tile, 2, SCREEN_HEIGHT);
        }

        addPrim(g_OrderingTable0[g_ActiveBufferIdx].org, (TILE*)packet);
        packet += sizeof(TILE);
    }

    GsOUT_PACKET_P = packet;

    Options_ScreenPosMenu_ArrowsDraw();
    Options_ScreenPosMenu_ConfigDraw();

    #undef OPT_SCREEN_POS_X_RANGE
    #undef OPT_SCREEN_POS_Y_RANGE
    #undef BG_FADE_STEP
}

void Options_ScreenPosMenu_ArrowsDraw(void) // 0x801E5A08
{
    #define DIR_COUNT 4

    const s_Triangle2d FRONT_ARROWS[] = {
        { {  0,  -100 }, { -8,   -92 }, {  8,  -92 } },
        { {  0,   100 }, { -8,    92 }, {  8,   92 } },
        { { -148, 0   }, { -140, -8  }, { -140, 8  } },
        { {  148, 0   }, {  140, -8  }, {  140, 8  } }
    };

    const s_Triangle2d BACK_ARROWS[] = {
        { {  0,  -101 }, { -10,  -91 }, {  9,  -91 } },
        { {  0,   101 }, { -10,   91 }, {  9,   91 } },
        { { -149, 0   }, { -139, -10 }, { -139, 10 } },
        { {  149, 0   }, {  139, -10 }, {  139, 10 } }
    };

    u8  dirs[DIR_COUNT]; // Booleans.
    s32 i;

    // Clear directions array for no reason.
    memset(dirs, 0, DIR_COUNT);

    // Draw flashing up/down/left/right arrows.
    for (i = 0; i < DIR_COUNT; i++)
    {
        Options_Selection_ArrowDraw(&FRONT_ARROWS[i], true, false);
    }

    if ((g_Controller0->clickedBtnFlags & ControllerFlag_LStickUp) ||
        (g_Controller0->heldBtnFlags     & ControllerFlag_LStickUp))
    {
        dirs[0] = true;
    }
    if ((g_Controller0->clickedBtnFlags & ControllerFlag_LStickDown) ||
        (g_Controller0->heldBtnFlags     & ControllerFlag_LStickDown))
    {
        dirs[1] = true;
    }
    if ((g_Controller0->clickedBtnFlags & ControllerFlag_LStickLeft) ||
        (g_Controller0->heldBtnFlags     & ControllerFlag_LStickLeft))
    {
        dirs[2] = true;
    }
    if ((g_Controller0->clickedBtnFlags & ControllerFlag_LStickRight) ||
        (g_Controller0->heldBtnFlags     & ControllerFlag_LStickRight))
    {
        dirs[3] = true;
    }

    // Draw border to highlight flashing up/down/left/right arrow corresponding to direction of UI navigation.
    for (i = 0; i < DIR_COUNT; i++)
    {
        if (dirs[i])
        {
            Options_Selection_ArrowDraw(&BACK_ARROWS[i], false, false);
        }
    }

    #undef DIR_COUNT
}

void Options_ScreenPosMenu_ConfigDraw(void) // 0x801E5CBC
{
    s32      i;
    LINE_F2* line;
    POLY_F4* poly;
    GsOT*    ot = &g_OrderingTable2[g_ActiveBufferIdx];

    const DVECTOR LINE_BASES[] = {
        { -60, 40 },
        { -60, 70 },
        { 60, 70 },
        { 60, 40 }
    };

    const char* AXIS_OFFSET_STRS[] = {
        "X:_",
        "Y:_"
    };

    for (i = 0; i < 4; i++)
    {
        line = (LINE_F2*)GsOUT_PACKET_P;

        setLineF2(line);
        setCodeWord(line, 0x40, (i < 2) ? COLOR_RGBC(240, 240, 240, 0) : COLOR_RGBC(128, 128, 128, 0));

        setXY0Fast(line, (u16)(LINE_BASES[i].vx             - g_GameWorkConst->config.screenPositionX), LINE_BASES[i].vy             - g_GameWorkConst->config.screenPositionY);
        setXY1Fast(line, (u16)(LINE_BASES[(i + 1) & 0x3].vx - g_GameWorkConst->config.screenPositionX), LINE_BASES[(i + 1) & 0x3].vy - g_GameWorkConst->config.screenPositionY);

        addPrim((u8*)ot->org + LAYER_40, line);
        GsOUT_PACKET_P = (u8*)line + sizeof(LINE_F2);
    }

    poly = (POLY_F4*)GsOUT_PACKET_P;
    setPolyF4(poly);

    setCodeWord(poly, 40, 0);
    setXY0Fast(poly, (u16)(LINE_BASES[0].vx - g_GameWorkConst->config.screenPositionX), LINE_BASES[0].vy - g_GameWorkConst->config.screenPositionY);
    setXY1Fast(poly, (u16)(LINE_BASES[1].vx - g_GameWorkConst->config.screenPositionX), LINE_BASES[1].vy - g_GameWorkConst->config.screenPositionY);
    setXY2Fast(poly, (u16)(LINE_BASES[3].vx - g_GameWorkConst->config.screenPositionX), LINE_BASES[3].vy - g_GameWorkConst->config.screenPositionY);
    setXY3Fast(poly, (u16)(LINE_BASES[2].vx - g_GameWorkConst->config.screenPositionX), LINE_BASES[2].vy - g_GameWorkConst->config.screenPositionY);

    addPrim((u8*)ot->org + LAYER_40, poly);
    GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_F4);

    Gfx_StringSetPosition(108 - g_GameWorkConst->config.screenPositionX, 162 - g_GameWorkConst->config.screenPositionY);
    Gfx_StringDraw(AXIS_OFFSET_STRS[0], 10);
    Gfx_StringDrawInt(3, g_GameWorkConst->config.screenPositionX);

    Gfx_StringSetPosition(168 - g_GameWorkConst->config.screenPositionX, 162 - g_GameWorkConst->config.screenPositionY);
    Gfx_StringDraw(AXIS_OFFSET_STRS[1], 10);
    Gfx_StringDrawInt(3, g_GameWorkConst->config.screenPositionY);
}

// ========================================
// BRIGHTNESS OPTION SCREEN
// ========================================

#ifdef SH_PC_PORT
/* The Brightness screen becomes a three-row image-adjust panel on PC:
 * Brightness / Contrast / Saturation, each driving a g_PcConfig float and the
 * matching live renderer global, saved to config on change. A procedural
 * reference bar (g_cfg_calibBar, drawn in the post shader) sits behind it. */
static s32 g_PcBrtRow = 0; /* 0 = Brightness, 1 = Contrast, 2 = Saturation */

static void Pc_BrightnessRowAdjust(s32 row, int dir)
{
    extern float g_cfg_brightness, g_cfg_contrast, g_cfg_saturation;
    struct { float* cfg; float* live; float mn, mx, step; const char* key; } R[3] = {
        { &g_PcConfig.brightness, &g_cfg_brightness, 0.25f, 2.0f, 0.05f, "brightness" },
        { &g_PcConfig.contrast,   &g_cfg_contrast,   0.5f,  2.0f, 0.05f, "contrast"   },
        { &g_PcConfig.saturation, &g_cfg_saturation, 0.0f,  2.0f, 0.05f, "saturation" },
    };
    float v;

    if (row < 0 || row > 2 || dir == 0)
        return;

    v = *R[row].cfg + (float)dir * R[row].step;
    if (v < R[row].mn) v = R[row].mn;
    if (v > R[row].mx) v = R[row].mx;

    *R[row].cfg  = v;
    *R[row].live = v;
    { char b[16]; snprintf(b, sizeof(b), "%.3f", v); PcConfig_SaveKeyValue(R[row].key, b); }
    Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
}
#endif

void Options_BrightnessMenu_Control(void) // 0x801E6018
{
#ifdef SH_PC_PORT
    /* Show the procedural reference bar while this screen is up. Cleared in the
     * Leave state below. */
    { extern int g_cfg_calibBar; g_cfg_calibBar = 1; }
#endif
    // Handle menu state.
    switch (g_GameWork.gameStateSteps[1])
    {
        case BrightnessMenuState_0:
            // Entry.
            g_GameWork.gameStateSteps[1] = BrightnessMenuState_1;
            g_GameWork.gameStateSteps[2] = 0;
            break;

        case BrightnessMenuState_1:
            // Set fade.
            ScreenFade_Start(true, true, false);
            g_GameWork.gameStateSteps[1] = BrightnessMenuState_2;
            g_GameWork.gameStateSteps[2] = 0;
            break;

        case BrightnessMenuState_2:
#ifdef SH_PC_PORT
            /* Up/Down select a row (Brightness/Contrast/Saturation), Left/Right
             * adjust it. Mouse: wheel adjusts the selected row; hovering a row
             * band selects it; left-click the arrow columns adjusts; right-click
             * leaves. Rows drawn from y 74 at 16px pitch (see ConfigDraw). */
            {
                int mx, my;

                if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickUp)
                {
                    g_PcBrtRow = (g_PcBrtRow + 2) % 3;
                    Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
                }
                if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickDown)
                {
                    g_PcBrtRow = (g_PcBrtRow + 1) % 3;
                    Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
                }
                if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
                    Pc_BrightnessRowAdjust(g_PcBrtRow, -1);
                if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
                    Pc_BrightnessRowAdjust(g_PcBrtRow, +1);

                if (Pc_MouseCursor_UiPos(&mx, &my))
                {
                    int wheel = Pc_MouseCursor_WheelStep();

                    /* Wheel adjusts the selected row; left of centre lowers, right
                     * raises it; right-click leaves. Kept geometry-free (no
                     * per-row hit-test) — Up/Down selects the row. */
                    if (wheel != 0)
                        Pc_BrightnessRowAdjust(g_PcBrtRow, wheel);
                    else if (Pc_MouseCursor_LeftClicked())
                        Pc_BrightnessRowAdjust(g_PcBrtRow, (mx < 160) ? -1 : +1);

                    if (Pc_MouseCursor_RightClicked())
                        PcMouse_InjectCancel();
                }
            }
#else
            // Set config.
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickLeft)
            {
                if (g_GameWork.config.brightness != 0)
                {
                    g_GameWork.config.brightness--;
                    Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
                }
            }
            if (g_Controller0->pulsedBtnFlags & ControllerFlag_LStickRight)
            {
                if (g_GameWork.config.brightness < 7)
                {
                    g_GameWork.config.brightness++;
                    Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
                }
            }
#endif

            // Fade screen and leave menu.
            if (g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.enter |
                                                 g_GameWorkPtr->config.controllerConfig.cancel))
            {
                if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
                {
                    Sd_PlaySfx(Sfx_MenuConfirm, 0, Q8(0.25f));
                }
                else
                {
                    Sd_PlaySfx(Sfx_MenuCancel, 0, Q8(0.25f));
                }

                ScreenFade_Start(true, false, false);
                g_GameWork.gameStateSteps[1]++;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case BrightnessMenuState_Leave:
#ifdef SH_PC_PORT
            { extern int g_cfg_calibBar; g_cfg_calibBar = 0; }
#endif
            // Switch to previous menu.
            // TODO: Odd check for `ScreenFade_IsFinished()`.
            if ( (g_Screen_FadeStatus & (1 << 2)) &&
                !(g_Screen_FadeStatus & (1 << 1)) &&
                 (g_Screen_FadeStatus & (1 << 0)))
            {
                ScreenFade_Start(true, true, false);
                g_GameWork.gameStateSteps[0]   = OptionsMenuState_LeaveBrightness;
                g_SysWork.counters_1C[1]       = 0;
                g_GameWork.gameStateSteps[1]   = 0;
                g_GameWork.gameStateSteps[2]   = 0;
                g_GameWork.background2dColor.r = 0;
                g_GameWork.background2dColor.g = 0;
                g_GameWork.background2dColor.b = 0;
            }
            break;
    }

    // Draw graphics.
    if (g_GameWork.gameStatePrev == GameState_MainMenu)
    {
        Screen_BackgroundImgDraw(&g_BrightnessScreenImg0);
    }
    else
    {
        Screen_BackgroundImgDraw(&g_BrightnessScreenImg1);
    }

    Options_BrightnessMenu_LinesDraw(g_GameWork.config.brightness);
    Options_BrightnessMenu_ArrowsDraw();
    Options_BrightnessMenu_ConfigDraw();
#ifdef SH_PC_PORT
    Pc_MouseCursor_Draw();
#endif
}

void Options_BrightnessMenu_ConfigDraw(void) // 0x801E6238
{
#ifdef SH_PC_PORT
    /* Three rows: Brightness / Contrast / Saturation, values shown as a
     * percentage (100 = neutral). The selected row is gold, the highlight
     * bracket marks it. Values read from the live renderer globals so the text
     * always matches what is applied. */
    extern float g_cfg_brightness, g_cfg_contrast, g_cfg_saturation;
    const char* const NAMES[3] = { "BRIGHTNESS_", "CONTRAST___", "SATURATION_" };
    const float*      vals[3];
    s32 i;

    vals[0] = &g_cfg_brightness;
    vals[1] = &g_cfg_contrast;
    vals[2] = &g_cfg_saturation;

    for (i = 0; i < 3; i++)
    {
        /* Kept above the bottom reference bar (bar occupies ~the bottom 15%).
         * Selection shown by gold vs white — no bracket glyph (the menu font
         * renders '[' as a curly quote). */
        Gfx_StringSetColor(i == g_PcBrtRow ? StringColorId_Gold : StringColorId_White);
        Gfx_StringSetPosition(SCREEN_POSITION_X(22.0f), SCREEN_POSITION_Y(52.0f + (float)i * 8.0f));
        Gfx_StringDraw(NAMES[i], 20);
        Gfx_StringDrawInt(3, (s32)((*vals[i] * 100.0f) + 0.5f));
    }
#else
    const char* LEVEL_STR = "LEVEL_________";

    Gfx_StringSetColor(StringColorId_White);
    Gfx_StringSetPosition(SCREEN_POSITION_X(25.0f), SCREEN_POSITION_Y(79.5f));
    Gfx_StringDraw(LEVEL_STR, 20);
    Gfx_StringDrawInt(1, g_GameWork.config.brightness);
#endif
}

void Options_BrightnessMenu_ArrowsDraw(void) // 0x801E628C
{
#ifdef SH_PC_PORT
    /* The knobs must flank the SELECTED row's value, not sit at a fixed y=84
     * (which now lands on the reference bar as "ears"). ConfigDraw draws each
     * row at SCREEN_POSITION_Y(52 + 8*row) in screen space; the arrow prims draw
     * in centre-origin space, so subtract the 112 draw-offset (+5 to centre the
     * knob on the text line, matching the original's 84 vs its 79.5% value row).
     * X (8/64) already brackets the value column. */
    s32 yc  = (s32)(SCREEN_HEIGHT * ((52.0f + (float)g_PcBrtRow * 8.0f) / 100.0f)) - 107;
    /* Shift both knobs right so the left one clears the label and they bracket
     * the value number (labels are longer than the retail "LEVEL"). */
    const s32 xo = 14;
    s32 dir = (g_Controller0->heldBtnFlags & ControllerFlag_LStickLeft)  ? 1 :
              (g_Controller0->heldBtnFlags & ControllerFlag_LStickRight) ? 2 : 0;
    const s_Triangle2d FRONT_ARROWS[2] = {
        { { 8 + xo,  yc }, { 16 + xo, yc - 8 }, { 16 + xo, yc + 8 } },
        { { 64 + xo, yc }, { 56 + xo, yc - 8 }, { 56 + xo, yc + 8 } }
    };
    const s_Triangle2d BORDER_ARROWS[2] = {
        { { 7 + xo,  yc }, { 17 + xo, yc - 10 }, { 17 + xo, yc + 10 } },
        { { 65 + xo, yc }, { 55 + xo, yc - 10 }, { 55 + xo, yc + 10 } }
    };
    s32 i;

    for (i = 0; i < 2; i++)
        Options_Selection_ArrowDraw(&FRONT_ARROWS[i], true, false);

    /* Border highlights the side being pushed; dir 0 would read [-1] (OOB), so
     * only draw when a direction is held. */
    if (dir != 0)
        Options_Selection_ArrowDraw(&BORDER_ARROWS[dir - 1], false, false);
#else
    const s_Triangle2d FRONT_ARROWS[] = {
        { { 8, 84  }, { 16, 76 }, { 16, 92 } },
        { { 64, 84 }, { 56, 76 }, { 56, 92 } }
    };

    const s_Triangle2d BORDER_ARROWS[] = {
        { { 7, 84  }, { 17, 74 }, { 17, 94 } },
        { { 65, 84 }, { 55, 74 }, { 55, 94 } }
    };

    s32 btnInput;
    s32 i;
    s32 dir;

    // Determine UI movement direction.
    btnInput = g_Controller0->heldBtnFlags;
    if (btnInput & ControllerFlag_LStickLeft)
    {
        dir = 1;
    }
    else if (btnInput & ControllerFlag_LStickRight)
    {
        dir = 2;
    }
    else
    {
        dir = 0;
    }

    // Draw flashing left/right arrows.
    for (i = 0; i < 2; i++)
    {
        Options_Selection_ArrowDraw(&FRONT_ARROWS[i], true, false);
    }

    // Draw border to highlight flashing left/right arrow corresponding to direction of UI navigation.
    for (i = dir - 1; i < dir; i++)
    {
        Options_Selection_ArrowDraw(&BORDER_ARROWS[i], false, false);
    }
#endif
}

// ========================================
// DRAW OPTIONS FEATURES SCREEN
// ========================================

void Options_Selection_HighlightDraw(const s_Line2d* line, bool hasShadow, bool invertGradient) // 0x801E641C
{
    #define STR_OFFSET_Y 16

    s_Line2d* localLine;
    LINE_G2*  linePrim;
    POLY_G4*  poly;
    GsOT*     ot;

    ot        = &g_OrderingTable2[g_ActiveBufferIdx];
    linePrim  = (LINE_G2*)GsOUT_PACKET_P;
    localLine = line;

    // Draw underline.
    setLineG2(linePrim);

    // @unused Non-functioning line color gradient inversion? Gradient is unchanged regardless of `invertGradient`'s value,
    // which itself is always passed as `false`. Purpose is guessed based on a similar parameter in `Options_Selection_BulletPointDraw`.
    if (invertGradient)
    {
        setRGBC0(linePrim, 176, 176, 176, PRIM_LINE | RECT_SIZE_8);
        setRGBC1(linePrim, 160, 128, 64,  PRIM_LINE | RECT_SIZE_8);
    }
    else
    {
        setRGBC0(linePrim, 176, 176, 176, PRIM_LINE | RECT_SIZE_8);
        setRGBC1(linePrim, 160, 128, 64,  PRIM_LINE | RECT_SIZE_8);
    }

    setXY0Fast(linePrim, localLine->vertex0.vx, localLine->vertex0.vy);
    setXY1Fast(linePrim, localLine->vertex1.vx, localLine->vertex1.vy);
    addPrim((u8*)ot->org + (hasShadow ? LAYER_36 : LAYER_24), linePrim);
    GsOUT_PACKET_P = (u8*)linePrim + sizeof(LINE_G2);

    // Draw shadow gradient.
    if (hasShadow)
    {
        poly = (POLY_G4*)GsOUT_PACKET_P;
        setPolyG4(poly);
        setSemiTrans(poly, true);
        setRGB0(poly, 0, 0, 0);
        setRGB1(poly, Q8_COLOR(0.375f), Q8_COLOR(0.375f), Q8_COLOR(0.375f));
        setRGB2(poly, 0, 0, 0);
        setRGB3(poly, Q8_COLOR(0.375f), Q8_COLOR(0.375f), Q8_COLOR(0.375f));
        setXY4(poly,
               localLine->vertex0.vx, localLine->vertex0.vy - STR_OFFSET_Y, localLine->vertex0.vx, localLine->vertex0.vy,
               localLine->vertex1.vx, localLine->vertex1.vy - STR_OFFSET_Y, localLine->vertex1.vx, localLine->vertex1.vy);
        addPrim((u8*)ot->org + LAYER_36, poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G4);

        Gfx_Primitive2dTextureSet(0, 0, 9, 2);
    }

    #undef STR_OFFSET_Y
}

void Options_Selection_ArrowDraw(const s_Triangle2d* arrow, bool isFlashing, bool resetColor) // 0x801E662C
{
    s32      colorFade;
    s32      colorStart;
    s32      colorEnd;
    POLY_G3* arrowPoly;
    GsOT*    ot;

    ot = &g_OrderingTable2[g_ActiveBufferIdx];

    // @unused `resetColor` doesn't serve any meaningful purpose.
    if (resetColor)
    {
        colorEnd   = 0;
        colorStart = 0;
    }

    colorFade = g_SysWork.counters_1C[0] & 0x7F;

    // Fade start color.
    if (colorFade >= 32)
    {
        colorStart = 32;
        if (colorFade < 64)
        {
            colorStart = 32;
        }
        else if (colorFade < 96)
        {
            colorStart = 96 - colorFade;
        }
        else
        {
            colorStart = 0;
        }
    }
    else
    {
        colorStart = colorFade;
    }

    // Fade end color.
    if (colorFade >= 32)
    {
        if (colorFade < 64)
        {
            colorEnd = colorFade - 32;
        }
        else if (colorFade >= 96)
        {
            colorEnd = 128 - colorFade;
        }
        else
        {
            colorEnd = 32;
        }
    }
    else
    {
        colorEnd = 0;
    }

    // Draw blue arrow.
    arrowPoly = (POLY_G3*)GsOUT_PACKET_P;
    setPolyG3(arrowPoly);

    // Flash color from blue to cyan.
    if (isFlashing)
    {
        // Base color is blue. `* 0x700` Shifts green component into place.
        *((u32*)&arrowPoly->r0) = (colorEnd   * 0x700) + COLOR_RGBC(0, 0, 255, PRIM_POLY | RECT_SIZE_8);
        *((u32*)&arrowPoly->r1) = (colorStart * 0x700) + COLOR_RGBC(0, 0, 255, PRIM_POLY | RECT_SIZE_8);
        *((u32*)&arrowPoly->r2) = (colorStart * 0x700) + COLOR_RGBC(0, 0, 255, PRIM_POLY | RECT_SIZE_8);
    }
    // Set solid cyan color.
    else
    {
        setRGBC0(arrowPoly, 0, 240, 240, PRIM_POLY | RECT_SIZE_8);
        setRGBC1(arrowPoly, 0, 240, 240, PRIM_POLY | RECT_SIZE_8);
        setRGBC2(arrowPoly, 0, 240, 240, PRIM_POLY | RECT_SIZE_8);
    }

    setXY0Fast(arrowPoly, arrow->vertex0.vx, arrow->vertex0.vy);
    setXY1Fast(arrowPoly, arrow->vertex1.vx, arrow->vertex1.vy);
    setXY2Fast(arrowPoly, arrow->vertex2.vx, arrow->vertex2.vy);
    addPrim((u8*)ot->org + LAYER_40, arrowPoly);
    GsOUT_PACKET_P = (u8*)arrowPoly + sizeof(POLY_G3);
}

void Options_Selection_BulletPointDraw(const s_Quad2d* quad, bool isBorder, bool isInactive) // 0x801E67B0
{
    #define TRI_COUNT 2

    GsOT*    ot = &g_OrderingTable2[g_ActiveBufferIdx];
    s32      i;
    POLY_G3* poly;

    // Draw quad as triangles to achieve diagonal gradient.
    for (i = 0; i < TRI_COUNT; i++)
    {
        poly = (POLY_G3*)GsOUT_PACKET_P;
        setPolyG3(poly);

        if (isBorder)
        {
            // Set color.
            switch (isInactive)
            {
                case false:
                    setRGBC0(poly, 255, 255, 255, PRIM_POLY | RECT_SIZE_8);
                    setRGBC1(poly, 160, 128, 64,  PRIM_POLY | RECT_SIZE_8);
                    setRGBC2(poly, 64,  64,  64,  PRIM_POLY | RECT_SIZE_8);
                    break;

                case true:
                    setRGBC0(poly, 128, 128, 128, PRIM_POLY | RECT_SIZE_8);
                    setRGBC1(poly, 40,  32,  16,  PRIM_POLY | RECT_SIZE_8);
                    setRGBC2(poly, 16,  16,  16,  PRIM_POLY | RECT_SIZE_8);
                    break;
            }

            // Draw triangle.
            if (i != 0)
            {
                setXY0Fast(poly, quad->vertex0.vx, quad->vertex0.vy);
                setXY1Fast(poly, quad->vertex1.vx, quad->vertex1.vy);
                setXY2Fast(poly, quad->vertex3.vx, quad->vertex3.vy);
            }
            else
            {
                setXY0Fast(poly, quad->vertex0.vx, quad->vertex0.vy);
                setXY1Fast(poly, quad->vertex2.vx, quad->vertex2.vy);
                setXY2Fast(poly, quad->vertex3.vx, quad->vertex3.vy);
            }
        }
        else
        {
            // Set color.
            switch (isInactive)
            {
                case false:
                    setRGBC0(poly, 160, 128, 64,  PRIM_POLY | RECT_SIZE_8);
                    setRGBC1(poly, 255, 255, 255, PRIM_POLY | RECT_SIZE_8);
                    setRGBC2(poly, 255, 255, 255, PRIM_POLY | RECT_SIZE_8);
                    break;

                case true:
                    setRGBC0(poly, 80,  64,  32,  PRIM_POLY | RECT_SIZE_8);
                    setRGBC1(poly, 160, 160, 160, PRIM_POLY | RECT_SIZE_8);
                    setRGBC2(poly, 160, 160, 160, PRIM_POLY | RECT_SIZE_8);
                    break;
            }

            // Draw triangle.
            if (i != 0)
            {
                setXY0Fast(poly, quad->vertex3.vx, quad->vertex3.vy);
                setXY1Fast(poly, quad->vertex1.vx, quad->vertex1.vy);
                setXY2Fast(poly, quad->vertex2.vx, quad->vertex2.vy);
            }
            else
            {
                setXY0Fast(poly, quad->vertex0.vx, quad->vertex0.vy);
                setXY1Fast(poly, quad->vertex1.vx, quad->vertex1.vy);
                setXY2Fast(poly, quad->vertex2.vx, quad->vertex2.vy);
            }
        }

        addPrim((u8*)ot->org + LAYER_24, poly);
        GsOUT_PACKET_P = (u8*)poly + sizeof(POLY_G3);
    }

    #undef TRI_COUNT
}

// ========================================
// CONTROLS OPTION SCREEN
// ========================================

void Options_ControllerMenu_Control(void) // 0x801E69BC
{
    s32                                     boundActionIdx = NO_VALUE;
    e_InputAction                           actionIdx;
    static s_ControllerMenu_SelectedEntries selectedEntries;

#ifdef SH_PC_PORT
    /* Mouse: hover selects in both panes; click confirms ONLY in the presets
     * pane (EXIT applies enter, TYPE rows apply the preset). In the actions
     * pane hover-select is all we do — while that state is live,
     * Options_ControllerMenu_ConfigUpdate BINDS any clicked button to the
     * hovered action, so injected enter/cancel bits would rebind instead of
     * confirm. Rebinding stays a keyboard/pad press, as retail intends. */
    {
        int mx, my;
        s32 st = g_GameWork.gameStateSteps[1];

        if ((st == ControllerMenuState_Exit  || st == ControllerMenuState_Type1 ||
             st == ControllerMenuState_Type2 || st == ControllerMenuState_Type3 ||
             st == ControllerMenuState_Actions) &&
            Pc_MouseCursor_UiPos(&mx, &my))
        {
            if (mx < 92) /* presets pane: rows at y = 22 + i*20 */
            {
                s32 row = -1, i;

                for (i = 0; i < ControllerMenuState_Count; i++)
                {
                    s32 top = 22 + (i * 20) - 3;
                    if (my >= top && my < top + 20) { row = i; break; }
                }
                if (row >= 0)
                {
                    if (Pc_MouseCursor_Moved() && st != row)
                    {
                        g_GameWork.gameStateSteps[1] = row;
                        g_GameWork.gameStateSteps[2] = 0;
                        SD_Call(Sfx_MenuMove);
                    }
                    if (Pc_MouseCursor_LeftClicked() && st == row)
                        PcMouse_InjectEnter();
                    if (Pc_MouseCursor_RightClicked() && st == row && st != ControllerMenuState_Exit)
                        PcMouse_InjectCancel();
                }
            }
            else /* actions pane: rows at y = 22, pitch 12, extra gap after SKIP */
            {
                s32 row = -1, i, y = 22;

                for (i = 0; i < InputAction_Count; i++)
                {
                    if (my >= y - 2 && my < y + 10) { row = i; break; }
                    y += 12 + ((i == 2) ? 12 : 0);
                }
                if (row >= 0 && Pc_MouseCursor_Moved())
                {
                    if (st != ControllerMenuState_Actions)
                    {
                        selectedEntries.preset       = st;
                        g_GameWork.gameStateSteps[1] = ControllerMenuState_Actions;
                        g_GameWork.gameStateSteps[2] = 0;
                    }
                    if (selectedEntries.action != (e_InputAction)row)
                    {
                        selectedEntries.action = (e_InputAction)row;
                        SD_Call(Sfx_MenuMove);
                    }
                }
            }
        }
        Pc_MouseCursor_Draw();
    }
#endif

    // Handle controller config menu state.
    switch (g_GameWork.gameStateSteps[1])
    {
        case ControllerMenuState_Exit:
            ScreenFade_Start(false, true, false);
            selectedEntries.preset = ControllerMenuState_Exit;

            // Leave menu.
            if (g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.enter |
                                                 g_GameWorkPtr->config.controllerConfig.cancel))
            {
                SD_Call(Sfx_MenuCancel);

                ScreenFade_Start(false, false, false);
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Leave;
                g_GameWork.gameStateSteps[2] = 0;
                break;
            }

            // Move selection cursor up/down.
            if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickUp)
            {
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Type3;
                g_GameWork.gameStateSteps[2] = 0;
            }
            else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickDown)
            {
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Type1;
                g_GameWork.gameStateSteps[2] = 0;
            }
            // Move selection cursor left/right.
            else if (g_Controller0->pulsedGuiBtnFlags & (ControllerFlag_LStickLeft | ControllerFlag_LStickRight))
            {
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Actions;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case ControllerMenuState_Type1:
        case ControllerMenuState_Type2:
        case ControllerMenuState_Type3:
            selectedEntries.preset = g_GameWork.gameStateSteps[1];

            // Set binding preset.
            if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
            {
                SD_Call(Sfx_MenuConfirm);
                Settings_RestoreControlDefaults(g_GameWork.gameStateSteps[1] - 1);
            }
            // Reset selection cursor.
            else if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
            {
                SD_Call(Sfx_MenuCancel);
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Exit;
                g_GameWork.gameStateSteps[2] = 0;
            }
            // Move selection cursor.
            else
            {
                // Move selection cursor up/down.
                if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickUp)
                {
                    g_GameWork.gameStateSteps[1] = (g_GameWork.gameStateSteps[1] - 1) & 3;
                    g_GameWork.gameStateSteps[2] = 0;
                }
                else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickDown)
                {
                    g_GameWork.gameStateSteps[1] = (g_GameWork.gameStateSteps[1] + 1) & 3;
                    g_GameWork.gameStateSteps[2] = 0;
                }
                // Move selection cursor left/right.
                else if (g_Controller0->pulsedGuiBtnFlags & (ControllerFlag_LStickLeft | ControllerFlag_LStickRight))
                {
                    g_GameWork.gameStateSteps[1] = ControllerMenuState_Actions;
                    g_GameWork.gameStateSteps[2] = 0;
                }
            }
            break;

        case ControllerMenuState_Actions:
            actionIdx = selectedEntries.action;

            // Move selection cursor up/down.
            if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickUp)
            {
                if (actionIdx != InputAction_Enter)
                {
                    selectedEntries.action = actionIdx - 1;
                }
                else
                {
                    selectedEntries.action = InputAction_Option;
                }
            }
            else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickDown)
            {
                if (actionIdx != InputAction_Option)
                {
                    selectedEntries.action = actionIdx + 1;
                }
                else
                {
                    selectedEntries.action = InputAction_Enter;
                }
            }
            // Move selection cursor left/right.
            else if (g_Controller0->pulsedGuiBtnFlags & (ControllerFlag_LStickLeft | ControllerFlag_LStickRight))
            {
                g_GameWork.gameStateSteps[2] = 0;
                g_GameWork.gameStateSteps[1] = selectedEntries.preset;
            }
            // Bind button to input action.
            else
            {
                boundActionIdx = Options_ControllerMenu_ConfigUpdate(actionIdx);
            }
            break;

        case ControllerMenuState_Leave:
            // Switch to previous menu.
            if (ScreenFade_IsFinished())
            {
                ScreenFade_Start(true, true, false);
                g_GameWork.gameStateSteps[0] = OptionsMenuState_LeaveController;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;
    }

    if (g_GameWork.gameStateSteps[1] == ControllerMenuState_Actions)
    {
        g_ControllerMenu_IsOnActionsPane = true;
    }
    else
    {
        g_ControllerMenu_IsOnActionsPane = false;
    }

    // Play cursor navigation SFX.
    if (g_Controller0->pulsedGuiBtnFlags & (ControllerFlag_LStickUp    |
                                           ControllerFlag_LStickRight |
                                           ControllerFlag_LStickDown  |
                                           ControllerFlag_LStickLeft))
    {
        SD_Call(Sfx_MenuMove);
    }

    // Draw menu graphics.
    Options_ControllerMenu_EntriesDraw(g_ControllerMenu_IsOnActionsPane, selectedEntries.preset, selectedEntries.action, boundActionIdx);
}

s32 Options_ControllerMenu_ConfigUpdate(s32 actionIdx) // 0x801E6CF4
{
    u16* bindings;
    u16  boundBtnFlag;
    u16  btnFlag;
    s32  curActionIdx;
    s32  boundActionIdx;
    s32  i;
    u32  j;

    boundActionIdx = NO_VALUE;
    bindings       = (u16*)&g_GameWorkPtr->config.controllerConfig;

    // Run through all controller flags, excluding stick axes.
    for (i = 0; i < 16; i++)
    {
        btnFlag = 1 << i;

        if ((btnFlag & (ControllerFlag_DpadUp    |
                        ControllerFlag_DpadRight |
                        ControllerFlag_DpadDown  |
                        ControllerFlag_DpadLeft)) ||
            !(btnFlag & g_Controller0->clickedBtnFlags))
        {
            continue;
        }

        boundBtnFlag = bindings[actionIdx];

        // Remove binding.
        if (boundBtnFlag & btnFlag)
        {
            if ((actionIdx <  InputAction_Skip   ||
                 actionIdx == InputAction_Action ||
                 actionIdx == InputAction_Aim    ||
                 actionIdx == InputAction_Item) &&
                !(bindings[actionIdx] & ~btnFlag))
            {
                boundActionIdx = actionIdx;
                SD_Call(Sfx_MenuError);
            }
            else
            {
                bindings[actionIdx] &= ~btnFlag;
                SD_Call(Sfx_MenuConfirm);
            }
        }
        else
        {
            curActionIdx = NO_VALUE;
            switch (actionIdx)
            {
                case 0:
                case 1:
                    curActionIdx = actionIdx == 0;
                    if (bindings[curActionIdx] & btnFlag)
                    {
                        if (!(bindings[curActionIdx] & ~btnFlag))
                        {
                            boundActionIdx = curActionIdx;
                            SD_Call(Sfx_MenuError);
                        }
                        else
                        {
                            bindings[curActionIdx] &= ~btnFlag;
                            bindings[actionIdx]    |= btnFlag;
                            SD_Call(Sfx_MenuConfirm);
                        }
                    }
                    else
                    {
                        bindings[actionIdx] = boundBtnFlag | btnFlag;
                        SD_Call(Sfx_MenuConfirm);
                    }
                    break;

                case 2:
                    bindings[InputAction_Skip] |= btnFlag;
                    SD_Call(Sfx_MenuConfirm);
                    break;

                default:
                    curActionIdx = NO_VALUE;
                    for (j = InputAction_Action; j < InputAction_Count; j++)
                    {
                        if (bindings[j] & btnFlag)
                        {
                            curActionIdx = j;
                            break;
                        }
                    }

                    if (curActionIdx != NO_VALUE)
                    {
                        if ((curActionIdx <  InputAction_Skip   ||
                             curActionIdx == InputAction_Action ||
                             curActionIdx == InputAction_Aim    ||
                             curActionIdx == InputAction_Item) &&
                            !(bindings[curActionIdx] & ~btnFlag))
                        {
                            SD_Call(Sfx_MenuError);
                            boundActionIdx = curActionIdx;
                        }
                        else
                        {
                            bindings[curActionIdx] &= ~btnFlag;
                            bindings[actionIdx]    |= btnFlag;
                            SD_Call(Sfx_MenuConfirm);
                        }
                    }
                    else
                    {
                        bindings[actionIdx] |= btnFlag;
                        SD_Call(Sfx_MenuConfirm);
                    }
                    break;
            }
        }
    }

    return boundActionIdx;
}


void Options_ControllerMenu_EntriesDraw(bool isOnRightPane, s32 presetsEntryIdx, s32 actionsEntryIdx, s32 boundActionIdx) // 0x801E6F60
{
    #define STR_BASE_Y    22
    #define STR_OFFSET_Y  20
    #define ICON_SIZE_Y   12
    #define ICON_OFFSET_X -12

    s16      highlightY0;
    s16      highlightY1;
    s32      strYPos;
    s32      i;
    u16*     contConfig;
    DR_MODE* drMode;
    POLY_G4* poly;
    GsOT*    ot;

    /** @brief Draw modes for textured entry selection highlights in the controller config menu.
     * 0: Left presets pane.
     * 1: Right actions pane.
     */
    static DR_MODE SELECTION_HIGHLIGHT_DRAW_MODES[2] = {
        {
#ifdef SH_PC_PORT
            .len  = 3,
#else
            .tag  = 0x03000000,
#endif
            .code = { 0xE1000200, 0 }
        },
        {
#ifdef SH_PC_PORT
            .len  = 3,
#else
            .tag  = 0x03000000,
#endif
            .code = { 0xE1000200, 0 }
        }
    };

    /** @brief Quads for textured entry selection highlights in the controller config menu.
     * 0: Left presets pane.
     * 1: Right actions pane.
     */
    static POLY_G4 SELECTION_HIGHLIGHT_QUADS[2] = {
        {
#ifdef SH_PC_PORT
            .len  = 8,
#else
            .tag  = 0x08000000,
#endif
            .r0   = 255,
            .g0   = 255,
            .b0   = 255,
            .code = 0x3A,
            .r3   = 255,
            .g3   = 255,
            .b3   = 255
        },
        {
#ifdef SH_PC_PORT
            .len  = 8,
#else
            .tag  = 0x08000000,
#endif
            .code = 0x3A,
            .r1   = 255,
            .g1   = 255,
            .b1   = 255,
            .r2   = 255,
            .g2   = 255,
            .b2   = 255
        },
    };

    /** @brief Controller menu entry strings for the presets pane on the left. */
    static const char* CONTROLLER_MENU_PRESETS_PANE_ENTRY_STRINGS[] = {
        "EXIT",
        "TYPE_1",
        "TYPE_2",
        "TYPE_3"
    };

    /** @brief Controller menu entry strings for the actions pane on the right. */
    static const char* CONTROLLER_MENU_ACTIONS_PANE_ENTRY_STRINGS[] = {
        "ENTER",
        "CANCEL",
        "SKIP",
        "ACTION",
        "AIM",
        "LIGHT",
        "RUN",
        "VIEW",
        "STEP L",
        "STEP R",
        "PAUSE",
        "ITEM",
        "MAP",
        "OPTION"
    };


    ot     = &g_OtTags0[g_ActiveBufferIdx][15];
    poly   = &SELECTION_HIGHLIGHT_QUADS[g_ActiveBufferIdx];
    drMode = &SELECTION_HIGHLIGHT_DRAW_MODES[g_ActiveBufferIdx];

    // Draw entry strings.
    for (i = 0; i < ControllerMenuState_Count; i++)
    {
        Gfx_StringSetPosition(24, STR_BASE_Y + (i * STR_OFFSET_Y));
        Gfx_StringDraw(CONTROLLER_MENU_PRESETS_PANE_ENTRY_STRINGS[i], 20);
    }

    if (!isOnRightPane)
    {
        highlightY1 = presetsEntryIdx * STR_OFFSET_Y;
        highlightY0 = highlightY1 - 91;
        setXY4(poly,
               -137, highlightY0,
               -76,  highlightY0,
               -137, highlightY1 - 76,
               -76,  highlightY1 - 76);
    }

    strYPos     = STR_BASE_Y;
    highlightY0 = -300;

    // Draw controller config.
    for (i = 0, contConfig = (u16*)&g_GameWorkPtr->config.controllerConfig; i < (u32)InputAction_Count; i++, contConfig++)
    {
        // Draw action string.
        Text_Debug_PositionSet(96, strYPos);
        Text_Debug_Draw(CONTROLLER_MENU_ACTIONS_PANE_ENTRY_STRINGS[i]);

        // Draw button icon.
        if (i != boundActionIdx)
        {
            Options_ControllerMenu_ButtonIconsDraw(ICON_OFFSET_X, strYPos - 114, *contConfig);
        }

        if (i == actionsEntryIdx)
        {
            highlightY0 = strYPos - 113;
        }

        strYPos = (strYPos + ICON_SIZE_Y) + ((i == 2) ? ICON_SIZE_Y : 0);
    }

    if (isOnRightPane == true)
    {
        setXY4(poly,
               -65, highlightY0,
               -15, highlightY0,
               -65, highlightY0 + 10,
               -15, highlightY0 + 10);
    }

    AddPrim(ot, poly);
    AddPrim(ot, drMode);

    #undef STR_BASE_Y
    #undef STR_OFFSET_Y
    #undef ICON_SIZE_Y
    #undef ICON_OFFSET_X
}

void Options_ControllerMenu_ButtonIconsDraw(s32 baseX, s32 baseY, u16 config) // 0x801E716C
{
    #define ICON_SIZE_X   12
    #define ICON_SIZE_Y   12
    #define ICON_OFFSET_X 14

    s32            i;
    s32            posX;
    u16            clutX;
    u32            clutY;
    s32            temp;
    s32            v0;
    GsOT*          ot;
    SPRT*          prim;
    DR_TPAGE*      tpage;
    PACKET*        packet;
    s_FsImageDesc* image;

    image = &g_ControllerButtonAtlasImg;

    ot     = &g_OtTags1[g_ActiveBufferIdx][16];
    packet = GsOUT_PACKET_P;

    // Draw button sprites.
    for (posX = baseX, i = ICON_SIZE_X; i < 28; i++)
    {
        temp = i & 0xF;
        v0   = ((temp + 8) & 0xF) << 4;

        if (!((config >> temp) & (1 << 0)))
        {
            continue;
        }

        prim = (SPRT*)packet;
        addPrimFast(ot, prim, 4);
        setCodeWord(prim, PRIM_RECT | RECT_TEXTURE, COLOR_RGBC(128, 128, 128, 0));
        setWH(prim, ICON_SIZE_X, ICON_SIZE_Y);

        clutY = image->clutY;
        clutX = image->clutX;

        setXY0Fast(prim, posX, baseY);
        posX += ICON_OFFSET_X;

        // setUV0AndClut(prim, 0xF4, v0, clutY, clutX);
        *(u32*)(&prim->u0) = 244 + (v0 << 8) + (((clutY << 6) | ((clutX >> 4) & 0x3F)) << 16);

        packet = (u8*)prim + sizeof(SPRT);
        tpage  = (DR_TPAGE*)packet;

        setDrawTPage(tpage, 0, 1, getTPageN(0, 0, 7, 0));
        AddPrim(ot, tpage);
        packet = (u8*)tpage + sizeof(DR_TPAGE);
    }

    GsOUT_PACKET_P = packet;

    #undef ICON_SIZE_X
    #undef ICON_SIZE_Y
    #undef ICON_OFFSET_X
}


/** @brief Unknown .rodata value.
 * The type is assumed. It is unknown where this is used and
 * could be something defined by a macro.
 */
static const u16 D_801E2D42 = 4160;
