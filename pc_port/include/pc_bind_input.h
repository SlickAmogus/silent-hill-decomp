/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_input.h - raw SDL navigation edges and bind capture for the bind
 * screens. Reads the keyboard, mouse and PsyCross pad directly so it works
 * while the game's own input is not being fed.
 */
#ifndef PC_BIND_INPUT_H
#define PC_BIND_INPUT_H

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

#endif
