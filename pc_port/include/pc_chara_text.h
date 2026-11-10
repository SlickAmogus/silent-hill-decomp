/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_TEXT_H
#define PC_CHARA_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Character Select and Additional Characters text. `en` is the English key
 * (QUICK.<en> in the language files, lang_quick*.inc). English draws in the
 * game font like every native menu; a translation draws in TrueType, whose
 * face carries every script the quick menu does (CJK, Arabic, Cyrillic),
 * which the game fonts do not all have. */

/* The translation, or `en` itself. UTF-8. */
const char* Ct_Tr(const char* en);
/* Translation with one {placeholder} filled. */
const char* Ct_Fill(const char* en, const char* placeholder, const char* value);
/* Draws `en` (translated). Returns the width drawn. maxW <= 0 = no limit. */
int  Ct_Draw(int x, int y, int tall, int color, const char* en, int maxW, int pan);
int  Ct_Width(const char* en, int tall);
void Ct_DrawCentered(int cx, int y, int tall, int color, const char* en);
/* Already-final text (a name, a translated status line): the game font in
 * English, TrueType once a translation is active, so a line mixing the two
 * reads in one face. */
int  Ct_DrawText(int x, int y, int tall, int color, const char* utf8, int maxW, int pan);
int  Ct_TextWidth(const char* utf8, int tall);
/* A character's name by its English label in any case ("MONSTER CYBIL"),
 * translated: the key is the title-cased form ("Monster Cybil"). */
const char* Ct_Name(const char* label);
/* Small detail lines: always TrueType (they run long and keep their case). */
void Ct_Small(int x, int y, int color, const char* utf8, int maxW);
/* Width of a line as Ct_Small draws it. */
int  Ct_SmallWidth(const char* utf8);

#ifdef __cplusplus
}
#endif

#endif
