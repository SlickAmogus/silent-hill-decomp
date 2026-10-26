/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_menu.h - the in-game Conscious menu (key_menu, M by default),
 * drawn with the native menu layer like Controls and Achievements. While it
 * is open the game is frozen and its pad input is swallowed.
 */
#ifndef PC_CONSCIOUS_MENU_H
#define PC_CONSCIOUS_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

int  Pc_ConsciousMenu_IsOpen(void);
/* Once a frame in the input stage, before the pad is masked: the open key,
 * and navigation while open. */
void Pc_ConsciousMenu_Update(void);
/* After the game state's update, so it lands in this frame's ordering table. */
void Pc_ConsciousMenu_Draw(void);

#ifdef __cplusplus
}
#endif

#endif
