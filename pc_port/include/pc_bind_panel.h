/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_panel.h - in-game controls panel (Options > Controller Config).
 *
 * Rebinds keyboard, mouse and controller inputs (controller only on iOS and
 * Android) for the control scheme that
 * matches the camera in use when it opens (classic, or the alternate-camera
 * scheme for Thirdperson / Over-the-Shoulder / First Person). Writes the same
 * config keys and value names as the launcher's Controls window, so either
 * one can edit what the other wrote.
 *
 * Self-contained GL overlay in the pc_confirm_dialog.c style: Update runs on
 * the game thread from the host screen and reads raw SDL input (so a bad bind
 * can never lock the player out of the panel), Draw runs from the post-capture
 * hook in dbg_overlay.c.
 */
#ifndef PC_BIND_PANEL_H
#define PC_BIND_PANEL_H

int  Pc_BindPanel_IsOpen(void);
void Pc_BindPanel_Open(void);
/* Starts the close fade (a pending rebind is dropped). For a host that is
 * going away underneath the panel and would stop feeding it input. */
void Pc_BindPanel_Close(void);

/* A phone edits only a paired controller's buttons, so there the panel needs
 * one connected. CanOpen answers that, and on a refusal beeps and shows a
 * "No controllers detected" toast itself; it is always 1 on desktop. TryOpen
 * is CanOpen then Open, returning whether the panel opened. */
int  Pc_BindPanel_CanOpen(void);
int  Pc_BindPanel_TryOpen(void);

/* Per-frame input while open. Returns 1 while the panel owns input (the host
 * should skip its own handling), 0 once it has fully closed. */
int  Pc_BindPanel_Update(void);

void Pc_BindPanel_Draw(void);

#endif /* PC_BIND_PANEL_H */
