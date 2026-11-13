/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_INVENTORY_GUARD_H
#define PC_INVENTORY_GUARD_H

/* Slot a new item may be written to, or -1 when all 40 hold items. */
int Pc_Inventory_AppendSlot(unsigned char itemId, unsigned char itemCount);

/* One log line per item added, with the inventory as it stands afterwards. */
void Pc_Inventory_LogAdded(unsigned char itemId, unsigned char itemCount);

/* The whole inventory in one log line; `when` says what prompted it. */
void Pc_Inventory_LogState(const char* when);

/* One log line when the last of an item leaves the inventory. */
void Pc_Inventory_LogRemoved(unsigned char itemId);

/* Slot holding the equipped weapon, or -1. `site` names the caller in the log. */
int Pc_Inventory_WeaponSlot(const char* site);

#endif
