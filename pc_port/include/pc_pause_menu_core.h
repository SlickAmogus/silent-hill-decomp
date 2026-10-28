/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PAUSE_MENU_CORE_H
#define PC_PAUSE_MENU_CORE_H

#include "pc_native_menu.h"

/* Renderer-free model of the in-game pause menu. SAVE/LOAD are disabled
 * (skipped by navigation, ignored by the mouse) while a block reason is set. */

typedef enum { PM_ROW_CONTINUE, PM_ROW_SAVE, PM_ROW_LOAD, PM_ROW_OPTION, PM_ROW_EXIT, PM_ROW_COUNT } PmRow;
typedef enum { PM_BLOCK_NONE, PM_BLOCK_RANDO, PM_BLOCK_BOSS } PmBlock;
typedef enum { PM_ACT_NONE, PM_ACT_MOVED, PM_ACT_RESUME, PM_ACT_SAVE, PM_ACT_LOAD,
               PM_ACT_OPTION, PM_ACT_EXIT_ASK, PM_ACT_EXIT_NO, PM_ACT_EXIT_YES } PmAction;
/* opening: set by PmMenu_Open, cleared by the next PmMenu_Update. */
typedef struct { int sel; PmBlock block; NmConfirm confirm; int opening; } PmMenu;

void     PmMenu_Open(PmMenu* m, PmBlock block);
unsigned PmMenu_EnabledMask(const PmMenu* m);      /* bit i = row i selectable */
/* hoverRow: row under the pointer or -1 (glue already masks disabled rows).
 * hoverAnswer: 0 = Yes, 1 = No, -1 = none; used only while confirm.open.
 * A click is in->mouseLeft with a valid hover; a right click is Cancel.
 * The update right after PmMenu_Open ignores pointer moves and left clicks:
 * that tick's motion is the mouse-look release, not a hover. */
/* Confirm-popup hover rule shared by the pause menu and the title EXIT popup:
 * sets c->yes from the hovered answer on move or click, and returns the input
 * to pass to NmConfirm_Update with a click turned into confirm and a right
 * click into cancel. */
NmInput PmConfirm_ApplyHover(NmConfirm* c, const NmInput* in, int hoverAnswer);
PmAction PmMenu_Update(PmMenu* m, const NmInput* in, int hoverRow, int hoverAnswer);
const char* PmBlock_Reason(PmBlock b);              /* NULL for PM_BLOCK_NONE */

#endif
