#include "game.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#include "map_registry.h"
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "main/fsqueue.h"

void GameBoot_SavegameInitialize(s8 overlayId, s32 difficulty) // 0x800350BC
{
    s32  i;
    s32* ovlEnemyStatesPtr;

    bzero(g_SavegamePtr, sizeof(s_Savegame));

    g_SavegamePtr->mapOverlayId_A4 = overlayId;

    difficulty = CLAMP(difficulty, GameDifficulty_Easy, GameDifficulty_Hard);

    ovlEnemyStatesPtr = g_SavegamePtr->ovlEnemyStates;

    g_SavegamePtr->gameDifficulty_260 = difficulty;
    g_SavegamePtr->paperMapIdx_A9     = PaperMapIdx_OldTown;

    // Define all enemies from an overlay as alive.
    // Odd code. Possibly a hack.
    for (i = 0; i < Chara_Count; i++)
    {
        ovlEnemyStatesPtr[44] = NO_VALUE;
        ovlEnemyStatesPtr--;
    }

    Game_SavegameResetPlayer();
}

void GameBoot_PlayerInit(void) // 0x80035178
{
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: WorldGfx_MapInit... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels[1]);
#endif
    WorldGfx_MapInit();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: CharaModel_AllModelsFree... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels[1]);
#endif
    CharaModel_AllModelsFree();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: Item_HeldItemModelFree... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels[1]);
#endif
    Item_HeldItemModelFree();
    Anim_BoneInit(FS_BUFFER_0, g_SysWork.playerBoneCoords); // Load player anim file?
    WorldGfx_PlayerModelProcessLoad();

#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: setting field_229C... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels[1]);
#endif
    g_SysWork.field_229C = NO_VALUE;

    if ((g_SavegamePtr->itemToggleFlags_AC >> 1) & (1 << 0)) // `& ItemToggleFlag_FlashlightOff`
    {
        Game_TurnFlashlightOff();
    }
    else
    {
        Game_TurnFlashlightOn();
    }

    g_CharaTypeAnimInfo[0].animBufferSize2_10 = 0x2E630;
    g_CharaTypeAnimInfo[0].animBufferSize1_C  = 0x2E630;
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: Game_PlayerInfoInit...");
#endif
    Game_PlayerInfoInit();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: done");
#endif
}

void GameBoot_MapLoad(s32 mapIdx) // 0x8003521C
{
#ifdef SH_PC_PORT
    /* fflush after every SH_DBG in this function. The death-transition
     * crash (mapIdx=1 from end-of-alley sequence) blows up somewhere
     * inside this function but the 64KB _IOLBF buffer + missing
     * SetUnhandledExceptionFilter means we lose every SH_DBG between
     * the SysState_LoadArea snapshot and the segfault. Forcing a flush
     * per line costs us perf only on this one rare path; localizing
     * the crash is worth it. */
    SH_DBG("[SH] GameBoot_MapLoad: ENTER mapIdx=%d (%s)", mapIdx, MapRegistry_GetName(mapIdx));
    fflush(g_ShDebugLog);

    /* PC-only convenience: when entering a non-tutorial map with an empty
     * weapon slot (equippedWeapon_AA == 0 = Unequipped), provide the basic
     * combat loadout. equippedWeapon_AA is the right gate because:
     *   - GameBoot_SavegameInitialize bzero's it to 0 on a New Game, so
     *     my code fires on the post-bzero map load.
     *   - After my auto-equip sets it to InvItemId_Handgun, subsequent
     *     room transitions see != 0 and skip — no re-fire on transitions.
     *   - Loaded saves carry their own equipped weapon (Handgun or
     *     whatever), so we won't overwrite a real save's equipment.
     *   - The earlier static-flag approach broke when an auto-load
     *     savegame state ran BEFORE the user clicked New Game: auto-load's
     *     map load consumed the static, then the user's New Game bzero
     *     wiped the items but the static stayed set so no re-fire. */
    SH_DBG("[AUTO-EQUIP-CHECK] mapIdx=%d processFlags=0x%x inventorySlotCount=%d equippedWeapon=%d",
           mapIdx, (unsigned)g_SysWork.processFlags,
           (int)g_SavegamePtr->inventorySlotCount_AB,
           (int)g_SavegamePtr->equippedWeapon_AA);
    fflush(g_ShDebugLog);
    /* Clear UnkSysFlag_4 on every non-tutorial map entry. This flag is
     * set by map2_s00.c:1948 (gated on EventFlag_146 / WaterWorks cutscene
     * progression) and gates all enemy spawning in Game_NpcRoomInitSpawn.
     * On a fresh PC playthrough we can't reach the cutscene that clears
     * it via the normal path, so streets stay enemy-free. Force-clearing
     * here unblocks spawning on any non-tutorial map. */
    if (mapIdx != MapIdx_MAP0_S00 && mapIdx != MapIdx_MAP0_S01)
    {
        g_SysWork.flags_22A4 &= ~UnkSysFlag_4;
    }

    if (mapIdx != MapIdx_MAP0_S00 && mapIdx != MapIdx_MAP0_S01 &&
        g_SavegamePtr->equippedWeapon_AA == InvItemId_Unequipped)
    {
        s_InventoryItem* items = g_SavegamePtr->items_0;
        items[0].id_0 = InvItemId_Flashlight;     items[0].count_1 = 1;
        items[1].id_0 = InvItemId_PocketRadio;    items[1].count_1 = 1;
        items[2].id_0 = InvItemId_KitchenKnife;   items[2].count_1 = 1;
        items[3].id_0 = InvItemId_Handgun;        items[3].count_1 = 1;
        items[4].id_0 = InvItemId_HandgunBullets; items[4].count_1 = 15;
        g_SavegamePtr->inventorySlotCount_AB = 5;

        g_SavegamePtr->equippedWeapon_AA = InvItemId_Handgun;
        g_SavegamePtr->itemToggleFlags_AC |= ItemToggleFlag_RadioOn;
        g_SavegamePtr->itemToggleFlags_AC &= ~ItemToggleFlag_FlashlightOff;

        g_SysWork.playerCombat.weaponAttack         = WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap);
        g_SysWork.playerCombat.weaponInventoryIdx   = 3;
        g_SysWork.playerCombat.totalWeaponAmmo      = 15;

        SH_DBG("[AUTO-EQUIP] FIRED on non-tutorial map %d: handgun+15+knife+radio+flashlight, equipped handgun",
               mapIdx);
        fflush(g_ShDebugLog);
    }
    /* Switch the active map overlay header to the requested map. */
    MapRegistry_Load(mapIdx);
    SH_DBG("[SH] GameBoot_MapLoad: post-MapRegistry_Load");
    fflush(g_ShDebugLog);
    /* Still read the overlay file — on PC this is a no-op but keeps the
     * filesystem queue state consistent. */
#endif
    Fs_QueueStartRead(FILE_VIN_MAP0_S00_BIN + mapIdx, g_OvlDynamic);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: pre Map_EffectTexturesLoad");
    fflush(g_ShDebugLog);
#endif
    Map_EffectTexturesLoad(mapIdx);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: pre GameFs_PlayerMapAnimLoad");
    fflush(g_ShDebugLog);
#endif
    GameFs_PlayerMapAnimLoad(mapIdx);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: post-PlayerMapAnimLoad processFlags=0x%X weaponAttack=%d",
           (unsigned)g_SysWork.processFlags, (int)g_SysWork.playerCombat.weaponAttack);
    fflush(g_ShDebugLog);
#endif

    // If the player spawns in the map with a weapon equipped (either because it's a demo
    // or because the player saved the game with a weapon equipped), this and the next function
    // make it appear and allocate its data.
    // @note This code has some special functionallity if the player spawns without an equipped weapon.
    if (g_SysWork.processFlags & (ProcessFlag_NewGame | ProcessFlag_LoadSave |
                                       ProcessFlag_Continue | ProcessFlag_BootDemo))
    {
#ifdef SH_PC_PORT
        SH_DBG("[SH] GameBoot_MapLoad: pre WorldGfx_PlayerPrevHeldItem");
        fflush(g_ShDebugLog);
#endif
        WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat);
#ifdef SH_PC_PORT
        SH_DBG("[SH] GameBoot_MapLoad: post-PlayerPrevHeldItem");
        fflush(g_ShDebugLog);
#endif
    }

#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: pre Gfx_PlayerHeldItemAttach weap=%d", (int)g_SysWork.playerCombat.weaponAttack);
    fflush(g_ShDebugLog);
#endif
    Gfx_PlayerHeldItemAttach(g_SysWork.playerCombat.weaponAttack);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: complete");
    fflush(g_ShDebugLog);
#endif
}
