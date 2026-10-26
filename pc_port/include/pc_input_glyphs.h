/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_input_glyphs.h - bind name to Kenney glyph mapping, no renderer, no SDL,
 * no game state. See pc_port/docs/InputGlyphs.md and the Kenney input glyphs
 * spec. The stateful half is pc_input_glyphs_apply.c.
 */
#ifndef PC_INPUT_GLYPHS_H
#define PC_INPUT_GLYPHS_H

#include "pc_input_glyphs_ids.h"

typedef enum {
    GLYPH_SET_AUTO,
    GLYPH_SET_PS_CLASSIC,
    GLYPH_SET_PS_MODERN,
    GLYPH_SET_XBOX,
    GLYPH_SET_NINTENDO,
    GLYPH_SET_STEAM,
    GLYPH_SET_COUNT
} e_GlyphSet;

typedef enum {
    GLYPH_DIR_UP,
    GLYPH_DIR_RIGHT,
    GLYPH_DIR_DOWN,
    GLYPH_DIR_LEFT
} e_GlyphDir;

#define INPUT_GLYPH_PX 16 /* one cell in 320x240 screen space */
#define INPUT_GLYPHS_VRAM_X 448 /* tpage 7, u 0 */

int         InputGlyphs_ClampSet(int set);
const char* InputGlyphs_SetLabel(int set);
int         InputGlyphs_ResolveAuto(int sdlType, int vendor, int product, int steamDeck);
int         InputGlyphs_ForKey(const char* bindName);
const char* InputGlyphs_KeySuffix(const char* bindName);
int         InputGlyphs_ForPad(int set, const char* sdlName);
int         InputGlyphs_ForStick(int set, int dir, int dpad);
void        InputGlyphs_UV(int glyph, int dark, int* u, int* v, int* texW, int* screenW);

/* Game side (pc_input_glyphs_apply.c). */

/* Last CLUT cell in VRAM, used by no TIM: the override keyed on it stands
 * over tpage 7's pixels without touching any TIM's own lookups there. */
#define INPUT_GLYPHS_CLUT_X 1008
#define INPUT_GLYPHS_CLUT_Y 511

void InputGlyphs_Load(void);
int  InputGlyphs_Ready(void);
int  InputGlyphs_ActiveSet(void);
void InputGlyphs_Step(int dir);

#endif
