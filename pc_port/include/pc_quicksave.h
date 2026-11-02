/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_quicksave.h - quick save/load entry points shared with the pause menu.
 */
#ifndef PC_QUICKSAVE_H
#define PC_QUICKSAVE_H

/* 1 while a live boss actor exists; quick SAVE is refused then. */
int  Pc_QuickSave_BossActive(void);

/* Load the save/load screen assets and switch to the original screen. */
void Pc_QuickSave_OpenSaveScreen(void);
void Pc_QuickSave_OpenLoadScreen(void);

#endif /* PC_QUICKSAVE_H */
