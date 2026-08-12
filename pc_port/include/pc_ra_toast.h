/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ra_toast.h - animated achievement-unlock toast.
 *
 * Console-style popup: the badge flies up from the bottom edge as the trophy
 * sound plays, the panel expands so the badge slides left and the text emerges
 * to its right, the text alternates between the achievement name and its
 * description, then the whole thing reverses back out.
 *
 * Main thread only. Drawing happens from the PsyX post-capture hook, so it
 * animates on wall-clock time and keeps running while the game is paused.
 */
#ifndef PC_RA_TOAST_H
#define PC_RA_TOAST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queue an unlock popup. Strings are copied. */
void Pc_RaToast_Show(const char* title, const char* desc,
                     const char* badgeName, unsigned points);

/* Hand over the raw PNG bytes for a badge that just finished downloading.
 * Copies what it needs; ignored when no pending toast wants this badge. */
void Pc_RaToast_ProvideBadge(const char* badgeName,
                            const unsigned char* png, size_t len);

/* 1 while a live or queued toast is still missing this badge image. */
int Pc_RaToast_WantsBadge(const char* badgeName);

/* Load fonts + sound from disk. Game-thread only, safe to call every frame
 * (self-gates after the first call). Must run before any toast appears. */
void Pc_RaToast_Preload(void);

/* Per-frame animate + draw. Cheap no-op when nothing is showing. */
void Pc_RaToast_Draw(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_RA_TOAST_H */
