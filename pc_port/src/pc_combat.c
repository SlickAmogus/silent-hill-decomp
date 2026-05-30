#include <SDL2/SDL.h>
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "pc_combat.h"

extern const unsigned char* g_sdlKeyboardState;

/* Returns true the frame `sdlScancode` transitions 0→1. Tracks previous
 * state per scancode in a small static cache. Used by PC convenience
 * hotkeys that live outside the PSX controller mapping. */
bool PC_KeyboardKeyClicked(int sdlScancode)
{
    #define PC_KEY_CACHE_SIZE 8
    static int  s_keys[PC_KEY_CACHE_SIZE] = {0};
    static bool s_prev[PC_KEY_CACHE_SIZE] = {0};
    static int  s_count                   = 0;

    if (!g_sdlKeyboardState) return false;

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_keys[i] == sdlScancode) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count >= PC_KEY_CACHE_SIZE) return false;
        slot = s_count++;
        s_keys[slot] = sdlScancode;
        s_prev[slot] = false;
    }

    bool nowHeld   = g_sdlKeyboardState[sdlScancode] != 0;
    bool risingEdge = nowHeld && !s_prev[slot];
    s_prev[slot] = nowHeld;
    return risingEdge;
}

/* Returns true on the rising edge of the manual-reload key (R) while a gun
 * weapon is equipped with reserve ammo available.
 *
 * The PSX game had no manual reload input — reload triggered automatically
 * on a fire-with-empty-clip. PC adds a dedicated R-key reload as a
 * convenience, bound outside the PSX controller mapping (so all PSX buttons
 * keep their original semantics: Triangle still opens map, Square still
 * runs, etc.). */
bool PC_PlayerManualReloadRequested(void)
{
    return PC_KeyboardKeyClicked(SDL_SCANCODE_R) &&
           g_SysWork.playerCombat.weaponAttack >= WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap) &&
           g_SysWork.playerCombat.totalWeaponAmmo != 0 &&
           INVENTORY_ITEM_GROUP(g_SavegamePtr->equippedWeapon_AA) == InvItemGroup_GunWeapons;
}
