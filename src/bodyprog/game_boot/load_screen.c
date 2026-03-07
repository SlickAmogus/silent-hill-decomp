#include "game.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/view/vc_main.h"
#include "bodyprog/math/math.h"

/** @brief Updates the translation and rotation of a matrix in a coordinate.
 *
 * @param pos Translation to apply.
 * @param rot Rotation to apply.
 * @param coord Coordinate to update.
 */
static void Math_MatrixTransform(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* coord) // 0x80035B04
{
    coord->flg        = false;
    coord->coord.t[0] = Q12_TO_Q8(pos->vx);
    coord->coord.t[1] = Q12_TO_Q8(pos->vy);
    coord->coord.t[2] = Q12_TO_Q8(pos->vz);

    Math_RotMatrixZxyNegGte(rot, (MATRIX*)&coord->coord);
}

void Gfx_MapEffectsSet(s32 unused) // 0x80035B58
{
    Gfx_MapEffectsAssign(&g_MapOverlayHeader);
    g_MapOverlayHeader.enviromentSet_16C(g_MapOverlayHeader.field_17, g_MapOverlayHeader.field_16);
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

#ifdef SH_PC_PORT
    /* Loading screen rendering depends on uninitialized game state (mapInfo, skeleton, etc.)
     * that would cause NULL dereferences on PC. Skip for now — loading screen is cosmetic. */
    return;
#endif

    boneCoords = g_SysWork.playerBoneCoords_890;
    model      = &g_SysWork.playerWork_4C.player_0.model_0;

    if (g_SysWork.sysState_8 == SysState_Gameplay)
    {
        if (g_SysWork.processFlags_2298 == SysWorkProcessFlag_OverlayTransition)
        {
            AreaLoad_UpdatePlayerPosition();
        }

#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: vcInitCamera\n"); fflush(stderr);
#endif
        vcInitCamera(&g_MapOverlayHeader, &g_SysWork.playerWork_4C.player_0.position_18);
#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: func_80040004\n"); fflush(stderr);
#endif
        func_80040004(&g_MapOverlayHeader);

        camLookAt.vy = Q12(-0.6f);
        camLookAt.vx = g_SysWork.playerWork_4C.player_0.position_18.vx;
        camLookAt.vz = g_SysWork.playerWork_4C.player_0.position_18.vz;

#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: vcUserWatchTarget\n"); fflush(stderr);
#endif
        vcUserWatchTarget(&camLookAt, NULL, true);

        camLookAt.vx -= Math_Sin(g_SysWork.playerWork_4C.player_0.rotation_24.vy - Q12_ANGLE(22.5f)) * 2;
        temp          = Math_Cos(g_SysWork.playerWork_4C.player_0.rotation_24.vy - Q12_ANGLE(22.5f));
        camLookAt.vy  = Q12(-1.0f);
        camLookAt.vz -= temp * 2;

#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: vcUserCamTarget\n"); fflush(stderr);
#endif
        vcUserCamTarget(&camLookAt, NULL, true);
#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: Game_SpotlightLoadScreenAttribsFix\n"); fflush(stderr);
#endif
        Game_SpotlightLoadScreenAttribsFix();
#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: Gfx_LoadScreenMapEffectsUpdate\n"); fflush(stderr);
#endif
        Gfx_LoadScreenMapEffectsUpdate(0, 0);
#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: setting anim flags\n"); fflush(stderr);
#endif

        model->anim_4.flags_2                                 |= AnimFlag_Visible;
        g_SysWork.playerWork_4C.extra_128.disabledAnimBones_18 = 0;
        model->anim_4.flags_2                                 |= AnimFlag_Unlocked | AnimFlag_Visible;
        model->anim_4.time_4                                   = Q12(26.0f);
        g_SysWork.playerWork_4C.player_0.position_18.vy        = Q12(0.2f);

        D_800A998C.status_4 = model->anim_4.status_0;

#ifdef SH_PC_PORT
        fprintf(stderr, "[SH] LoadScreen_PlayerRun: Math_MatrixTransform\n"); fflush(stderr);
#endif
        Math_MatrixTransform(&g_SysWork.playerWork_4C.player_0.position_18, &g_SysWork.playerWork_4C.player_0.rotation_24, boneCoords);
        g_SysWork.sysState_8++;
    }

#ifdef SH_PC_PORT
    fprintf(stderr, "[SH] LoadScreen_PlayerRun: Anim_Update1 model=%p skeleton=%p bones=%p\n",
            (void*)model, FS_BUFFER_0, (void*)boneCoords); fflush(stderr);
#endif
    Anim_Update1(model, (s_Skeleton*)FS_BUFFER_0, boneCoords, &D_800A998C);
#ifdef SH_PC_PORT
    fprintf(stderr, "[SH] LoadScreen_PlayerRun: vcMoveAndSetCamera\n"); fflush(stderr);
#endif
    vcMoveAndSetCamera(true, false, false, false, false, false, false, false);
#ifdef SH_PC_PORT
    fprintf(stderr, "[SH] LoadScreen_PlayerRun: Gfx_FlashlightUpdate\n"); fflush(stderr);
#endif
    Gfx_FlashlightUpdate();
#ifdef SH_PC_PORT
    fprintf(stderr, "[SH] LoadScreen_PlayerRun: func_8003DA9C\n"); fflush(stderr);
#endif
    func_8003DA9C(Chara_Harry, boneCoords, 1, g_SysWork.playerWork_4C.player_0.timer_C6, 0);
#ifdef SH_PC_PORT
    fprintf(stderr, "[SH] LoadScreen_PlayerRun: done\n"); fflush(stderr);
#endif
}
