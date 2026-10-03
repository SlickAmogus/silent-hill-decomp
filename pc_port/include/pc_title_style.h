/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_TITLE_STYLE_H
#define PC_TITLE_STYLE_H

/* Which title screen the player sees, independent of the disc.
 *
 * TIM/TITLE.TIM -- the Japanese one, black with the scratched logo -- is
 * byte-identical on all three discs, so that look is available everywhere for
 * free. "Western" is each disc's own: the US/JP discs carry a full 320x480
 * picture in TITLE_E.TIM, while PAL composes its title from a small logo block
 * (Pc_TitleLogoDrawEur), which is why this is a style rather than a file name.
 *
 * config: menu_style = auto | western | japanese.  Console: MENUSTYLE. */

/* Non-zero when the Japanese title art should be used. */
int Pc_TitleUsesJpArt(void);

/* Re-read the title art for the current style and redraw. Safe to call at the
 * title screen; a no-op elsewhere. Returns non-zero if it reloaded. */
int Pc_TitleStyleApply(void);

/* Point g_TitleImg at the VRAM home the chosen style's art wants. */
void Pc_TitleStyleApplyDesc(void);

#endif
