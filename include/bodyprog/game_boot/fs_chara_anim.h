#ifndef _BODYPROG_GAMEBOOT_FSCHARAANIM_H
#define _BODYPROG_GAMEBOOT_FSCHARAANIM_H

#include "bodyprog/chara/chara.h"
#include "bodyprog/formats/anm.h"

/** @brief Character model animation data. */
typedef struct _CharaAnimData
{
    /* 0x0  */ s8             allocCharaId;  /** `e_CharaId` */
    /* 0x1  */ s8             activeCharaId; /** `e_CharaId` */
               // 2 bytes of padding.
#ifdef SH_PC_PORT
    /* 0x4  */ s_AnmHeader*   allocAddr; // On PSX this is s32 (a pointer fits in 32 bits); 64-bit needs a real pointer or it truncates+sign-extends.
#else
    /* 0x4  */ s32            allocAddr; // TODO: Should be `s_AnmHeader*` but doesn't match.
#endif
    /* 0x8  */ s_AnmHeader*   activeAnmHdr;
    /* 0xC  */ s32            allocSize;
    /* 0x10 */ s32            activeSize;
    /* 0x14 */ GsCOORDINATE2* boneCoords;
} s_CharaAnimData;
#ifndef SH_PC_PORT
STATIC_ASSERT_SIZEOF(s_CharaAnimData, 24);
#endif

#ifdef SH_PC_PORT
/* Global chara pool (pc_chara_pool.c): every charaId gets a dedicated anim
 * slot appended after the 4 vanilla group slots, fed through the vanilla
 * Fs_CharaAnimDataAlloc explicit-buffer path. Vanilla code only ever walks
 * slots 0..3 (bump chain, overlap sweep, Fs_CharaAnimBoneInfoUpdate), so the
 * pool slots are invisible to it; consumers index via g_CharaAnimDataIdxs
 * and work for any idx. Chara_SpawnFlagsSet/PositionSet derive a spawn-table
 * ROW from idx-1 and are guarded against pool idxs (rows > 1). */
#define PC_CHARA_ANIM_SLOT(charaId) (CHARA_GROUP_COUNT + (charaId))
#define CHARA_ANIM_DATA_COUNT       (CHARA_GROUP_COUNT + Chara_Count)
#else
#define CHARA_ANIM_DATA_COUNT       CHARA_GROUP_COUNT
#endif

/** @brief Animation data for loaded character models.
 * NOTE (PC port): the Jun 2026 merge briefly had this same PSX array under
 * two names — upstream `g_CharaModelAnimsData` (read by `Chara_BonesInit`)
 * and fork `g_CharaTypeAnimInfo` (written by the loader). Cutscene NPCs
 * (e.g. the school cat) read all-NULL anim headers from the never-written
 * copy and rendered with zeroed bones (invisible) / crashed at scene end.
 * There must be exactly ONE definition of this array. */
extern s_CharaAnimData g_CharaModelAnimsData[CHARA_ANIM_DATA_COUNT];

/** @brief Checks if the raw file streaming memory allocations of two character animation data slots overlap.
 *
 * @param animDataIdx0 First animation data index.
 * @param animDataIdx1 Second animation data index.
 * @return `true` if the animation data overlaps, `false` otherwise.
 */
bool Fs_CharaAnimDataOverlapCheck(s32 animDataIdx0, s32 animDataIdx1);

/** @brief Finds the character animation data slot index.
 * If not found, it returns index 0.
 *
 * @param charaId ID of the character for which to find the animation data.
 * @return Animation data index.
 */
s32 Fs_CharaAnimDataIdxGet(e_CharaId charaId);

/** @brief Allocates memory for character animation data and bone coords.
 *
 * Handles three critical scenarios:
 * 1. If the data is already at the slot, it ensures the memory layout hasn't drifted.
 * 2. If the data is loaded in a different slot, it copies the data over to reuse it.
 * 3. If the data is new, it calls `Fs_QueueStartReadAnm` to read data from the disk.
 *
 * As a final step, it performs an overlap sweep and zeroes out any existing slots that the new allocation collided
 * with.
 *
 * @param animDataIdx Index of the animation data to allocate.
 * @param charaId ID of the character associated with the animation.
 * @param anmHdr ANM data.
 * @param boneCoords Character bone coords.
 */
void Fs_CharaAnimDataAlloc(s32 animDataIdx, e_CharaId charaId, s_AnmHeader* anmHdr, GsCOORDINATE2* boneCoords);

/** @brief Assigns character animation data to a free slot and initializes character model bones.
 * Called by `Fs_QueuePostLoadAnm`.
 *
 * @param animDataIdx Index of the animation data to update.
 * @param charaId ID of the character associated with the animation.
 * @param anmHdr ANM data.
 * @param boneCoords Character bone coords.
 */
void Fs_CharaAnimDataUpdate(s32 animDataIdx, e_CharaId charaId, s_AnmHeader* anmHdr, GsCOORDINATE2* boneCoords);

/** @brief Dynamically allocates where the next character animation data bone coords should be stored  in
 * `g_SysWork.npcBoneCoordBuffer` based on the space consumed by the current character. */
void Fs_CharaAnimBoneInfoUpdate(void);

#endif
