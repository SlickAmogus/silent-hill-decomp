/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_core.h - renderer-free bind table logic shared by the GL bind panel
 * and the native Controls screen: rows, values, set, reset, duplicates.
 * Launcher compatibility rules are documented in pc_bind_panel.c.
 */
#ifndef PC_BIND_CORE_H
#define PC_BIND_CORE_H

#include <stddef.h>

enum { BC_KB1 = 0, BC_KB2, BC_PAD1, BC_PAD2, BC_COLS };

#define BCF_STICK   1 /* controller side is the stick / d-pad, not a bind */
#define BCF_ALTCAM  2 /* only does anything with an alternate camera */
#define BCF_GLOBAL  4 /* one value shared by both schemes */
#define BCF_MOUSE   8 /* keyboard columns accept mouse buttons */
#define BCF_WHEEL  16 /* ...and the mouse wheel */

typedef struct
{
    const char* label;
    const char* key[BC_COLS];
    int         flags;
} BcRow;

int          BindCore_RowCount(void);
const BcRow* BindCore_Row(int row);
void         BindCore_SetScheme(int scheme); /* 0 classic, 1 altcam */
int          BindCore_Scheme(void);
int          BindCore_Unbound(const char* v);
const char*  BindCore_Value(int row, int col);
int          BindCore_RowEnabled(int row);
int          BindCore_CellUsable(int row, int col);
int          BindCore_IsDuplicate(int row, int col);
void         BindCore_Set(int row, int col, const char* value);
void         BindCore_ResetScheme(void);
void         BindCore_FormatRebindToast(int row, int col, char* out, size_t n);
/* Short on-screen name for a keyboard-column value. Display only: the saved
 * value is never changed. */
void         BindCore_DisplayKey(const char* v, char* out, size_t n);

#endif
