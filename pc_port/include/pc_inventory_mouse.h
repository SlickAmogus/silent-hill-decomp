#ifndef PC_INVENTORY_MOUSE_H
#define PC_INVENTORY_MOUSE_H

/* PC port: mouse control for the inventory screen. See pc_inventory_mouse.c.
 *
 * Call once from the top of Inventory_Logic, BEFORE Inventory_DirectionalInputSet()
 * — the injected controller bits have to be in place before that function derives
 * the g_Inventory_Is*Clicked/Held/Pulsed flags from them. */
void Pc_Inventory_MouseUpdate(void);

#endif /* PC_INVENTORY_MOUSE_H */
