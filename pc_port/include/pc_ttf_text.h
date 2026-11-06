/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ttf_text.h -- TrueType lines placed in native-menu coordinates.
 *
 * Native menus draw with the disc's FONT16, which on the Arabic discs holds
 * Arabic forms in every Latin cell. Text that arrives at run time in English
 * (RetroAchievements titles and descriptions) has no translation to fall back
 * on, so it is drawn with a TrueType face instead, over the menu, at the place
 * the game-font line would have gone.
 */
#ifndef PC_TTF_TEXT_H
#define PC_TTF_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* The mounted disc's FONT16 cannot draw Latin. */
int Pc_TtfText_Needed(void);

/* Queue one line for this frame. x/y and maxW are native-menu units (320x224,
 * y = top of the line, as Nm_TextDraw); tall = NM_TEXT_TALL size, else small.
 * colorId is a StringColorId. Lines wider than maxW shrink, then clip. */
void Pc_TtfText_Queue(int x, int y, int tall, int colorId, const char* utf8, int maxW);

/* Width of a line in native-menu units, as Pc_TtfText_Queue would draw it. */
int Pc_TtfText_Width(const char* utf8, int tall);

/* Overlay pass (dbg_overlay.c), once per presented frame. */
void Pc_TtfText_Draw(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_TTF_TEXT_H */
