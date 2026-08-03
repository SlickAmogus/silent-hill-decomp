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
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "main/fsqueue.h"
#ifdef SH_PC_PORT
#include "main/fileinfo.h"          /* g_GameRegion, g_FileTable — PAL font/language hooks */
#include "bodyprog/item_screens.h"  /* func_8004F190 — inventory sort + slot recount */
#endif

void GameBoot_SavegameInitialize(s8 overlayId, s32 difficulty) // 0x800350BC
{
    s32  i;
    s32* ovlEnemyStatesPtr;

    bzero(g_SavegamePtr, sizeof(s_Savegame));

    g_SavegamePtr->mapIdx = overlayId;

    difficulty = CLAMP(difficulty, GameDifficulty_Easy, GameDifficulty_Hard);

    ovlEnemyStatesPtr = g_SavegamePtr->ovlEnemyStates;

    g_SavegamePtr->gameDifficulty = difficulty;
    g_SavegamePtr->paperMapIdx     = PaperMapIdx_OldTown;

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
    WorldGfx_MapInit();
    CharaModel_AllModelsFree();
    Item_HeldItemModelFree();
    Anim_BoneInit(FS_BUFFER_0, g_SysWork.playerBoneCoords); // Load player anim file?
    WorldGfx_PlayerModelProcessLoad();

    g_SysWork.unused_229C = NO_VALUE;

    if ((g_SavegamePtr->itemToggleFlags >> 1) & (1 << 0)) // `& ItemToggleFlag_FlashlightOff`
    {
        Game_TurnFlashlightOff();
    }
    else
    {
        Game_TurnFlashlightOn();
    }

    g_CharaModelAnimsData[0].activeSize = 0x2E630;
    g_CharaModelAnimsData[0].allocSize  = 0x2E630;
    Game_PlayerInfoInit();
}

#ifdef SH_PC_PORT
/* True only when all 40 slots hold the canonical empty marker. `id_0` is u8 and
 * `InvItemId_Empty` is NO_VALUE (-1), so an empty slot reads back as 0xFF and
 * never as 0 — hence the cast, the same idiom item_screens_2.c uses.
 * `inventorySlotCount` cannot answer this: Game_SavegameResetPlayer leaves it at
 * 8 with all 40 slots empty, and Game_PlayerInfoInit clamps it to [8,40]
 * whatever the real occupancy is, so it carries no occupancy information. */
static int Pc_SavegameInventoryIsEmpty(void)
{
    s32 i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 != (u8)InvItemId_Empty)
        {
            return 0;
        }
    }

    return 1;
}
#endif

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
    fflush(g_ShDebugLog);

    /* PC-only convenience with no PSX counterpart: a New Game booted straight
     * onto a config-selected map (config `map` key / console MAP) skips the
     * intro that normally hands Harry his starting gear, so hand it over here.
     *
     * The gate is New Game AND a provably empty inventory. It used to be
     * `equippedWeapon == InvItemId_Unequipped`, which is NOT New-Game-exclusive:
     * the inventory screen's Unequip clears that field mid-playthrough
     * (item_screens_3.c, Inventory_PlayerItemScroll case 6), and this function
     * runs on every area transition AND every save load. The block below
     * OVERWRITES items[0..4] rather than adding, so it destroyed live
     * inventories at the next door. Worse, func_8004F190 keeps the inventory
     * sorted by D_80025EB0, where the health stacks sort first and a gun with
     * its ammo right after - so slots 0..4 are exactly the health items plus
     * the gun and its whole spare stack, and the damage landed precisely on
     * healing items and ammo. */
    SH_DBG("[AUTO-EQUIP-CHECK] mapIdx=%d processFlags=0x%x inventorySlotCount=%d equippedWeapon=%d invEmpty=%d",
           mapIdx, (unsigned)g_SysWork.processFlags,
           (int)g_SavegamePtr->inventorySlotCount,
           (int)g_SavegamePtr->equippedWeapon,
           Pc_SavegameInventoryIsEmpty());
    fflush(g_ShDebugLog);
    /* Clear SysFlag_NoEnemySpawn on every non-tutorial map entry. This flag is
     * set by map2_s00.c:1948 (gated on EventFlag_146 / WaterWorks cutscene
     * progression) and gates all enemy spawning in Game_NpcRoomInitSpawn.
     * On a fresh PC playthrough we can't reach the cutscene that clears
     * it via the normal path, so streets stay enemy-free. Force-clearing
     * here unblocks spawning on any non-tutorial map.
     *
     * map6_s04 (Cybil carousel) is included: its approach area has ambient
     * larval stalkers / grey children that must spawn. The boss-spawn hazard
     * (ambient enemies filling the 3-slot NPC cap before MonsterCybil's
     * Chara_Spawn) is handled at the boss cutscene itself, which now frees the
     * cap via GameBoot_NpcClear right after it sets NoEnemySpawn — see
     * map6_s04_2.c. Blanket-blocking the whole map here killed the approach
     * enemies, so do NOT special-case it. */
    /* Force-clearing the spawn gate rewrites state an attract demo just installed
     * from its recorded savegame - it changes which enemies the recording meets -
     * so it stands down for the demo. */
    if (mapIdx != MapIdx_MAP0_S00 && mapIdx != MapIdx_MAP0_S01 &&
        !(g_SysWork.processFlags & ProcessFlag_BootDemo))
    {
        g_SysWork.sysFlags &= ~SysFlag_NoEnemySpawn;
    }

    if (mapIdx != MapIdx_MAP0_S00 && mapIdx != MapIdx_MAP0_S01 &&
        (g_SysWork.processFlags & ProcessFlag_NewGame) &&
        Pc_SavegameInventoryIsEmpty())
    {
        s_InventoryItem* items = g_SavegamePtr->items;
        items[0].id_0 = InvItemId_Flashlight;     items[0].count_1 = 1;
        items[1].id_0 = InvItemId_PocketRadio;    items[1].count_1 = 1;
        items[2].id_0 = InvItemId_KitchenKnife;   items[2].count_1 = 1;
        items[3].id_0 = InvItemId_Handgun;        items[3].count_1 = 1;
        items[4].id_0 = InvItemId_HandgunBullets; items[4].count_1 = 15;

        /* Before the recount: func_8004F190 only derives weaponInventoryIdx and
         * totalWeaponAmmo when a weapon is equipped. g_Inventory_EquippedItem is
         * deliberately left alone - the inventory screen rebuilds it from this
         * field every time it opens. */
        g_SavegamePtr->equippedWeapon   = InvItemId_Handgun;
        g_SavegamePtr->itemToggleFlags |= ItemToggleFlag_RadioOn;
        g_SavegamePtr->itemToggleFlags &= ~ItemToggleFlag_FlashlightOff;

        /* Randomizer starts Harry with extra rounds on top of this loadout. Written
         * in place rather than through Inventory_AddSpecialItem, which would open a
         * second bullet stack next to items[4]. Returns 0 when the mode is off.
         * Before the recount so totalWeaponAmmo comes out of the final stack. */
        {
            extern int Pc_Rando_ExtraHandgunAmmo(void);
            int extra = Pc_Rando_ExtraHandgunAmmo();
            if (extra > 0)
            {
                items[4].count_1 = (u8)(15 + extra);
            }
        }

        /* Sorts, dedups, derives weaponInventoryIdx + totalWeaponAmmo, and
         * returns the count with the game's own [8,40] floor. The old
         * hard-assigned 5 sat below that floor, and nothing on a transition path
         * repairs it (the clamp lives in Game_PlayerInfoInit, which overlay
         * transitions never call), so the next pickup landed on top of the item
         * in slot 5. */
        g_SavegamePtr->inventorySlotCount = func_8004F190(g_SavegamePtr);

        g_SysWork.playerCombat.weaponAttack = WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap);
        if (g_SysWork.playerCombat.weaponInventoryIdx != NO_VALUE)
        {
            /* Never set before, so the starting handgun read as empty and the
             * next ammo writeback stored that 0 over the loaded round. */
            g_SysWork.playerCombat.currentWeaponAmmo =
                g_SavegamePtr->items[g_SysWork.playerCombat.weaponInventoryIdx].count_1;
        }

        SH_DBG("[AUTO-EQUIP] FIRED on New Game, map %d: handgun+%d+knife+radio+flashlight, slots=%d idx=%d",
               mapIdx, (int)g_SysWork.playerCombat.totalWeaponAmmo,
               (int)g_SavegamePtr->inventorySlotCount,
               (int)g_SysWork.playerCombat.weaponInventoryIdx);
        fflush(g_ShDebugLog);
    }
    /* Switch the active map overlay header to the requested map. */
    MapRegistry_Load(mapIdx);
    /* MapRegistry_Load swaps g_MapOverlayHdr synchronously here, unlike PSX where
     * the overlay BIN is read asynchronously (Fs_QueueStartRead below) and the
     * header only changes once the load completes. That synchronous swap opens a
     * one-frame window where the NEW map's bgmEvent — ticked by Bgm_TrackUpdate at
     * the tail of GameState_InGame_Update this same frame — runs Bgm_ChannelSet
     * while g_GameWork.bgmIdx (the currently-loaded track) still holds the previous
     * area's index (e.g. map0_s01 combat idx 30), replaying it as a brief BGM blip
     * before Bgm_Init reloads this map's track. Clearing the loaded-track index to
     * None makes Bgm_ChannelSet early-return until Bgm_TrackSet loads the correct
     * one; Bgm_ActiveBgmTrackCheck (None != target) still drives the proper reload. */
    g_GameWork.bgmIdx = BgmTrackIdx_None;
    fflush(g_ShDebugLog);
    /* Still read the overlay file â€” on PC this is a no-op but keeps the
     * filesystem queue state consistent. */
#endif
    Fs_QueueStartRead(FILE_VIN_MAP0_S00_BIN + mapIdx, g_OvlDynamic);
#ifdef SH_PC_PORT
    fflush(g_ShDebugLog);
    /* PAL language text: the file table redirected this overlay read to the
     * chosen language's copy (VIN = PAL-English — its own retranslation —
     * or VIN2-5 for DE/FR/ES/IT); once it lands, extract + translate its
     * message table and repoint the compiled map header (compiled maps bake
     * the US script).
     * Also re-queue FONT16 on EUR: its atlas home (768,128) shares tpage 12
     * with boot images, and the auto-load-save boot path never passes the
     * title-screen reload — a per-map reload is cheap insurance. */
    {
        extern void Fs_QueueWaitForEmpty(void);
        extern int  Pc_LangActive(void);
        extern void Pc_LangPatchMapMessages(int mapIdx, void* ovl, unsigned int ovlSize);
        extern e_GameRegion g_GameRegion;

        if (g_GameRegion == Region_EUR)
        {
            Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
            /* BG_ETC needs the same insurance: PAL reslices the tree/branch
             * billboard band to texels (128..191, 0..63) = VRAM (800..816, 0..63),
             * which the hospital/mall TV bank (map4_s03 func_800D7450 case 1,
             * TV2.TIM at (800,0 64x256), US coordinates) overwrites. Retail SLES
             * relocates that TV bank; until its EUR coordinates are known, reload
             * BG_ETC per map so every exterior gets intact leaves — without this,
             * tree billboards render as opaque garbage squares for the rest of
             * the session after the first TV room visit. */
            {
                extern void GameFs_BgEtcGfxLoad(void);
                GameFs_BgEtcGfxLoad();
            }
        }
        if (Pc_LangActive() || g_GameRegion == Region_JPN)
        {
            Fs_QueueWaitForEmpty();
            Pc_LangPatchMapMessages(
                mapIdx, g_OvlDynamic,
                (unsigned int)g_FileTable[FILE_VIN_MAP0_S00_BIN + mapIdx].blockCount << 8);
        }
        else if (g_GameRegion == Region_USA)
        {
            /* Fan-translated discs edit the US overlays in place. The USA
             * patcher reads the overlay bytes off the disc image itself (no
             * queue drain — the vanilla load path stays untouched) and is a
             * no-op unless the disc text differs from the compiled strings. */
            Pc_LangPatchMapMessages(mapIdx, g_OvlDynamic,
                (unsigned int)g_FileTable[FILE_VIN_MAP0_S00_BIN + mapIdx].blockCount << 8);
        }
        if (g_GameRegion == Region_JPN)
        {
            /* Fresh kanji atlas per map: drops stale cells and re-uploads the
             * margin-strip CLUT in case anything scribbled those rows. */
            extern void Pc_KanjiAtlasReset(void);
            extern void CharaData_ApplyJpnMapPatches(s32 mapIdx);
            Pc_KanjiAtlasReset();
            /* Before any NPC of the new map spawns — chara model/texture
             * files resolve through CHARA_FILE_INFOS at spawn time. */
            CharaData_ApplyJpnMapPatches(mapIdx);
        }

        /* Mod text overrides (gamedata/load/text_overrides.txt) — after any
         * localization swap so a mod's replacement wins for every region. */
        {
            extern void Pc_TextOverrideApply(int mapIdx);
            Pc_TextOverrideApply(mapIdx);
        }
    }
#endif
    Map_EffectTexturesLoad(mapIdx);
#ifdef SH_PC_PORT
    fflush(g_ShDebugLog);
#endif
    GameFs_PlayerMapAnimLoad(mapIdx);
#ifdef SH_PC_PORT
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
        fflush(g_ShDebugLog);
#endif
        WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat);
#ifdef SH_PC_PORT
        fflush(g_ShDebugLog);
#endif
    }

#ifdef SH_PC_PORT
    fflush(g_ShDebugLog);
#endif
    Gfx_PlayerHeldItemAttach(g_SysWork.playerCombat.weaponAttack);
#ifdef SH_PC_PORT
    fflush(g_ShDebugLog);

    /* Randomizer gamemode. Last writer wins: this installs its own copy of the
     * map header (rewritten doors / spawns / messages), and it must come after
     * the language patch above, which installs a header copy of its own. No-op
     * unless the mode is running. */
    {
        extern void Pc_Rando_OnMapLoad(s32 mapIdx);
        Pc_Rando_OnMapLoad(mapIdx);
    }
#endif
}
