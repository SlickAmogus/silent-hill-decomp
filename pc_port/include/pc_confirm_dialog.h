/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_confirm_dialog.h - modal Yes/No message box.
 *
 * An overlay modelled on the randomizer settings panel (pc_rando_settings.c):
 * its own GL program + stb_truetype text, drawn from the post-capture hook so it
 * always sits above the PSX frame, updated on the game thread. Navigable by
 * keyboard, controller and mouse. One dialog at a time.
 */
#ifndef PC_CONFIRM_DIALOG_H
#define PC_CONFIRM_DIALOG_H

/* 1 while the dialog owns input (open or animating shut). The caller must yield
 * its own input handling in that case. */
int  Pc_ConfirmDialog_IsOpen(void);

/* Open with a short title band and a one-line message. Strings are copied.
 * Selection starts on "No". */
void Pc_ConfirmDialog_Open(const char* title, const char* message);

enum
{
    PC_CONFIRM_NONE = 0,
    PC_CONFIRM_YES,
    PC_CONFIRM_NO
};

/* Per-frame input while open (game thread). left/right move the selection,
 * confirm activates it, cancel dismisses as "No". Mouse is read internally.
 * Returns PC_CONFIRM_YES / PC_CONFIRM_NO on the frame the dialog is answered,
 * else PC_CONFIRM_NONE. */
int  Pc_ConfirmDialog_Update(int left, int right, int confirm, int cancel);

/* Per-frame draw from the post-capture GL hook (cheap no-op while closed). */
void Pc_ConfirmDialog_Draw(void);

#endif /* PC_CONFIRM_DIALOG_H */
