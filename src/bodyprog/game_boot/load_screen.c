#include "game.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include "sh_log.h"
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/view/vc_main.h"
#include "bodyprog/math/math.h"

extern s_WorldEnvWork g_WorldEnvWork;

/** @brief Updates the translation and rotation of a matrix in a coordinate.
 *
 * @param pos Translation to apply.
 * @param rot Rotation to apply.
 * @param coord Coordinate to update.
 */
#ifdef SH_PC_PORT
/* Non-static on PC so map overlay code (compiled separately) can call it. */
void Math_MatrixTransform(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* coord) // 0x80035B04
#else
static void Math_MatrixTransform(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* coord) // 0x80035B04
#endif
{
    coord->flg        = false;
    coord->coord.t[0] = Q12_TO_Q8(pos->vx);
    coord->coord.t[1] = Q12_TO_Q8(pos->vy);
    coord->coord.t[2] = Q12_TO_Q8(pos->vz);

    Math_RotMatrixZxyNegGte(rot, (MATRIX*)&coord->coord);
}

void Gfx_MapEffectsSet(s32 unused) // 0x80035B58
{
    Gfx_MapEffectsAssign(&g_MapOverlayHdr);
    g_MapOverlayHdr.enviromentSet(g_MapOverlayHdr.field_17, g_MapOverlayHdr.field_16);
}

void func_80035B98(void) // 0x80035B98
{
    Screen_BackgroundImgDraw(&g_ItemInspectionImg);
}

void GameBoot_LoadScreen_BackgroundImg(void) // 0x80035BBC
{
    Screen_BackgroundImgDraw(&g_LoadingScreenImg);
}

void GameBoot_LoadScreen_PlayerRun(void) // 0x80035BE0
{
    VECTOR3        camLookAt; // Q19.12
    s32            temp;
    s_Model*       model;
    GsCOORDINATE2* boneCoords;

    boneCoords = g_SysWork.playerBoneCoords;
    model      = &g_SysWork.playerWork.player.model;

    if (g_SysWork.sysState == SysState_Gameplay)
    {
        if (g_SysWork.processFlags == ProcessFlag_OverlayTransition)
        {
            AreaLoad_UpdatePlayerPosition();
        }

        vcInitCamera(&g_MapOverlayHdr, &g_SysWork.playerWork.player.position);
        World_CollisionTriggersSet(&g_MapOverlayHdr);

        camLookAt.vy = Q12(-0.6f);
        camLookAt.vx = g_SysWork.playerWork.player.position.vx;
        camLookAt.vz = g_SysWork.playerWork.player.position.vz;

        vcUserWatchTarget(&camLookAt, NULL, true);

        camLookAt.vx -= Math_Sin(g_SysWork.playerWork.player.rotation.vy - Q12_ANGLE(22.5f)) * 2;
        temp          = Math_Cos(g_SysWork.playerWork.player.rotation.vy - Q12_ANGLE(22.5f));
        camLookAt.vy  = Q12(-1.0f);
        camLookAt.vz -= temp * 2;

        vcUserCamTarget(&camLookAt, NULL, true);
        Game_SpotlightLoadScreenAttribsFix();
        Gfx_LoadScreenMapEffectsUpdate(0, 0);

        model->anim.flags                                 |= AnimFlag_Visible;
        g_SysWork.playerWork.extra.disabledAnimBones = 0;
        model->anim.flags                                 |= AnimFlag_Unlocked | AnimFlag_Visible;
        model->anim.time                                   = Q12(26.0f);
        g_SysWork.playerWork.player.position.vy        = Q12(0.2f);

        D_800A998C.status = model->anim.status;

        Math_MatrixTransform(&g_SysWork.playerWork.player.position, &g_SysWork.playerWork.player.rotation, boneCoords);
        g_SysWork.sysState++;
    }

#ifdef SH_PC_PORT
    /* Ensure anim flags are set every frame — the sysState==Gameplay init block
     * only runs once and may be skipped if sysState was already incremented.
     * Without these flags, Anim_PlaybackLoop skips Anim_BoneUpdate entirely
     * (line 359 check), leaving bone transforms stale. */
    model->anim.flags |= AnimFlag_Unlocked | AnimFlag_Visible;
    g_SysWork.playerWork.extra.disabledAnimBones = 0;
    /* Reset root bone flg so hierarchy is recomputed from scratch */
    boneCoords[0].flg = 0;

    /* Ensure environment is set up for character rendering during loading.
     * Force flat-lit, no-fog environment with neutral tint so Harry is visible
     * on the black loading screen background. */
    {
        extern void func_80055330(u8, s32, u8, s32, s32, s32, q23_8);
        func_80055330(0, Q12(1.0f), 0,
                      128 << 5, 128 << 5, 128 << 5,  /* neutral tint */
                      0);                              /* no brightness overlay */
        g_WorldEnvWork.isFogEnabled = 0;
    }
    /* NOTE: the per-load func_800453E8(skel, true) force-show was removed —
     * it re-showed Harry's hidden weapon-hand variant meshes (duplicate
     * hands). The merge-era invisibility it covered for was the mis-mapped
     * MODEL_BONE_IDX_0_GET in world_draw.c's variant selectors, now fixed. */

    /* Reset ALL bone flg values to force full hierarchy recomputation.
     * This eliminates stale cached workm matrices from previous frames. */
    {
        int _bi;
        for (_bi = 0; _bi < HarryBone_Count; _bi++) {
            boneCoords[_bi].flg = 0;
        }
    }
#endif

    Anim_PlaybackLoop(model, (s_Skeleton*)FS_BUFFER_0, boneCoords, &D_800A998C);
    vcMoveAndSetCamera(true, false, false, false, false, false, false, false);
    Gfx_FlashlightUpdate();
#ifdef SH_PC_PORT
    /* Pass timer=0 so func_8003DA9C does not apply a fade-to-black tint.
     * timer_C6 is often near Q12(1.0f) during loading, which scales the
     * tint color to almost zero, making Harry nearly invisible. */
    func_8003DA9C(Chara_Harry, boneCoords, 1, 0, 0);
#else
    func_8003DA9C(Chara_Harry, boneCoords, 1, g_SysWork.playerWork.player.timer_C6, 0);
#endif
}
