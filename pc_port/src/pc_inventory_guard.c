/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The original inventory code keeps two cached numbers beside the 40-slot
 * array and never checks either against it:
 *
 *   inventorySlotCount   where the next item is appended
 *   weaponInventoryIdx   where the equipped weapon's loaded rounds are written
 *
 * A stale count lands the new item on top of a live one (or, with all 40
 * counted, four bytes past the array). A stale weapon index writes the loaded
 * round count into some other item, and the recount deletes any non-gun whose
 * count reads zero: an item gone with nothing to show why. The PlayStation had
 * a single way into the inventory and never desynchronised them; the port has
 * several (quick weapon switch, quick heal, the console, shared co-op items),
 * and the starting-loadout bug of 2026-08 was exactly a stale count. These
 * checks make a stale number harmless, and the log lines mean the next missing
 * item can be traced from the player's log instead of from guesswork. */
#include <stdio.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "sh_log.h"
#include "pc_inventory_guard.h"

static int Inv_SlotFree(int slot)
{
    u8 id = g_SavegamePtr->items[slot].id_0;

    return id == (u8)InvItemId_Empty || id == InvItemId_Unequipped;
}

static void Inv_Describe(char* out, int cap)
{
    int n = 0, i;

    out[0] = '\0';
    for (i = 0; i < INV_ITEM_COUNT_MAX && n < cap - 12; i++)
    {
        if (!Inv_SlotFree(i))
        {
            n += snprintf(out + n, (size_t)(cap - n), " %d:%d", (int)g_SavegamePtr->items[i].id_0,
                          (int)g_SavegamePtr->items[i].count_1);
        }
    }
}

int Pc_Inventory_AppendSlot(unsigned char itemId, unsigned char itemCount)
{
    int slot = g_SavegamePtr->inventorySlotCount;
    int want = slot;

    if (slot >= 0 && slot < INV_ITEM_COUNT_MAX && Inv_SlotFree(slot))
    {
        return slot;
    }

    /* Any free slot gives the same result: the recount that follows every add
     * sorts and packs the whole array. */
    for (slot = 0; slot < INV_ITEM_COUNT_MAX && !Inv_SlotFree(slot); slot++);

    if (slot == INV_ITEM_COUNT_MAX)
    {
        SH_DBG("[INV] item %d x%d NOT added: all %d slots hold items", (int)itemId, (int)itemCount, INV_ITEM_COUNT_MAX);
        return -1;
    }

    SH_DBG("[INV] append slot %d was not free (holds item %d x%d); item %d goes to slot %d instead", want,
           (want >= 0 && want < INV_ITEM_COUNT_MAX) ? (int)g_SavegamePtr->items[want].id_0 : -1,
           (want >= 0 && want < INV_ITEM_COUNT_MAX) ? (int)g_SavegamePtr->items[want].count_1 : -1, (int)itemId, slot);
    return slot;
}

void Pc_Inventory_LogAdded(unsigned char itemId, unsigned char itemCount)
{
    char inv[320];
    int  i, held = 0;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == itemId)
        {
            held = 1;
            break;
        }
    }

    Inv_Describe(inv, (int)sizeof(inv));
    SH_DBG("[INV] +%d x%d%s | map %d, %d slots:%s", (int)itemId, (int)itemCount,
           held ? "" : " BUT IT IS NOT IN THE INVENTORY", (int)g_SavegamePtr->mapIdx,
           (int)g_SavegamePtr->inventorySlotCount, inv);
}

void Pc_Inventory_LogState(const char* when)
{
    char inv[320];

    Inv_Describe(inv, (int)sizeof(inv));
    SH_DBG("[INV] %s | map %d, %d slots, weapon %d in slot %d:%s", when, (int)g_SavegamePtr->mapIdx,
           (int)g_SavegamePtr->inventorySlotCount, (int)g_SavegamePtr->equippedWeapon,
           (int)g_SysWork.playerCombat.weaponInventoryIdx, inv);
}

void Pc_Inventory_LogRemoved(unsigned char itemId)
{
    SH_DBG("[INV] -%d (last one) | map %d", (int)itemId, (int)g_SavegamePtr->mapIdx);
}

int Pc_Inventory_WeaponSlot(const char* site)
{
    static int s_loggedIdx = -2;
    static int s_loggedId  = -2;
    int        idx         = g_SysWork.playerCombat.weaponInventoryIdx;
    u8         equipped    = g_SavegamePtr->equippedWeapon;
    int        i;

    if (idx >= 0 && idx < INV_ITEM_COUNT_MAX && !Inv_SlotFree(idx))
    {
        u8 held = g_SavegamePtr->items[idx].id_0;

        /* The item screen names the weapon in its own variable while it is
         * open and copies it to the savegame on the way out. */
        if (held == equipped || held == g_Inventory_EquippedItem)
        {
            return idx;
        }
    }
    else if (idx == NO_VALUE && equipped == InvItemId_Unequipped)
    {
        return -1;
    }

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (equipped != InvItemId_Unequipped && g_SavegamePtr->items[i].id_0 == equipped)
        {
            break;
        }
    }

    if (s_loggedIdx != idx || s_loggedId != (int)equipped)
    {
        s_loggedIdx = idx;
        s_loggedId  = (int)equipped;
        SH_DBG("[INV] equipped-weapon slot was stale at %s: cached %d holds item %d, weapon %d is %s%d", site, idx,
               (idx >= 0 && idx < INV_ITEM_COUNT_MAX) ? (int)g_SavegamePtr->items[idx].id_0 : -1, (int)equipped,
               (i < INV_ITEM_COUNT_MAX) ? "in slot " : "not held ", (i < INV_ITEM_COUNT_MAX) ? i : 0);
    }

    if (i == INV_ITEM_COUNT_MAX)
    {
        return -1;
    }

    g_SysWork.playerCombat.weaponInventoryIdx = (s8)i;
    return i;
}
