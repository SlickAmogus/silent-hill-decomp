#include "game.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/npc_main.h"
#include "bodyprog/events/player_pos_update.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/view/vc_util.h"
#include "main/fsqueue.h"

// ========================================
// CHARACTER & GAME INIT
// ========================================

static void GameBoot_NpcClear(void) // 0x80034EC8
{
    s32 i;

    g_SysWork.field_228C[0] = 0;
    g_SysWork.npcFlags = 0;

    bzero(g_SysWork.npcs, ARRAY_SIZE(g_SysWork.npcs) * sizeof(s_SubCharacter));

    for (i = 0; i < CHARA_GROUP_COUNT; i++)
    {
        g_SysWork.charaGroupFlags[i] = 0;
    }
}

void GameBoot_NpcInit(void) // 0x80034F18
{
    vcSetCameraUseWarp(&g_SysWork.playerWork.player.position, g_SysWork.cameraAngleY);
    func_8005E70C();

    if (g_SysWork.field_234A)
    {
        g_MapOverlayHeader.enviromentSet_16C(g_SysWork.field_2349, 127);
        g_MapOverlayHeader.particlesUpdate_168(0, g_SavegamePtr->mapOverlayId_A4, 0);
    }

    GameBoot_NpcClear();
    Game_NpcRoomInitSpawn(false);
    Game_PlayerHeightUpdate();
}

void GameBoot_InGameInit(void) // 0x80034FB8
{
    s32 mapOvlId;

    mapOvlId = g_SavegamePtr->mapOverlayId_A4;

#ifdef SH_PC_PORT
#define HARRY_CHECK(label) SH_DBG("[SH] InGameInit %s harry=%p", label, (void*)g_WorldGfxWork.registeredCharaModels_18[1])
    HARRY_CHECK("start");
#endif
    vcInitCamera(&g_MapOverlayHeader, &g_SysWork.playerWork.player.position);
    vcSetCameraUseWarp(&g_SysWork.playerWork.player.position, g_SysWork.cameraAngleY);
#ifdef SH_PC_PORT
    HARRY_CHECK("after vcInit");
#endif
    func_80040004(&g_MapOverlayHeader);
#ifdef SH_PC_PORT
    HARRY_CHECK("after func_80040004");
#endif
    Gfx_MapEffectsSet(0);
#ifdef SH_PC_PORT
    HARRY_CHECK("after MapEffectsSet");
#endif
    WorldGfx_CharaModelProcessAllLoads();
#ifdef SH_PC_PORT
    HARRY_CHECK("after CharaModelProcessAllLoads");
#endif
    Game_FlashlightAttributesFix();
#ifdef SH_PC_PORT
    HARRY_CHECK("after FlashlightAttribsFix");
    SH_DBG("[SH]   &g_WorldGfxWork=%p &registeredCharaModels[1]=%p offset=%zu",
            (void*)&g_WorldGfxWork, (void*)&g_WorldGfxWork.registeredCharaModels_18[1],
            (size_t)((char*)&g_WorldGfxWork.registeredCharaModels_18[1] - (char*)&g_WorldGfxWork));
#endif
    g_MapOverlayHeader.particlesUpdate_168(0, mapOvlId, NO_VALUE);
#ifdef SH_PC_PORT
    HARRY_CHECK("after particlesUpdate");
#endif

    GameBoot_NpcClear();
    g_SysWork.npcFlagsId = 5;
    func_8005E650(mapOvlId);
#ifdef SH_PC_PORT
    HARRY_CHECK("after func_8005E650");
#endif
    func_80037124();
#ifdef SH_PC_PORT
    HARRY_CHECK("after func_80037124");
#endif
    func_8007E8C0();
#ifdef SH_PC_PORT
    HARRY_CHECK("after func_8007E8C0");
#endif
    Game_NpcRoomInitSpawn(false);
#ifdef SH_PC_PORT
    HARRY_CHECK("after NpcRoomInitSpawn");
#endif
    Game_PlayerHeightUpdate();
    Fs_CharaAnimBoneInfoUpdate();
#ifdef SH_PC_PORT
    HARRY_CHECK("after AnimBoneInfoUpdate");
#endif
    GameFs_WeaponInfoUpdate();
#ifdef SH_PC_PORT
    HARRY_CHECK("after WeaponInfoUpdate");
#endif
    GameFs_Tim00TIMLoad();
    Fs_QueueWaitForEmpty();
    GameFs_MapItemsModelLoad(mapOvlId);
#ifdef SH_PC_PORT
    HARRY_CHECK("COMPLETE");
#undef HARRY_CHECK
#endif
}
