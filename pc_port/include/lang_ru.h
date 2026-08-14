/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LANG_RU_H
#define LANG_RU_H

/* Russian fan-translation support.
 *
 * The Russian patches translate everything the DISC owns (item names and
 * descriptions in BODYPROG, story text in the VIN overlays) and repaint the
 * FONT16 atlas so its Latin cells draw Cyrillic. The port already re-reads the
 * disc's item and map text (lang_text.c), so those come out right — but every
 * menu, option, pause and save-screen string is COMPILED INTO the port and is
 * still English ASCII, which the repainted atlas renders as Cyrillic nonsense.
 * The port also adds option rows no disc ever had, so adopting the disc's own
 * menu strings could not cover them even if they were findable.
 *
 * So the port carries its own Russian menu text (lang_ru_menu.inc, written in
 * UTF-8) and encodes it into whatever byte<->glyph mapping the mounted patch
 * uses. Each patch family gets one charset table, selected by an FNV-1a hash of
 * that disc's FONT16.TIM; an unrecognized disc logs its hash and keeps English,
 * so adding a new patch is one table plus one hash. */

/* Read FONT16.TIM off the mounted disc, pick the matching charset and encode
 * the menu table into it. No-op (and leaves everything English) on a disc whose
 * font is not a known Russian repaint — including every retail disc. Call after
 * Font_ApplyRegionPatches, from Pc_LangInit. */
void Pc_RuInit(void);

/* 1 once a Russian patch has been detected and its menu table encoded. */
int Pc_RuActive(void);

/* Russian for one compiled US menu literal, or NULL to keep the original. */
const char* Pc_RuMenuText(const char* us);

#endif
