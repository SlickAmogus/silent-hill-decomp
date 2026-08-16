/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: touchscreen controls.
 *
 * Menus already work by touch without this: SDL synthesizes mouse events from
 * taps, so pc_mouse_cursor's hover/click path drives them. This covers the part
 * that has no mouse equivalent -- moving, looking and fighting -- by presenting
 * touch to the rest of the engine as an ordinary analog pad, so nothing
 * downstream of libpad needs to know a finger is involved.
 *
 * Scheme (landscape):
 *   left side    drag   floating movement stick, appears where the thumb lands
 *   left side    push   past ~85% deflection also holds Run
 *   right side   drag   camera look, as a rate (drag speed = stick deflection)
 *   either side  tap    Action -- attack with a weapon ready, interact without
 *   buttons      tap    Aim (hold), Item, Map, Start
 *
 * Buttons emit whatever the player's own controller config has bound to each
 * action, so a rebind carries over instead of being silently ignored.
 */
#ifndef PC_TOUCH_H
#define PC_TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

/** Poll fingers and rebuild the virtual pad. Called once per pad update, from
 *  PsyX_Pad_InternalPadUpdates, so it cannot land out of order with the read. */
void Pc_Touch_Update(void);

/** 1 when touch is enabled, present, and the game is in settled gameplay --
 *  i.e. when the virtual pad should be driving. Menus stay on the mouse path. */
int Pc_Touch_Active(void);

/** Virtual pad state. `word` is an active-low PSX button word (a bit CLEAR is
 *  pressed) to AND into the pad; the analog bytes are PSX-style, 128 centered. */
void Pc_Touch_GetPad(unsigned short* word,
                     unsigned char* rightX, unsigned char* rightY,
                     unsigned char* leftX,  unsigned char* leftY);

/** 1 if a finger has touched the screen recently. Used to suppress the drawn
 *  mouse cursor, which represents a pointer a touchscreen does not have. */
int Pc_Touch_UsedRecently(void);

/** On-screen controls. Drawn from the same overlay pass as the crosshair. */
void Pc_Touch_Draw(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_TOUCH_H */
