/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_input_glyphs.c - bind name to Kenney glyph mapping, no renderer, no SDL,
 * no game state. See pc_input_glyphs.h.
 */

#include <stddef.h>
#include <string.h>

#include "pc_input_glyphs.h"
#include "pc_input_glyphs_atlas.inc"

/* Mirrors SDL_GameControllerType so this module stays SDL-free. */
enum {
    CT_UNKNOWN = 0, CT_XBOX360 = 1, CT_XBOXONE = 2, CT_PS3 = 3, CT_PS4 = 4,
    CT_SWITCH_PRO = 5, CT_VIRTUAL = 6, CT_PS5 = 7,
    CT_JOYCON_LEFT = 11, CT_JOYCON_RIGHT = 12, CT_JOYCON_PAIR = 13
};

#define VID_VALVE 0x28DE

/* The atlas is registered over tpage 7 at tpage u 0 (VRAM x 448) at 2x: at u 128
 * the dark half's last column would end at u 256, past a u8 UV. */
#define ATLAS_U0 (INPUT_GLYPHS_VRAM_X % 64 * 4)

static const char* const s_setLabels[GLYPH_SET_COUNT] = {
    "Auto", "PS_Classic", "PS_Modern", "Xbox", "Nintendo", "Steam"
};

static char lower_ch(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ci_equal(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        if (lower_ch(*a) != lower_ch(*b))
            return 0;
    }
    return *a == *b;
}

static int ci_prefix(const char* s, const char* prefix)
{
    for (; *prefix; s++, prefix++) {
        if (lower_ch(*s) != lower_ch(*prefix))
            return 0;
    }
    return 1;
}

int InputGlyphs_ClampSet(int set)
{
    return (set >= 0 && set < GLYPH_SET_COUNT) ? set : GLYPH_SET_AUTO;
}

const char* InputGlyphs_SetLabel(int set)
{
    return s_setLabels[InputGlyphs_ClampSet(set)];
}

int InputGlyphs_ResolveAuto(int sdlType, int vendor, int product, int steamDeck)
{
    if (sdlType < 0)
        return GLYPH_SET_XBOX;

    if (vendor == VID_VALVE) {
        if (product == 0x1102 || product == 0x1142 || product == 0x1205)
            return GLYPH_SET_STEAM;
        if (steamDeck && product == 0x11FF &&
            (sdlType == CT_UNKNOWN || sdlType == CT_XBOX360 ||
             sdlType == CT_XBOXONE || sdlType == CT_VIRTUAL))
            return GLYPH_SET_STEAM;
    }

    switch (sdlType) {
    case CT_PS3:
        return GLYPH_SET_PS_CLASSIC;
    case CT_PS4:
    case CT_PS5:
        return GLYPH_SET_PS_MODERN;
    case CT_SWITCH_PRO:
    case CT_JOYCON_LEFT:
    case CT_JOYCON_RIGHT:
    case CT_JOYCON_PAIR:
        return GLYPH_SET_NINTENDO;
    default:
        return GLYPH_SET_XBOX;
    }
}

static const struct { const char* name; int glyph; } s_keys[] = {
    { "a", IG_KEY_A }, { "b", IG_KEY_B }, { "c", IG_KEY_C }, { "d", IG_KEY_D },
    { "e", IG_KEY_E }, { "f", IG_KEY_F }, { "g", IG_KEY_G }, { "h", IG_KEY_H },
    { "i", IG_KEY_I }, { "j", IG_KEY_J }, { "k", IG_KEY_K }, { "l", IG_KEY_L },
    { "m", IG_KEY_M }, { "n", IG_KEY_N }, { "o", IG_KEY_O }, { "p", IG_KEY_P },
    { "q", IG_KEY_Q }, { "r", IG_KEY_R }, { "s", IG_KEY_S }, { "t", IG_KEY_T },
    { "u", IG_KEY_U }, { "v", IG_KEY_V }, { "w", IG_KEY_W }, { "x", IG_KEY_X },
    { "y", IG_KEY_Y }, { "z", IG_KEY_Z },
    { "0", IG_KEY_0 }, { "1", IG_KEY_1 }, { "2", IG_KEY_2 }, { "3", IG_KEY_3 },
    { "4", IG_KEY_4 }, { "5", IG_KEY_5 }, { "6", IG_KEY_6 }, { "7", IG_KEY_7 },
    { "8", IG_KEY_8 }, { "9", IG_KEY_9 },
    { "f1", IG_KEY_F1 },   { "f2", IG_KEY_F2 },   { "f3", IG_KEY_F3 },
    { "f4", IG_KEY_F4 },   { "f5", IG_KEY_F5 },   { "f6", IG_KEY_F6 },
    { "f7", IG_KEY_F7 },   { "f8", IG_KEY_F8 },   { "f9", IG_KEY_F9 },
    { "f10", IG_KEY_F10 }, { "f11", IG_KEY_F11 }, { "f12", IG_KEY_F12 },
    { "return", IG_KEY_ENTER }, { "enter", IG_KEY_ENTER },
    { "escape", IG_KEY_ESC }, { "backspace", IG_KEY_BACKSPACE },
    { "tab", IG_KEY_TAB }, { "space", IG_KEY_SPACE },
    { "capslock", IG_KEY_CAPS }, { "pageup", IG_KEY_PAGEUP },
    { "pagedown", IG_KEY_PAGEDOWN }, { "delete", IG_KEY_DELETE },
    { "insert", IG_KEY_INSERT }, { "home", IG_KEY_HOME }, { "end", IG_KEY_END },
    { "printscreen", IG_KEY_PRINT }, { "scrolllock", IG_KEY_SCROLL },
    { "pause", IG_KEY_PAUSE }, { "numlock", IG_KEY_NUMLOCK },
    { "up", IG_KEY_UP }, { "down", IG_KEY_DOWN },
    { "left", IG_KEY_LEFT }, { "right", IG_KEY_RIGHT },
    { "left shift", IG_KEY_SHIFT }, { "right shift", IG_KEY_SHIFT },
    { "left ctrl", IG_KEY_CTRL },   { "right ctrl", IG_KEY_CTRL },
    { "left alt", IG_KEY_ALT },     { "right alt", IG_KEY_ALT },
    { "-", IG_KEY_MINUS }, { "+", IG_KEY_PLUS }, { "=", IG_KEY_EQUALS },
    { "[", IG_KEY_LBRACKET }, { "]", IG_KEY_RBRACKET },
    { "\\", IG_KEY_BACKSLASH }, { "'", IG_KEY_APOSTROPHE },
    { ";", IG_KEY_SEMICOLON }, { "*", IG_KEY_ASTERISK }, { "`", IG_KEY_GRAVE },
    { ",", IG_KEY_COMMA }, { ".", IG_KEY_PERIOD }, { "/", IG_KEY_SLASH },
    { "mouse1", IG_MOUSE_LEFT }, { "mouse2", IG_MOUSE_RIGHT },
    { "mouse3", IG_MOUSE_MIDDLE }, { "mouse4", IG_MOUSE }, { "mouse5", IG_MOUSE },
    { "mousewheelup", IG_MOUSE_WHEEL_UP },
    { "mousewheeldown", IG_MOUSE_WHEEL_DOWN },
};

int InputGlyphs_ForKey(const char* bindName)
{
    size_t i;

    if (bindName == NULL)
        return -1;
    if (ci_prefix(bindName, "keypad "))
        bindName += 7;
    for (i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        if (ci_equal(bindName, s_keys[i].name))
            return s_keys[i].glyph;
    }
    return -1;
}

const char* InputGlyphs_KeySuffix(const char* bindName)
{
    if (bindName == NULL)
        return NULL;
    if (ci_equal(bindName, "mouse4"))
        return "4";
    if (ci_equal(bindName, "mouse5"))
        return "5";
    return NULL;
}

enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_BACK, PB_START, PB_COUNT };

static const char* const s_padNames[PB_COUNT] = {
    "a", "b", "x", "y", "leftshoulder", "rightshoulder",
    "lefttrigger", "righttrigger", "back", "start"
};

static const int s_padGlyphs[GLYPH_SET_COUNT][PB_COUNT] = {
    [GLYPH_SET_PS_CLASSIC] = {
        IG_PS_CROSS, IG_PS_CIRCLE, IG_PS_SQUARE, IG_PS_TRIANGLE,
        IG_PS_L1, IG_PS_R1, IG_PS_L2, IG_PS_R2, IG_PSC_SELECT, IG_PSC_START },
    [GLYPH_SET_PS_MODERN] = {
        IG_PS_CROSS, IG_PS_CIRCLE, IG_PS_SQUARE, IG_PS_TRIANGLE,
        IG_PS_L1, IG_PS_R1, IG_PS_L2, IG_PS_R2, IG_PS_SHARE, IG_PS_OPTIONS },
    [GLYPH_SET_XBOX] = {
        IG_XBOX_A, IG_XBOX_B, IG_XBOX_X, IG_XBOX_Y,
        IG_XBOX_LB, IG_XBOX_RB, IG_XBOX_LT, IG_XBOX_RT, IG_XBOX_VIEW, IG_XBOX_MENU },
    [GLYPH_SET_NINTENDO] = {
        IG_NIN_A, IG_NIN_B, IG_NIN_X, IG_NIN_Y,
        IG_NIN_L, IG_NIN_R, IG_NIN_ZL, IG_NIN_ZR, IG_NIN_MINUS, IG_NIN_PLUS },
    [GLYPH_SET_STEAM] = {
        IG_DECK_A, IG_DECK_B, IG_DECK_X, IG_DECK_Y,
        IG_DECK_L1, IG_DECK_R1, IG_DECK_L2, IG_DECK_R2, IG_DECK_VIEW, IG_DECK_MENU },
};

int InputGlyphs_ForStick(int set, int dir, int dpad)
{
    static const int ls[4] = { IG_LS_UP, IG_LS_RIGHT, IG_LS_DOWN, IG_LS_LEFT };
    static const int dp[4] = { IG_DPAD_UP, IG_DPAD_RIGHT, IG_DPAD_DOWN, IG_DPAD_LEFT };
    static const int psc[4] = { IG_PSC_DPAD_UP, IG_PSC_DPAD_RIGHT, IG_PSC_DPAD_DOWN, IG_PSC_DPAD_LEFT };

    if (dir < 0 || dir > 3)
        return IG_PAD_EMPTY;
    if (!dpad)
        return ls[dir];
    return InputGlyphs_ClampSet(set) == GLYPH_SET_PS_CLASSIC ? psc[dir] : dp[dir];
}

int InputGlyphs_ForPad(int set, const char* sdlName)
{
    int i;

    if (sdlName == NULL)
        return -1;
    set = InputGlyphs_ClampSet(set);
    if (set == GLYPH_SET_AUTO)
        set = GLYPH_SET_XBOX;

    for (i = 0; i < PB_COUNT; i++) {
        if (ci_equal(sdlName, s_padNames[i]))
            return s_padGlyphs[set][i];
    }
    if (ci_equal(sdlName, "leftstick"))
        return IG_L3;
    if (ci_equal(sdlName, "rightstick"))
        return IG_R3;
    if (ci_equal(sdlName, "dpup"))
        return InputGlyphs_ForStick(set, GLYPH_DIR_UP, 1);
    if (ci_equal(sdlName, "dpright"))
        return InputGlyphs_ForStick(set, GLYPH_DIR_RIGHT, 1);
    if (ci_equal(sdlName, "dpdown"))
        return InputGlyphs_ForStick(set, GLYPH_DIR_DOWN, 1);
    if (ci_equal(sdlName, "dpleft"))
        return InputGlyphs_ForStick(set, GLYPH_DIR_LEFT, 1);
    return IG_PAD_EMPTY;
}

void InputGlyphs_UV(int glyph, int dark, int* u, int* v, int* texW, int* screenW)
{
    int wide;

    if (glyph < 0 || glyph >= IG_COUNT)
        glyph = IG_PAD_EMPTY;
    wide = s_igAtlas[glyph].wide;
    *u = ATLAS_U0 + (s_igAtlas[glyph].col + (dark ? 8 : 0)) * 8;
    *v = s_igAtlas[glyph].row * 8;
    *texW = wide ? 16 : 8;
    *screenW = wide ? 32 : 16;
}
