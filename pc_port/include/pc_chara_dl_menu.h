/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_DL_MENU_H
#define PC_CHARA_DL_MENU_H

#include "pc_native_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Additional Characters screens, hosted by Character Select: a hub
 * (Downloadable / Locally installed), the two category lists, and the model
 * picker that Character Select opens on a character's model field. */

enum { CDM_HUB, CDM_PICKER };

/* picker: `stock` is the stock model name the list picks a model for (HERO,
 * SIBYL...), `current` the chosen file base or NULL for the stock model. */
void Cdm_Open(int page, const char* stock, const char* current, int stockCharaId);
int  Cdm_IsOpen(void);
/* 0 once the screen closes back to Character Select. */
int  Cdm_Update(const NmInput* in);
void Cdm_Draw(int fieldL, int fieldR);
/* The picker's result once it closed: 1 with *file set (NULL = stock model). */
int  Cdm_PickerResult(const char** file);
/* Preview: the file to show fitted to HERO (or the picker's stock model). */
const char* Cdm_PreviewTarget(const char** file);
void Cdm_PreviewRect(int* x0, int* y0, int* x1, int* y1);
/* The stock character the selected row stands for (Chara_*), or -1. */
int  Cdm_PreviewStockId(void);
/* Set after anything was installed, removed or switched, so the host rescans. */
int  Cdm_TakeChanged(void);

#ifdef __cplusplus
}
#endif

#endif
