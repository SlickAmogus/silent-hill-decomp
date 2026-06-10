#include "game.h"
#include "inline_no_dmpsx.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include "sh_log.h"
#include <PsyX/PsyX_render.h> /* g_PsxSkipFramebufferStore */
#endif

#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/background_draw.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/sound/sound_system.h"
#include "main/fsqueue.h"
extern const RECT D_8002AB10; // defined in events/events_util.c

#ifdef SH_PC_PORT
extern int g_PcHorPlusEnabled;
extern int g_PcMapScreenActive;
#endif

VECTOR3 D_800C4640[2][8];
q3_12   D_800C4700[8];
q19_12  D_800C4710[6];

#ifdef SH_PC_PORT
/* Re-upload the paper-map TIM from FS_BUFFER_2 to its target VRAM region AND
 * disable PsyCross's per-frame framebuffer→VRAM blit for this tick.
 *
 * Why this exists on PC only:
 *   1. `PsyX_EndScene` calls `GR_StoreFrameBuffer` every frame, which both
 *      (a) GL-blits the rendered framebuffer onto the `g_vramTexture` at
 *      (disp.x, disp.y, disp.w, disp.h) and (b) reads it back into the CPU
 *      `vram[]` array via `GR_ReadFramebufferDataToVRAM`.
 *   2. The paper-map CLUT lives at VRAM (224, 15) — INSIDE the (0,0)-(320,240)
 *      display rect. Every frame, the framebuffer blit overwrites the CLUT in
 *      the GPU texture, AND the readback overwrites the CLUT in vram[]. The
 *      next LoadImage anywhere marks `vram_need_update=1` and reuploads the
 *      corrupted vram[]. The pickup screen then samples corrupted CLUT and
 *      renders tiled gameplay framebuffer content instead of the map.
 *
 * Game state machines that run as gameplay sub-states (Event_MapTake,
 * `map0_s01_events.c` cafe map take, `func_800867B4`) hit this. The full-screen
 * map screen doesn't, because gameplay doesn't render in that GameState.
 *
 * Fix: (a) set `g_PsxSkipFramebufferStore` to suppress the framebuffer→VRAM
 * blit for this frame and (b) re-LoadImage the TIM each tick so the fresh
 * bytes land in the next GR_UpdateVRAM upload. The flag auto-clears in
 * PsyX_EndScene, so the game must call this helper every tick during the
 * pickup screen. */
void PaperMap_ReuploadTimToVram_PC(void)
{
    static int s_logCount = 0;
    TIM_IMAGE tim;
    RECT16    pixRect;
    RECT16    clutRect;
    u_char    magic;
    int       openOk;

    /* Suppress the framebuffer→VRAM blit for this frame so PsyX_EndScene's
     * GR_StoreFrameBuffer can't clobber the paper-map CLUT at VRAM (224,15).
     * The flag auto-clears at end-of-frame inside PsyX_EndScene. */
    g_PsxSkipFramebufferStore = 1;

    /* Log first few invocations so we can verify the helper actually runs
     * and the source buffer still holds a TIM. */
    magic  = ((u_char*)FS_BUFFER_2)[0];
    openOk = OpenTIM((u_long*)FS_BUFFER_2);
    if (s_logCount < 5) {
        s_logCount++;
    }

    if (openOk == 0) {
        return;
    }
    if (ReadTIM(&tim) == NULL) {
        return;
    }

    pixRect = *tim.prect;
    if (g_PaperMapImg.u != 0xFF) {
        pixRect.x = g_PaperMapImg.u + ((g_PaperMapImg.tPage[1] & 0xF) << 6);
        pixRect.y = g_PaperMapImg.v + ((g_PaperMapImg.tPage[1] << 4) & 0x100);
    }
    LoadImage(&pixRect, tim.paddr);

    if (tim.caddr != NULL) {
        clutRect = *tim.crect;
        if (g_PaperMapImg.clutX != NO_VALUE) {
            clutRect.x = g_PaperMapImg.clutX;
            clutRect.y = g_PaperMapImg.clutY;
        }
        LoadImage(&clutRect, tim.caddr);
    } else {
        clutRect.x = -1;
        clutRect.y = -1;
        clutRect.w = 0;
        clutRect.h = 0;
    }

    /* Hypothesis-5 nuclear option: even with the framebuffer-store gating,
     * paper-map screens still showed tiled gameplay framebuffer content
     * sampled from VRAM (320, 16+). The double-buffered g_vramTexture +
     * indirect upload path (LoadImage → vram[] → GR_UpdateVRAM → GPU)
     * leaves a window where stale data can persist in whichever texture
     * is NOT the current one. Force the freshly-loaded TIM data into BOTH
     * GPU vram textures right now, in this region only. This bypasses the
     * vram_need_update flag and texture-swap dance entirely. */
    GR_DirectUploadVRAMRegion(pixRect.x, pixRect.y, pixRect.w, pixRect.h);
    if (clutRect.w > 0 && clutRect.h > 0) {
        GR_DirectUploadVRAMRegion(clutRect.x, clutRect.y, clutRect.w, clutRect.h);
    }

    if (s_logCount < 5) {
    }
}
#endif

// ========================================
// EVENT AND INTERACTIONS RELATED
// ========================================

u8 D_800AFD04 = 0;
u8 D_800AFD05 = 0;
// 2 bytes of padding.

bool (*D_800AFD08[])(s_SysWork_2514* arg0, s_func_8009ECCC* arg1, s_8002AC04* ptr, u32* arg3) =
{
    func_80089A30,
    func_80089BB8,
    func_80089DF0,
    func_8008973C,
    func_80089D0C
};

void func_80085D78(bool reset) // 0x80085D78
{
    if (reset)
    {
        SysWork_StateStepIncrement(1);
    }
    else
    {
        SysWork_StateStepIncrement(0);
    }
}

void func_80085DC0(bool arg0, s32 sysStateStep) // 0x80085DC0
{
    if (arg0)
    {
        SysWork_StateStepSet(1, sysStateStep);
    }
    else
    {
        SysWork_StateStepSet(0, sysStateStep);
    }
}

void func_80085DF0(void) // 0x80085DF0
{
    g_SysWork.timer_2C += g_DeltaTimeRaw;
#ifdef SH_PC_PORT
    /* Diagnostic for the doghouse-note / dog head freeze. The latest
     * log shows EventCallFunc param=11 (MapEvent_DoghouseNote) stuck
     * at step0=1, which is THIS function. Either the moveDist check is
     * lying or g_DeltaTimeRaw is 0 so the 1s timeout never trips. */
    {
        static int _85df0LogN = 0;
        if (_85df0LogN < 30) {
            void* mdz = (void*)g_MapOverlayHdr.playerMoveSpeedIsZero;
            _85df0LogN++;
        }
    }
#endif
    if (g_MapOverlayHdr.playerMoveSpeedIsZero() != NULL || g_SysWork.timer_2C > Q12(1.0f))
    {
        SysWork_StateStepIncrement(0);
    }
}

void SysWork_StateStepIncrementDelayed(q19_12 delay, bool reset) // 0x80085E6C
{
    g_SysWork.timer_2C += g_DeltaTimeRaw;
    if (delay < g_SysWork.timer_2C)
    {
        func_80085D78(reset);
    }
}

void func_80085EB8(u32 arg0, s_SubCharacter* chara, s32 arg2, bool reset) // 0x80085EB8
{
    s32 keyframeState; // TODO: Not final name, only an indication.

    switch (arg0)
    {
        case 0:
            if (chara == &g_SysWork.playerWork.player)
            {
                g_MapOverlayHdr.playerAnimStateSet(arg2);
            }
            else
            {
                /* Upstream fixed the signature to (chara, afkTime); on PSX a2
                 * held the scripted anim index (arg2) from the caller. Passing
                 * arg2 lets the map overlay's charaAnimStateSet seed the anim
                 * natively — this replaces the old PC workaround that manually
                 * reconstructed anim.status/state because the decomp signature
                 * dropped a2 (the Cybil-cafe "walks in place" fix). */
                g_MapOverlayHdr.charaAnimStateSet(chara, arg2);
            }
            break;

        case 1:
            if (chara == &g_SysWork.playerWork.player)
            {
#ifdef SH_PC_PORT
                {
                    static int _85eb8c1LogN = 0;
                    if (_85eb8c1LogN < 30) {
                        _85eb8c1LogN++;
                    }
                }
#endif
                keyframeState = g_MapOverlayHdr.playerAnimPlaybackStateGet();
#ifdef SH_PC_PORT
                {
                    static int _85eb8c1RetLogN = 0;
                    if (_85eb8c1RetLogN < 30) {
                        _85eb8c1RetLogN++;
                    }
                }
#endif
                if (keyframeState == 1)
                {
                    func_80085D78(reset);
                }
            }
            else
            {
                keyframeState = g_MapOverlayHdr.charaAnimPlaybackStateGet(chara);
                if (keyframeState == 1)
                {
                    func_80085D78(reset);
                }
            }
            break;

        case 2:
            if (chara == &g_SysWork.playerWork.player)
            {
                g_MapOverlayHdr.playerAnimLock();
            }
            else
            {
                g_MapOverlayHdr.charaAnimLock(chara);
            }
            break;

        case 3:
            if (chara == &g_SysWork.playerWork.player)
            {
                g_MapOverlayHdr.playerAnimUnlock();
            }
            else
            {
                g_MapOverlayHdr.charaAnimUnlock(chara);
            }
            break;

        case 4:
            if (chara == &g_SysWork.playerWork.player)
            {
                g_MapOverlayHdr.playerAnimUnlock();
                g_MapOverlayHdr.playerAnimReset();
            }
            else
            {
                g_MapOverlayHdr.charaAnimReset(chara);
            }
            break;
    }
}

void func_8008605C(e_EventFlag eventFlagIdx, s32 stepTrue, s32 stepFalse, bool stepSecondary) // 0x8008605C
{
    if (!Savegame_EventFlagGet(eventFlagIdx))
    {
        func_80085DC0(stepSecondary, stepFalse);
    }
    else
    {
        func_80085DC0(stepSecondary, stepTrue);
    }
}

void MapMsg_DisplayAndHandleSelection(bool hasSelection, s32 mapMsgIdx, s32 step0, s32 step1, s32 step2, bool stepSecondary) // 0x800860B0
{
    s32 mapMsgState;

    mapMsgState = Gfx_MapMsg_Draw(mapMsgIdx);
    if (mapMsgState <= MapMsgState_Idle)
    {
        return;
    }

    if (!hasSelection)
    {
        func_80085D78(stepSecondary);
        return;
    }

    if (mapMsgState == MapMsgState_SelectEntry0)
    {
        func_80085DC0(stepSecondary, step0);
    }
    if (mapMsgState == MapMsgState_SelectEntry1)
    {
        func_80085DC0(stepSecondary, step1);
    }
    if (mapMsgState == MapMsgState_SelectEntry2)
    {
        func_80085DC0(stepSecondary, step2);
    }
}

void SysWork_StateStepIncrementAfterFade(s32 stateStep, bool cond, s32 fadeType, q19_12 fadeTimestep, bool reset) // 0x8008616C
{
    typedef enum _FadeType
    {
        FadeType_Black = 0,
        FadeType_White = 1,
        FadeType_Unk2  = 2, // TODO: Investigate. Some state machine flow logic when this is used.
        FadeType_Unk3  = 3  // TODO: Investigate.
    } s_FadeType;

    s32 activeStateStep;

    // If `stateStep != 2`, `field_14` dictates what happens. This field is manipulated often in map event functions.
    if (stateStep != 2)
    {
        activeStateStep = stateStep;
    }
    else
    {
        activeStateStep = g_SysWork.sysStateSteps[2];
    }

    switch (activeStateStep)
    {
        case 0:
            if (fadeType != FadeType_Unk2)
            {
                g_ScreenFadeTimestep = fadeTimestep;
            }

            if (cond)
            {
                if (fadeType == FadeType_Black)
                {
                    ScreenFade_Start(false, false, false);
                }
                else if (fadeType == FadeType_White)
                {
                    ScreenFade_Start(false, false, true);
                }
                else
                {
                    g_SysWork.cutsceneBorderState = 18;

                    if (fadeType == FadeType_Unk3)
                    {
                        g_SysWork.sysFlags |= SysFlag_CutsceneActive;
                    }
                }
            }
            else if (fadeType == FadeType_Black)
            {
                ScreenFade_Start(false, true, false);
            }
            else if (fadeType == FadeType_White)
            {
                ScreenFade_Start(false, true, true);
            }
            else
            {
                g_SysWork.cutsceneBorderState = 22;
            }

            if (stateStep != 0)
            {
                SysWork_StateStepIncrement(2);
            }
            break;

        case 1:
            if (fadeType < FadeType_Unk2)
            {
                if (cond || g_Screen_FadeStatus != activeStateStep)
                {
                    if (cond == activeStateStep && ScreenFade_IsFinished())
                    {
                        func_80085D78(reset);
                    }
                    break;
                }
            }
            else if ((cond || g_SysWork.cutsceneBorderState != activeStateStep) && !(cond == activeStateStep && g_SysWork.cutsceneBorderState == 21))
            {
                break;
            }

            func_80085D78(reset);
            break;
    }
}

void func_800862F8(s32 stateStep, e_FsFile fileIdx, bool reset) // 0x800862F8
{
    s32 activeStateStep;

    if (stateStep == 7)
    {
        activeStateStep = g_SysWork.sysStateSteps[2];
    }
    else
    {
        activeStateStep = stateStep;
        if (activeStateStep == 8)
        {
            activeStateStep = 1;
            if (g_SysWork.sysStateSteps[2] == 0)
            {
                activeStateStep = 4;
            }
        }
    }

    switch (activeStateStep)
    {
        case 0:
            Fs_QueueStartReadTim(fileIdx, FS_BUFFER_1, &g_ItemInspectionImg);

            if (stateStep != 0)
            {
                SysWork_StateStepIncrement(2);

                if (Fs_QueueChunksLoad())
                {
                    func_80085D78(reset);
                }
            }
            break;

        case 1:
            if (Fs_QueueChunksLoad())
            {
                func_80085D78(reset);
            }
            break;

        case 2:
            Screen_BackgroundImgDrawAlt(&g_ItemInspectionImg);
            break;

        case 3:
            DrawSync(SyncMode_Wait);
            StoreImage(&D_8002AB10, IMAGE_BUFFER_2);
            DrawSync(SyncMode_Wait);
            break;

        case 4:
            Fs_QueueStartReadTim(fileIdx, FS_BUFFER_1, &D_800A9A04);

            if (stateStep == 8)
            {
                SysWork_StateStepSet(2, 1);
            }
            break;

        case 5:
            Screen_BackgroundImgDrawAlt(&D_800A9A04);
            break;

        case 6:
            LoadImage(&D_8002AB10, IMAGE_BUFFER_2);
            DrawSync(SyncMode_Wait);
            break;
    }
}

void func_80086470(u32 stateStep, e_InvItemId itemId, s32 itemCount, bool reset) // 0x80086470
{
    s32 activeStateStep;

    if (stateStep == 6 && g_SysWork.sysStateSteps[2] == 0)
    {
        SysWork_StateStepSet(2, 2);
    }

    activeStateStep = stateStep;
    if (stateStep >= 2)
    {
        if (stateStep == 2)
        {
            activeStateStep = 3;
        }
        else if (stateStep == 3)
        {
            activeStateStep = 2;
        }
        else
        {
            activeStateStep = g_SysWork.sysStateSteps[2];
        }
    }

    switch (activeStateStep)
    {
        case 0:
            GameFs_UniqueItemModelLoad(itemId);

            if (stateStep == 0)
            {
                SysWork_StateStepIncrement(1);
                g_SysWork.sysStateSteps[1]--;
            }

            SysWork_StateStepIncrement(2);

        case 1:
            if (!Fs_QueueChunksLoad())
            {
                break;
            }

            func_80054A04(itemId);

            if (stateStep == 1 || stateStep == 4)
            {
                func_80085D78(reset);
                break;
            }

            SysWork_StateStepIncrement(2);

        case 2:
            SysWork_StateStepSet(2, 0);

            if (stateStep == 3 || stateStep == 6)
            {
                Inventory_AddSpecialItem(itemId, itemCount);
            }
            break;
    }
}

void func_800865FC(bool isPos, s32 idx0, s32 idx1, q3_12 angleY, q19_12 offsetOrPosX, q19_12 offsetOrPosZ) // 0x800865FC
{
    if (!isPos)
    {
        D_800C4640[idx0][idx1].vx = g_SysWork.playerWork.player.position.vx + offsetOrPosX;
        D_800C4640[idx0][idx1].vy = g_SysWork.playerWork.player.position.vy;
        D_800C4640[idx0][idx1].vz = g_SysWork.playerWork.player.position.vz + offsetOrPosZ;

        D_800C4700[idx0] = angleY;
    }
    else if (isPos == true)
    {
        D_800C4640[idx0][idx1].vx = offsetOrPosX;
        D_800C4640[idx0][idx1].vy = g_SysWork.playerWork.player.position.vy;
        D_800C4640[idx0][idx1].vz = offsetOrPosZ;

        D_800C4700[idx0] = angleY;
    }
}

void func_800866D4(s32 arg0, s32 arg1, bool reset) // 0x800866D4
{
    if (g_MapOverlayHdr.playerPathWaypointExecute(arg0, &D_800C4640, D_800C4700[0], arg1) == 1)
    {
        func_80085D78(reset);
    }
}

void func_80086728(s_SubCharacter* chara, s32 arg1, s32 arg2, bool reset) // 0x80086728
{
#ifdef SH_PC_PORT
    /* charaPathWaypointExecute is declared as (s32, s32, void*, s16, s32) but the actual
     * function (sharedFunc_800D8A00_0_s00) takes (s_SubCharacter*, s32, VECTOR3*, s32, s32).
     * On MIPS, pointer/s32 are both 32-bit so this works. On x86-64, the
     * pointer gets truncated to s32. Cast to the real signature. */
    {
        typedef bool (*NpcWaypointFunc)(s_SubCharacter*, s32, VECTOR3*, s32, s32);
        NpcWaypointFunc realFunc = (NpcWaypointFunc)g_MapOverlayHdr.charaPathWaypointExecute;
        if (realFunc == NULL) return;
        if (realFunc(chara, arg1, &D_800C4640[1][0], D_800C4700[1], arg2) == 1)
        {
            func_80085D78(reset);
        }
    }
#else
    if (g_MapOverlayHdr.charaPathWaypointExecute(chara, arg1, &D_800C4640[1][0], D_800C4700[1], arg2) == 1)
    {
        func_80085D78(reset);
    }
#endif
}

void func_8008677C(s_SubCharacter* chara, s32 arg1, s32 arg2) // 0x8008677C
{
#ifdef SH_PC_PORT
    typedef bool (*NpcWaypointFunc)(s_SubCharacter*, s32, VECTOR3*, s32, s32);
    ((NpcWaypointFunc)g_MapOverlayHdr.charaPathWaypointExecute)(chara, arg1, &D_800C4640[1][0], D_800C4700[1], arg2);
#else
    g_MapOverlayHdr.charaPathWaypointExecute(chara, arg1, &D_800C4640[1][0], D_800C4700[1], arg2);
#endif
}

void func_800867B4(s32 state, s32 paperMapFileIdx) // 0x800867B4
{
    switch (state)
    {
        case 0:
            DrawSync(SyncMode_Wait);
            StoreImage(&D_8002AB10, IMAGE_BUFFER_2);
            DrawSync(SyncMode_Wait);

            Fs_QueueStartReadTim(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[paperMapFileIdx], FS_BUFFER_2, &g_PaperMapImg);
            Fs_QueueStartReadTim(FILE_TIM_MR_0TOWN_TIM + g_PaperMapMarkingFileIdxs[paperMapFileIdx], FS_BUFFER_1, &g_PaperMapMarkingAtlasImg);

            Screen_Init(SCREEN_WIDTH, true);
            GsSwapDispBuff();
            Fs_QueueWaitForEmpty();
#ifdef SH_PC_PORT
            g_PcMapScreenActive = 1;
#endif
            break;

        case 1:
#ifdef SH_PC_PORT
            /* Same framebuffer-readback corruption as Event_MapTake — this
             * helper is used by gameplay sub-state map-zoom events (map1_s06
             * church, map2_s00 waterworks/school/sketchbook). Re-upload TIM
             * each frame before sampling. */
            PaperMap_ReuploadTimToVram_PC();
#endif
            Screen_BackgroundImgDraw(&g_PaperMapImg);
            break;

        case 2:
            LoadImage(&D_8002AB10, IMAGE_BUFFER_2);
            DrawSync(SyncMode_Wait);
            Screen_Init(SCREEN_WIDTH, false);
#ifdef SH_PC_PORT
            g_PcMapScreenActive = 0;
#endif
            break;
    }
}

void func_800868DC(s32 idx)
{
    D_800C4710[idx] = 0;
}

s32 func_800868F4(s32 arg0, s32 arg1, s32 idx)
{
    D_800C4710[idx] += g_DeltaTime;
    D_800C4710[idx]  = (arg1 < D_800C4710[idx]) ? arg1 : D_800C4710[idx];

    return (arg0 * D_800C4710[idx]) / arg1;
}

s32 func_8008694C(s32 arg0, s16 arg1, s16 arg2, s32 arg3, s32 idx)
{
    D_800C4710[idx] += g_DeltaTime;
    D_800C4710[idx] = (arg3 < D_800C4710[idx]) ? arg3 : D_800C4710[idx];
    return Q12_MULT(arg0, Math_Sin(arg1 + ((arg2 * D_800C4710[idx]) / arg3)));
}

void Map_MessageWithAudio(s32 mapMsgIdx, u8* audioIdx, const u16* audioCmds) // 0x800869E4
{
    s32 mapMsgState;

    g_SysWork.bgmStatusFlags |= BgmStatusFlag_VoiceDialog;

    mapMsgState = Gfx_MapMsg_Draw(mapMsgIdx);
    if (mapMsgState == MapMsgState_SelectEntry0)
    {
        SysWork_StateStepIncrement(0);
    }
    else if (mapMsgState == MapMsgState_Finish)
    {
        SD_Call(audioCmds[*audioIdx]);
        *audioIdx += 1;
    }
}

void Camera_PositionSet(VECTOR3* pos, q19_12 offsetOrPosX, q19_12 offsetOrPosY, q19_12 offsetOrPosZ,
                        q19_12 accelXz, q19_12 accelY, q19_12 speedXzMax, q19_12 speedYMax, bool warp) // 0x80086A94
{
    VECTOR3         posTarget;
    VC_CAM_MV_PARAM camMoveParams;

    // Set position target.
    if (pos != NULL)
    {
        posTarget.vx = pos->vx + offsetOrPosX;
        posTarget.vy = pos->vy + offsetOrPosY;
        posTarget.vz = pos->vz + offsetOrPosZ;
    }
    else
    {
        posTarget.vx = offsetOrPosX;
        posTarget.vy = offsetOrPosY;
        posTarget.vz = offsetOrPosZ;
    }

    // Set acceleration on XZ plane.
    if (accelXz == Q12(0.0f))
    {
        camMoveParams.accel_xz = cam_mv_prm_user.accel_xz;
    }
    else
    {
        camMoveParams.accel_xz = accelXz;
    }

    // Set acceleration on Y axis.
    if (accelY == Q12(0.0f))
    {
        camMoveParams.accel_y = cam_mv_prm_user.accel_y;
    }
    else
    {
        camMoveParams.accel_y = accelY;
    }

    // Set max speed on XZ plane.
    if (speedXzMax == Q12(0.0f))
    {
        camMoveParams.max_spd_xz = cam_mv_prm_user.max_spd_xz;
    }
    else
    {
        camMoveParams.max_spd_xz = speedXzMax;
    }

    // Set max speed on Y axis.
    if (speedYMax == Q12(0.0f))
    {
        camMoveParams.max_spd_y = cam_mv_prm_user.max_spd_y;
    }
    else
    {
        camMoveParams.max_spd_y = speedYMax;
    }

    // Set camera position target.
    vcUserCamTarget(&posTarget, &camMoveParams, warp);
}

void Camera_LookAtSet(VECTOR3* lookAt, q19_12 lookAtOffsetOrPosX, q19_12 lookAtOffsetOrPosY, q19_12 lookAtOffsetOrPosZ,
                      q19_12 angularAccelX, q19_12 angularAccelY, q19_12 angularSpeedXMax, q19_12 angularSpeedYMax, bool warp) // 0x80086B70
{
    VECTOR3           lookAtTarget;
    VC_WATCH_MV_PARAM camLookAtMoveParams;

    // Set look-at target.
    if (lookAt != NULL)
    {
        lookAtTarget.vx = lookAt->vx + lookAtOffsetOrPosX;
        lookAtTarget.vy = lookAt->vy + lookAtOffsetOrPosY;
        lookAtTarget.vz = lookAt->vz + lookAtOffsetOrPosZ;
    }
    else
    {
        lookAtTarget.vx = lookAtOffsetOrPosX;
        lookAtTarget.vy = lookAtOffsetOrPosY;
        lookAtTarget.vz = lookAtOffsetOrPosZ;
    }

    // Set angular acceleration on X axis.
    if (angularAccelX == Q12_ANGLE(0.0f))
    {
        camLookAtMoveParams.ang_accel_x = deflt_watch_mv_prm.ang_accel_x;
    }
    else
    {
        camLookAtMoveParams.ang_accel_x = angularAccelX;
    }

    // Set angular acceleration on Y axis.
    if (angularAccelY == Q12_ANGLE(0.0f))
    {
        camLookAtMoveParams.ang_accel_y = deflt_watch_mv_prm.ang_accel_y;
    }
    else
    {
        camLookAtMoveParams.ang_accel_y = angularAccelY;
    }

    // Set max angular speed on X axis.
    if (angularSpeedXMax == Q12_ANGLE(0.0f))
    {
        camLookAtMoveParams.max_ang_spd_x = deflt_watch_mv_prm.max_ang_spd_x;
    }
    else
    {
        camLookAtMoveParams.max_ang_spd_x = angularSpeedXMax;
    }

    // Set max angular speed on Y axis.
    if (angularSpeedYMax == Q12_ANGLE(0.0f))
    {
        camLookAtMoveParams.max_ang_spd_y = deflt_watch_mv_prm.max_ang_spd_y;
    }
    else
    {
        camLookAtMoveParams.max_ang_spd_y = angularSpeedYMax;
    }

    // Set camera flags and rotation target.
    vcWorkSetFlags(0, VC_VISIBLE_CHARA_F);
    vcUserWatchTarget(&lookAtTarget, &camLookAtMoveParams, warp);
}

void func_80086C58(s_SubCharacter* chara, s32 arg1) // 0x80086C58
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            func_80085EB8(0, chara, arg1, false);
            SysWork_StateStepIncrement(1);
            break;

        case 1:
            func_80085EB8(1, chara, 0, true);
            break;

        default:
            SysWork_StateStepIncrement(0);
            break;
    }
}

void func_80086D04(s_SubCharacter* chara) // 0x80086D04
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            func_80085EB8(3, chara, 0, false);
            SysWork_StateStepIncrement(1);
            break;

        case 1:
            func_80085EB8(1, chara, 0, true);
            break;

        default:
            SysWork_StateStepIncrement(0);
            break;
    }
}

void func_80086DA8(e_FsFile fileIdx, q19_12 fadeTimestep) // 0x80086DA8
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            SysWork_StateStepIncrementAfterFade(0, true, 0, fadeTimestep, false);
            SysWork_StateStepIncrement(1);

        case 1:
            func_800862F8(7, fileIdx, true);
            break;

        default:
            SysWork_StateStepIncrementAfterFade(1, true, 0, Q12(0.0f), false);
            break;
    }
}

void func_80086E50(e_FsFile fileIdx, q19_12 fadeTimestep0, q19_12 fadeTimestep1) // 0x80086E50
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            SysWork_StateStepIncrementAfterFade(0, true, 0, fadeTimestep0, false);
            SysWork_StateStepIncrement(1);

        case 1:
            func_800862F8(7, fileIdx, true);
            break;

        case 2:
            SysWork_StateStepIncrementAfterFade(1, true, 0, Q12(0.0f), true);
            break;

        default:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, false, 0, fadeTimestep1, false);
    }
}

void func_80086F44(q19_12 fadeTimestep0, q19_12 fadeTimestep1) // 0x80086F44
{
    if (g_SysWork.sysStateSteps[1] == 0)
    {
        func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
        SysWork_StateStepIncrementAfterFade(2, true, 0, fadeTimestep1, true);
        return;
    }

    SysWork_StateStepIncrementAfterFade(0, false, 0, fadeTimestep0, false);
    SysWork_StateStepIncrement(0);
}

void Map_MessageWithSfx(s32 mapMsgIdx, e_SfxId sfxId, VECTOR3* sfxPos) // 0x80086FE8
{
    s32 i;

    if (!(g_SysWork.sysFlags & SysFlag_5))
    {
        // Run through NPCs.
        for (i = 0; i < ARRAY_SIZE(g_SysWork.npcs); i++)
        {
            if (g_SysWork.npcs[i].model.charaId >= Chara_Harry &&
                g_SysWork.npcs[i].model.charaId <= Chara_MonsterCybil &&
                g_SysWork.npcs[i].health > Q12(0.0f))
            {
                break;
            }
        }

        if (i != ARRAY_SIZE(g_SysWork.npcs))
        {
            g_DeltaTime = Q12(0.0f);
        }
    }

    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            g_MapOverlayHdr.playerControlFreeze();
            func_8005DC1C(sfxId, sfxPos, Q8(0.5f), 0);
            SysWork_StateStepIncrement(1);

        case 1:
            SysWork_StateStepIncrementDelayed(Q12(0.2f), true);
            break;

        case 2:
            MapMsg_DisplayAndHandleSelection(false, mapMsgIdx, 0, 0, 0, true);
            break;

        default:
            g_MapOverlayHdr.playerControlUnfreeze(0);
            SysWork_StateSetNext(SysState_Gameplay);
            break;
    }
}

void func_8008716C(e_FsFile textureFileIdx, q19_12 fadeTimestep0, q19_12 fadeTimestep1) // 0x8008716C
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            g_MapOverlayHdr.playerControlFreeze();
            SysWork_StateStepIncrementAfterFade(0, true, 0, fadeTimestep0, false);
            SysWork_StateStepIncrement(1);

        case 1:
            func_800862F8(7, textureFileIdx, true);
            break;

        case 2:
            SysWork_StateStepIncrementAfterFade(1, true, 0, Q12(0.0f), true);
            break;

        case 3:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, false, 0, fadeTimestep1, true);
            break;

        case 4:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);

            if (g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.enter |
                                                 g_GameWorkPtr->config.controllerConfig.cancel))
            {
                SysWork_StateStepIncrement(1);
            }
            break;

        case 5:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, true, 0, fadeTimestep1, true);
            break;

        default:
            SysWork_StateStepIncrementAfterFade(0, false, 0, fadeTimestep0, false);
            g_MapOverlayHdr.playerControlUnfreeze(0);
            SysWork_StateSetNext(SysState_Gameplay);
            break;
    }
}

void MapMsg_DisplayWithTexture(e_FsFile textureFileIdx, q19_12 fadeTimestep0, q19_12 fadeTimestep1, s32 mapMsgIdx) // 0x80087360
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            g_MapOverlayHdr.playerControlFreeze();
            SysWork_StateStepIncrementAfterFade(0, true, 0, fadeTimestep0, false);
            SysWork_StateStepIncrement(1);

        case 1:
            func_800862F8(7, textureFileIdx, true);
            break;

        case 2:
            SysWork_StateStepIncrementAfterFade(1, true, 0, Q12(0.0f), true);
            break;

        case 3:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, false, 0, fadeTimestep1, true);
            break;

        case 4:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            MapMsg_DisplayAndHandleSelection(false, mapMsgIdx, 0, 0, 0, true);
            break;

        case 5:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, true, 0, fadeTimestep1, true);
            break;

        default:
            SysWork_StateStepIncrementAfterFade(0, false, 0, fadeTimestep0, false);
            g_MapOverlayHdr.playerControlUnfreeze(0);
            SysWork_StateSetNext(SysState_Gameplay);
            break;
    }
}

void MapMsg_DisplayWithTexture1(e_FsFile textureFileIdx, q19_12 fadeTimestep0, q19_12 fadeTimestep1, s32 mapMsgIdx0, s32 mapMsgIdx1) // 0x80087540
{
    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            g_MapOverlayHdr.playerControlFreeze();
            SysWork_StateStepIncrementAfterFade(0, true, 0, fadeTimestep0, false);
            SysWork_StateStepIncrement(1);

        case 1:
            func_800862F8(7, textureFileIdx, true);
            break;

        case 2:
            SysWork_StateStepIncrementAfterFade(1, true, 0, Q12(0.0f), true);
            break;

        case 3:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, false, 0, fadeTimestep1, true);
            break;

        case 4:
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);

            if (mapMsgIdx0 != MapMsgCode_None)
            {
                MapMsg_DisplayAndHandleSelection(false, mapMsgIdx0, 0, 0, 0, true);
                break;
            }

            if (g_Controller0->clickedBtnFlags & (g_GameWorkPtr->config.controllerConfig.enter |
                                                 g_GameWorkPtr->config.controllerConfig.cancel))
            {
                SysWork_StateStepIncrement(1);
            }
            break;

        case 5:
            g_Screen_BackgroundImgGamma = Q8(6.0f / 32.0f);
            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            MapMsg_DisplayAndHandleSelection(false, mapMsgIdx1, 0, 0, 0, true);
            break;

        case 6:
            g_Screen_BackgroundImgGamma = Q8(6.0f / 32.0f);

            func_800862F8(2, FILE_1ST_2ZANKO80_TIM, false);
            SysWork_StateStepIncrementAfterFade(2, true, 0, fadeTimestep1, true);
            break;

        default:
            SysWork_StateStepIncrementAfterFade(0, false, 0, fadeTimestep0, false);
            g_MapOverlayHdr.playerControlUnfreeze(0);
            SysWork_StateSetNext(SysState_Gameplay);
            break;
    }
}

void Event_MapTake(s32 mapFlagIdx, e_EventFlag eventFlagIdx, s32 mapMsgIdx) // 0x80087AF4
{
    static const RECT RECT = {
        SCREEN_POSITION_X(100.0f), 256,
        SCREEN_WIDTH / 2, SCREEN_HEIGHT
    };

    s32 mapFlagIdxCpy;

    g_DeltaTime   = Q12(0.0f);
    mapFlagIdxCpy = mapFlagIdx;

    switch (g_SysWork.sysStateSteps[1])
    {
        case 0:
            g_MapOverlayHdr.playerControlFreeze();
            Fs_QueueStartSeek(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[mapFlagIdx]);
            SysWork_StateStepIncrement(1);

        case 1:
            SysWork_StateStepIncrementAfterFade(2, true, 0, Q12(0.0f), true);
            break;

        case 2:
            DrawSync(SyncMode_Wait);
            StoreImage(&RECT, IMAGE_BUFFER);
            DrawSync(SyncMode_Wait);
            Fs_QueueStartReadTim(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[mapFlagIdx], FS_BUFFER_2, &g_PaperMapImg);
            Screen_Init(SCREEN_WIDTH, true);
#ifdef SH_PC_PORT
            g_PcMapScreenActive = 1;
#endif
            g_IntervalVBlanks = 1;

            GsSwapDispBuff();
            SysWork_StateStepIncrementAfterFade(0, false, 0, Q12(0.0f), false);
            Fs_QueueWaitForEmpty();

            SysWork_StateStepIncrement(1);

        case 3:
            g_Screen_BackgroundImgGamma = Q8(11.0f / 32.0f);

#ifdef SH_PC_PORT
            PaperMap_ReuploadTimToVram_PC();
#endif
            Screen_BackgroundImgDraw(&g_PaperMapImg);
            MapMsg_DisplayAndHandleSelection(true, mapMsgIdx, 4, 5, 0, true); // 4 is "No", 5 is "Yes".
            break;

        case 4:
            mapFlagIdxCpy                                            = mapFlagIdx >> 5;
            ((s32*)&g_SavegamePtr->paperMapFlags)[mapFlagIdxCpy] |= 1 << (mapFlagIdx & 0x1F); // Maybe union?

            switch (mapFlagIdx)
            {
                case 6:
                    g_SavegamePtr->paperMapFlags |= 0x1FA0;
                    break;

                case 17:
                    g_SavegamePtr->paperMapFlags |= 1 << 18;
                    g_SavegamePtr->paperMapFlags |= 1 << 19;
                    g_SavegamePtr->paperMapFlags |= 1 << 21;
                    g_SavegamePtr->paperMapFlags |= 1 << 22;
                    g_SavegamePtr->paperMapFlags |= 1 << 23;
                    break;

                case 16:
                    g_SavegamePtr->paperMapFlags |= 1 << 20;
                    break;

                case 13:
                    g_SavegamePtr->paperMapFlags |= 1 << 14;
                    break;

                case 2:
                    g_SavegamePtr->paperMapFlags |= 1 << 3;
                    break;
            }

            Savegame_EventFlagSet(eventFlagIdx);
            SysWork_StateStepIncrement(1);

        case 5:
            g_Screen_BackgroundImgGamma = Q8(11.0f / 32.0f);

#ifdef SH_PC_PORT
            PaperMap_ReuploadTimToVram_PC();
#endif
            Screen_BackgroundImgDraw(&g_PaperMapImg);
            SysWork_StateStepIncrementAfterFade(2, true, 0, Q12(0.0f), true);
            break;

        default:
            LoadImage(&RECT, IMAGE_BUFFER);
            DrawSync(SyncMode_Wait);
            Screen_Init(SCREEN_WIDTH, false);
#ifdef SH_PC_PORT
            g_PcMapScreenActive = 0;
#endif
            SysWork_StateStepIncrementAfterFade(0, false, 0, Q12(0.0f), false);

            g_MapOverlayHdr.playerControlUnfreeze(0);
            SysWork_StateSetNext(SysState_Gameplay);
            break;
    }
}
