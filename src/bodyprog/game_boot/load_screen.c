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
#include "bodyprog/view/vc_main.h"
#include "bodyprog/math/math.h"

extern s_WorldEnvWork g_WorldEnvWork;

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

    boneCoords = g_SysWork.playerBoneCoords_890;
    model      = &g_SysWork.playerWork_4C.player_0.model_0;

    if (g_SysWork.sysState_8 == SysState_Gameplay)
    {
        if (g_SysWork.processFlags_2298 == SysWorkProcessFlag_OverlayTransition)
        {
            AreaLoad_UpdatePlayerPosition();
        }

#ifdef SH_PC_PORT
        /* Camera/map init — use vanilla code, it works with fallback road data */
#endif
        vcInitCamera(&g_MapOverlayHeader, &g_SysWork.playerWork_4C.player_0.position_18);
        func_80040004(&g_MapOverlayHeader);

        camLookAt.vy = Q12(-0.6f);
        camLookAt.vx = g_SysWork.playerWork_4C.player_0.position_18.vx;
        camLookAt.vz = g_SysWork.playerWork_4C.player_0.position_18.vz;

        vcUserWatchTarget(&camLookAt, NULL, true);

        camLookAt.vx -= Math_Sin(g_SysWork.playerWork_4C.player_0.rotation_24.vy - Q12_ANGLE(22.5f)) * 2;
        temp          = Math_Cos(g_SysWork.playerWork_4C.player_0.rotation_24.vy - Q12_ANGLE(22.5f));
        camLookAt.vy  = Q12(-1.0f);
        camLookAt.vz -= temp * 2;

        vcUserCamTarget(&camLookAt, NULL, true);
        Game_SpotlightLoadScreenAttribsFix();
        Gfx_LoadScreenMapEffectsUpdate(0, 0);

        model->anim_4.flags_2                                 |= AnimFlag_Visible;
        g_SysWork.playerWork_4C.extra_128.disabledAnimBones_18 = 0;
        model->anim_4.flags_2                                 |= AnimFlag_Unlocked | AnimFlag_Visible;
        model->anim_4.time_4                                   = Q12(26.0f);
        g_SysWork.playerWork_4C.player_0.position_18.vy        = Q12(0.2f);

        D_800A998C.status_4 = model->anim_4.status_0;

        Math_MatrixTransform(&g_SysWork.playerWork_4C.player_0.position_18, &g_SysWork.playerWork_4C.player_0.rotation_24, boneCoords);
        g_SysWork.sysState_8++;
    }

#ifdef SH_PC_PORT
    /* Ensure anim flags are set every frame — the sysState==Gameplay init block
     * only runs once and may be skipped if sysState was already incremented.
     * Without these flags, Anim_PlaybackLoop skips Anim_BoneUpdate entirely
     * (line 359 check), leaving bone transforms stale. */
    model->anim_4.flags_2 |= AnimFlag_Unlocked | AnimFlag_Visible;
    g_SysWork.playerWork_4C.extra_128.disabledAnimBones_18 = 0;
    /* Reset root bone flg so hierarchy is recomputed from scratch */
    boneCoords[0].flg = 0;
#endif

    Anim_PlaybackLoop(model, (s_Skeleton*)FS_BUFFER_0, boneCoords, &D_800A998C);
    vcMoveAndSetCamera(true, false, false, false, false, false, false, false);
    Gfx_FlashlightUpdate();
#ifdef SH_PC_PORT
    {
        static int ls_dbg = 0;
        if (ls_dbg < 10) {
            SH_DBG("[LOADSCR] timer_C6=%d env.field_0=%d env.field_20=%d tint=(%d,%d,%d) bone0.flg=%d bone0.t=(%d,%d,%d)",
                    g_SysWork.playerWork_4C.player_0.timer_C6,
                    g_WorldEnvWork.field_0, g_WorldEnvWork.field_20,
                    g_WorldEnvWork.worldTintColor_28.r, g_WorldEnvWork.worldTintColor_28.g, g_WorldEnvWork.worldTintColor_28.b,
                    boneCoords[0].flg, boneCoords[0].coord.t[0], boneCoords[0].coord.t[1], boneCoords[0].coord.t[2]);
            SH_DBG("[LOADSCR] skel@FS_BUFFER_0=%p model.anim.status=%d model.anim.flags=%d",
                    (void*)FS_BUFFER_0, model->anim_4.status_0, model->anim_4.flags_2);
            ls_dbg++;
        }
        /* Ensure environment is set up for character rendering during loading.
         * Force flat-lit, no-fog environment with neutral tint so Harry is visible
         * on the black loading screen background. */
        {
            extern void func_80055330(u8, s32, u8, s32, s32, s32, q23_8);
            func_80055330(0, Q12(1.0f), 0,
                          128 << 5, 128 << 5, 128 << 5,  /* neutral tint */
                          0);                              /* no brightness overlay */
            g_WorldEnvWork.isFogEnabled_1 = 0;
        }
        /* Force all skeleton bones visible (same as InGame Harry render) */
        {
            s_CharaModel* harryModel = g_WorldGfxWork.registeredCharaModels_18[Chara_Harry];
            if (harryModel != NULL) {
                func_800453E8(&harryModel->skeleton_14, true);
            }
        }
        /* Reset ALL bone flg values to force full hierarchy recomputation.
         * This eliminates stale cached workm matrices from previous frames. */
        {
            int _bi;
            for (_bi = 0; _bi < HarryBone_Count; _bi++) {
                boneCoords[_bi].flg = 0;
            }
        }
    }
#endif
#ifdef SH_PC_PORT
    /* Pass timer=0 so func_8003DA9C does not apply a fade-to-black tint.
     * timer_C6 is often near Q12(1.0f) during loading, which scales the
     * tint color to almost zero, making Harry nearly invisible. */
    func_8003DA9C(Chara_Harry, boneCoords, 1, 0, 0);
#else
    func_8003DA9C(Chara_Harry, boneCoords, 1, g_SysWork.playerWork_4C.player_0.timer_C6, 0);
#endif
}
