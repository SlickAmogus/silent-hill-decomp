#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "main/fsqueue.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/ranking.h"
#include "bodyprog/sound/sound_system.h"
#include "sh_log.h"

// ========================================
// GLOBAL VARIABLES
// ========================================

/* g_CharaAnimInfoIdxs (fork name) merged into g_CharaAnimDataIdxs (upstream name,
 * defined below + exported in bodyprog.h). The merge renamed the READ sites
 * (npc_main, chara_spawn) but left this write-side table separate, so loaded
 * NPCs never read as "anim loaded" and never rendered. The write in
 * Fs_CharaAnimDataUpdate now targets g_CharaAnimDataIdxs directly. */

#ifdef SH_PC_PORT
/* On PC, FS_BUFFER_0 is not a compile-time constant (points into g_PsxRam[]).
 * Initialize at runtime instead. See also: Fs_QueueInitialize or GameState_Boot. */
s_CharaAnimData g_CharaModelAnimsData[CHARA_GROUP_COUNT] = {
    {
        .allocCharaId         = Chara_Harry,
        .activeCharaId         = Chara_Harry,
        .allocAddr        = NULL,
        .activeAnmHdr        = NULL,
        .allocSize  = 0x2E630,
        .activeSize = 0x2E630,
        .boneCoords       = NULL
    }, {}, {}, {}
};
/* Called from main_pc.c after PsxMemory_Init */
void PcPort_InitCharaAnimInfo(void)
{
    g_CharaModelAnimsData[0].allocAddr = FS_BUFFER_0;
    g_CharaModelAnimsData[0].activeAnmHdr = (s_AnmHeader*)FS_BUFFER_0;
}
#else
s_CharaAnimData g_CharaModelAnimsData[CHARA_GROUP_COUNT] = {
    {
        .allocCharaId         = Chara_Harry,
        .activeCharaId         = Chara_Harry,
        .allocAddr        = FS_BUFFER_0,
        .activeAnmHdr        = (s_AnmHeader*)FS_BUFFER_0,
        .allocSize  = 0x2E630,
        .activeSize = 0x2E630,
        .boneCoords       = NULL
    }, {}, {}, {}
};
#endif

s_AnimInfo D_800A998C = {
    .playbackFunc           = Anim_PlaybackLoop,
    .status               = 0,
    .hasVariableDuration = false,
    .linkStatus              = ANIM_STATUS(0, false),
    .duration            = { Q12(8.0f) },
    .startKeyframeIdx    = 26,
    .endKeyframeIdx      = 44
};

bool Fs_CharaAnimDataSizeCheck(s32 charaDataAnimInfoIdx0, s32 charaDataAnimInfoIdx1) // 0x8003528C
{
#ifdef SH_PC_PORT
    uintptr_t            animBufferAddress1;
    uintptr_t            animBufferAddress0;
#else
    u32                  animBufferAddress1;
    u32                  animBufferAddress0;
#endif
    s_CharaAnimData* animDataInfo0;
    s_CharaAnimData* animDataInfo1;

    animDataInfo0      = &g_CharaModelAnimsData[charaDataAnimInfoIdx0];
    animDataInfo1      = &g_CharaModelAnimsData[charaDataAnimInfoIdx1];
    animBufferAddress0 = (uintptr_t)animDataInfo0->allocAddr;
    animBufferAddress1 = (uintptr_t)animDataInfo1->activeAnmHdr;

    if (animBufferAddress0 >= (animBufferAddress1 + animDataInfo1->activeSize) ||
        animBufferAddress1 >= (animBufferAddress0 + animDataInfo0->allocSize))
    {
        return false;
    }

    return true;
}

s32 Fs_CharaAnimDataInfoIdxGet(e_CharaId charaId) // 0x800352F8
{
    s32 i;

    for (i = 1; (i < CHARA_GROUP_COUNT); i++)
    {
        if (g_CharaModelAnimsData[i].activeCharaId == charaId)
        {
            return i;
        }
    }

    return 0;
}

void Fs_CharaAnimDataAlloc(s32 idx, e_CharaId charaId, s_AnmHeader* animFile, GsCOORDINATE2* coord) // 0x80035338
{
    s32                  i;
    s_AnmHeader*         localAnimFile; // Local pointer required for match.
    s_CharaAnimData* initAnimDataInfo;
    s_CharaAnimData* npcAnimDataInfo;

    localAnimFile    = animFile;
    initAnimDataInfo = &g_CharaModelAnimsData[idx];

    if (charaId == Chara_None)
    {
        return;
    }

    // Estimates animation buffer data pointer by adding buffer size and current pointer position.
    for (npcAnimDataInfo = &initAnimDataInfo[-1]; localAnimFile == NULL; npcAnimDataInfo--)
    {
#ifdef SH_PC_PORT
        /* allocSize is a byte count, not an element count.
         * Cast through u8* to avoid s_AnmHeader* pointer arithmetic scaling. */
        localAnimFile = (s_AnmHeader*)((u8*)npcAnimDataInfo->allocAddr + npcAnimDataInfo->allocSize);
#else
        localAnimFile = npcAnimDataInfo->allocAddr + npcAnimDataInfo->allocSize;
#endif
    }

    // If the target character ID matches with the selected element from `g_CharaModelAnimsData`
    // then it ensures the animation buffer pointer matches with the previously estimated one, but
    // if the estimated pointer is in a position behind of the currently saved one then it moves
    // data to the position of the estimated pointer.
    //
    // If any of the previous checks fail, values previously assigned at index are cleared. Then, if
    // the character hasn't been loaded in a different `g_InitializedCharaAnimInfo` slot, the
    // animation file is loaded.
    if (initAnimDataInfo->activeCharaId == charaId)
    {
        if (idx == 1 || localAnimFile == initAnimDataInfo->activeAnmHdr)
        {
            Fs_CharaAnimDataUpdate(idx, charaId, initAnimDataInfo->activeAnmHdr, coord);
            return;
        }
        else if (localAnimFile < initAnimDataInfo->activeAnmHdr)
        {
            initAnimDataInfo->allocAddr = localAnimFile;

            Mem_Move32(localAnimFile, g_CharaModelAnimsData[idx].activeAnmHdr, g_CharaModelAnimsData[idx].activeSize);
            Fs_CharaAnimDataUpdate(idx, charaId, localAnimFile, coord);
            return;
        }
    }

    initAnimDataInfo->boneCoords       = &g_SysWork.npcBoneCoordBuffer[0];
    initAnimDataInfo->activeCharaId         = Chara_None;
    initAnimDataInfo->activeAnmHdr        = NULL;
    initAnimDataInfo->activeSize = 0;
    initAnimDataInfo->allocCharaId         = charaId;
    initAnimDataInfo->allocAddr        = localAnimFile;
    initAnimDataInfo->allocSize  = Fs_GetFileSectorAlignedSize(CHARA_FILE_INFOS[charaId].animFileIdx);

    i = Fs_CharaAnimDataInfoIdxGet(charaId);

    if (i > 0)
    {
        Mem_Move32(g_CharaModelAnimsData[idx].allocAddr, g_CharaModelAnimsData[i].activeAnmHdr, g_CharaModelAnimsData[i].activeSize);
        Fs_CharaAnimDataUpdate(idx, charaId, initAnimDataInfo->allocAddr, coord);
    }
    else
    {
        Fs_QueueStartReadAnm(idx, charaId, localAnimFile, coord);
    }

    for (i = 1; i < CHARA_GROUP_COUNT; i++)
    {
        if (i != idx && g_CharaModelAnimsData[i].activeCharaId != Chara_None && Fs_CharaAnimDataSizeCheck(idx, i) != false)
        {
            bzero(&g_CharaModelAnimsData[i], sizeof(s_CharaAnimData));
        }
    }
}

void Fs_CharaAnimDataUpdate(s32 idx, e_CharaId charaId, s_AnmHeader* animFile, GsCOORDINATE2* coord) // 0x80035560
{
    s32                  idx0;
    GsCOORDINATE2*       localCoord;
    s_CharaAnimData* animDataInfo;

    localCoord   = coord;
    animDataInfo = &g_CharaModelAnimsData[idx];

    if (localCoord == NULL)
    {
        if (idx == 1)
        {
            localCoord = &g_SysWork.npcBoneCoordBuffer[0];
        }
        else if (idx >= 2)
        {
#ifdef SH_PC_PORT
            /* Guard: previous slot's animFile may be NULL if that NPC never
             * completed loading (e.g. Air Screamer async ANM read arriving
             * before the previous slot is initialised).  Fall back to the
             * start of the NPC coord area to avoid a NULL deref crash. */
            if (g_CharaModelAnimsData[idx - 1].activeAnmHdr == NULL) {
                SH_DBG("[ANIM_ALLOC] Fs_CharaAnimBoneInfoUpdate idx=%d: prev slot animFile=NULL, using npcBoneCoordBuffer[0]", idx);
                localCoord = &g_SysWork.npcBoneCoordBuffer[0];
            } else {
#endif
            idx0        = g_CharaModelAnimsData[idx - 1].activeAnmHdr->boneCount;
            localCoord  = g_CharaModelAnimsData[idx - 1].boneCoords;
            localCoord += idx0 + 1;
#ifdef SH_PC_PORT
            }
#endif

            // Check for end of `g_SysWork.npcBoneCoordBuffer` array.
            if ((&localCoord[animFile->boneCount] + 1) >= &g_SysWork.npcBoneCoordBuffer[NPC_BONE_COUNT_MAX])
            {
                localCoord = g_MapOverlayHdr.npcBoneCoordBuffer;
            }
        }
    }

    animDataInfo->activeCharaId         = charaId;
    animDataInfo->activeAnmHdr        = animFile;
    animDataInfo->activeSize = Fs_GetFileSectorAlignedSize(CHARA_FILE_INFOS[charaId].animFileIdx);
    animDataInfo->boneCoords       = localCoord;

    Anim_BoneInit(animFile, localCoord);

    g_CharaAnimDataIdxs[charaId] = idx;
}

void Fs_CharaAnimBoneInfoUpdate(void) // 0x8003569C
{
    s32            i;
    GsCOORDINATE2* coord;
    s_AnmHeader*   animFile;

    for (i = 1; i < CHARA_GROUP_COUNT - 1; i++)
    {
        if (g_MapOverlayHdr.charaGroupIds[i] != Chara_None)
        {
            coord    = g_CharaModelAnimsData[i].boneCoords;
            animFile = g_CharaModelAnimsData[i + 1].activeAnmHdr;
            coord   += g_CharaModelAnimsData[i].activeAnmHdr->boneCount + 1;

            // Check for end of `g_SysWork.npcBoneCoordBuffer` array.
            if ((&coord[animFile->boneCount] + 1) >= &g_SysWork.npcBoneCoordBuffer[NPC_BONE_COUNT_MAX])
            {
                coord = g_MapOverlayHdr.npcBoneCoordBuffer;
            }

            g_CharaModelAnimsData[i + 1].boneCoords = coord;
            Anim_BoneInit(animFile, coord);
        }
    }
}

// --- Ported data globals from upstream ---
s8 g_CharaAnimDataIdxs[Chara_Count] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // 3 bytes of padding.
};

/* The duplicate `g_CharaModelAnimsData` definition that used to live here was
 * a Jun 2026 merge artifact: the same PSX array existed twice (fork name
 * `g_CharaModelAnimsData` above, upstream name here). `Chara_BonesInit` read
 * the never-written upstream copy, so cutscene NPCs (school cat) had NULL
 * anim headers -> zeroed bones -> invisible, and crashed at scene end.
 * Both names now resolve to the single definition at the top of this file. */
