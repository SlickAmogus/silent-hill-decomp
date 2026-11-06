/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_RESET_SETTINGS_H
#define PC_RESET_SETTINGS_H

/* Reset all settings to their defaults, from PC Options and the quick menu:
 * R on a keyboard, both triggers on a controller, or the circled-R touch
 * button. Every route opens the same Yes/No confirm (pc_confirm_dialog.c).
 * The on-screen hint names whichever of those the player is using. */

enum
{
    PC_INPUT_KEYBOARD = 0, /* keyboard and mouse */
    PC_INPUT_PAD      = 1, /* a game controller */
    PC_INPUT_TOUCH    = 2  /* the touch overlay (Android, iOS, phone browsers) */
};

/* Once per frame while a menu that offers the reset is up. */
void        Pc_ResetSettings_Track(void);
int         Pc_ResetSettings_InputMode(void);
/* "Press R to reset" or "Press triggers to reset". */
const char* Pc_ResetSettings_Hint(void);
/* 1 on the frame a reset was asked for: R pressed, both triggers pulled, or
 * the touch button tapped since the last call. */
int         Pc_ResetSettings_RequestEdge(void);
/* From the touch overlay's circled-R button. */
void        Pc_ResetSettings_TouchRequest(void);

#endif
