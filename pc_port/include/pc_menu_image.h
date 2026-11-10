/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Images from gamedata/image drawn inside the native menus, as textured prims
 * in the menu's own ordering table so the game's text can sit on top of them. */
#ifndef PC_MENU_IMAGE_H
#define PC_MENU_IMAGE_H

enum { PC_MENU_IMAGE_BUTTON_WIDE, PC_MENU_IMAGE_WARNING, PC_MENU_IMAGE_CHECKMARK, PC_MENU_IMAGE_COUNT };

/* Corners in the caller's primitive space (already offset for the menu OT).
 * shade is the vertex colour, 128 = the image as drawn. Returns 0 when the
 * file is missing, so the caller can fall back. */
int Pc_MenuImage_Draw(int id, int x0, int y0, int x1, int y1, int bucket, int shade);

#endif
