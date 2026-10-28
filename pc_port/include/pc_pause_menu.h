/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PAUSE_MENU_H
#define PC_PAUSE_MENU_H

#include "pc_native_menu.h"

/* In-game pause menu: CONTINUE / SAVE / LOAD / OPTION / EXIT over the frozen
 * frame, drawn and driven from SysState_GamePaused_Update. */

/* The whole PC body of SysState_GamePaused_Update after its freeze lines. */
void Pc_PauseMenu_Update(void);
/* 1 while away on the Save, Load or Option screen. */
int  Pc_PauseMenu_ReturnPending(void);
/* Returns and clears the pending return. */
int  Pc_PauseMenu_TakeReturn(void);
/* Clears the pending return and the freeze-frame capture hold. */
void Pc_PauseMenu_Reset(void);

/* Shared Yes/No prompt (pause EXIT, title EXIT). 320x240 space, both screen
 * modes. `yes` = Yes is highlighted; the answers sit on row y + 24. */
void Pc_PauseMenu_ConfirmDraw(const char* question, int yes, int y);
/* -1 none, 0 Yes, 1 No. Reads in->mouseOk/mouseX/mouseY (already 320x240 on
 * both screens via NmInput_FromGame); hit box is each answer's marked-label
 * width x rows y+24..y+40. */
int  Pc_PauseMenu_ConfirmHover(const NmInput* in, int y);

#endif
