/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LANG_JPN_H
#define LANG_JPN_H

/* NTSC-J menu text, read from the disc's own VIN/OPTION.BIN and
 * VIN/SAVELOAD.BIN instead of the compiled English literals.
 *
 * Retail NTSC-J draws those strings in Japanese; the port compiles the decomp's
 * US branch and so drew English. Reading them back fixes that, and gives the
 * Chinese fan translation for free — its patch is a PPF, which can only replace
 * bytes in place, so its strings sit at exactly the Japanese offsets. Which
 * language appears is therefore whatever the mounted disc carries; the
 * Japanese/Chinese setting only chooses the glyph set (see pc_kanji.h). */

/* Read and cache the strings. No-op off NTSC-J; call from Pc_LangInit. */
void Pc_JpnMenuInit(void);

/* Disc text for one compiled US menu literal, or NULL to keep the original. */
const char* Pc_JpnMenuText(const char* us);

#endif
