#include "game.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#include "map_registry.h"
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "main/fsqueue.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"

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
    SH_DBG("[SH] GameBoot_PlayerInit: WorldGfx_MapInit... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
#endif
    WorldGfx_MapInit();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: CharaModel_AllModelsFree... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
#endif
    CharaModel_AllModelsFree();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: Item_HeldItemModelFree... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
#endif
    Item_HeldItemModelFree();
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: Anim_BoneInit... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
#endif
    Anim_BoneInit(FS_BUFFER_0, g_SysWork.playerBoneCoords_890); // Load player anim file?
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: WorldGfx_PlayerModelProcessLoad... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
#endif
    WorldGfx_PlayerModelProcessLoad();

#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_PlayerInit: setting field_229C... harry=%p", (void*)g_WorldGfxWork.registeredCharaModels_18[1]);
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
    SH_DBG("[SH] GameBoot_MapLoad: mapIdx=%d (%s)", mapIdx, MapRegistry_GetName(mapIdx));
    /* Switch the active map overlay header to the requested map. */
    MapRegistry_Load(mapIdx);
    /* Still read the overlay file — on PC this is a no-op but keeps the
     * filesystem queue state consistent. */
#endif
    Fs_QueueStartRead(FILE_VIN_MAP0_S00_BIN + mapIdx, g_OvlDynamic);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: Map_EffectTexturesLoad");
#endif
    Map_EffectTexturesLoad(mapIdx);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: GameFs_PlayerMapAnimLoad");
#endif
    GameFs_PlayerMapAnimLoad(mapIdx);

#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: WorldGfx_PlayerPrevHeldItem check");
#endif
    if (g_SysWork.processFlags_2298 & (SysWorkProcessFlag_NewGame | SysWorkProcessFlag_LoadSave |
                                       SysWorkProcessFlag_Continue | SysWorkProcessFlag_BootDemo))
    {
        WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat_38);
    }

#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: Gfx_PlayerHeldItemAttach weaponAttack=%d", g_SysWork.playerCombat_38.weaponAttack_F);
#endif
    Gfx_PlayerHeldItemAttach(g_SysWork.playerCombat_38.weaponAttack_F);
#ifdef SH_PC_PORT
    SH_DBG("[SH] GameBoot_MapLoad: done");
#endif
}
