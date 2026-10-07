/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LANG_QUICK_H
#define LANG_QUICK_H

/* Text for the PC overlays that draw with a TrueType font: the quick options
 * menu, its cheat/debug pages, the controls panel and the confirm boxes.
 *
 * Those take UTF-8, not the game font's bytes, so they cannot share
 * Pc_LangMenuText's output. The language is picked the same way, though:
 * Japanese discs stay English, a Russian disc wins, then a pack language, then
 * the PAL columns. Anything without a translation comes back as the English.
 *
 * Returned strings live in a small ring of static buffers: bake or copy them
 * before the next few calls. */

/* A QUICK. string, by its English text as drawn ("Music Volume"). */
const char* Pc_LangQuick(const char* en);

/* A PC Options / menu literal as options.c holds it ("Film_Grain"), with the
 * underscores turned into spaces. */
const char* Pc_LangQuickMenu(const char* us);

/* Pc_LangQuick of a template holding one {placeholder}, filled with `value`.
 * A translation that lost the placeholder falls back to the English one. */
const char* Pc_LangQuickFill(const char* en, const char* placeholder, const char* value);

/* Upper-cases Latin (with Latin-1 and Polish accents) and Cyrillic letters. */
void Pc_LangUtf8Upper(const char* in, char* out, int outSize);

/* Decodes one UTF-8 sequence and advances *pp. A malformed byte comes back as
 * itself, so Latin-1 text still draws. */
unsigned int Pc_LangUtf8Next(const char** pp);

#endif
