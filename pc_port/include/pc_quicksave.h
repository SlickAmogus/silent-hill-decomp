/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_quicksave.h - quick save/load entry points shared with the pause menu.
 */
#ifndef PC_QUICKSAVE_H
#define PC_QUICKSAVE_H

/* 1 while a live boss actor exists. */
int  Pc_QuickSave_BossActive(void);
/* 1 while that refuses a save: always, unless quicksave_boss is on. */
int  Pc_QuickSave_BossBlocks(void);

/* Load the save/load screen assets and switch to the original screen. */
void Pc_QuickSave_OpenSaveScreen(void);
void Pc_QuickSave_OpenLoadScreen(void);

#endif /* PC_QUICKSAVE_H */
