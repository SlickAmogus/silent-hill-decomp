#include <SDL2/SDL.h>
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/player.h"
#include "pc_combat.h"

extern const unsigned char* g_sdlKeyboardState;

/* Returns true on the frame `sdlScancode` transitions 0→1.
 *
 * Frame-stable: prev-state is sampled at most once per VBlank, so multiple
 * callers in the same frame all see the same rising-edge result. Without
 * this, a press that opens the inventory in gameplay state would also fire
 * a "rising edge" again the first time the inventory state queries it
 * (since each fresh slot starts with prev=0), instantly closing it. */
bool PC_KeyboardKeyClicked(int sdlScancode)
{
    #define PC_KEY_CACHE_SIZE 8
    static int  s_keys[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_prev[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_edge[PC_KEY_CACHE_SIZE]   = {0};
    static s32  s_frame[PC_KEY_CACHE_SIZE]  = {0};
    static int  s_count                     = 0;
    static bool s_initFrames                = false;

    if (!g_sdlKeyboardState) return false;

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_keys[i] == sdlScancode) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count >= PC_KEY_CACHE_SIZE) return false;
        slot = s_count++;
        s_keys[slot]  = sdlScancode;
        /* Seed prev with current held state — if the key is already down
         * when first queried, that's NOT a rising edge. */
        s_prev[slot]  = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot]  = false;
        s_frame[slot] = g_VBlanks;
        return false;
    }

    /* Resample only once per frame (per slot). Other call sites in the
     * same frame get the cached edge result. */
    if (s_frame[slot] != g_VBlanks) {
        bool nowHeld = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot] = nowHeld && !s_prev[slot];
        s_prev[slot] = nowHeld;
        s_frame[slot] = g_VBlanks;
    }
    return s_edge[slot];
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
           INV_ITEM_GROUP(g_SavegamePtr->equippedWeapon) == InvItemGroup_GunWeapons;
}
