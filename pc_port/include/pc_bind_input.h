/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_input.h - raw SDL navigation edges and bind capture for the bind
 * screens. Reads the keyboard, mouse and PsyCross pad directly so it works
 * while the game's own input is not being fed.
 */
#ifndef PC_BIND_INPUT_H
#define PC_BIND_INPUT_H

#include <stddef.h>

enum { BN_UP = 0, BN_DOWN, BN_LEFT, BN_RIGHT, BN_OK, BN_BACK, BN_CLEAR, BN_COUNT };

typedef struct
{
    int      prev[BN_COUNT];
    unsigned repeatAt[BN_COUNT];
} BindNav;

void        BindNav_Reset(BindNav* n); /* whatever is held must be released first */
void        BindNav_Poll(BindNav* n, int pressed[BN_COUNT]);
void        BindCapture_Begin(int row, int col);
const char* BindCapture_Update(void); /* NULL waiting, "" cancelled, else bind name */
int         BindInput_PadConnected(void);

/* Saved key binds are SDL scancode names, i.e. US-layout key positions, so a
 * config means the same physical keys on every layout. These convert between
 * that and the key's label on the active layout (QWERTZ's Y is scancode "Z").
 * Names that are not keys (Mouse1, MouseWheelUp) pass through unchanged. */
void        BindInput_KeyLabel(const char* v, char* out, size_t n);
int         BindInput_ScancodeFromLabel(const char* label); /* SDL_Scancode */

/* A bind the port's own actions can test: an SDL scancode for a key name, or
 * BIND_MOUSE_BASE + the SDL button number for Mouse1..Mouse5. 0 = neither.
 * They used to resolve key names only, so a mouse button (the side buttons
 * most of all) set on Reload, Cycle Weapons and the rest did nothing. */
#define BIND_MOUSE_BASE 1000
int         BindInput_MouseCode(const char* name);
int         BindInput_Code(const char* name);
int         BindInput_CodeHeld(int code);
const char* BindInput_MouseName(int code); /* "Mouse4", or NULL for a key */

#endif
