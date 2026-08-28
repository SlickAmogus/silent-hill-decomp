/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_cheats.h - Cheats / Debug rows for the quick options overlay (F10). */
#ifndef PC_CHEATS_H
#define PC_CHEATS_H

enum { PC_CHEATS_PAGE_CHEATS = 0, PC_CHEATS_PAGE_DEBUG = 1 };

int         Pc_Cheats_Count(int page);
const char* Pc_Cheats_Name(int page, int idx);
/* 1 for one-shot actions (drawn without a value column). */
int         Pc_Cheats_IsAction(int page, int idx);
const char* Pc_Cheats_Label(int page, int idx, char* buf, int bufsz);
/* dir: +1 / -1 (toggles ignore it; Play-as cycles by it). */
void        Pc_Cheats_Adjust(int page, int idx, int dir);
/* Confirm/click on a row (Spawn fires its browsed entry; others step up). */
void        Pc_Cheats_Confirm(int page, int idx);
/* List rows (Spawn): >0 entries means "button on the left, browsable value on
 * the right, dropdown on click". */
int         Pc_Cheats_ListCount(int page, int idx);
const char* Pc_Cheats_ListName(int page, int idx, int i);
int         Pc_Cheats_ListGet(int page, int idx);
void        Pc_Cheats_ListSet(int page, int idx, int i);

#endif /* PC_CHEATS_H */
