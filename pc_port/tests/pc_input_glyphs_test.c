/* SPDX-License-Identifier: GPL-3.0-or-later */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_input_glyphs.h"

static void test_glyphs_clamp_set(void)
{
    assert(InputGlyphs_ClampSet(0) == GLYPH_SET_AUTO);
    assert(InputGlyphs_ClampSet(5) == GLYPH_SET_STEAM);
    assert(InputGlyphs_ClampSet(6) == GLYPH_SET_AUTO);
    assert(InputGlyphs_ClampSet(-1) == GLYPH_SET_AUTO);
    assert(InputGlyphs_ClampSet(9) == GLYPH_SET_AUTO);
}
static void test_glyphs_set_label(void)
{
    assert(strcmp(InputGlyphs_SetLabel(GLYPH_SET_PS_CLASSIC), "PS_Classic") == 0);
    assert(strcmp(InputGlyphs_SetLabel(GLYPH_SET_AUTO), "Auto") == 0);
}
static void test_glyphs_auto_types(void)
{
    assert(InputGlyphs_ResolveAuto(4, 0x054C, 0x09CC, 0) == GLYPH_SET_PS_MODERN);
    assert(InputGlyphs_ResolveAuto(7, 0x054C, 0x0CE6, 0) == GLYPH_SET_PS_MODERN);
    assert(InputGlyphs_ResolveAuto(3, 0x054C, 0x0268, 0) == GLYPH_SET_PS_CLASSIC);
    assert(InputGlyphs_ResolveAuto(2, 0x045E, 0x0B12, 0) == GLYPH_SET_XBOX);
    assert(InputGlyphs_ResolveAuto(5, 0x057E, 0x2009, 0) == GLYPH_SET_NINTENDO);
    assert(InputGlyphs_ResolveAuto(13, 0x057E, 0x200E, 0) == GLYPH_SET_NINTENDO);
    assert(InputGlyphs_ResolveAuto(11, 0x057E, 0x2006, 0) == GLYPH_SET_NINTENDO);
    assert(InputGlyphs_ResolveAuto(12, 0x057E, 0x2007, 0) == GLYPH_SET_NINTENDO);
    assert(InputGlyphs_ResolveAuto(10, 0x0955, 0x7214, 0) == GLYPH_SET_XBOX);
}
static void test_glyphs_auto_steam(void)
{
    assert(InputGlyphs_ResolveAuto(0, 0x28DE, 0x1102, 0) == GLYPH_SET_STEAM);
    assert(InputGlyphs_ResolveAuto(1, 0x28DE, 0x1205, 0) == GLYPH_SET_STEAM);
    assert(InputGlyphs_ResolveAuto(1, 0x28DE, 0x11FF, 1) == GLYPH_SET_STEAM);
    assert(InputGlyphs_ResolveAuto(7, 0x28DE, 0x11FF, 1) == GLYPH_SET_PS_MODERN);
    assert(InputGlyphs_ResolveAuto(7, 0x28DE, 0x11FF, 0) == GLYPH_SET_PS_MODERN);
    assert(InputGlyphs_ResolveAuto(1, 0x28DE, 0x11FF, 0) == GLYPH_SET_XBOX);
}
static void test_glyphs_auto_none(void)
{
    assert(InputGlyphs_ResolveAuto(-1, 0, 0, 0) == GLYPH_SET_XBOX);
    assert(InputGlyphs_ResolveAuto(-1, 0, 0, 1) == GLYPH_SET_XBOX);
}
static void test_glyphs_key(void)
{
    assert(InputGlyphs_ForKey("A") == IG_KEY_A);
    assert(InputGlyphs_ForKey("Return") == IG_KEY_ENTER);
    assert(InputGlyphs_ForKey("Keypad Enter") == IG_KEY_ENTER);
    assert(InputGlyphs_ForKey("Keypad 5") == IG_KEY_5);
    assert(InputGlyphs_ForKey("Keypad .") == IG_KEY_PERIOD);
    assert(InputGlyphs_ForKey("Left Shift") == IG_KEY_SHIFT);
    assert(InputGlyphs_ForKey("Right Shift") == IG_KEY_SHIFT);
    assert(InputGlyphs_ForKey("Right Ctrl") == IG_KEY_CTRL);
    assert(InputGlyphs_ForKey("escape") == IG_KEY_ESC);
    assert(InputGlyphs_ForKey("Space") == IG_KEY_SPACE);
    assert(InputGlyphs_ForKey("Up") == IG_KEY_UP);
    assert(InputGlyphs_ForKey("F12") == IG_KEY_F12);
}
static void test_glyphs_key_unknown(void)
{
    assert(InputGlyphs_ForKey("F13") == -1);
    assert(InputGlyphs_ForKey("Left GUI") == -1);
    assert(InputGlyphs_ForKey("Keypad ") == -1);
    assert(InputGlyphs_ForKey("") == -1);
    assert(InputGlyphs_ForKey(NULL) == -1);
}
static void test_glyphs_mouse(void)
{
    assert(InputGlyphs_ForKey("Mouse1") == IG_MOUSE_LEFT);
    assert(InputGlyphs_ForKey("Mouse3") == IG_MOUSE_MIDDLE);
    assert(InputGlyphs_ForKey("Mouse2") == IG_MOUSE_RIGHT);
    assert(InputGlyphs_ForKey("MouseWheelUp") == IG_MOUSE_WHEEL_UP);
    assert(InputGlyphs_ForKey("MouseWheelDown") == IG_MOUSE_WHEEL_DOWN);
    assert(InputGlyphs_ForKey("Mouse4") == IG_MOUSE);
    assert(strcmp(InputGlyphs_KeySuffix("Mouse5"), "5") == 0);
    assert(InputGlyphs_KeySuffix("Mouse1") == NULL);
    assert(InputGlyphs_KeySuffix(NULL) == NULL);
}
static void test_glyphs_pad(void)
{
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "a") == IG_XBOX_A);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "A") == IG_XBOX_A);
    assert(InputGlyphs_ForPad(GLYPH_SET_STEAM, "a") == IG_DECK_A);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "leftstick") == IG_L3);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "rightstick") == IG_R3);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_MODERN, "a") == IG_PS_CROSS);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_MODERN, "b") == IG_PS_CIRCLE);
    assert(InputGlyphs_ForPad(GLYPH_SET_NINTENDO, "a") == IG_NIN_A);
    assert(InputGlyphs_ForPad(GLYPH_SET_NINTENDO, "b") == IG_NIN_B);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_CLASSIC, "back") == IG_PSC_SELECT);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_MODERN, "back") == IG_PS_SHARE);
    assert(InputGlyphs_ForPad(GLYPH_SET_NINTENDO, "start") == IG_NIN_PLUS);
    assert(InputGlyphs_ForPad(GLYPH_SET_STEAM, "lefttrigger") == IG_DECK_L2);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "rightshoulder") == IG_XBOX_RB);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_CLASSIC, "dpup") == IG_PSC_DPAD_UP);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "dpleft") == IG_DPAD_LEFT);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, "guide") == IG_PAD_EMPTY);
    assert(InputGlyphs_ForPad(GLYPH_SET_PS_MODERN, "guide") == IG_PAD_EMPTY);
    assert(InputGlyphs_ForPad(GLYPH_SET_AUTO, "a") == IG_XBOX_A);
    assert(InputGlyphs_ForPad(GLYPH_SET_XBOX, NULL) == -1);
}
static void test_glyphs_stick(void)
{
    assert(InputGlyphs_ForStick(GLYPH_SET_XBOX, GLYPH_DIR_UP, 0) == IG_LS_UP);
    assert(InputGlyphs_ForStick(GLYPH_SET_XBOX, GLYPH_DIR_LEFT, 1) == IG_DPAD_LEFT);
    assert(InputGlyphs_ForStick(GLYPH_SET_PS_CLASSIC, GLYPH_DIR_DOWN, 1) == IG_PSC_DPAD_DOWN);
}
static void test_glyphs_uv(void)
{
    int u, v, tw, sw, ud, vd;

    InputGlyphs_UV(IG_KEY_A, 0, &u, &v, &tw, &sw);
    InputGlyphs_UV(IG_KEY_A, 1, &ud, &vd, &tw, &sw);
    assert(ud == u + 64 && vd == v);
    assert(u >= 0 && u < 64 && v >= 0 && v < 256 && u % 8 == 0 && v % 8 == 0);
    assert(tw == 8 && sw == 16);
    InputGlyphs_UV(IG_KEY_SHIFT, 0, &u, &v, &tw, &sw);
    assert(tw == 16 && sw == 32);
    InputGlyphs_UV(IG_KEY_ENTER, 0, &u, &v, &tw, &sw);
    assert(tw == 16 && sw == 32);
    InputGlyphs_UV(IG_KEY_SPACE, 0, &u, &v, &tw, &sw);
    assert(tw == 8 && sw == 16);
    InputGlyphs_UV(IG_PAD_EMPTY, 0, &u, &v, &tw, &sw);
    InputGlyphs_UV(-1, 0, &ud, &vd, &tw, &sw);
    assert(ud == u && vd == v);
    InputGlyphs_UV(IG_COUNT, 0, &ud, &vd, &tw, &sw);
    assert(ud == u && vd == v);
}

int main(void)
{
    test_glyphs_clamp_set();
    test_glyphs_set_label();
    test_glyphs_auto_types();
    test_glyphs_auto_steam();
    test_glyphs_auto_none();
    test_glyphs_key();
    test_glyphs_key_unknown();
    test_glyphs_mouse();
    test_glyphs_pad();
    test_glyphs_stick();
    test_glyphs_uv();
    printf("pc_input_glyphs_test: ok\n");
    return 0;
}
