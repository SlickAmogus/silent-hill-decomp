/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NATIVE_MENU_H
#define PC_NATIVE_MENU_H

#include <stddef.h>

/* Renderer-free core for the PSX-drawn native menus: input snapshot, scroll
 * window, FONT16 text fitting and a row-page state machine. No game headers. */

typedef struct { int up, down, left, right;      /* pulsed (repeat) */
                 int confirm, cancel, clear;       /* press edges */
                 int wheel;                        /* notches, + = up */
                 int mouseOk, mouseX, mouseY;      /* 320x240 text space */
                 int mouseMoved, mouseLeft, mouseRight; } NmInput;

typedef struct { int count, visible, top, sel; } NmScroll;
void NmScroll_Init(NmScroll* s, int count, int visible);
void NmScroll_SetCount(NmScroll* s, int count);
int  NmScroll_Move(NmScroll* s, int delta, int wrap);
void NmScroll_Select(NmScroll* s, int index);
void NmScroll_Wheel(NmScroll* s, int notches);
int  NmScroll_CanUp(const NmScroll* s);
int  NmScroll_CanDown(const NmScroll* s);

typedef int (*NmMeasureFn)(const char* s);
#define NM_LINE_MAX 128
size_t Nm_SanitizeGlyphs(const char* utf8, char* out, size_t outSize, int upper);
int    Nm_FitEllipsis(const char* in, char* out, size_t outSize, int maxWidth, NmMeasureFn m);
int    Nm_WrapLines(const char* in, char lines[][NM_LINE_MAX], int maxLines, int maxWidth, NmMeasureFn m);
void   Nm_FormatDate(int day, int month, int year, int hour, int minute, char* out, size_t outSize);

typedef enum { NM_ROW_ACTION, NM_ROW_VALUE, NM_ROW_INFO } NmRowKind;
typedef struct NmRow {
    const char* label;
    NmRowKind   kind;
    const char* (*value)(void* ctx);
    void        (*step)(void* ctx, int dir);
    void        (*activate)(void* ctx);
} NmRow;
typedef struct { const char* heading; const NmRow* rows; int rowCount; void* ctx; NmScroll scroll; } NmPage;
typedef enum { NM_EV_NONE, NM_EV_MOVED, NM_EV_STEPPED, NM_EV_ACTIVATED, NM_EV_BACK, NM_EV_ERROR } NmEvent;
void    NmPage_Init(NmPage* p, int visibleRows);
NmEvent NmPage_Update(NmPage* p, const NmInput* in, int hoverRow);

typedef enum { NM_CONFIRM_PENDING, NM_CONFIRM_YES, NM_CONFIRM_NO } NmConfirmResult;
typedef struct { int open, yes; } NmConfirm;
void            NmConfirm_Open(NmConfirm* c);
NmConfirmResult NmConfirm_Update(NmConfirm* c, const NmInput* in);

/* Title menu: visible entry ids (bit i of visibleFlags) in order; returns n.
 * Slot base bottom-aligns a list that fits the window, as retail does. */
int Nm_CompactRows(unsigned visibleFlags, int entryCount, int order[]);
int Nm_SlotBase(int n, int windowSlots);
/* Compact index drawn on a window slot, or -1 when that slot is empty. */
int Nm_SlotToIndex(int slot, int base, int top, int n);

#endif
