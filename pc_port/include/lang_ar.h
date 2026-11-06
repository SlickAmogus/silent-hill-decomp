/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lang_ar.h -- Arabic fan-translated discs.
 *
 * Two in-place patches exist, and they solve right-to-left differently:
 *   - USA, Mansour (ETRD): logical-order text in pre-shaped FONT16 cells, and a
 *     patched BODYPROG whose text drawers walk RIGHT TO LEFT.
 *   - PAL, Ahmed Khaled / M2D / r.h.h (Obeid) / RITO: visual-order text drawn
 *     by the stock left-to-right drawer, plus a few accent-scheme constants.
 * The port runs its own compiled drawers, so each patch's code edits are
 * reproduced here, switched on by the disc's FONT16 (hashed like lang_ru.c).
 */
#ifndef LANG_AR_H
#define LANG_AR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    AR_DISC_NONE = 0,
    AR_DISC_USA_MANSOUR,
    AR_DISC_PAL_AHMED
} e_ArDisc;

/* Identify the mounted disc and load its UI text. Re-entrant (Pc_LangInit). */
void Pc_ArInit(void);

int Pc_ArDisc(void);
int Pc_ArActive(void);

/* The USA patch's drawers advance leftward from the start position. */
int Pc_ArRtl(void);

/* The 4 combining-mark bytes the USA patch layers one OT slot later. */
int Pc_ArRtlMark(unsigned int charCode);

/* Arabic for a compiled UI literal, in the disc's own glyph encoding, or NULL. */
const char* Pc_ArMenuText(const char* us);

/* Shape logical-order UTF-8 Arabic into that disc's FONT16 bytes; returns the
 * length written (NUL-terminated, truncated to outCap). */
int Pc_ArEncode(const char* utf8, int disc, char* out, int outCap);

/* Layout constants the patches rewrote: `ar` on that disc, `def` otherwise. */
#define PC_AR_USA(ar, def) (Pc_ArDisc() == AR_DISC_USA_MANSOUR ? (ar) : (def))
#define PC_AR_PAL(ar, def) (Pc_ArDisc() == AR_DISC_PAL_AHMED ? (ar) : (def))

/* "USA" / "PAL" and the translators, for logs and the launcher. */
const char* Pc_ArDiscRegionName(void);
const char* Pc_ArDiscGroupName(void);

#ifdef __cplusplus
}
#endif

#endif /* LANG_AR_H */
