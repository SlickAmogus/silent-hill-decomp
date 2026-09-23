/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_panel.c - in-game controls panel. See pc_bind_panel.h.
 *
 * The GL plumbing repeats pc_confirm_dialog.c on purpose (the overlays do not
 * share code): its own program / VAO / textures with full state restore,
 * legacy GLSL, all GL work in Draw, wall-clock timing.
 *
 * Launcher compatibility is the design constraint. Every value written here is
 * one the launcher's Controls window writes itself: keyboard keys by SDL
 * scancode name, mouse as Mouse1..Mouse5 / MouseWheelUp / MouseWheelDown,
 * controller inputs by SDL button name from the launcher's list, "NONE" for
 * unbound, and the alternate-camera scheme under the same key + "_altcam".
 * Each change is saved as its own line (PcConfig_SaveKeyValue re-reads the file
 * first), and the launcher re-reads before it saves, so neither clobbers the
 * other.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>

#include "stb_truetype.h"

#include "pc_bind_panel.h"
#include "pc_confirm_dialog.h"
#include "pc_mouse_cursor.h"
#include "pc_config.h"
#include "control_style.h"
#include "sh_log.h"
#include "game.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"

extern const char* PsyX_Pad_ConnectedControllerName(void);
extern int         PsyX_Pad_ConnectedControllerType(void);
extern const char* PsyX_Pad_HeldBindName(void);
extern int         PsyX_Pad_AxisValue(int sdlAxis);
extern int         g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
extern int         g_DebugThirdPersonCam;
extern void        PsyX_GetDisplayViewport(int* outX, int* outY, int* outW, int* outH);

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

enum { BP_KB1 = 0, BP_KB2, BP_PAD1, BP_PAD2, BP_COLS };

/* A phone is played by touch, so the only inputs worth rebinding there are a
 * paired controller's. The keyboard and mouse columns are left out, and so are
 * the rows only they can reach; the panel opens only with a controller
 * connected (Pc_BindPanel_CanOpen). */
#if defined(SH_IOS) || defined(__ANDROID__)
#define BP_MOBILE    1
#define BP_COL_FIRST BP_PAD1
#else
#define BP_COL_FIRST BP_KB1
#endif
#define BP_VIS_COLS  (BP_COLS - BP_COL_FIRST)

#define BPF_STICK   1 /* controller side is the stick / d-pad, not a bind */
#define BPF_ALTCAM  2 /* only does anything with an alternate camera */
#define BPF_GLOBAL  4 /* one value shared by both schemes */
#define BPF_MOUSE   8 /* keyboard columns accept mouse buttons */
#define BPF_WHEEL  16 /* ...and the mouse wheel */

typedef struct
{
    const char* label;
    const char* key[BP_COLS];
    int         flags;
} BpRow;

/* Labels and order follow the launcher. The PSX-button rows resolve mouse
 * names in Pc_ApplyKeyOrMouse; the port's own actions resolve keyboard names
 * only (Swap Shoulder also takes a mouse button), so the mouse is offered only
 * where the game can use it. */
static const BpRow s_rows[] = {
#if !defined(BP_MOBILE)
    { "Move Up",        { "key_up",            "key_up_2",       NULL,                NULL },             BPF_STICK | BPF_MOUSE | BPF_WHEEL },
    { "Move Down",      { "key_down",          "key_down_2",     NULL,                NULL },             BPF_STICK | BPF_MOUSE | BPF_WHEEL },
    { "Turn Left",      { "key_left",          "key_left_2",     NULL,                NULL },             BPF_STICK | BPF_MOUSE | BPF_WHEEL },
    { "Turn Right",     { "key_right",         "key_right_2",    NULL,                NULL },             BPF_STICK | BPF_MOUSE | BPF_WHEEL },
#endif
    { "Action / Shoot", { "key_cross",         "key_cross_2",    "pad_cross",         "pad_cross_2" },    BPF_MOUSE | BPF_WHEEL },
    { "Flashlight",     { "key_circle",        "key_circle_2",   "pad_circle",        "pad_circle_2" },   BPF_MOUSE | BPF_WHEEL },
    { "Map",            { "key_triangle",      "key_triangle_2", "pad_triangle",      "pad_triangle_2" }, BPF_MOUSE | BPF_WHEEL },
    { "Run",            { "key_square",        "key_square_2",   "pad_square",        "pad_square_2" },   BPF_MOUSE | BPF_WHEEL },
    { "Sidestep Left",  { "key_l1",            "key_l1_2",       "pad_l1",            "pad_l1_2" },       BPF_MOUSE | BPF_WHEEL },
    { "Sidestep Right", { "key_r1",            "key_r1_2",       "pad_r1",            "pad_r1_2" },       BPF_MOUSE | BPF_WHEEL },
    { "View",           { "key_l2",            "key_l2_2",       "pad_l2",            "pad_l2_2" },       BPF_MOUSE | BPF_WHEEL },
    { "Aim",            { "key_r2",            "key_r2_2",       "pad_r2",            "pad_r2_2" },       BPF_MOUSE | BPF_WHEEL },
    { "Pause",          { "key_start",         "key_start_2",    "pad_start",         "pad_start_2" },    BPF_MOUSE | BPF_WHEEL },
    { "Inventory",      { "key_select",        "key_select_2",   "pad_select",        "pad_select_2" },   BPF_MOUSE | BPF_WHEEL },
    { "Reload",         { "key_reload",        "key_reload_2",   "pad_reload",        NULL },             0 },
    { "Cycle Weapons",  { "key_cycle_weapons", NULL,             "pad_cycle_weapons", NULL },             0 },
    { "Quick Heal",     { "key_quick_heal",    NULL,             "pad_quick_heal",    NULL },             0 },
    { "Quick Turn",     { "key_quick_turn",    NULL,             "pad_quick_turn",    NULL },             0 },
    { "Rear Look",      { "key_rear_look",     NULL,             "pad_rear_look",     NULL },             BPF_ALTCAM },
    { "Change Camera",  { "key_change_cam",    NULL,             "pad_change_cam",    NULL },             0 },
    /* The quick menu, the one control a phone otherwise reaches by touch
     * alone. Shared by both schemes, like the other global rows. */
    { "Quick Menu",     { "key_quick_options", NULL,             "pad_quick_options", NULL },             BPF_GLOBAL },
    /* Was desktop-only while its only binds were a key and a mouse button.
     * It takes a controller button now, and a phone reaches the OTS camera it
     * applies to whenever a pad is connected -- which is also the only way to
     * open this panel here. */
    { "Swap Shoulder",  { "key_swap_shoulder", NULL,             "pad_swap_shoulder", NULL },             BPF_GLOBAL | BPF_MOUSE },
#if !defined(BP_MOBILE)
    { "Quick Save",     { "key_quicksave",     NULL,             NULL,                NULL },             BPF_GLOBAL },
    { "Quick Load",     { "key_quickload",     NULL,             NULL,                NULL },             BPF_GLOBAL },
#endif
};

#define BP_ROWS      ((int)(sizeof(s_rows) / sizeof(s_rows[0])))
#define BP_ROW_RESET BP_ROWS
#define BP_ROW_CLOSE (BP_ROWS + 1)
#define BP_ROW_COUNT (BP_ROWS + 2)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { BP_CLOSED = 0, BP_OPENING, BP_SHOWN, BP_CLOSING };
static int    s_phase;
static Uint32 s_phaseStart;
#define BP_OPEN_MS  160u
#define BP_CLOSE_MS 140u

static int s_scheme;       /* 0 = classic, 1 = alternate cameras; fixed while open */
static int s_style;        /* g_ControlStyle when opened, for the header */
static int s_row, s_col, s_scroll;
static int s_visRows = 16; /* published by Draw */
/* Scroll to the selection on the next Draw. Only when the selection moved by
 * key or pad: a list scrolled by wheel or drag must stay where it was put. */
static int s_followSel;

static int    s_listen;       /* waiting for an input for (s_row, s_col) */
static int    s_listenArmed;  /* the press that opened the capture is done */
static Uint32 s_listenStart;
#define BP_LISTEN_PAD_MS 8000u

/* The pad bind held when a capture opened, and when it was last seen held. It
 * is not a candidate until it has been let go for BP_ARM_MS -- see bp_capture. */
static char   s_listenSkip[32];
static Uint32 s_skipSeenMs;
#define BP_ARM_MS 300u

static int s_resetAsked;      /* the confirm dialog is ours */

#if defined(BP_MOBILE)
/* Touch (phone rules, as on the save screen): a tap on a bind selects it, a
 * tap on the selected bind changes it, a drag scrolls the list. */
static int   s_tTracking, s_tDragged, s_tScroll0;
static float s_tY0;

/* "No controllers detected", drawn by Pc_BindPanel_Draw whether or not the
 * panel is open. */
static char   s_toastMsg[64];
static int    s_toastOn;
static Uint32 s_toastStart;
#define BP_TOAST_MS 2400u
#endif

/* Raw navigation edges and repeat. */
enum { BN_UP = 0, BN_DOWN, BN_LEFT, BN_RIGHT, BN_OK, BN_BACK, BN_CLEAR, BN_COUNT };
static int    s_navPrev[BN_COUNT];
static Uint32 s_navRepeatAt[BN_COUNT];
/* Last time each input was seen held; a press counts only once it has been
 * released for BP_SETTLE_MS (see bp_nav_edges). */
static Uint32 s_navHeldAt[BN_COUNT];
#define BP_SETTLE_MS 140u

/* One action per press. After any of them nothing acts again until everything
 * has been released and stayed released for a moment. A pad press used to run
 * twice over: the log shows one A writing the same bind five times and
 * answering the reset dialog twice. */
static int    s_gate;
static Uint32 s_gateAt;
#define BP_GATE_MS 150u

/* Set while the panel is shut but the press that shut it is still down. The
 * host clears the pad for as long as this file owns input, so that press
 * cannot also act on the screen underneath -- it re-entered Controller Config
 * and reopened the panel (two "panel open" lines in a row in the log). */
static int s_drain;

/* GL */
#define BP_GARBAGE 256
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locColor;
static int    s_glReady;
static GLuint s_texWhite, s_texCursor;
static GLuint s_garbage[BP_GARBAGE];
static int    s_garbageCount;

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontsTried;

/* Baked strings, cached by text + pixel size. Cleared on resize or when full. */
#define BP_TEXT_MAX 192
typedef struct
{
    char   text[64];
    int    px;
    GLuint tex;
    int    w, h;
} BpText;
static BpText s_text[BP_TEXT_MAX];
static int    s_textCount;
static int    s_bakedForPx;

/* Geometry published by Draw for Update's mouse hit-test (viewport px, y up). */
static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static float s_geoColL[BP_COLS + 1], s_geoColR[BP_COLS + 1]; /* [0] = label column */
static float s_geoRowT[64], s_geoRowB[64];
static int   s_geoRowIdx[64];
static int   s_geoRows;
static float s_geoListL, s_geoListR;
static int   s_geoValid;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void bp_beep(int sfxId)
{
    Sd_PlaySfx((u16)sfxId, 0, 64);
}

static int bp_unbound(const char* v)
{
    return v == NULL || v[0] == '\0' || SDL_strcasecmp(v, "NONE") == 0;
}

static const char* bp_value(int row, int col)
{
    if (row < 0 || row >= BP_ROWS || s_rows[row].key[col] == NULL)
        return NULL;
    return PcConfig_BindField(s_rows[row].key[col], s_scheme, NULL);
}

static int bp_row_enabled(int row)
{
    if (row >= BP_ROWS)
        return 1;
    return !((s_rows[row].flags & BPF_ALTCAM) && s_scheme == 0);
}

static int bp_cell_usable(int row, int col)
{
    return row >= 0 && row < BP_ROWS && s_rows[row].key[col] != NULL && bp_row_enabled(row);
}

static void bp_binds_changed(void)
{
    extern void Pc_QuickOptions_InvalidateRows(void);

    g_PcBindsGen++;
    Pc_ControlStyle_ReapplyBinds();
    /* The quick options footer names the Quick Options key. */
    Pc_QuickOptions_InvalidateRows();
}

static void bp_cfg_key(const char* key, int perScheme, char* out, size_t outSize)
{
    if (perScheme && s_scheme != 0)
        snprintf(out, outSize, "%s_altcam", key);
    else
        snprintf(out, outSize, "%s", key);
}

static void bp_set(int row, int col, const char* value)
{
    const char* key       = s_rows[row].key[col];
    int         perScheme = 0;
    char*       field     = PcConfig_BindField(key, s_scheme, &perScheme);
    char        cfgKey[64];

    if (field == NULL)
        return;
    snprintf(field, 24, "%s", value);
    bp_cfg_key(key, perScheme, cfgKey, sizeof(cfgKey));
    PcConfig_SaveKeyValue(cfgKey, field);
    SH_DBG("[BINDS] %s = %s", cfgKey, field);
    bp_binds_changed();
}

/* This scheme's binds back to the built-in values; the shared (global) rows
 * are left alone because they belong to both schemes. One file write. */
static void bp_reset_scheme(void)
{
    static char keyBuf[BP_ROWS * BP_COLS][40];
    const char* keys[BP_ROWS * BP_COLS];
    const char* vals[BP_ROWS * BP_COLS];
    int         n = 0, r, c;

    for (r = 0; r < BP_ROWS; r++)
    {
        if (s_rows[r].flags & BPF_GLOBAL)
            continue;
        for (c = BP_COL_FIRST; c < BP_COLS; c++)
        {
            const char* key = s_rows[r].key[c];
            const char* def;
            char*       field;
            int         perScheme = 0;

            if (key == NULL)
                continue;
            field = PcConfig_BindField(key, s_scheme, &perScheme);
            if (field == NULL)
                continue;
            def = PcConfig_BindDefault(key, s_scheme);
            snprintf(field, 24, "%s", bp_unbound(def) ? "NONE" : def);
            bp_cfg_key(key, perScheme, keyBuf[n], sizeof(keyBuf[n]));
            keys[n] = keyBuf[n];
            vals[n] = field;
            n++;
        }
    }
    PcConfig_SaveKeyValues(keys, vals, n);
    SH_DBG("[BINDS] %s scheme reset to defaults (%d binds)", s_scheme ? "altcam" : "classic", n);
    bp_binds_changed();
}

static const char* bp_pretty_key(const char* v)
{
    static const struct { const char* id; const char* name; } map[] = {
        { "Mouse1", "Left Mouse" },   { "Mouse2", "Right Mouse" }, { "Mouse3", "Middle Mouse" },
        { "Mouse4", "Mouse 4" },      { "Mouse5", "Mouse 5" },
        { "MouseWheelUp", "Wheel Up" }, { "MouseWheelDown", "Wheel Down" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    {
        if (SDL_strcasecmp(v, map[i].id) == 0)
            return map[i].name;
    }
    return v;
}

/* Labels as printed on the connected pad: PlayStation symbols for Sony pads,
 * Xbox letters for everything else (the SDL names follow the Xbox layout). */
static const char* bp_pretty_pad(const char* v, int padType)
{
    static const struct { const char* id; const char* xb; const char* ps; } map[] = {
        { "a", "A", "Cross" },          { "b", "B", "Circle" },
        { "x", "X", "Square" },         { "y", "Y", "Triangle" },
        { "back", "Back", "Select" },   { "guide", "Guide", "PS" },
        { "start", "Start", "Start" },
        { "leftstick", "LS Click", "L3" },     { "rightstick", "RS Click", "R3" },
        { "leftshoulder", "LB", "L1" },        { "rightshoulder", "RB", "R1" },
        { "lefttrigger", "LT", "L2" },         { "righttrigger", "RT", "R2" },
        { "dpup", "D-pad Up", "D-pad Up" },    { "dpdown", "D-pad Down", "D-pad Down" },
        { "dpleft", "D-pad Left", "D-pad Left" }, { "dpright", "D-pad Right", "D-pad Right" },
    };
    const int ps  = padType == SDL_CONTROLLER_TYPE_PS3 || padType == SDL_CONTROLLER_TYPE_PS4 ||
                    padType == SDL_CONTROLLER_TYPE_PS5;
    const int ps4 = padType == SDL_CONTROLLER_TYPE_PS4 || padType == SDL_CONTROLLER_TYPE_PS5;
    int       i;

    if (ps4 && SDL_strcasecmp(v, "start") == 0)
        return "Options";
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    {
        if (SDL_strcasecmp(v, map[i].id) == 0)
            return ps ? map[i].ps : map[i].xb;
    }
    return v;
}

static const char* bp_cell_text(int row, int col, int padType)
{
    const char* v;

    if (row >= BP_ROWS)
        return "";
    if (s_rows[row].key[col] == NULL)
    {
        if (col == BP_PAD1 && (s_rows[row].flags & BPF_STICK))
            return "Left Stick";
        return "";
    }
    v = bp_value(row, col);
    if (bp_unbound(v))
        return "-";
    return (col >= BP_PAD1) ? bp_pretty_pad(v, padType) : bp_pretty_key(v);
}

/* The same input on two actions of this scheme, same device. Allowed, as the
 * launcher allows it, but shown so it is never a surprise. */
static int bp_is_duplicate(int row, int col)
{
    const char* v = bp_value(row, col);
    int         pad = (col >= BP_PAD1);
    int         r, c;

    if (bp_unbound(v))
        return 0;
    for (r = 0; r < BP_ROWS; r++)
    {
        if (!bp_row_enabled(r))
            continue;
        for (c = pad ? BP_PAD1 : BP_KB1; c <= (pad ? BP_PAD2 : BP_KB2); c++)
        {
            const char* o;
            if (r == row && c == col)
                continue;
            o = bp_value(r, c);
            if (!bp_unbound(o) && SDL_strcasecmp(o, v) == 0)
                return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

int Pc_BindPanel_IsOpen(void)
{
    return s_phase != BP_CLOSED;
}

static void bp_nav_reset(void)
{
    int i;
    for (i = 0; i < BN_COUNT; i++)
    {
        s_navPrev[i]     = 1; /* whatever opened the panel must be released first */
        s_navRepeatAt[i] = 0;
        s_navHeldAt[i]   = SDL_GetTicks(); /* and settle before it counts again */
    }
    s_gate   = 1;
    s_gateAt = SDL_GetTicks();
}

static int bp_nav_any(const int held[BN_COUNT])
{
    int i;
    for (i = 0; i < BN_COUNT; i++)
    {
        if (held[i])
            return 1;
    }
    return 0;
}

/* 1 while the last action is still waiting for its press to end. */
static int bp_gated(const int held[BN_COUNT])
{
    if (!s_gate)
        return 0;
    if (bp_nav_any(held) || (SDL_GetTicks() - s_gateAt) < BP_GATE_MS)
        return 1;
    s_gate = 0;
    return 0;
}

void Pc_BindPanel_Open(void)
{
    if (s_phase == BP_OPENING || s_phase == BP_SHOWN)
        return;
    s_scheme      = g_DebugThirdPersonCam ? 1 : 0;
    s_style       = g_ControlStyle;
    s_row         = 0;
    s_col         = BP_COL_FIRST;
    s_scroll      = 0;
    s_followSel   = 1;
    s_listen      = 0;
    s_resetAsked  = 0;
    s_geoValid    = 0;
#if defined(BP_MOBILE)
    s_tTracking   = 0;
#endif
    s_phase       = BP_OPENING;
    s_phaseStart  = SDL_GetTicks();
    bp_nav_reset();
    SH_DBG("[BINDS] panel open: %s scheme (style %s)", s_scheme ? "altcam" : "classic", Pc_ControlStyleId(s_style));
}

#if defined(BP_MOBILE)
static void bp_toast(const char* msg)
{
    snprintf(s_toastMsg, sizeof(s_toastMsg), "%s", msg);
    s_toastOn    = 1;
    s_toastStart = SDL_GetTicks();
    SH_DBG("[BINDS] toast: %s", msg);
}
#endif

int Pc_BindPanel_CanOpen(void)
{
#if defined(BP_MOBILE)
    if (PsyX_Pad_ConnectedControllerName() == NULL)
    {
        bp_beep(Sfx_MenuError);
        bp_toast("No controllers detected");
        return 0;
    }
#endif
    return 1;
}

int Pc_BindPanel_TryOpen(void)
{
    if (!Pc_BindPanel_CanOpen())
        return 0;
    Pc_BindPanel_Open();
    return 1;
}

static void bp_close(void)
{
    s_listen     = 0;
    s_drain      = 1;
    s_phase      = BP_CLOSING;
    s_phaseStart = SDL_GetTicks();
}

void Pc_BindPanel_Close(void)
{
    /* Its reset question is fed by this panel's Update too. */
    if (s_resetAsked)
    {
        Pc_ConfirmDialog_Cancel();
        s_resetAsked = 0;
    }
    if (s_phase == BP_OPENING || s_phase == BP_SHOWN)
        bp_close();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void bp_read_nav(int held[BN_COUNT])
{
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    const int    ly = PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_LEFTY);
    const int    lx = PsyX_Pad_AxisValue(SDL_CONTROLLER_AXIS_LEFTX);
    const char*  pb = PsyX_Pad_HeldBindName();

    memset(held, 0, sizeof(int) * BN_COUNT);
    if (ks)
    {
        held[BN_UP]    = ks[SDL_SCANCODE_UP];
        held[BN_DOWN]  = ks[SDL_SCANCODE_DOWN];
        held[BN_LEFT]  = ks[SDL_SCANCODE_LEFT];
        held[BN_RIGHT] = ks[SDL_SCANCODE_RIGHT];
        held[BN_OK]    = ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] || ks[SDL_SCANCODE_SPACE];
        held[BN_BACK]  = ks[SDL_SCANCODE_ESCAPE];
        held[BN_CLEAR] = ks[SDL_SCANCODE_DELETE] || ks[SDL_SCANCODE_BACKSPACE];
    }
    held[BN_UP]    |= ly < -16000 || (pb && strcmp(pb, "dpup") == 0);
    held[BN_DOWN]  |= ly >  16000 || (pb && strcmp(pb, "dpdown") == 0);
    held[BN_LEFT]  |= lx < -16000 || (pb && strcmp(pb, "dpleft") == 0);
    held[BN_RIGHT] |= lx >  16000 || (pb && strcmp(pb, "dpright") == 0);
    if (pb)
    {
        held[BN_OK]    |= strcmp(pb, "a") == 0;
        held[BN_BACK]  |= strcmp(pb, "b") == 0 || strcmp(pb, "start") == 0;
        held[BN_CLEAR] |= strcmp(pb, "x") == 0;
    }
}

/* Press edges, with key-repeat on the four directions.
 *
 * An input counts as a new press only once it has been seen released for
 * BP_SETTLE_MS. A controller's face buttons are ANALOG on an MFi pad (and on
 * plenty of others), and SDL turns that pressure into a digital state at a
 * threshold: one ordinary press crosses it several times on the way in and out,
 * so a plain rising edge fired two or three times. That is the double
 * assignment and the double reset from the first controller session, and a very
 * light tap avoided it precisely because it never wavered. */
static void bp_nav_edges(int held[BN_COUNT], int pressed[BN_COUNT])
{
    const Uint32 now = SDL_GetTicks();
    int          i;

    for (i = 0; i < BN_COUNT; i++)
    {
        pressed[i] = 0;
        if (held[i] && !s_navPrev[i] && (now - s_navHeldAt[i]) >= BP_SETTLE_MS)
        {
            pressed[i]       = 1;
            s_navRepeatAt[i] = now + 360u;
        }
        else if (held[i] && i <= BN_RIGHT && now >= s_navRepeatAt[i])
        {
            pressed[i]       = 1;
            s_navRepeatAt[i] = now + 90u;
        }
        if (held[i])
            s_navHeldAt[i] = now;
        s_navPrev[i] = held[i];
    }
}

static void bp_move_row(int dir)
{
    s_row = (s_row + dir + BP_ROW_COUNT) % BP_ROW_COUNT;
}

static void bp_clamp_scroll(void)
{
    if (s_scroll > BP_ROW_COUNT - s_visRows)
        s_scroll = BP_ROW_COUNT - s_visRows;
    if (s_scroll < 0)
        s_scroll = 0;
}

static void bp_keep_visible(void)
{
    if (s_visRows < 1)
        s_visRows = 1;
    if (s_row < s_scroll)
        s_scroll = s_row;
    if (s_row >= s_scroll + s_visRows)
        s_scroll = s_row - s_visRows + 1;
    if (s_scroll > BP_ROW_COUNT - s_visRows)
        s_scroll = BP_ROW_COUNT - s_visRows;
    if (s_scroll < 0)
        s_scroll = 0;
}

static void bp_activate(void)
{
    if (s_row == BP_ROW_CLOSE)
    {
        bp_beep(Sfx_MenuCancel);
        bp_close();
        return;
    }
    if (s_row == BP_ROW_RESET)
    {
        bp_beep(Sfx_MenuConfirm);
        Pc_ConfirmDialog_Open("RESET CONTROLS",
                              s_scheme ? "Reset the alternate camera binds to their defaults?"
                                       : "Reset the classic camera binds to their defaults?");
        s_resetAsked = 1;
        return;
    }
    if (!bp_cell_usable(s_row, s_col))
    {
        bp_beep(Sfx_MenuError);
        return;
    }
    if (s_col >= BP_PAD1 && PsyX_Pad_ConnectedControllerName() == NULL)
    {
        bp_beep(Sfx_MenuError);
        return;
    }
    bp_beep(Sfx_MenuConfirm);
    s_listen      = 1;
    s_listenArmed = 0;
    s_listenStart = SDL_GetTicks();
    {
        /* Remember the button doing the opening, so bp_capture can tell it from
         * the one the player picks next. */
        const char* pb = PsyX_Pad_HeldBindName();

        snprintf(s_listenSkip, sizeof(s_listenSkip), "%s", (pb != NULL) ? pb : "");
        s_skipSeenMs = s_listenStart;
    }
    bp_nav_reset(); /* the press that opened this must not also be captured */
}

static void bp_clear_cell(void)
{
    if (!bp_cell_usable(s_row, s_col))
    {
        bp_beep(Sfx_MenuError);
        return;
    }
    bp_set(s_row, s_col, "NONE");
    bp_beep(Sfx_MenuCancel);
    bp_nav_reset();
}

/* Returns the captured bind name, "" to cancel, or NULL while still waiting. */
static const char* bp_capture(void)
{
    static char  name[32];
    static Uint8 prevKeys[SDL_NUM_SCANCODES];
    const Uint8* ks;
    int          nk, sc;
    Uint32       mb;
    int          flags = s_rows[s_row].flags;

    ks = SDL_GetKeyboardState(&nk);
    mb = SDL_GetMouseState(NULL, NULL);

    if (s_col >= BP_PAD1)
    {
        const char*  pb  = PsyX_Pad_HeldBindName();
        const Uint32 now = SDL_GetTicks();

        if (ks && ks[SDL_SCANCODE_ESCAPE])
            return "";
        if (now - s_listenStart > BP_LISTEN_PAD_MS || PsyX_Pad_ConnectedControllerName() == NULL)
            return "";
#if defined(BP_MOBILE)
        /* A phone has no Esc: a tap anywhere cancels. */
        if (Pc_MouseCursor_TouchPressed())
            return "";
#endif
        if (pb != NULL)
        {
            /* The button that opened this capture is not a choice until it has
             * been properly let go. Arming on a single released frame armed in
             * the middle of the opening press -- an analog face button dips
             * below SDL's threshold on the way down -- and bound A to itself on
             * every ordinary press (reported; only a feather-light tap worked).
             * Any OTHER button is the player choosing, and is taken at once. */
            if (!s_listenArmed && s_listenSkip[0] != '\0' &&
                SDL_strcasecmp(pb, s_listenSkip) == 0)
            {
                s_skipSeenMs = now;
                return NULL;
            }
            snprintf(name, sizeof(name), "%s", pb);
            return name;
        }
        if (!s_listenArmed && (now - s_skipSeenMs) >= BP_ARM_MS)
            s_listenArmed = 1;
        return NULL;
    }

    if (!s_listenArmed)
    {
        int any = (mb & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK |
                         SDL_BUTTON_X1MASK | SDL_BUTTON_X2MASK)) != 0;
        /* Lock keys are skipped here: some platforms report a toggled lock as
         * held, which would keep the panel waiting forever. They can still be
         * captured as binds below. */
        for (sc = 1; ks && sc < nk && !any; sc++)
            any = ks[sc] && sc != SDL_SCANCODE_CAPSLOCK && sc != SDL_SCANCODE_NUMLOCKCLEAR &&
                  sc != SDL_SCANCODE_SCROLLLOCK;
        if (!any)
        {
            s_listenArmed = 1;
            if (ks)
                memcpy(prevKeys, ks, (size_t)(nk < SDL_NUM_SCANCODES ? nk : SDL_NUM_SCANCODES));
        }
        return NULL;
    }

    if (ks && ks[SDL_SCANCODE_ESCAPE])
        return "";

    /* A key counts only on its press: a lock key that reads as held from the
     * start is never taken. */
    for (sc = 1; ks && sc < nk && sc < SDL_NUM_SCANCODES; sc++)
    {
        const char* kn;
        const int   pressed = ks[sc] && !prevKeys[sc];

        prevKeys[sc] = ks[sc];
        if (!pressed)
            continue;
        kn = SDL_GetScancodeName((SDL_Scancode)sc);
        if (kn == NULL || kn[0] == '\0')
            continue;
        snprintf(name, sizeof(name), "%s", kn);
        return name;
    }

    if (flags & BPF_MOUSE)
    {
        static const struct { Uint32 mask; const char* id; } mouse[] = {
            { SDL_BUTTON_LMASK, "Mouse1" },  { SDL_BUTTON_RMASK, "Mouse2" },
            { SDL_BUTTON_MMASK, "Mouse3" },  { SDL_BUTTON_X1MASK, "Mouse4" },
            { SDL_BUTTON_X2MASK, "Mouse5" },
        };
        int i;
        for (i = 0; i < (int)(sizeof(mouse) / sizeof(mouse[0])); i++)
        {
            if (mb & mouse[i].mask)
                return mouse[i].id;
        }
        if (flags & BPF_WHEEL)
        {
            if (g_PsyX_WheelUpFrames == 2)
                return "MouseWheelUp";
            if (g_PsyX_WheelDownFrames == 2)
                return "MouseWheelDown";
        }
    }
    return NULL;
}

/* Row and column under a viewport point (px, y up), from the geometry Draw
 * published. *outCol is -1 on the label column and on the Reset / Close rows. */
static int bp_hit(float px, float py, int* outRow, int* outCol)
{
    int i, c;

    if (px < s_geoListL || px > s_geoListR)
        return 0;
    for (i = 0; i < s_geoRows; i++)
    {
        if (py > s_geoRowT[i] || py < s_geoRowB[i])
            continue;
        *outRow = s_geoRowIdx[i];
        *outCol = -1;
        if (*outRow < BP_ROWS)
        {
            for (c = BP_COL_FIRST; c < BP_COLS; c++)
            {
                const int v = c - BP_COL_FIRST + 1;
                if (px >= s_geoColL[v] && px <= s_geoColR[v])
                {
                    *outCol = c;
                    break;
                }
            }
        }
        return 1;
    }
    return 0;
}

#if defined(BP_MOBILE)
static void bp_touch(int* activate)
{
    const float rowPx = (s_geoRows > 0 && s_geoRowT[0] > s_geoRowB[0]) ? (s_geoRowT[0] - s_geoRowB[0]) : 1.0f;
    float       tx, ty;
    int         row, col;

    if (Pc_MouseCursor_TouchPressed())
    {
        if (Pc_MouseCursor_TouchViewportPos(&tx, &ty))
        {
            s_tTracking = 1;
            s_tDragged  = 0;
            s_tY0       = (1.0f - ty) * s_vpH;
            s_tScroll0  = s_scroll;
        }
        return;
    }
    if (!s_tTracking)
        return;

    if (Pc_MouseCursor_TouchDown(NULL, NULL))
    {
        float dy;
        int   sc;

        if (!Pc_MouseCursor_TouchViewportPos(&tx, &ty))
            return;
        /* y is up, so a finger moving up the glass is positive and pulls the
         * rows below into view, the way a phone list follows the finger. */
        dy = (1.0f - ty) * s_vpH - s_tY0;
        if (!s_tDragged && fabsf(dy) > rowPx * 0.5f)
            s_tDragged = 1;
        if (!s_tDragged)
            return;
        sc = s_tScroll0 + (int)(dy / rowPx);
        if (sc > BP_ROW_COUNT - s_visRows) sc = BP_ROW_COUNT - s_visRows;
        if (sc < 0) sc = 0;
        s_scroll = sc;
        return;
    }

    if (!Pc_MouseCursor_TouchReleased(NULL, NULL))
        return;
    s_tTracking = 0;
    if (s_tDragged || !Pc_MouseCursor_TouchViewportPos(&tx, &ty))
        return;
    if (!bp_hit(tx * s_vpW, (1.0f - ty) * s_vpH, &row, &col))
        return;

    if (row >= BP_ROWS)
    {
        if (s_row == row)
            *activate = 1;
        else
        {
            s_row = row;
            bp_beep(Sfx_MenuMove);
        }
        return;
    }
    if (col < 0)
    {
        if (s_row != row)
        {
            s_row = row;
            bp_beep(Sfx_MenuMove);
        }
        return;
    }
    if (s_row == row && s_col == col)
        *activate = 1;
    else
    {
        s_row = row;
        s_col = col;
        bp_beep(Sfx_MenuMove);
    }
}
#endif

static void bp_mouse(int* activate, int* clear)
{
    float mx, my;
    int   moved, lclick, rclick, wheel, row, col;

    *activate = 0;
    *clear    = 0;
    if (!s_geoValid)
        return;

#if defined(BP_MOBILE)
    /* SDL's mouse made from a touch clicks on the very tap meant to select. */
    if (Pc_MouseCursor_TouchDriving())
    {
        bp_touch(activate);
        return;
    }
#endif

    wheel = Pc_MouseCursor_WheelStep();
    if (wheel != 0)
    {
        s_scroll -= wheel * 2;
        if (s_scroll > BP_ROW_COUNT - s_visRows) s_scroll = BP_ROW_COUNT - s_visRows;
        if (s_scroll < 0) s_scroll = 0;
    }

    moved  = Pc_MouseCursor_Moved();
    lclick = Pc_MouseCursor_LeftClicked();
    rclick = Pc_MouseCursor_RightClicked();
    if (!(moved || lclick || rclick) || !Pc_MouseCursor_ViewportPos(&mx, &my))
        return;

    if (!bp_hit(mx * s_vpW, (1.0f - my) * s_vpH, &row, &col))
        return;
    if (row >= BP_ROWS)
    {
        if (moved && s_row != row) { s_row = row; bp_beep(Sfx_MenuMove); }
        if (lclick) { s_row = row; *activate = 1; }
        return;
    }
    if (col < 0)
    {
        if (moved && s_row != row) { s_row = row; bp_beep(Sfx_MenuMove); }
        return;
    }
    if (moved && (s_row != row || s_col != col)) { s_row = row; s_col = col; bp_beep(Sfx_MenuMove); }
    if (lclick) { s_row = row; s_col = col; *activate = 1; }
    if (rclick) { s_row = row; s_col = col; *clear = 1; }
}

int Pc_BindPanel_Update(void)
{
    int held[BN_COUNT], pressed[BN_COUNT];
    int mActivate, mClear;

    if (s_phase == BP_CLOSED)
    {
        if (s_drain)
        {
            bp_read_nav(held);
            bp_nav_edges(held, pressed);
            if (bp_nav_any(held))
                return 1;
            s_drain = 0;
        }
        return 0;
    }
    if (s_phase != BP_SHOWN)
        return 1;

#if defined(BP_MOBILE)
    /* Nothing here does anything without the controller it edits. */
    if (PsyX_Pad_ConnectedControllerName() == NULL)
    {
        if (s_resetAsked)
        {
            Pc_ConfirmDialog_Update(0, 0, 0, 1);
            s_resetAsked = 0;
        }
        bp_beep(Sfx_MenuError);
        bp_toast("No controllers detected");
        bp_close();
        return 1;
    }
#endif

    /* The reset confirmation owns input while it is up. */
    if (s_resetAsked)
    {
        int res;
        bp_read_nav(held);
        bp_nav_edges(held, pressed);
        if (bp_gated(held))
            memset(pressed, 0, sizeof(pressed));
        if (!Pc_ConfirmDialog_IsOpen() && !pressed[BN_OK])
        {
            s_resetAsked = 0;
            return 1;
        }
        if (pressed[BN_LEFT] || pressed[BN_RIGHT])
            bp_beep(Sfx_MenuMove);
        res = Pc_ConfirmDialog_Update(pressed[BN_LEFT], pressed[BN_RIGHT], pressed[BN_OK], pressed[BN_BACK]);
        if (res == PC_CONFIRM_YES)
        {
            bp_beep(Sfx_MenuConfirm);
            bp_reset_scheme();
            s_resetAsked = 0;
            bp_nav_reset(); /* or the same press re-opens the dialog */
        }
        else if (res == PC_CONFIRM_NO)
        {
            bp_beep(Sfx_MenuCancel);
            s_resetAsked = 0;
            bp_nav_reset();
        }
        return 1;
    }

    if (s_listen)
    {
        const char* got = bp_capture();
        if (got != NULL)
        {
            s_listen = 0;
            if (got[0] == '\0')
            {
                bp_beep(Sfx_MenuCancel);
            }
            else
            {
                bp_set(s_row, s_col, got);
                bp_beep(Sfx_MenuConfirm);
            }
            bp_nav_reset(); /* the captured press must not also navigate */
        }
        return 1;
    }

    bp_read_nav(held);
    bp_nav_edges(held, pressed);
    if (bp_gated(held))
        memset(pressed, 0, sizeof(pressed));
    bp_mouse(&mActivate, &mClear);

    if (pressed[BN_BACK])
    {
        bp_beep(Sfx_MenuCancel);
        bp_close();
        return 1;
    }
    if (pressed[BN_UP])    { bp_move_row(-1); bp_beep(Sfx_MenuMove); }
    if (pressed[BN_DOWN])  { bp_move_row(+1); bp_beep(Sfx_MenuMove); }
    if (s_row < BP_ROWS)
    {
        const int v = s_col - BP_COL_FIRST;
        if (pressed[BN_LEFT])  { s_col = BP_COL_FIRST + (v + BP_VIS_COLS - 1) % BP_VIS_COLS; bp_beep(Sfx_MenuMove); }
        if (pressed[BN_RIGHT]) { s_col = BP_COL_FIRST + (v + 1) % BP_VIS_COLS;               bp_beep(Sfx_MenuMove); }
    }
    if (pressed[BN_UP] || pressed[BN_DOWN])
    {
        bp_keep_visible();
        s_followSel = 1;
    }

    if (pressed[BN_OK] || mActivate)
        bp_activate();
    else if (pressed[BN_CLEAR] || mClear)
        bp_clear_cell();

    return 1;
}

/* ------------------------------------------------------------------ */
/* GL primitives (mirrors pc_confirm_dialog.c)                         */
/* ------------------------------------------------------------------ */

static GLuint bp_make_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[BINDS] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void bp_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0, prevProg = 0;

    s_glReady = -1;
    vs = bp_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = bp_make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[BINDS] link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    /* PsyCross caches the bound program; leave it as found. */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");
    glUseProgram((GLuint)prevProg);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    s_glReady = 1;
}

static GLuint bp_upload_rgba(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void bp_retire(GLuint tex)
{
    if (tex && s_garbageCount < BP_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void bp_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Quad in NDC (yTop > yBot). */
static void bp_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    if (!tex)
        return;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = 0; v[0][3] = 0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = 0; v[1][3] = 1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = 1; v[2][3] = 0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = 1; v[3][3] = 0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = 0; v[4][3] = 1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = 1; v[5][3] = 1;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

static unsigned char* bp_read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    long  n;
    unsigned char* buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static void bp_fonts_init(void)
{
    static const char* paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    int i;
    if (s_fontsTried)
        return;
    s_fontsTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        unsigned char* data = bp_read_file(paths[i]);
        if (!data)
            continue;
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData = data; /* stbtt keeps pointers into this - never free */
            s_fontOk   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[BINDS] no usable font - text disabled");
}

/* Rasterize one line of ASCII to a white-coverage RGBA texture. */
static GLuint bp_bake(const char* text, float px, int* outW, int* outH)
{
    const char* p;
    float scale, penX, baseY;
    int   asc, desc, gap, W, H, i, pad = 1, prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint tex;

    if (!s_fontOk || !text || !text[0] || px < 4.0f)
        return 0;

    scale = stbtt_ScaleForPixelHeight(&s_font, px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)(px * 0.62f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf(px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return 0; }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    p     = text;
    while (*p)
    {
        int cp = (unsigned char)*p++;
        int gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx;
                    unsigned char v;
                    if (dx < 0 || dx >= W) continue;
                    v = scratch[sy * gw + sx];
                    if (v > cov[dy * W + dx]) cov[dy * W + dx] = v;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }

    {
        int stride = W;
        int inkW   = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
        for (i = 0; i < W * H; i++)
        {
            rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
        }
    }
    tex = bp_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static void bp_text_flush(void)
{
    int i;
    for (i = 0; i < s_textCount; i++)
        bp_retire(s_text[i].tex);
    s_textCount = 0;
}

static const BpText* bp_text(const char* text, int px)
{
    BpText* t;
    int     i;

    if (text == NULL || text[0] == '\0')
        return NULL;
    for (i = 0; i < s_textCount; i++)
    {
        if (s_text[i].px == px && strcmp(s_text[i].text, text) == 0)
            return s_text[i].tex ? &s_text[i] : NULL;
    }
    if (s_textCount >= BP_TEXT_MAX)
        bp_text_flush();
    t = &s_text[s_textCount++];
    snprintf(t->text, sizeof(t->text), "%s", text);
    t->px  = px;
    t->tex = bp_bake(t->text, (float)px, &t->w, &t->h);
    return t->tex ? t : NULL;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static void bp_gl_bind(void)
{
    if (!s_texWhite)
    {
        unsigned char one[4] = { 255, 255, 255, 255 };
        s_texWhite = bp_upload_rgba(one, 1, 1);
    }

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* Re-assert the attribute layout per draw (see pc_confirm_dialog.c). */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

#if defined(BP_MOBILE)
/* Bottom centre, clear of the options list and the quick menu's rows. */
static void bp_draw_toast(float vpW, float vpH)
{
    const Uint32  age = SDL_GetTicks() - s_toastStart;
    const BpText* t;
    float         a = 1.0f, w, h, l, b;

    if (!s_toastOn)
        return;
    if (age >= BP_TOAST_MS)
    {
        s_toastOn = 0;
        return;
    }
    if (age < 150u)
        a = (float)age / 150.0f;
    else if (age > BP_TOAST_MS - 400u)
        a = (float)(BP_TOAST_MS - age) / 400.0f;

    t = bp_text(s_toastMsg, (int)(vpH * 0.045f));
    w = (t ? (float)t->w : vpH * 0.5f) + vpH * 0.10f;
    h = vpH * 0.10f;
    l = (vpW - w) * 0.5f;
    b = vpH * 0.12f;

    bp_gl_bind();
    #define TNX(px_) (-1.0f + 2.0f * (px_) / vpW)
    #define TNY(py_) (-1.0f + 2.0f * (py_) / vpH)
    bp_quad(s_texWhite, TNX(l), TNY(b + h), TNX(l + w), TNY(b), 0.055f, 0.05f, 0.065f, 0.94f * a);
    bp_quad(s_texWhite, TNX(l), TNY(b + h), TNX(l + w), TNY(b + h - 3.0f), 0.47f, 0.11f, 0.08f, a);
    if (t)
    {
        const float ty = b + (h + (float)t->h) * 0.5f;
        const float tx = l + (w - (float)t->w) * 0.5f;
        bp_quad(t->tex, TNX(tx), TNY(ty), TNX(tx + (float)t->w), TNY(ty - (float)t->h), 1.0f, 0.93f, 0.86f, a);
    }
    #undef TNX
    #undef TNY
}
#endif

void Pc_BindPanel_Draw(void)
{
    GLint vp[4], prevVp[4];
    float vpW, vpH, panelW, panelH, panelL, panelR, panelT, panelB, dim = 1.0f;
    float pad, titleH, subH, headH, footH, rowH, listT, listB, listL, listR;
    float colX[BP_COLS + 2];
    int   pxTitle, pxRow, pxSmall, padType, i, c, v, shown;
    const char* padName;
    char  line[160];

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_phase == BP_CLOSED
#if defined(BP_MOBILE)
        && !s_toastOn
#endif
       )
        return;

    /* Lay out against the picture's rect, not the GL_VIEWPORT the frame left:
     * a menu frame that drew nothing keeps the full-window viewport while one
     * that drew leaves the pillarboxed one, and the panel changed size between
     * them (the "stretch" as it opened). The rect is the one the mouse
     * fractions are measured in, so hit-testing and the cursor still line up. */
    PsyX_GetDisplayViewport(&vp[0], &vp[1], &vp[2], &vp[3]);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    if (!s_glReady) bp_gl_init();
    if (s_glReady != 1) return;
    if (!s_fontsTried) bp_fonts_init();

    glGetIntegerv(GL_VIEWPORT, prevVp);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    bp_gl_pump();

    if (s_phase == BP_CLOSED)
        goto toast;
    {
        Uint32 age = SDL_GetTicks() - s_phaseStart;
        if (s_phase == BP_OPENING)
        {
            dim = (float)age / (float)BP_OPEN_MS;
            if (dim >= 1.0f) { dim = 1.0f; s_phase = BP_SHOWN; }
        }
        else if (s_phase == BP_CLOSING)
        {
            dim = 1.0f - (float)age / (float)BP_CLOSE_MS;
            if (dim <= 0.0f) { s_phase = BP_CLOSED; s_geoValid = 0; goto toast; }
        }
    }

    /* Layout (viewport px, origin bottom-left), scaled off the viewport height. */
    panelH = 0.92f * vpH;
    panelW = 1.45f * vpH;
    if (panelW > 0.94f * vpW) panelW = 0.94f * vpW;
    panelL = (vpW - panelW) * 0.5f;
    panelR = panelL + panelW;
    panelB = (vpH - panelH) * 0.5f;
    panelT = panelB + panelH;

    pad     = panelH * 0.025f;
#if defined(BP_MOBILE)
    /* Fingertip rows and text that reads at arm's length. */
    titleH  = panelH * 0.080f;
    subH    = panelH * 0.050f;
    headH   = panelH * 0.055f;
    footH   = panelH * 0.110f;
    rowH    = panelH * 0.062f;
#else
    titleH  = panelH * 0.070f;
    subH    = panelH * 0.034f;
    headH   = panelH * 0.040f;
    footH   = panelH * 0.070f;
    rowH    = panelH * 0.034f;
#endif
    pxTitle = (int)(titleH * 0.52f);
    pxRow   = (int)(rowH * 0.64f);
    pxSmall = (int)(subH * 0.62f);
    if (pxRow < 8) pxRow = 8;
    if (pxSmall < 8) pxSmall = 8;

    if (s_bakedForPx != pxRow)
    {
        bp_text_flush();
        s_bakedForPx = pxRow;
    }

    listL = panelL + pad;
    listR = panelR - pad;
    listT = panelT - titleH - 2.0f * subH - headH - pad * 0.5f;
    listB = panelB + footH;
    s_visRows = (int)((listT - listB) / rowH);
    if (s_visRows < 1) s_visRows = 1;
    if (s_visRows > 64) s_visRows = 64;
    if (s_followSel)
    {
        bp_keep_visible();
        s_followSel = 0;
    }
    bp_clamp_scroll();

    colX[0] = listL;
#if defined(BP_MOBILE)
    colX[1] = listL + (listR - listL) * 0.40f;
#else
    colX[1] = listL + (listR - listL) * 0.26f;
#endif
    for (v = 1; v <= BP_VIS_COLS; v++)
        colX[v + 1] = colX[1] + (listR - colX[1]) * ((float)v / (float)BP_VIS_COLS);

    padName = PsyX_Pad_ConnectedControllerName();
    padType = PsyX_Pad_ConnectedControllerType();

    bp_gl_bind();

#define NX(px_) (-1.0f + 2.0f * (px_) / vpW)
#define NY(py_) (-1.0f + 2.0f * (py_) / vpH)
#define TEXT_AT(t_, x_, yMid_, r_, g_, b_) \
    do { const BpText* _t = (t_); if (_t) { float _y = (yMid_) + (float)_t->h * 0.5f; \
        bp_quad(_t->tex, NX(x_), NY(_y), NX((x_) + _t->w), NY(_y - _t->h), (r_), (g_), (b_), dim); } } while (0)

    /* Backdrop, panel, title band. */
    bp_quad(s_texWhite, NX(0.0f), NY(vpH), NX(vpW), NY(0.0f), 0.0f, 0.0f, 0.0f, 0.66f * dim);
    bp_quad(s_texWhite, NX(panelL), NY(panelT), NX(panelR), NY(panelB), 0.055f, 0.05f, 0.065f, 0.96f * dim);
    bp_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - 2.0f), NX(panelR - 2.0f), NY(panelT - titleH),
            0.10f, 0.05f, 0.05f, 0.95f * dim);
    bp_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - titleH), NX(panelR - 2.0f), NY(panelT - titleH - 2.0f),
            0.47f, 0.11f, 0.08f, dim);
    {
        const BpText* t = bp_text("CONTROLS", pxTitle);
        if (t)
            TEXT_AT(t, panelL + (panelW - (float)t->w) * 0.5f, panelT - titleH * 0.5f, 1.0f, 0.93f, 0.86f);
    }

    /* Which binds these are, and which controller is live. */
    if (s_scheme)
        snprintf(line, sizeof(line), "Editing: alternate camera binds (current camera: %s)", Pc_ControlStyleLabel(s_style));
    else
        snprintf(line, sizeof(line), "Editing: classic camera binds");
    TEXT_AT(bp_text(line, pxSmall), listL, panelT - titleH - subH * 0.5f, 0.92f, 0.86f, 0.78f);
    if (padName != NULL)
        snprintf(line, sizeof(line), "Controller: %s", padName);
    else
        snprintf(line, sizeof(line), "Controller: none connected");
    TEXT_AT(bp_text(line, pxSmall), listL, panelT - titleH - subH * 1.5f,
            padName ? 0.72f : 0.60f, padName ? 0.84f : 0.60f, padName ? 0.72f : 0.62f);

    /* Column headings. */
    {
        static const char* heads[BP_COLS + 1] = { "Action", "Keyboard", "Keyboard 2", "Controller", "Controller 2" };
        float hy = panelT - titleH - 2.0f * subH - headH * 0.5f;
        TEXT_AT(bp_text(heads[0], pxRow), colX[0] + pad * 0.4f, hy, 0.62f, 0.62f, 0.68f);
        for (c = BP_COL_FIRST; c < BP_COLS; c++)
            TEXT_AT(bp_text(heads[c + 1], pxRow), colX[c - BP_COL_FIRST + 1] + pad * 0.4f, hy, 0.62f, 0.62f, 0.68f);
        bp_quad(s_texWhite, NX(listL), NY(listT + 1.0f), NX(listR), NY(listT), 1.0f, 1.0f, 1.0f, 0.14f * dim);
    }

    /* Rows. */
    s_geoRows = 0;
    shown     = 0;
    for (i = s_scroll; i < BP_ROW_COUNT && shown < s_visRows; i++, shown++)
    {
        float rt = listT - (float)shown * rowH;
        float rb = rt - rowH;
        float ym = (rt + rb) * 0.5f;
        int   enabled = bp_row_enabled(i);
        int   selRow  = (i == s_row);

        if (s_geoRows < 64)
        {
            s_geoRowT[s_geoRows]   = rt;
            s_geoRowB[s_geoRows]   = rb;
            s_geoRowIdx[s_geoRows] = i;
            s_geoRows++;
        }

        if (i >= BP_ROWS)
        {
            const char* lbl = (i == BP_ROW_RESET) ? "Reset to Defaults" : "Close";
            const BpText* t = bp_text(lbl, pxRow);
            if (selRow)
                bp_quad(s_texWhite, NX(listL), NY(rt - 1.0f), NX(listR), NY(rb + 1.0f), 0.42f, 0.16f, 0.12f, 0.85f * dim);
            if (t)
                TEXT_AT(t, listL + (listR - listL - (float)t->w) * 0.5f, ym,
                        selRow ? 1.0f : 0.80f, selRow ? 0.96f : 0.78f, selRow ? 0.92f : 0.76f);
            continue;
        }

        if (shown % 2 == 1)
            bp_quad(s_texWhite, NX(listL), NY(rt), NX(listR), NY(rb), 1.0f, 1.0f, 1.0f, 0.03f * dim);
        if (selRow)
            bp_quad(s_texWhite, NX(listL), NY(rt), NX(listR), NY(rb), 0.42f, 0.16f, 0.12f, 0.22f * dim);

        {
            float lc = enabled ? 0.88f : 0.42f;
            TEXT_AT(bp_text(s_rows[i].label, pxRow), colX[0] + pad * 0.4f, ym, lc, lc * 0.97f, lc * 0.94f);
        }

        for (c = BP_COL_FIRST; c < BP_COLS; c++)
        {
            float       cl = colX[c - BP_COL_FIRST + 1], cr = colX[c - BP_COL_FIRST + 2];
            int         selCell = selRow && (c == s_col);
            const char* txt;
            float       r = 0.86f, g = 0.86f, b = 0.90f;

            if (selCell)
            {
                bp_quad(s_texWhite, NX(cl + 1.0f), NY(rt - 1.0f), NX(cr - 1.0f), NY(rb + 1.0f),
                        0.42f, 0.16f, 0.12f, 0.85f * dim);
            }
            if (selCell && s_listen)
                txt = (c >= BP_PAD1) ? "Press a button..." : "Press a key...";
            else
                txt = bp_cell_text(i, c, padType);

            if (s_rows[i].key[c] == NULL || !enabled)
            {
                r = g = b = 0.42f;
            }
            else if (strcmp(txt, "-") == 0)
            {
                r = g = b = 0.50f;
            }
            else if (!(selCell && s_listen) && bp_is_duplicate(i, c))
            {
                r = 1.0f; g = 0.72f; b = 0.30f;
            }
            if (selCell)
            {
                r = r * 0.3f + 0.7f; g = g * 0.3f + 0.7f; b = b * 0.3f + 0.7f;
            }
            TEXT_AT(bp_text(txt, pxRow), cl + pad * 0.4f, ym, r, g, b);
        }
    }

    /* Scroll hints. */
    if (s_scroll > 0)
        TEXT_AT(bp_text("more above", pxSmall), listR - pad * 4.0f, listT + headH * 0.5f, 0.55f, 0.55f, 0.60f);
    if (s_scroll + s_visRows < BP_ROW_COUNT)
        TEXT_AT(bp_text("more below", pxSmall), listR - pad * 4.0f, listB - pxSmall * 0.6f, 0.55f, 0.55f, 0.60f);

    /* Footer: what the keys do right now. */
    {
        const char* hint;
        const char* hint2 = NULL;
        if (s_listen && s_row < BP_ROWS)
        {
            if (s_col >= BP_PAD1)
            {
                snprintf(line, sizeof(line), "Press a controller button for %s", s_rows[s_row].label);
#if defined(BP_MOBILE)
                hint2 = "Tap the screen, or wait a few seconds, to cancel";
#else
                hint2 = "Esc, or wait a few seconds, to cancel";
#endif
            }
            else
            {
                snprintf(line, sizeof(line), "Press a key%s for %s",
                         (s_rows[s_row].flags & BPF_MOUSE) ? " or mouse button" : "", s_rows[s_row].label);
                hint2 = "Esc to cancel";
            }
            hint = line;
        }
        else
        {
#if defined(BP_MOBILE)
            const int ps = padType == SDL_CONTROLLER_TYPE_PS3 || padType == SDL_CONTROLLER_TYPE_PS4 ||
                           padType == SDL_CONTROLLER_TYPE_PS5;
            hint  = ps ? "Cross: change     Square: clear     Circle: back"
                       : "A: change     X: clear     B: back";
            hint2 = "Tap a button to select it, tap it again to change it. Drag to scroll. Orange = used twice.";
#else
            hint  = "Enter / A: rebind     Delete / X: clear     Esc / B: back";
            hint2 = "Mouse: click a bind to change it, right-click to clear. Orange = used twice.";
#endif
        }
        TEXT_AT(bp_text(hint, pxRow), panelL + pad, panelB + footH * 0.62f, 0.90f, 0.88f, 0.84f);
        if (hint2)
            TEXT_AT(bp_text(hint2, pxSmall), panelL + pad, panelB + footH * 0.24f, 0.62f, 0.62f, 0.68f);
    }

    /* Publish geometry for Update's mouse hit-test. */
    s_vpW = vpW; s_vpH = vpH;
    for (v = 0; v <= BP_VIS_COLS; v++) { s_geoColL[v] = colX[v]; s_geoColR[v] = colX[v + 1]; }
    s_geoListL = listL;
    s_geoListR = listR;
    s_geoValid = (s_phase == BP_SHOWN);

    /* Mouse cursor last, so it rides above the panel. */
    if (!s_texCursor)
    {
        unsigned char rgba[32 * 32 * 4];
        if (Pc_MouseCursor_SpriteRgba(rgba))
        {
            s_texCursor = bp_upload_rgba(rgba, 32, 32);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }
    if (s_texCursor && !Pc_ConfirmDialog_IsOpen()
#if defined(BP_MOBILE)
        && !Pc_MouseCursor_TouchDriving()
#endif
       )
    {
        float cx, cy, cw, ch;
        if (Pc_MouseCursor_GlRect(vpW, vpH, &cx, &cy, &cw, &ch))
            bp_quad(s_texCursor, NX(cx), NY(cy), NX(cx + cw), NY(cy - ch), 1.0f, 1.0f, 1.0f, 1.0f);
    }

    /* [PANELMISS] The panel vanishes from single presented frames. Hand the
     * renderer's present-time readback one pixel of the red rule under the
     * title -- opaque (0.47,0.11,0.08), so (120,28,20) whatever is behind the
     * panel, and no scene pixel is near it -- plus the GL state this draw ran
     * under, so a frame that comes out without the panel says whether the draw
     * was rejected or the image was lost later. Only while fully shown: the
     * fades change the colour. */
    if (dim >= 1.0f)
    {
        extern int g_PsxPanelProbe[6], g_PsxPanelState[16];
        GLint     iv = 0, box[4] = { 0, 0, 0, 0 }, cvp[4] = { 0, 0, 0, 0 };

        g_PsxPanelProbe[0] = 1;
        g_PsxPanelProbe[1] = vp[0] + (int)(panelL + 8.0f);
        g_PsxPanelProbe[2] = vp[1] + (int)(panelT - titleH - 1.0f);
        g_PsxPanelProbe[3] = 120; g_PsxPanelProbe[4] = 28; g_PsxPanelProbe[5] = 20;

        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &iv); g_PsxPanelState[0] = iv;
        g_PsxPanelState[1] = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
        glGetIntegerv(GL_SCISSOR_BOX, box);
        g_PsxPanelState[2] = box[0]; g_PsxPanelState[3] = box[1];
        g_PsxPanelState[4] = box[2]; g_PsxPanelState[5] = box[3];
        g_PsxPanelState[6] = glIsEnabled(GL_STENCIL_TEST) ? 1 : 0;
        glGetIntegerv(GL_STENCIL_FUNC, &iv);       g_PsxPanelState[7] = iv;
        glGetIntegerv(GL_STENCIL_REF, &iv);        g_PsxPanelState[8] = iv;
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &iv); g_PsxPanelState[9] = iv;
        glGetIntegerv(GL_VIEWPORT, cvp);
        g_PsxPanelState[10] = cvp[2]; g_PsxPanelState[11] = cvp[3];
        g_PsxPanelState[12] = glIsProgram(s_prog) ? 1 : 0;
        g_PsxPanelState[13] = glIsTexture(s_texWhite) ? (int)s_texWhite : -(int)s_texWhite;
        g_PsxPanelState[14] = (int)glGetError();
        g_PsxPanelState[15] = (int)s_phase;
    }

#undef TEXT_AT
#undef NX
#undef NY

toast:
#if defined(BP_MOBILE)
    bp_draw_toast(vpW, vpH);
#endif

    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
