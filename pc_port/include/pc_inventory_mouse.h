/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_INVENTORY_MOUSE_H
#define PC_INVENTORY_MOUSE_H

/* PC port: mouse control for the inventory screen. See pc_inventory_mouse.c.
 *
 * Call once from GameState_ItemScreens_Update before the stock inventory logic.
 * Wheel/swipe targets recycle models through the original slot loader and use
 * a short visual transition; command input still uses the stock controller bits. */
void Pc_Inventory_MouseUpdate(void);
int Pc_Inventory_ScrollAdvance(void);

#endif /* PC_INVENTORY_MOUSE_H */
