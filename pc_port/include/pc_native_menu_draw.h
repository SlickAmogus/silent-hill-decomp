/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NATIVE_MENU_DRAW_H
#define PC_NATIVE_MENU_DRAW_H

#include "decomp/types.h"
#include "pc_native_menu.h"

/* PSX-primitive draw layer for the native menus. Every coordinate is in one
 * 320x240 space (x 0..320, y 0..224, the space Gfx_StringSetPosition takes on
 * a progressive screen) in both screen modes, so a layout works unchanged on
 * the 448-line title screen and the 224-line Options screen. A text row at y
 * occupies y..y+16 (TALL) or y..y+8 (SMALL, 448-line mode only). */

enum { NM_TEXT_TALL, NM_TEXT_SMALL };

/* Label x relative to its bullet's x, as on the retail Options pages. */
#define NM_BULLET_TEXT_DX 24

void Nm_TextDraw(int x, int y, int size, int color, const char* s);
/* Draws `s` as is, without the menu localization; measure it with
 * Pc_LangMenuTextWidth(s). */
void Nm_TextDrawVerbatim(int x, int y, int size, int color, const char* s);
int  Nm_TextWidth(const char* s);
void Nm_TextDrawCentered(int cx, int y, int size, int color, const char* s);
void Nm_TextDrawRight(int rx, int y, int size, int color, const char* s);
/* Draws `s` verbatim in x..x+maxW. Text wider than that pans to its end
 * and starts over, in step with any other pan on screen, when `pan` is set
 * (the selected row, or text that cannot be selected), and is cut with "..."
 * otherwise. Every pan restarts from its left end when the highlight moves.
 * Returns the width drawn, at most maxW. */
int  Nm_TextDrawMarquee(int x, int y, int size, int color, const char* s, int maxW, int pan);

/* y is the arrow's vertical centre. */
void Nm_ScrollArrowDraw(int cx, int y, int up, int enabled);
/* Retail look: yBottom = row y + 14, x0 = bullet x - 1. */
void Nm_HighlightDraw(int x0, int x1, int yBottom);
/* The next Nm_HighlightDraw jumps to its row instead of gliding (mouse picks). */
void Nm_HighlightSnap(void);
/* x is the bullet's left edge, y the row y of its label. */
void Nm_BulletDraw(int x, int y, int selected);
void Nm_RuleDraw(int x0, int x1, int y, int gold);
/* xLeft..xRight is the value text's extent, y its row y; the arrows sit outside it. */
void Nm_ValueArrowsDraw(int xLeft, int xRight, int y);
void Nm_BlackBackgroundDraw(void);
/* Full 0..255 UV span of the hires-override pool slot named by `clut`. */
void Nm_SpriteDraw(int x, int y, int w, int h, u16 clut, u8 rgb);

void NmInput_FromGame(NmInput* out);

/* (x, y) is the first visible row's label; the heading sits centred above it. */
void NmPage_Draw(const NmPage* p, int x, int y, int pitch, int valueX);
/* Draws nothing while the prompt is closed. */
void NmConfirm_Draw(const NmConfirm* c, const char* question, int cy);

#endif
