/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_TITLE_STYLE_H
#define PC_TITLE_STYLE_H

/* Which of the three main menus the player sees, independent of the disc.
 *
 *   us        a full 320x480 picture: background, logo and all.
 *   pal       no picture -- black, the logo block, and fog, composed at draw
 *             time. Same menu text as the US one.
 *   japanese  no background either, but its own stylized logo art.
 *
 * No disc has all three, so the port ships the two a disc can lack under
 * gamedata/title/ (the Japanese art is byte-identical on every disc and is
 * always read from it).
 *
 * config: menu_style = auto | us | pal | japanese.  Console: MENUSTYLE. */

#define TITLE_STYLE_AUTO (-1)
#define TITLE_STYLE_US     0
#define TITLE_STYLE_PAL    1
#define TITLE_STYLE_JP     2

/* The style in effect, with auto resolved to the mounted disc's own. */
int Pc_TitleStyle(void);

int Pc_TitleUsesJpArt(void);   /* the Japanese picture */
int Pc_TitleUsesPalLogo(void); /* the composed PAL title, not a picture */

/* Point g_TitleImg at the VRAM home the chosen style's art wants, then read
 * that art -- off the disc when it has it, otherwise from gamedata/title/. */
void Pc_TitleStyleApplyDesc(void);
void Pc_TitleArtLoad(void);

/* Re-read the title art for the current style. Safe at the title screen; a
 * no-op elsewhere. Returns non-zero if it reloaded. */
int Pc_TitleStyleApply(void);

#endif
