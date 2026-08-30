/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo.h - the messages players leave for each other.
 *
 * TEMPLATED, NOT TYPED. A memo is a phrase index plus a word index, and the
 * wire carries only those two numbers (sh_net_proto.h). Nobody can type
 * anything, so there is nothing to moderate, nothing to translate at runtime,
 * and a memo costs four bytes. It is the Dark Souls arrangement and it is the
 * right one for a game with no keyboard UI and no moderation team.
 *
 * The tables live here rather than on the server for the same reason: the
 * server stores indices, so a client with a translated table shows the same
 * memo in another language without the server knowing anything about it.
 *
 * No GL in this file. The composer is a state machine the UI draws
 * (sh_net_ui.c) and the game thread drives.
 */
#ifndef SH_NET_MEMO_H
#define SH_NET_MEMO_H

#include "sh_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

int         ShNetMemo_PhraseCount(void);
const char* ShNetMemo_Phrase(int i);     /* display form, "%s" where the word goes */

int         ShNetMemo_WordCount(void);
const char* ShNetMemo_Word(int i);
/* Words are grouped so the picker is navigable; this is the heading the group
 * containing word `i` sits under, or NULL when `i` is not the first of a
 * group. */
const char* ShNetMemo_WordGroup(int i);

/* Render a marker to one display line. Handles the non-memo kinds too (a death
 * or save marker has no phrase, only an owner). */
void ShNetMemo_Text(const ShNetMemo* m, char* out, int cap);

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Index of the nearest marker the player is standing close enough to read, or
 * -1. Reads the player's position itself; call from the game thread. */
int ShNetMemo_NearestReadable(void);

/* ------------------------------------------------------------------ */
/* Composing                                                           */
/* ------------------------------------------------------------------ */

int  ShNetMemo_ComposerActive(void);
void ShNetMemo_ComposerToggle(void);
void ShNetMemo_ComposerClose(void);

/* One frame of edge-triggered input. Placing the memo (confirm on the last
 * column) closes the composer and queues the marker at the player's feet. */
void ShNetMemo_ComposerInput(int up, int down, int left, int right,
                             int confirm, int cancel);

/* What the composer is showing, for the panel. */
int         ShNetMemo_ComposerColumn(void);  /* 0 = phrase, 1 = word */
int         ShNetMemo_ComposerPhrase(void);
int         ShNetMemo_ComposerWord(void);
void        ShNetMemo_ComposerPreview(char* out, int cap);

/* Non-zero for a few seconds after a placement, so the UI can say so. A pure
 * read: the draw path asks more than once a frame. ShNetMemo_Tick, once a
 * frame from the game thread, is what makes it expire. */
int         ShNetMemo_JustPlaced(void);
void        ShNetMemo_Tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_MEMO_H */
