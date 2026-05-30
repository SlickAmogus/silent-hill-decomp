#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "pc_combat.h"

/* Returns true if the player is pressing the manual-reload button (Triangle/Z)
 * while aiming a gun weapon with reserve ammo available.
 *
 * The PSX game had no manual reload input — reload triggered automatically.
 * On PC we add Triangle (Z key) as a manual reload while still respecting
 * the original auto-reload on fire-with-empty-clip path. */
bool PC_PlayerManualReloadRequested(void)
{
    return g_SysWork.playerCombat.weaponAttack >= WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap) &&
           g_SysWork.playerCombat.totalWeaponAmmo != 0 &&
           INVENTORY_ITEM_GROUP(g_SavegamePtr->equippedWeapon_AA) == InvItemGroup_GunWeapons &&
           (g_Controller0->btnsClicked_10 & ControllerFlag_Triangle);
}
