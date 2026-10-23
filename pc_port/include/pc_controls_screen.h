/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_controls_screen.h - native Controls (rebind) screen, Options > Controller
 * Config. Drawn with PSX primitives in Options' 224-line screen; input is raw
 * SDL (pc_bind_input.h) plus the mouse, so a bad bind cannot lock the player
 * out. Bind editing goes through pc_bind_core.h. Game thread only.
 */
#ifndef PC_CONTROLS_SCREEN_H
#define PC_CONTROLS_SCREEN_H

/* Edits the classic or the alternate-camera scheme, whichever the camera in
 * use selects (g_DebugThirdPersonCam). */
void Pc_ControlsScreen_Open(void);
int  Pc_ControlsScreen_IsOpen(void);
/* 1 while open, 0 once the player leaves. The screen stays open (and
 * drawable) until Pc_ControlsScreen_Close. */
int  Pc_ControlsScreen_Update(void);
void Pc_ControlsScreen_Close(void);
void Pc_ControlsScreen_Draw(void);

#endif
