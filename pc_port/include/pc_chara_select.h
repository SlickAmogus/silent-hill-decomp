/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_SELECT_H
#define PC_CHARA_SELECT_H

#include "pc_native_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Character Select: one row per human character, the model that draws it
 * beside it, and a preview of that model fitted to the character. Harry's row
 * is the Play As roster; every other row is a skinned glb from
 * gamedata/load/CHARA fitted to that character, saved as `chara_model_NAME`.
 *
 * A native (PSX-primitive) screen in the achievements menu's style. The title
 * menu hosts it like Achievements; in game the quick menu opens it and stands
 * down while it is up. The results music plays while it is open. */

void Pc_CharaSelect_Init(void);                                        /* after Pc_PlayAs_Init */
void Pc_CharaSelect_ConfigKey(const char* name, const char* value);    /* config: chara_model_NAME */

void Pc_CharaSelect_Open(void);
int  Pc_CharaSelect_IsOpen(void);
/* 0 once the screen wants to close (the host runs its fade, then Close). */
int  Pc_CharaSelect_Update(const NmInput* in);
void Pc_CharaSelect_Close(void);
void Pc_CharaSelect_Draw(void);

/* The preview column's rectangle in the 320x224 menu space. */
void Pc_CharaSelect_PreviewRect(int* x0, int* y0, int* x1, int* y1);
/* What the preview shows: the stock model name (HERO, SIBYL...) the fit is
 * for, and the swap file base, or NULL when the stock model is chosen. */
const char* Pc_CharaSelect_PreviewTarget(const char** file);

#ifdef __cplusplus
}
#endif

#endif
