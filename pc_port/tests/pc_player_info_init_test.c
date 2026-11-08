/* Exercise the real save-load initialization without starting SDL or the game. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/player.h"

s_SysWork g_SysWork;
s_GameWork g_GameWork;
s_AnimInfo HARRY_BASE_ANIM_INFOS[256];
s_Savegame* const g_SavegamePtr = &g_GameWork.savegame;
u8 g_Inventory_EquippedItem;
u8 g_Player_DisableControl;
s32 g_Player_GrabReleaseInputTimer;
s32 g_Player_LastWeaponSelected;

void SysWork_SavegameReadPlayer(void)
{
    g_SysWork.playerWork.player.health = g_SavegamePtr->playerHealth;
}

static void reset_save(u8 weapon)
{
    int i;

    memset(&g_GameWork, 0, sizeof(g_GameWork));
    memset(&g_SysWork, 0x55, sizeof(g_SysWork));
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        g_SavegamePtr->items[i].id_0 = (u8)InvItemId_Empty;
    }
    g_SavegamePtr->equippedWeapon = weapon;
    g_SavegamePtr->playerHealth = Q12(100.0f);
    g_SavegamePtr->inventorySlotCount = 8;
    g_SavegamePtr->gameDifficulty = GameDifficulty_Normal;
}

static void assert_unequipped(void)
{
    assert(g_SavegamePtr->equippedWeapon == InvItemId_Unequipped);
    assert(g_Inventory_EquippedItem == InvItemId_Unequipped);
    assert(g_SysWork.playerCombat.weaponAttack == NO_VALUE);
    assert(g_SysWork.playerCombat.weaponInventoryIdx == NO_VALUE);
    assert(g_SysWork.playerCombat.currentWeaponAmmo == 0);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 0);
}

int main(void)
{
    reset_save(InvItemId_Handgun);
    g_SavegamePtr->items[1].id_0 = InvItemId_Handgun;
    g_SavegamePtr->items[1].count_1 = 9;
    g_SavegamePtr->items[2].id_0 = InvItemId_HandgunBullets;
    g_SavegamePtr->items[2].count_1 = 30;
    Game_PlayerInfoInit();
    assert(g_SysWork.playerCombat.weaponInventoryIdx == 1);
    assert(g_SysWork.playerCombat.currentWeaponAmmo == 9);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 30);
    assert(g_SysWork.playerCombat.weaponAttack ==
           (u8)(InvItemId_Handgun + InvItemId_KitchenKnife));

    /* Observed crash: nine rounds in the handgun, no reserve-ammo entry. */
    reset_save(InvItemId_Handgun);
    g_SavegamePtr->items[0].id_0 = InvItemId_HealthDrink;
    g_SavegamePtr->items[0].count_1 = 2;
    g_SavegamePtr->items[1].id_0 = InvItemId_Handgun;
    g_SavegamePtr->items[1].count_1 = 9;
    g_SavegamePtr->items[2].id_0 = InvItemId_KitchenKnife;
    g_SavegamePtr->items[3].id_0 = InvItemId_Flashlight;
    g_SavegamePtr->items[4].id_0 = InvItemId_PocketRadio;
    Game_PlayerInfoInit();
    assert(g_SavegamePtr->equippedWeapon == InvItemId_Handgun);
    assert(g_SysWork.playerCombat.weaponInventoryIdx == 1);
    assert(g_SysWork.playerCombat.currentWeaponAmmo == 9);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 0);

    reset_save(InvItemId_Handgun);
    Game_PlayerInfoInit();
    assert_unequipped();

    reset_save(InvItemId_KitchenKnife);
    Game_PlayerInfoInit();
    assert_unequipped();

    reset_save(InvItemId_Handgun);
    g_SavegamePtr->items[INV_ITEM_COUNT_MAX - 1].id_0 = InvItemId_Handgun;
    g_SavegamePtr->items[INV_ITEM_COUNT_MAX - 1].count_1 = 3;
    g_SavegamePtr->items[0].id_0 = InvItemId_HandgunBullets;
    g_SavegamePtr->items[0].count_1 = 12;
    Game_PlayerInfoInit();
    assert(g_SysWork.playerCombat.weaponInventoryIdx == INV_ITEM_COUNT_MAX - 1);
    assert(g_SysWork.playerCombat.currentWeaponAmmo == 3);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 12);

    reset_save(InvItemId_Handgun);
    g_SavegamePtr->items[0].id_0 = InvItemId_Handgun;
    g_SavegamePtr->items[INV_ITEM_COUNT_MAX - 1].id_0 = InvItemId_HandgunBullets;
    g_SavegamePtr->items[INV_ITEM_COUNT_MAX - 1].count_1 = 15;
    Game_PlayerInfoInit();
    assert(g_SysWork.playerCombat.weaponInventoryIdx == 0);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 15);

    reset_save(InvItemId_KitchenKnife);
    g_SavegamePtr->items[INV_ITEM_COUNT_MAX - 1].id_0 = InvItemId_KitchenKnife;
    Game_PlayerInfoInit();
    assert(g_SavegamePtr->equippedWeapon == InvItemId_KitchenKnife);
    assert(g_SysWork.playerCombat.weaponInventoryIdx == INV_ITEM_COUNT_MAX - 1);
    assert(g_SysWork.playerCombat.totalWeaponAmmo == 0);

    reset_save(InvItemId_Unequipped);
    Game_PlayerInfoInit();
    assert_unequipped();

    puts("pc_player_info_init_test: 8 save-load cases passed");
    return 0;
}
