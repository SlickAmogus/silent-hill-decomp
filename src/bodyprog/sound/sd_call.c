#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include "xa_player.h"
#include <stdio.h>
#endif

#include <psyq/libcd.h>
#include <psyq/libetc.h>
#include <psyq/libspu.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/libsd.h"
#include "bodyprog/math/math.h"
#include "main/fsqueue.h"

#define BSS_HACK_SD_CALL_C
#include "bodyprog/sound/sound_system.h"

#ifndef PAD_HACK_IGNORE
    const s32 pad_rodata_80025BF4 = 0;
    s32 __pad_bss_800C15EC;
    s32 __pad_bss_800C1674;
    s32 __pad_bss_800C1694;
    char __pad_bss_800C37C9[3];
    char __pad_bss_800C37D1[3];
#endif

// ========================================
// DEFINES
// ========================================

#define VAB_BUFFER_LIMIT 0xC800u

#ifdef SH_PC_PORT
/* Emitter-azimuth side-channel, armed in Vc_StereoBalanceGet (vc_util.c) by
 * the positional-SFX wrappers on this same call chain. Every key-on/update
 * entry below claims-and-clears it so no unrelated sound inherits an angle. */
extern s32  g_Pc_SfxAzimuth, g_Pc_SfxAzimuthValid;
extern void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuthQ12);
extern void PsyX_SPUAL_ClearNextKeyOnAzimuth(void);
extern void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12);
#endif

// ========================================
// STATIC VARIABLES
// ========================================

CdlLOC     XaCdLocation;
static s_800C15F0 D_800C15F0[4];

/** @brief `e_SfxId` | Stores the index of the currently playing SFX. */
static u16 g_AudioPlayingIdxList[24];

/** @brief Stores the pitch of currently playing SFX.
* Shares the same index of where the SFX is stored in `g_AudioPlayingIdxList`.
*/
static s16            g_AudioPlayingPitchList[24];
static s_Sd_AudioWork g_Sd_AudioWork;

/** @brief Holds states for different audio types streaming. */
static s_AudioStreamingStates g_Sd_AudioStreamingStates;

/** @note Name from retrieved debug symbols in Konami International Rally Championship.
 *
 * Symbols don't suggest a struct name. The size may not
 * contradict this definition as it seems the variable is intended
 * to be 48/0x30 bytes, while this is 16/0x10
 * bytes. They are used to adjust the volume channels.
 */
static s_ChannelsVolumeController gSDVolConfig;

static s_800C1688                 D_800C1688;

/** @brief Task pool related to audio and audio data streaming.
 * `Sd_TaskPoolExecute` is the main function responsible for executing tasks.
 *
 * @note Possible name retrieved from debug symbols.
 * `Tokimeki Memorial ~Forever With You~` symbols have a global variable named
 * `gSDEvt`. This function can't be restored, but the name would fit for
 * this purpose. This game also features a similar command pool system
 * to the one in SH1.
 */
static s_VabPlayingInfo g_Sd_VabPlayingInfo;
static u8               g_Sd_TaskPool[32];
/** Dead code. */
s32              D_800C16C8[0x840];
/** @brief The type of audio file being loaded. See `e_AudioType`. */
u8               g_Sd_AudioType;

/** @brief Amount of data moved when loading KDT/VAB files. */
u32              g_Sd_DataMoved;
u8               g_Sd_VabLoadAttemps;
/* @brief Pointer to the data of the VAB loading to be used for music. */
s_AudioItemData* g_Sd_VabTargetLoad;
s_AudioItemData* g_Sd_KdtTargetLoad;
u8               D_800C37DC; // Boolean.
u8               g_Sd_CurrentTask;

// ========================================
// GENERAL AUDIO SYSTEM CORE
// ========================================

void SD_Call(u32 cmd) // 0x80045A7C
{
    // Execute sound command based on category.
    switch ((cmd >> 8) & 0xFF)
    {
        // Sound effect management and VAB + KDT file loading. Range [0, 255].
        case 0:
            SD_BranchCTRL(cmd);
            return;

        // Related to music. Range [300, 1279].
        case 3:
        case 4:
            func_80046A24(cmd);
            return;

        // Play SFX. Range [1280, 1791].
        case 5:
        case 6:
            Sd_PlaySfx(cmd, 0, 0);
            return;

        // Stop SFX. Range [1792, 2303].
        case 7:
        case 8:
            Sd_SfxStopStep(cmd - 0x200);
            return;

        // Unknown. Range [2816, 3327].
        case 11:
        case 12:
            Sd_LastSfxStop();
            return;

        // Play XA audio files (voice lines). Range [4096, 5887].
        case 16:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
            Sd_XaAudioPlayTaskAdd(cmd);
    }
}

u8 Sd_AudioStreamingCheck(void) // 0x80045B28
{
    u8 state;

    state = 1;
    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        return state;
    }

    state = 2;
    if (g_Sd_AudioWork.isAudioLoading_15 == 0)
    {
        if (g_Sd_AudioStreamingStates.xaPreLoadState_3 != 0)
        {
            D_800C1688.field_8 = VSync(SyncMode_Count);
            D_800C1688.field_4 = 0;
            return 3;
        }

        if (D_800C37DC == false)
        {
            if (g_Sd_CurrentTask == 0)
            {
                return 0;
            }

            state = 5;
            return state;
        }

        state = 4;
        return state;
    }

    return state;
}

u16 func_80045BC8(void) // 0x80045BC8
{
    return g_Sd_AudioWork.field_E;
}

void SD_BranchCTRL(u16 task) // 0x80045BD8
{
    switch (task)
    {
        case 1: // Set audio to mono.
            Sd_AudioSystemSet(false);
            break;

        case 2: // Set audio to stereo.
            Sd_AudioSystemSet(true);
            break;

        case 16:
            Sd_AllSfxStop();
            Sd_LastSfxStop();
            break;

        case 17:
            Sd_AllSfxWithRRStop();
            Sd_LastSfxStop();
            break;

        case 18:
            Sd_StopBgmTaskAdd();
            break;

        case 21:
            Sd_AllSfxWithRRStop();

        case 20:
            Sd_AllSfxStop();
            Sd_LastSfxStop();
            Sd_StopBgmTaskAdd();

        case 19:
            Sd_XaAudioStopTaskAdd();
            break;

        case 22:
            g_Sd_AudioWork.bgmFadeSpeed_14 = 1;

        default:
            break;

        case 23:
            g_Sd_AudioWork.bgmFadeSpeed_14 = 2;
            break;

        case 3:
            g_Sd_AudioWork.muteGame_17 = true;
            break;

        case 4:
            g_Sd_AudioWork.muteGame_17 = false;
            break;
    }

    // Load VAB audio.
    if (task >= 160 && task < 245)
    {
        Sd_VabLoad_TaskAdd(task);
    }

    // Load KDT song file and VAB music samples.
    // Passes command to previous conditional to load VAB audio.
    if (task >= 32 && task < 72)
    {
        Sd_KdtLoad_TaskAdd(task);
    }
}

void Sd_AudioSystemSet(u8 isStereo) // 0x80045D28
{
    CdlATV vol;

    switch (isStereo)
    {
        case false:
            SdSetMono();

            // SPU (L).
            vol.val0 =
            vol.val2 = 79;

            // SPU (R).
            vol.val1 =
            vol.val3 = 79;

            CdMix(&vol);

            gSDVolConfig.volumeSe_4           = 127;
            g_Sd_AudioWork.isStereoEnabled_12 = false;
            return;

        case true:
            SdSetStereo();

            // SPU (L).
            vol.val0 =
            vol.val2 = 127;

            // SPU (R).
            vol.val1 =
            vol.val3 = 0;
            CdMix(&vol);

            gSDVolConfig.volumeSe_4           = 127;
            g_Sd_AudioWork.isStereoEnabled_12 = true;
            return;
    }
}

void SD_Init(void) // 0x80045DD4
{
    SdInit();
    SdSetTickMode(1);
    Sd_AudioSystemSet(true);
    SdSetReservedVoice(24);
    SdStart();
    SdSetTableSize(&D_800C16C8, 16, 3);

    gSDVolConfig.globalVolumeSe_C  = OPT_SOUND_VOLUME_MAX - 1;
    gSDVolConfig.globalVolumeBgm_D = OPT_SOUND_VOLUME_MAX - 1;
    gSDVolConfig.globalVolumeXa_E  = OPT_SOUND_VOLUME_MAX - 1;

    sd_work_init();
}

void sd_work_init(void) // 0x80045E44
{
    static s32 i;

    SdSetAutoKeyOffMode(0);
    SdUtSetReverbType(1);
    SpuClearReverbWorkArea(1);
    SdUtReverbOn();
    SpuSetTransferMode(0);

    gSDVolConfig.reverbDepth_F = 20;

    SdUtSetReverbDepth(20, 20);
    Sd_SetReverbEnable(0);
    SdSetSerialAttr(0, 0, 0);
    Sd_SetVolXa(0, 0);

    D_800C15F0[0].field_0 = CdlModeSpeed;
    Sd_CdPrimitiveCmdTry(CdlSetmode, &D_800C15F0[0].field_0, NULL);

    for (i = 0; i < (ARRAY_SIZE(g_Sd_TaskPool) - 1); i++)
    {
        g_Sd_TaskPool[i] = 0;
    }

    for (i = 0; i < ARRAY_SIZE(g_AudioPlayingIdxList); i++)
    {
        g_AudioPlayingIdxList[i] = 0;
    }

    g_Sd_AudioWork.bgmLoadedSongIdx_6         = 0;
    g_Sd_AudioWork.lastVabAudioLoadedIdx_8[0] = 0;
    g_Sd_AudioWork.lastVabAudioLoadedIdx_8[1] = 0;
    g_Sd_AudioWork.lastVabAudioLoadedIdx_8[2] = 0;
    g_Sd_AudioWork.xaAudioIdx_4               = 0;
    g_Sd_AudioWork.isXaStopping_13            = false;
    g_Sd_AudioWork.cdErrorCount_0             = 0;
    g_Sd_AudioWork.bgmFadeSpeed_14            = 0;
    g_Sd_AudioWork.isAudioLoading_15          = false;
    g_Sd_AudioWork.isXaNotPlaying_16          = false;
    g_Sd_AudioWork.muteGame_17                = 0;
    gSDVolConfig.volumeGlobal_A               = 127;

    SdSetMVol(127, 127);

    D_800C37DC                                 = false;
    g_Sd_AudioWork.field_E                     = 0;
    g_Sd_AudioWork.field_10                    = 0;
    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Reset;
    g_Sd_AudioStreamingStates.xaLoadState_1    = XaLoadState_Initialize;
    g_Sd_AudioStreamingStates.xaStopState_2    = 0;
    g_Sd_AudioStreamingStates.xaPreLoadState_3 = 0;
    gSDVolConfig.volumeXa_0                    = 84;
    gSDVolConfig.volumeBgm_8                   = 40;
    gSDVolConfig.volumeBgm_6                   = 40;

    Sd_SetVolBgm(gSDVolConfig.volumeBgm_6, gSDVolConfig.volumeBgm_6);
}

// ========================================
// AUDIO VAB
// ========================================

void Sd_AudioStop(void) // 0x80045FF8
{
    s32 i;

    SdSeqClose(0);

    for (i = 4; i >= 0; i--)
    {
        SdVabClose(i);
    }

    SdEnd();
    SdQuit();
}

static inline void WriteVolume(s16* left, s16* right, s16 vol)
{
    *left  = vol;
    *right = vol;
}

u8 Sd_PlaySfx(u16 sfxId, q0_7 balance, u8 vol) // 0x80046048
{
    static s16   audioIdx;
    SpuVoiceAttr attr;
    s16          convertedVol;
    s16          volCpy;
    s32          i;

    if (sfxId == Sfx_Base)
    {
        return NO_VALUE;
    }

#ifdef SH_PC_PORT
    /* Temporary: log every SPU SFX play so a looping sound (e.g. the good+
     * ending's stuck Cybil-grunt / boss-attack loops) can be identified by id
     * and given an explicit Sd_SfxStop on PC (no SPU voice-stealing here). */
    SH_DBG("[SFX] Sd_PlaySfx: sfxId=%d (Sfx_Base+%d) vol=%d", sfxId, sfxId - Sfx_Base, vol);

    /* Hand the emitter azimuth (latched by Vc_StereoBalanceGet on this same
     * call chain) to PsyCross for the voice about to key on. Claim-and-clear
     * unconditionally so a sound played with no positional source (UI beeps,
     * SD_Call band 5/6) never inherits a stale angle. */
    if (g_Pc_SfxAzimuthValid)
    {
        PsyX_SPUAL_SetNextKeyOnAzimuth(g_Pc_SfxAzimuth);
    }
    else
    {
        PsyX_SPUAL_ClearNextKeyOnAzimuth();
    }
    g_Pc_SfxAzimuthValid = 0;
#endif

    audioIdx = sfxId - Sfx_Base;
    volCpy   = vol;

    // Copy key VAB information.
    g_Sd_VabPlayingInfo.typeIdx = g_Vab_InfoTable[audioIdx].vab_progIdx_2 >> 8;
    g_Sd_VabPlayingInfo.progIdx = g_Vab_InfoTable[audioIdx].vab_progIdx_2 & 0xFF;
    g_Sd_VabPlayingInfo.noteIdx = g_Vab_InfoTable[audioIdx].noteIdx_4;

    convertedVol = gSDVolConfig.volumeSe_4 + g_Vab_InfoTable[audioIdx].field_5;
    convertedVol = convertedVol - (convertedVol * volCpy) / 255;

    WriteVolume(&g_Sd_VabPlayingInfo.volumeLeft, &g_Sd_VabPlayingInfo.volumeRight, convertedVol);

    // Apply stereo balance.
    if (g_Sd_AudioWork.isStereoEnabled_12 == true)
    {
        if (balance < 0)
        {
            g_Sd_VabPlayingInfo.volumeRight -= (g_Sd_VabPlayingInfo.volumeLeft * ABS(balance)) >> 7;
        }
        else
        {
            g_Sd_VabPlayingInfo.volumeLeft -= (g_Sd_VabPlayingInfo.volumeLeft * balance) >> 7;
        }
    }

    // Clamp volume to positive range.
    if (g_Sd_VabPlayingInfo.volumeLeft < 0)
    {
        g_Sd_VabPlayingInfo.volumeLeft = 0;
    }
    if (g_Sd_VabPlayingInfo.volumeRight < 0)
    {
        g_Sd_VabPlayingInfo.volumeRight = 0;
    }

    if (sfxId == Sfx_RadioInterferenceLoop)
    {
        g_Sd_VabPlayingInfo.toneIdx = g_Vab_InfoTable[audioIdx].audioVabIdx;
        SdUtKeyOnV(22, g_Sd_VabPlayingInfo.typeIdx, g_Sd_VabPlayingInfo.progIdx, g_Sd_VabPlayingInfo.toneIdx, g_Sd_VabPlayingInfo.noteIdx, 0,
                   Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeLeft), Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeRight));
        g_Sd_VabPlayingInfo.audioVabIdx = 22;
    }
    else if (sfxId == Sfx_RadioStaticLoop)
    {
        g_Sd_VabPlayingInfo.toneIdx = g_Vab_InfoTable[audioIdx].audioVabIdx;
        SdUtKeyOnV(23, g_Sd_VabPlayingInfo.typeIdx, g_Sd_VabPlayingInfo.progIdx, g_Sd_VabPlayingInfo.toneIdx, g_Sd_VabPlayingInfo.noteIdx, 120,
                   Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeLeft), Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeRight));
        g_Sd_VabPlayingInfo.audioVabIdx = 23;
    }
    else
    {
        g_Sd_VabPlayingInfo.audioVabIdx = SdVoKeyOn(g_Vab_InfoTable[audioIdx].vab_progIdx_2, g_Sd_VabPlayingInfo.noteIdx * 0x100,
                                                      Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeLeft), Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeRight));
    }

    for (i = 0; i < ARRAY_SIZE(g_AudioPlayingIdxList); i++)
    {
        if (g_AudioPlayingIdxList[i] == sfxId)
        {
            g_AudioPlayingIdxList[i] = 0;
        }
    }

    if (g_Sd_VabPlayingInfo.audioVabIdx < ARRAY_SIZE(g_AudioPlayingIdxList))
    {
        g_AudioPlayingIdxList[g_Sd_VabPlayingInfo.audioVabIdx] = sfxId;
        attr.voice                                               = 1 << g_Sd_VabPlayingInfo.audioVabIdx;

        SpuGetVoiceAttr(&attr);

        g_AudioPlayingPitchList[g_Sd_VabPlayingInfo.audioVabIdx] = attr.pitch;
        return g_Sd_VabPlayingInfo.audioVabIdx;
    }

#ifdef SH_PC_PORT
    /* Key-on failed (no free voice / bad VAB) — no start-address write ever
     * claimed the armed azimuth; disarm it so a later sound can't inherit
     * it (PsyCross also TTL-expires orphans as a backstop). */
    PsyX_SPUAL_ClearNextKeyOnAzimuth();
#endif

    return NO_VALUE;
}

void Sd_SfxAttributesUpdate(u16 sfxId, q0_7 balance, u8 vol, s8 pitch) // 0x800463C0
{
    static s16   audioIdx;
    static u16   audioPitch;
    SpuVoiceAttr attr;
    s16          convertedVol;
    s32          voiceIdx;
    s32          i;
#ifdef SH_PC_PORT
    /* Claim the azimuth latch into locals AT ENTRY so every early return
     * (sfx not playing, voiceIdx < 0) still consumes it — a leaked latch
     * would mistag the next unrelated sound. The radio-restart paths re-arm
     * the global before delegating to Sd_PlaySfx. */
    s32 pcAzimuth      = g_Pc_SfxAzimuth;
    s32 pcAzimuthValid = g_Pc_SfxAzimuthValid;

    g_Pc_SfxAzimuthValid = 0;
#endif

    if (sfxId == Sfx_Base)
    {
        return;
    }

    audioIdx   = sfxId - Sfx_Base;
    g_Sd_VabPlayingInfo.volumeLeft = gSDVolConfig.volumeSe_4 + g_Vab_InfoTable[audioIdx].field_5;

#ifdef SH_PC_PORT
    if (sfxId == Sfx_RadioInterferenceLoop || sfxId == Sfx_RadioStaticLoop) {
    }
#endif

    if (sfxId == Sfx_RadioInterferenceLoop)
    {
        voiceIdx   = 22;
        attr.voice = 1 << 22;
#ifdef SH_PC_PORT
        /* Radio keyon miss: D_800BCDA8[].field_0 starts at BSS-zero (0) rather
         * than NO_VALUE (-1) before func_80037154 runs, so the keyon branch in
         * npc_main.c never fires. Detect the stopped state here and restart the
         * voice so the radio static is audible. */
        if (!SpuGetKeyStatus(attr.voice)) {
            if (pcAzimuthValid) { g_Pc_SfxAzimuth = pcAzimuth; g_Pc_SfxAzimuthValid = 1; }
            Sd_PlaySfx(sfxId, 0, 0);
            return;
        }
#endif
    }
    else if (sfxId == Sfx_RadioStaticLoop)
    {
        voiceIdx   = 23;
        attr.voice = 1 << 23;
#ifdef SH_PC_PORT
        if (!SpuGetKeyStatus(attr.voice)) {
            if (pcAzimuthValid) { g_Pc_SfxAzimuth = pcAzimuth; g_Pc_SfxAzimuthValid = 1; }
            Sd_PlaySfx(sfxId, 0, 0);
            return;
        }
#endif
    }
    else
    {
        voiceIdx = NO_VALUE;
        for (i = 0; i < SD_VOICE_COUNT; i++)
        {
            if (g_AudioPlayingIdxList[i] == sfxId)
            {
                voiceIdx = i;
            }
        }

        if (voiceIdx < 0)
        {
            return;
        }

        attr.voice = 1 << voiceIdx;
    }

    g_Sd_VabPlayingInfo.pitch   = 0;
    g_Sd_VabPlayingInfo.noteIdx = g_Vab_InfoTable[audioIdx].noteIdx_4;
    audioPitch                    = g_AudioPlayingPitchList[voiceIdx] + (pitch * 2);
    convertedVol                  = vol;
    convertedVol                  = g_Sd_VabPlayingInfo.volumeLeft - ((g_Sd_VabPlayingInfo.volumeLeft * (convertedVol)) / 255);

    WriteVolume(&g_Sd_VabPlayingInfo.volumeLeft, &g_Sd_VabPlayingInfo.volumeRight, convertedVol);

    // Apply stereo balance.
    if (g_Sd_AudioWork.isStereoEnabled_12 == true)
    {
        if (balance < 0)
        {
            g_Sd_VabPlayingInfo.volumeRight -= (convertedVol * ABS(balance)) >> 7;
        }
        else
        {
            g_Sd_VabPlayingInfo.volumeLeft -= (convertedVol * balance) >> 7;
        }
    }

    SpuGetVoiceAttr(&attr);

    attr.mask          = 0x1F;
    attr.volmode.left  = 0;
    attr.volmode.right = 0;
    attr.volmode.left  = 0;
    attr.volmode.right = 0;

    // Clamp volume to positive range.
    if (g_Sd_VabPlayingInfo.volumeLeft < 0)
    {
        g_Sd_VabPlayingInfo.volumeLeft = 0;
    }
    if (g_Sd_VabPlayingInfo.volumeRight < 0)
    {
        g_Sd_VabPlayingInfo.volumeRight = 0;
    }

    attr.volume.right = Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeRight << 7);
    attr.volume.left  = Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeLeft << 7);
    attr.pitch        = audioPitch;

#ifdef SH_PC_PORT
    /* Radio (and any other reserved-voice) bug: g_AudioPlayingPitchList
     * for voices 22/23 reads back as zero on PC (the post-SdUtKeyOnV
     * SpuGetVoiceAttr cache doesn't populate for these voices), so
     * audioPitch ends up = 0 + (input_pitch*2) = 0 for the radio's
     * per-frame proximity update. PsyX_SPUAL_SetVoiceAttr then sees
     * pitch==0 with mask|SPU_VOICE_PITCH and PAUSES the OpenAL source —
     * radio static plays for one frame on first proximity check then
     * goes silent for the rest of the encounter.
     *
     * Drop the PITCH bit from the mask when audioPitch is 0 so the
     * volume change lands but PsyCross doesn't touch the voice's pitch.
     * The voice keeps the pitch SdUtKeyOnV configured during keyon and
     * stays AL_PLAYING. Doesn't affect non-radio sfx — those route
     * through SdVoKeyOn whose post-cache path works correctly. */
    if (audioPitch == 0) {
        attr.mask &= ~SPU_VOICE_PITCH;
        if (sfxId == Sfx_RadioInterferenceLoop || sfxId == Sfx_RadioStaticLoop) {
        }
    }

    /* Live positional update (radio proximity, looped ambience): hand the
     * azimuth claimed at entry to the voice before the volume write below
     * repositions it. */
    if (pcAzimuthValid)
    {
        PsyX_SPUAL_SetVoiceAzimuth(voiceIdx, pcAzimuth);
    }
#endif

    SpuSetVoiceAttr(&attr);
}

// Plays audio, but it's only used on player's movement sounds and features pitch option unlike `Sd_PlaySfx`.
void func_80046620(u16 sfxId, q0_7 balance, u8 vol, s8 pitch) // 0x80046620
{
    static s16 audioIdx;
    s16        temp;
    s16        convertedVol;

    if (sfxId == Sfx_Base)
    {
        return;
    }

#ifdef SH_PC_PORT
    /* Same claim as Sd_PlaySfx: this is the third key-on entry (footsteps,
     * melee via func_8005DD44) — without it every movement sound left the
     * azimuth latch armed for the next unrelated sound to inherit. */
    if (g_Pc_SfxAzimuthValid)
    {
        PsyX_SPUAL_SetNextKeyOnAzimuth(g_Pc_SfxAzimuth);
    }
    else
    {
        PsyX_SPUAL_ClearNextKeyOnAzimuth();
    }
    g_Pc_SfxAzimuthValid = 0;
#endif

    audioIdx                      = sfxId - Sfx_Base;
    g_Sd_VabPlayingInfo.typeIdx = g_Vab_InfoTable[audioIdx].vab_progIdx_2 >> 8;
    g_Sd_VabPlayingInfo.progIdx = g_Vab_InfoTable[audioIdx].vab_progIdx_2 & 0xFF;
    g_Sd_VabPlayingInfo.toneIdx = g_Vab_InfoTable[audioIdx].audioVabIdx;
    g_Sd_VabPlayingInfo.noteIdx = g_Vab_InfoTable[audioIdx].noteIdx_4 + (s8)(pitch * 5 / 127);

    if (pitch > 0)
    {
        g_Sd_VabPlayingInfo.pitch = ABS(pitch * 5) % 127;
    }
    else
    {
        g_Sd_VabPlayingInfo.pitch = 127 - ABS(pitch * 5) % 127;
    }

    temp                             = gSDVolConfig.volumeSe_4 + g_Vab_InfoTable[audioIdx].field_5;
    convertedVol                     = vol;
    g_Sd_VabPlayingInfo.volumeLeft = temp - (temp * convertedVol) / 255;

    WriteVolume(&g_Sd_VabPlayingInfo.volumeLeft, &g_Sd_VabPlayingInfo.volumeRight, g_Sd_VabPlayingInfo.volumeLeft);

    // Apply stereo balance.
    if (g_Sd_AudioWork.isStereoEnabled_12 == true)
    {
        if (balance < 0)
        {
            g_Sd_VabPlayingInfo.volumeRight -= (g_Sd_VabPlayingInfo.volumeRight * ABS(balance)) >> 7;
        }
        else
        {
            g_Sd_VabPlayingInfo.volumeLeft -= (g_Sd_VabPlayingInfo.volumeLeft * balance) >> 7;
        }
    }

    // Clamp volume to positive range.
    if (g_Sd_VabPlayingInfo.volumeLeft < 0)
    {
        g_Sd_VabPlayingInfo.volumeLeft = 0;
    }
    if (g_Sd_VabPlayingInfo.volumeRight < 0)
    {
        g_Sd_VabPlayingInfo.volumeRight = 0;
    }

    g_Sd_VabPlayingInfo.audioVabIdx = SdUtKeyOn(g_Sd_VabPlayingInfo.typeIdx, g_Sd_VabPlayingInfo.progIdx, g_Sd_VabPlayingInfo.toneIdx, g_Sd_VabPlayingInfo.noteIdx, g_Sd_VabPlayingInfo.pitch,
                                                  Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeLeft), Sd_GetVolSe(g_Sd_VabPlayingInfo.volumeRight));
}

void Sd_LastSfxStop(void) // 0x800468EC
{
    SdUtKeyOffV(23);
}

void Sd_SfxStop(u16 sfxId) // 0x8004690C
{
#ifdef SH_PC_PORT
    /* Cutscenes "stop ambient SFX X" every frame, which spammed the log
     * 4 calls/frame x 60 fps. Dedupe via a tiny ring of recent IDs. */
    {
        static u16 _recent[8] = {0};
        static u8  _ridx      = 0;
        int i;
        for (i = 0; i < 8; i++) if (_recent[i] == sfxId) goto _no_log;
        SH_DBG("[SFX] Sd_SfxStop: sfxId=%d (Sfx_Base+%d)", sfxId, sfxId - Sfx_Base);
        _recent[_ridx] = sfxId;
        _ridx = (_ridx + 1) & 7;
    _no_log:;
    }
#endif
    Sd_SfxStopStep(sfxId);
}

void Sd_SfxStopStep(u16 sfxId) // 0x8004692C
{
    static s16 vabInfoIdx;
    static s16 vabProgIdxs;
    static s16 pitch;

    if (sfxId == Sfx_Base)
    {
        return;
    }

    vabInfoIdx  = sfxId - Sfx_Base;
    vabProgIdxs = g_Vab_InfoTable[vabInfoIdx].vab_progIdx_2;
    pitch       = g_Vab_InfoTable[vabInfoIdx].noteIdx_4 << 8;
    SdVoKeyOff(vabProgIdxs, pitch);
}

void Sd_AllSfxStop(void) // 0x800469AC
{
    s32 i;

    for (i = 0; i < 24; i++)
    {
        SdUtKeyOffV(i);
    }
}

void Sd_AllSfxWithRRStop(void) // 0x800469E8
{
    s32 i;

    for (i = 0; i < 24; i++)
    {
        SdUtKeyOffVWithRROff(i);
    }
}

// ========================================
// BGM HANDLING
// ========================================

void func_80046A24(u16 cmd) // 0x80046A24
{
    if (g_Sd_AudioWork.field_10 != cmd && g_Sd_AudioWork.field_E != cmd)
    {
        g_Sd_AudioWork.field_10 = cmd;
        Sd_TaskPoolAdd(7);
    }
#ifdef SH_PC_PORT
    else
    {
        SH_DBG("[BGM] ChannelSet cmd=%d dropped (field_10=%d field_E=%d)",
               (int)cmd, (int)g_Sd_AudioWork.field_10, (int)g_Sd_AudioWork.field_E);
    }
#endif
}

static inline void func_80046A70_0(u16 temp)
{
    g_Sd_AudioWork.bgmFadeSpeed_14 = 0;
    g_Sd_AudioWork.field_10        = 0;
    g_Sd_AudioWork.field_E         = temp;
}

void func_80046A70(void) // 0x80046A70
{
#ifdef SH_PC_PORT
    SH_DBG("[SH_AUDIO] func_80046A70: field_10=%d -> SdSeqPlay(0,1,0)", g_Sd_AudioWork.field_10);
#endif
    func_80046A70_0(g_Sd_AudioWork.field_10);

    SdSeqPlay(0, 1, 0);

    gSDVolConfig.volumeBgm_6 = 40;
    gSDVolConfig.volumeBgm_8 = 40;

#ifdef SH_PC_PORT
    SH_DBG("[SH_AUDIO] func_80046A70: volBgm set to 40, field_E=%d", g_Sd_AudioWork.field_E);
#endif
    Sd_SetVolBgm(gSDVolConfig.volumeBgm_6, gSDVolConfig.volumeBgm_6);
    Sd_TaskPoolUpdate();
}

void Sd_StopBgmTaskAdd(void) // 0x80046AD8
{
    g_Sd_AudioWork.field_E = NO_VALUE;
    Sd_TaskPoolAdd(8);
}

void Sd_StopBgm(void) // 0x80046B04
{
    if (gSDVolConfig.volumeBgm_8 > 0)
    {
        gSDVolConfig.volumeBgm_8 -= 4;
    }

    if (gSDVolConfig.volumeBgm_8 <= 0)
    {
        gSDVolConfig.volumeBgm_8 = 0;
        Sd_StopBgmStep();
        Sd_TaskPoolUpdate();
    }

    gSDVolConfig.volumeBgm_6 = gSDVolConfig.volumeBgm_8;
    Sd_SetVolBgm(gSDVolConfig.volumeBgm_6, gSDVolConfig.volumeBgm_6);
}

void Sd_StopBgmStep(void) // 0x80046B78
{
    Sd_SetVolBgm(0, 0);
    SdSeqStop(0);

    g_Sd_AudioWork.bgmFadeSpeed_14 = 0;
    g_Sd_AudioWork.field_E         = 0;
}

u8 Sd_BgmLayerVolumeGet(u8 layerIdx) // 0x80046BB4
{
    u32 i;
    u8  vol;

    if (layerIdx == 0)
    {
        return 0;
    }

    if (g_Sd_AudioWork.field_E >= 809)
    {
        return 0;
    }

    vol = 0;

    for (i = 0; i < 15; i++)
    {
        if (D_800AA604[(u8)g_Sd_AudioWork.field_E][i] == layerIdx)
        {
            vol = SdGetMidiVol(0, i);
            break;
        }
    }

    return vol;
}

void Sd_BgmLayerVolumeSet(u8 layerIdx, u8 vol) // 0x80046C54
{
    u32 i;
    s16 volCpy;
    u8  var1;
    u8  idx;

    if (layerIdx == 0)
    {
        gSDVolConfig.volumeBgm_6 = (vol * 40) / 127;
    }
    else if (g_Sd_AudioWork.field_E < 809)
    {

        idx = g_Sd_AudioWork.field_E;

        for (i = 0; i < 15; i++)
        {
            var1   = D_800AA604[idx][i];
            volCpy = vol;

            if (var1 == layerIdx)
            {
#ifdef SH_PC_PORT
                if (g_Sd_AudioWork.field_E == 796) {
                    /* Log only when a layer's written volume changes, so a full
                     * multi-room sewer walk stays compact (per-layer last-value). */
                    static s16 sewerLastVol[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
                    if (layerIdx < 8 && sewerLastVol[layerIdx] != volCpy) {
                        sewerLastVol[layerIdx] = volCpy;
                        SH_DBG("[SH_BGM] LayerVolSet sewer: layer=%d -> ch=%d vol=%d", layerIdx, i, volCpy);
                    }
                }
#endif
                SdSetMidiVol(0, i, volCpy);
            }
        }
    }
}

// ========================================
// XA FILES RELATED
// ========================================

void Sd_XaAudioPlayTaskAdd(u16 sfx) // 0x80046D3C
{
    g_Sd_AudioWork.xaAudioIdxCheck_2 = sfx & 0xFFF;
#ifdef SH_PC_PORT
    SH_DBG("[SH_AUDIO] Sd_XaAudioPlayTaskAdd: sfx=0x%04X idx=%u fileIdx=%u",
           sfx, g_Sd_AudioWork.xaAudioIdxCheck_2,
           g_XaItemData[g_Sd_AudioWork.xaAudioIdxCheck_2].xaFileIdx_0);
#endif

    if (g_XaItemData[g_Sd_AudioWork.xaAudioIdxCheck_2].xaFileIdx_0 != 0)
    {
        D_800C37DC         = true;
        D_800C1688.field_8 = VSync(SyncMode_Count);
        D_800C1688.field_4 = 0;

        Sd_TaskPoolAdd(2);

        g_Sd_AudioWork.xaAudioIdx_4 = g_Sd_AudioWork.xaAudioIdxCheck_2;

        Sd_TaskPoolAdd(1);
#ifdef SH_PC_PORT
        SH_DBG("[SH_AUDIO] XA tasks queued: idx=%u", g_Sd_AudioWork.xaAudioIdx_4);
#endif
    }
}

s32 Sd_XaAudioLengthGet(s32 idx) // 0x80046DCC
{
    return (g_XaItemData[idx & 0xFFF].audioLength_8 & 0xFFFFFF) + 32;
}

void Sd_XaAudioPlay(void) // 0x80046E00
{
    #define PREGAP 150

    static u16 xaAudioIdx;
    static u32 xaFileOffset;
    u32*       xaFileOffsetsPtr;
    u32*       xaFileOffsetTargetPtr;

    g_Sd_AudioWork.cdErrorCount_0++;

    switch (g_Sd_AudioStreamingStates.xaLoadState_1)
    {
        case XaLoadState_Initialize:
            if (g_Sd_AudioWork.bgmFadeSpeed_14 == 0)
            {
                gSDVolConfig.volumeBgm_6 = 24;
            }

            xaAudioIdx = g_Sd_AudioWork.xaAudioIdxCheck_2;
            switch (xaAudioIdx)
            {
                case 53:
                case 56:
                case 596:
                case 597:
                case 598:
                case 600:
                case 602:
                case 612:
                case 614:
                case 620:
                case 657:
                case 606:
                    gSDVolConfig.volumeXa_2 = Sd_GetVolSe(84);
                    break;

                case 723:
                case 725:
                    gSDVolConfig.volumeXa_2 = 50;
                    break;

                case 724:
                    gSDVolConfig.volumeXa_2 = 40;
                    break;

                default:
                    gSDVolConfig.volumeXa_2 = 84;
                    break;
            }

            gSDVolConfig.volumeXa_0                 = gSDVolConfig.volumeXa_2;
            Sd_SetVolXa(gSDVolConfig.volumeXa_2, gSDVolConfig.volumeXa_2);
            D_800C15F0[0].field_0                   = CdlModeSF | CdlModeRT | CdlModeSpeed;
            g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_SetMode;
            break;

        case XaLoadState_SetMode:
            if (!Sd_CdPrimitiveCmdTry(CdlSetmode, &D_800C15F0[0].field_0, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0           = 0;
                g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_PrepareFilter;
            }
            break;

        default:
            break;

        case XaLoadState_PrepareFilter:
            D_800C15F0[0].field_0                   = g_XaItemData[xaAudioIdx].field_8_24;
            D_800C15F0[0].field_1                   = g_XaItemData[xaAudioIdx].field_4_24;
            g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_SetFilter;
            break;

        case XaLoadState_SetFilter:
            if (!Sd_CdPrimitiveCmdTry(CdlSetfilter, &D_800C15F0[0].field_0, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0           = 0;
                g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_CalculateLba;
            }
            break;

        case XaLoadState_CalculateLba:
            // @hack Needed for match, weird code.
            xaFileOffsetsPtr      = g_FileXaLoc;
            xaFileOffsetTargetPtr = &xaFileOffsetsPtr[g_XaItemData[xaAudioIdx].xaFileIdx_0];
            xaFileOffset          = *xaFileOffsetTargetPtr;
            xaFileOffset         += PREGAP + g_XaItemData[xaAudioIdx].sector_4;

            D_800C1688.field_0 = g_XaItemData[xaAudioIdx].audioLength_8 + 32;

            g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_Seek;
            XaCdLocation.sector                     = itob(xaFileOffset % 75);
            xaFileOffset                           /= 75;
            XaCdLocation.second                     = itob(xaFileOffset % 60);
            xaFileOffset                           /= 60;
            XaCdLocation.minute                     = itob(xaFileOffset);
            break;

        case XaLoadState_Seek:
            if (!Sd_CdPrimitiveCmdTry(CdlSeekL, &XaCdLocation.minute, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0           = 0;
                g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_StartRead;
            }
            break;

        case XaLoadState_StartRead:
            if (!Sd_CdPrimitiveCmdTry(CdlReadN, NULL, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0           = 0;
                D_800C37DC                              = false;
                g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_EnableAudio;
            }
            break;

        case XaLoadState_EnableAudio:
            g_Sd_AudioWork.xaAudioIdx_4 = xaAudioIdx;

            SdSetSerialAttr(0, 0, 1);
            D_800C1688.field_8                      = VSync(SyncMode_Count);
            D_800C1688.field_4                      = 0;
            g_Sd_AudioStreamingStates.xaLoadState_1 = XaLoadState_Initialize;

            Sd_TaskPoolUpdate();
            g_Sd_AudioWork.cdErrorCount_0    = 0;
            g_Sd_AudioWork.isXaNotPlaying_16 = false;
            break;
    }
}

void Sd_XaPreLoadAudioPreTaskAdd(u16 xaIdx) // 0x8004729C
{
    Sd_XaPreLoadAudioTaskAdd(xaIdx);
}

void Sd_XaPreLoadAudioTaskAdd(s32 xaIdx) // 0x800472BC
{
    g_Sd_AudioWork.xaAudioIdxCheck_2 = xaIdx & 0xFFF;
    D_800C37DC                       = true;

    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        Sd_TaskPoolAdd(2);
    }

    Sd_TaskPoolAdd(6);
}

void Sd_XaPreLoadAudio(void) // 0x80047308
{
    static u16 xaAudioIdx;
    static u16 pad_800C15D2;
    static u32 xaFileOffset;
    u32*       xaFileOffsetsPtr;
    u32*       xaFileOffsetTargetPtr;

    g_Sd_AudioWork.cdErrorCount_0++;

    switch (g_Sd_AudioStreamingStates.xaPreLoadState_3)
    {
        case 0:
            xaAudioIdx                                 = g_Sd_AudioWork.xaAudioIdxCheck_2;
            Sd_SetVolXa(0, 0);
            D_800C15F0[0].field_0                      = CdlModeSF | CdlModeRT | CdlModeSpeed;
            g_Sd_AudioStreamingStates.xaPreLoadState_3 = 1;
            break;

        case 1:
            if (!Sd_CdPrimitiveCmdTry(CdlSetmode, &D_800C15F0[0].field_0, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0              = 0;
                g_Sd_AudioStreamingStates.xaPreLoadState_3 = 2;
            }
            break;

        case 2:
            D_800C15F0[0].field_0                      = g_XaItemData[xaAudioIdx].field_8_24;
            D_800C15F0[0].field_1                      = g_XaItemData[xaAudioIdx].field_4_24;
            g_Sd_AudioStreamingStates.xaPreLoadState_3 = 3;
            return;

        case 3:
            if (!Sd_CdPrimitiveCmdTry(CdlSetfilter, &D_800C15F0[0].field_0, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0              = 0;
                g_Sd_AudioStreamingStates.xaPreLoadState_3 = 4;
            }
            break;

        case 4:
            // @hack Needed for match, weird code.
            xaFileOffsetsPtr      = g_FileXaLoc;
            xaFileOffsetTargetPtr = &xaFileOffsetsPtr[g_XaItemData[xaAudioIdx].xaFileIdx_0];
            xaFileOffset          = *xaFileOffsetTargetPtr;
            xaFileOffset         += 0x96 + g_XaItemData[xaAudioIdx].sector_4;

            D_800C1688.field_0 = g_XaItemData[xaAudioIdx].audioLength_8 + 32;

            g_Sd_AudioStreamingStates.xaPreLoadState_3 = 5;
            XaCdLocation.sector                        = itob(xaFileOffset % 75);
            xaFileOffset                              /= 75;
            XaCdLocation.second                        = itob(xaFileOffset % 60);
            xaFileOffset                              /= 60;
            XaCdLocation.minute                        = itob(xaFileOffset);
            break;

        case 5:
            if (!Sd_CdPrimitiveCmdTry(CdlSeekL, &XaCdLocation.minute, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0              = 0;
                g_Sd_AudioStreamingStates.xaPreLoadState_3 = 6;
            }
            break;

        case 6:
            if (!Sd_CdPrimitiveCmdTry(CdlPause, NULL, NULL))
            {
                g_Sd_AudioStreamingStates.xaPreLoadState_3 = 0;
                D_800C37DC                                 = false;
                Sd_TaskPoolUpdate();
                g_Sd_AudioWork.cdErrorCount_0              = 0;
            }
            break;

        default:
            break;
    }
}

void Sd_XaAudioStopTaskAdd(void) // 0x8004760C
{
    Sd_TaskPoolAdd(2);
    g_Sd_AudioWork.isXaNotPlaying_16 = true;
}

void Sd_XaAudioStop(void) // 0x80047634
{
    g_Sd_AudioWork.isXaStopping_13 = true;

    switch (g_Sd_AudioStreamingStates.xaStopState_2)
    {
        case XaStopState_FadeOut:
            Sd_SetVolXa(gSDVolConfig.volumeXa_0, gSDVolConfig.volumeXa_0);
            gSDVolConfig.volumeXa_0 -= 24;
            gSDVolConfig.volumeXa_2  = gSDVolConfig.volumeXa_0;

            if (gSDVolConfig.volumeXa_0 < 2)
            {
                g_Sd_AudioStreamingStates.xaStopState_2 = XaStopState_Mute;
            }
            break;

        case XaStopState_Mute:
            gSDVolConfig.volumeXa_0 = 0;
            gSDVolConfig.volumeXa_2 = 0;

            Sd_SetVolXa(0, 0);
            SdSetSerialAttr(0, 0, 0);

            g_Sd_AudioStreamingStates.xaStopState_2 = XaStopState_PauseDisc;
            break;

        case XaStopState_PauseDisc:
            if (!Sd_CdPrimitiveCmdTry(CdlPause, NULL, NULL))
            {
                g_Sd_AudioWork.cdErrorCount_0           = 0;
                g_Sd_AudioStreamingStates.xaStopState_2 = XaStopState_Cleanup;
            }

            g_Sd_AudioWork.cdErrorCount_0++;
            break;

        case XaStopState_Cleanup:
            g_Sd_AudioWork.isXaStopping_13          = false;
            g_Sd_AudioWork.xaAudioIdx_4             = 0;
            g_Sd_AudioStreamingStates.xaStopState_2 = XaStopState_FadeOut;

            if (g_Sd_AudioWork.bgmFadeSpeed_14 == 0)
            {
                gSDVolConfig.volumeBgm_6 = 40;
            }

            Sd_TaskPoolUpdate();
            g_Sd_AudioWork.cdErrorCount_0 = 0;
            break;

        default:
            break;
    }
}

// ========================================
// VOLUME SET
// ========================================

void Sd_SetVolume(u8 xaVol, s16 bgmVol, u8 seVol) // 0x80047798
{
    gSDVolConfig.globalVolumeXa_E  = xaVol;
    gSDVolConfig.globalVolumeBgm_D = bgmVol;
    gSDVolConfig.globalVolumeSe_C  = seVol;

    if (g_Sd_AudioWork.field_E != 0)
    {
        Sd_SetVolBgm(gSDVolConfig.volumeBgm_8, gSDVolConfig.volumeBgm_8);
    }

    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        Sd_SetVolXa(gSDVolConfig.volumeXa_0, gSDVolConfig.volumeXa_0);
    }
}

void Sd_SetVolBgm(s16 volLeft, s16 volRight) // 0x80047808
{
    SdSeqSetVol(0, (volLeft * gSDVolConfig.globalVolumeBgm_D) >> 7, (volRight * gSDVolConfig.globalVolumeBgm_D) >> 7);
}

void Sd_SetVolXa(s16 volLeft, s16 volRight) // 0x80047860
{
    SdSetSerialVol(0, (volLeft * gSDVolConfig.globalVolumeXa_E) >> 7, (volRight * gSDVolConfig.globalVolumeXa_E) >> 7);
#ifdef SH_PC_PORT
    XaPlayer_SetVolume(volLeft, volRight);
#endif
}

s16 Sd_GetVolSe(s16 arg0) // 0x800478B8
{
    return (arg0 * gSDVolConfig.globalVolumeSe_C) >> 7;
}

// ========================================
// TASK POOL HANDLING
// ========================================

void Sd_TaskPoolAdd(u8 task) // 0x800478DC
{
    static s32 i;
    static s32 y;

    // If `task` is 2, shift field next to element containing value that matches 1 in `g_Sd_TaskPool`.
    if (task == 2)
    {
        for (i = 1; i < (ARRAY_SIZE(g_Sd_TaskPool) - 2); i++)
        {
            if (g_Sd_TaskPool[i] == 1)
            {
                for (y = i; y < (ARRAY_SIZE(g_Sd_TaskPool) - 2); y++)
                {
                    g_Sd_TaskPool[y] = g_Sd_TaskPool[y + 1];
                }

                g_Sd_TaskPool[31] = 0;
                D_800C37DC        = false;
            }
        }
    }

    // Shift field next to element containing value that matches `task` in `g_Sd_TaskPool`.
    for (i = 1; i < (ARRAY_SIZE(g_Sd_TaskPool) - 2); i++)
    {
        if (g_Sd_TaskPool[i] == task)
        {
            for (y = i; y < (ARRAY_SIZE(g_Sd_TaskPool) - 2); y++)
            {
                g_Sd_TaskPool[y] = g_Sd_TaskPool[y + 1];
            }

            g_Sd_TaskPool[31] = 0;
        }
    }

    // If `g_Sd_TaskPool` field is empty, assign value of `task`.
    for (i = 0; i < (ARRAY_SIZE(g_Sd_TaskPool) - 1); i++)
    {
        if (g_Sd_TaskPool[i] == 0)
        {
            g_Sd_TaskPool[i] = task;
            break;
        }
    }

    g_Sd_CurrentTask = g_Sd_TaskPool[0];
}

void Sd_TaskPoolUpdate(void) // 0x80047A70
{
    static s32 i;
    static s32 pad_800C15E4;
    if (g_Sd_TaskPool[0] != 0)
    {
        for (i = 0; i < (ARRAY_SIZE(g_Sd_TaskPool) - 1); i++)
        {
            g_Sd_TaskPool[i] = g_Sd_TaskPool[i + 1];
        }

        g_Sd_TaskPool[31] = 0;
    }
}

// ========================================
// REVERB HANDLING
// ========================================

void Sd_SetReverbDepth(u8 depth) // 0x80047AD0
{
    s32 depthCpy;

    gSDVolConfig.reverbDepth_F = depth;

    depthCpy = depth;
    SdUtSetReverbDepth(depthCpy, depthCpy);
}

void Sd_SetReverbEnable(s32 mode) // 0x80047AFC
{
    SdSetSerialAttr(0, 1, mode);
}

// ========================================
// LOAD VAB FILE
// ========================================

void Sd_VabLoad_TaskAdd(s32 cmd) // 0x80047B24
{
    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        Sd_TaskPoolAdd(2);
    }

    g_Sd_VabLoadAttemps = 0;
    Sd_TaskPoolAdd(cmd);
    g_Sd_AudioWork.isAudioLoading_15 = true;
}

void Sd_VabLoad(void) // 0x80047B80
{
    u8 depth;
    u8 cmd;

    switch (g_Sd_AudioStreamingStates.audioLoadState_0)
    {
        case AudioLoadState_Reset:
            cmd                = g_Sd_TaskPool[0];
            g_Sd_VabTargetLoad = &g_AudioData[cmd - 160];
            g_Sd_AudioType     = g_Sd_VabTargetLoad->typeIdx_0;

#ifdef SH_PC_PORT
            if (cmd >= 170 && cmd <= 204) {
                SH_DBG("[SH_AMB] Sd_VabLoad ambient cmd=%d type=%d dedup=%d",
                       cmd, g_Sd_AudioType,
                       (int)(g_Sd_AudioType != 0 && g_Sd_AudioWork.lastVabAudioLoadedIdx_8[g_Sd_AudioType - 1] == cmd));
            }
#endif

            // If audio being loaded isn't BASE.VAB or KDT file.
            if (g_Sd_AudioType != 0)
            {
                if (g_Sd_AudioWork.lastVabAudioLoadedIdx_8[g_Sd_AudioType - 1] == cmd)
                {
                    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Reset;
                    g_Sd_AudioWork.isAudioLoading_15           = false;
                    Sd_TaskPoolUpdate();
                    break;
                }

                g_Sd_AudioWork.lastVabAudioLoadedIdx_8[g_Sd_AudioType - 1] = cmd;
            }

            // Ambient sounds.
            if (cmd >= 170 && cmd <= 204)
            {
                depth = g_Sd_ReverbDepths[cmd - 170];
                if (depth != gSDVolConfig.reverbDepth_F)
                {
                    Sd_SetReverbDepth(depth);
                }
            }

            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
            break;

        case AudioLoadState_Stop:
            Sd_VabLoad_TypeClear();
            break;

        case AudioLoadState_SetOff:
            Sd_VabLoad_OffSet();
            break;

        case AudioLoadState_LoadFile:
            Sd_VabLoad_FileLoad();
            break;

        case AudioLoadState_CheckLoad:
            Sd_VabLoad_OffVagDataSet();
            break;

        case AudioLoadState_Move:
            Sd_VabLoad_VagDataMove();
            break;

        case AudioLoadState_SetNext:
            Sd_VabLoad_OffVagNextDataSet();
            break;

        case AudioLoadState_MoveNext:
            Sd_VabLoad_NextVagDataMove();
            break;

        case AudioLoadState_MoveLast:
            Sd_VabLoad_LastVagDataMove();
            break;

        case AudioLoadState_Finalize:
            Sd_VabLoad_Finalization();
            break;

        default:
            break;
    }
}

void Sd_VabLoad_TypeClear(void) // 0x80047D1C
{
    g_Sd_DataMoved = 0;
    SdVabClose(g_Sd_AudioType);
    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_SetOff;
}

void Sd_VabLoad_OffSet(void) // 0x80047D50
{
    CdlLOC sp10;

    if (!Sd_CdPrimitiveCmdTry(CdlSetloc, (u8*)CdIntToPos(g_Sd_VabTargetLoad->fileOffset_8 + (g_Sd_DataMoved  / 2048), &sp10), 0))
    {
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_LoadFile;
    }
}

void Sd_VabLoad_FileLoad(void) // 0x80047DB0
{
    if (CdSync(1, 0) == CdlComplete)
    {
        if (g_Sd_VabTargetLoad->fileSize_4 < VAB_BUFFER_LIMIT)
        {
            CdRead((g_Sd_VabTargetLoad->fileSize_4 + 2047) / 2048, CD_ADDR_0, 128);
        }
        else
        {
            CdRead(25, CD_ADDR_0, 128);
        }

        // @hack
        if (g_Sd_AudioStreamingStates.audioLoadState_0 != AudioLoadState_Reset)
        {
            char unk = -unk;
        }

        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_CheckLoad;
        g_Sd_AudioWork.cdErrorCount_0              = 0;
    }

    g_Sd_AudioWork.cdErrorCount_0++;
}

void Sd_VabLoad_OffVagDataSet(void) // 0x80047E3C
{
    s32 i;
    u8* ptr0;
    u8* ptr1;

    if (CdReadSync(1, NULL) == 0)
    {
        ptr1 = (u8*)CD_ADDR_0;
        ptr0 = g_Sd_VabBuffers[g_Sd_AudioType];

        for (i = 0; i < g_Sd_VabTargetLoad->vagDataOffset_2; i++)
        {
            *ptr0++ = *ptr1++;
        }

        SdVabOpenHeadSticky(g_Sd_VabBuffers[g_Sd_AudioType], g_Sd_AudioType, D_800A9FDC[g_Sd_AudioType]);
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Move;
    }

    g_Sd_AudioWork.cdErrorCount_0++;
}

void Sd_VabLoad_VagDataMove(void) // 0x80047F18
{
    s32  dataMoveCheck;
    s32* ptr;

    if (g_Sd_VabTargetLoad->fileSize_4 < VAB_BUFFER_LIMIT)
    {
        dataMoveCheck = SdVabTransBody((u8*)CD_ADDR_0 + g_Sd_VabTargetLoad->vagDataOffset_2, g_Sd_AudioType);
        ptr           = &g_Sd_VabTargetLoad->fileSize_4;

        g_Sd_DataMoved                             = *ptr;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Finalize;
    }
    else
    {
        dataMoveCheck = SdVabTransBodyPartly((u8*)CD_ADDR_0 + g_Sd_VabTargetLoad->vagDataOffset_2, VAB_BUFFER_LIMIT - g_Sd_VabTargetLoad->vagDataOffset_2, g_Sd_AudioType);

        g_Sd_DataMoved                             = VAB_BUFFER_LIMIT;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_SetNext;
    }

    if (dataMoveCheck == NO_VALUE && g_Sd_VabLoadAttemps < 16)
    {
        g_Sd_VabLoadAttemps++;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
    }
}

void Sd_VabLoad_OffVagNextDataSet(void) // 0x80048000
{
    s32 i;
    CdlLOC  cdLocArg;
    CdlLOC* cdLocRes;

    if (SdVabTransCompleted(0) == 1)
    {
        i        = g_Sd_VabTargetLoad->fileOffset_8 + ((g_Sd_DataMoved + 2047) / 2048);
        cdLocRes = CdIntToPos(i, &cdLocArg);

        if (!Sd_CdPrimitiveCmdTry(CdlSetloc, (u8*)cdLocRes, 0))
        {
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_MoveNext;
        }
    }
}

void Sd_VabLoad_NextVagDataMove(void) // 0x8004807C
{
    u32 remainingData;

    if (CdSync(1, 0) != CdlComplete)
    {
        return;
    }

    remainingData = g_Sd_VabTargetLoad->fileSize_4 - g_Sd_DataMoved;
    if (remainingData < VAB_BUFFER_LIMIT)
    {
        CdRead(((remainingData + 2047) / 2048), CD_ADDR_0, 128);
    }
    else
    {
        CdRead(25, CD_ADDR_0, 128);
    }

    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_MoveLast;
}

void Sd_VabLoad_LastVagDataMove(void) // 0x800480FC
{
    s32 dataMoveCheck;
    u32 remainingData;

    if (CdReadSync(1, 0) != 0)
    {
        return;
    }

    remainingData = g_Sd_VabTargetLoad->fileSize_4 - g_Sd_DataMoved;
    if (remainingData < VAB_BUFFER_LIMIT)
    {
        dataMoveCheck                              = SdVabTransBodyPartly((u8*)CD_ADDR_0, remainingData, g_Sd_AudioType);
        g_Sd_DataMoved                             = g_Sd_VabTargetLoad->fileSize_4;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Finalize;
    }
    else
    {
        dataMoveCheck                              = SdVabTransBodyPartly((u8*)CD_ADDR_0, VAB_BUFFER_LIMIT, g_Sd_AudioType);
        g_Sd_DataMoved                            += VAB_BUFFER_LIMIT;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_SetNext;
    }

    if (dataMoveCheck == NO_VALUE && g_Sd_VabLoadAttemps < 16)
    {
        g_Sd_VabLoadAttemps++;
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
    }
}

void Sd_VabLoad_Finalization(void) // 0x800481F8
{
    if (SdVabTransCompleted(0) != 1)
    {
        return;
    }

    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Reset;
    g_Sd_AudioWork.cdErrorCount_0              = 0;
    g_Sd_AudioWork.isAudioLoading_15           = false;
    Sd_TaskPoolUpdate();
}

// ========================================
// LOAD KDT FILE FOR MUSIC
// ========================================

void Sd_KdtLoad_TaskAdd(u16 songIdx) // 0x80048244
{
    if (g_Sd_AudioWork.bgmLoadedSongIdx_6 == songIdx)
    {
        return;
    }

    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        Sd_TaskPoolAdd(2);
    }

    Sd_StopBgmTaskAdd();
    SD_Call((u16)(songIdx + 173));
    Sd_TaskPoolAdd(songIdx);

    g_Sd_VabLoadAttemps               = 0;
    g_Sd_AudioWork.bgmLoadedSongIdx_6 = songIdx;
    g_Sd_AudioWork.isAudioLoading_15  = true;
}

void Sd_KdtLoad(void) // 0x800482D8
{
    switch (g_Sd_AudioStreamingStates.audioLoadState_0)
    {
        case AudioLoadState_Reset:
            g_Sd_KdtTargetLoad                         = &g_AudioData[54 + g_Sd_TaskPool[0]];
            g_Sd_AudioType                             = g_Sd_KdtTargetLoad->typeIdx_0;
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
            break;

        case AudioLoadState_Stop:
            Sd_KdtLoad_StopSeq();
            break;

        case AudioLoadState_SetOff:
            Sd_KdtLoad_OffSet();
            break;

        case AudioLoadState_LoadFile:
            Sd_KdtLoad_FileLoad();
            break;

        case AudioLoadState_CheckLoad:
            Sd_KdtLoad_LoadCheck();
            break;

        default:
            break;
    }
}

void Sd_KdtLoad_StopSeq(void) // 0x8004839C
{
    Sd_StopBgmStep();
    SdSeqClose(g_Sd_AudioType);

    g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_SetOff;
}

void Sd_KdtLoad_OffSet(void) // 0x800483D4
{
    CdlLOC cdLoc;

    if (!Sd_CdPrimitiveCmdTry(CdlSetloc, (u8*)CdIntToPos(g_Sd_KdtTargetLoad->fileOffset_8, &cdLoc), 0))
    {
        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_LoadFile;
    }
}

void Sd_KdtLoad_FileLoad(void) // 0x80048424
{
    if (CdSync(1, 0) == 2)
    {
        CdRead((g_Sd_KdtTargetLoad->fileSize_4 + 2047) / 2048, FS_BUFFER_1, 128);

        g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_CheckLoad;
        g_Sd_AudioWork.cdErrorCount_0              = 0;
    }

    g_Sd_AudioWork.cdErrorCount_0++;
}

void Sd_KdtLoad_LoadCheck(void) // 0x80048498
{
    s32 i;
    u8* ptr0;
    u8* ptr1;

    if (CdReadSync(1, NULL) == 0)
    {
        ptr1 = (u8*)FS_BUFFER_1;
        ptr0 = g_Sd_KdtBuffer[g_Sd_AudioType];

        for (i = 0; i < g_Sd_KdtTargetLoad->fileSize_4; i++)
        {
            *ptr0++ = *ptr1++;
        }

        i = SdSeqOpen(g_Sd_KdtBuffer[g_Sd_AudioType], 3);
        if (i == NO_VALUE && g_Sd_VabLoadAttemps < 16)
        {
            g_Sd_VabLoadAttemps++;
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
        }
        else
        {
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Reset;
            g_Sd_AudioWork.isAudioLoading_15           = false;

            Sd_TaskPoolUpdate();
        }

        g_Sd_AudioWork.cdErrorCount_0 = 0;
    }

    g_Sd_AudioWork.cdErrorCount_0++;
}

/** In Jan 16 Demo and the Nov 24, 98 Preview,
 * all these functions also nullsub. Additionally,
 * `func_800485C0` doesn't exist in the Jan 16 Demo.
 */

void func_800485B0(s16 arg0, u8 arg1, u8 arg2, s16 arg3, s16 arg4) {} // 0x800485B0

void func_800485B8(s32 arg0, u8 arg1, u32 arg2) {} // 0x800485B8

void func_800485C0(s32 idx) // 0x800485C0
{
    g_AudioPlayingIdxList[idx] = 0;
}

#define CD_ERROR_LIMIT 600 // Matches value used in beatmania `FSCD.C`.

void Sd_TaskPoolExecute(void) // 0x800485D8
{
    g_Sd_CurrentTask = g_Sd_TaskPool[0];
#ifdef SH_PC_PORT
    {
        static u8 prevTask = 255;
        if (g_Sd_CurrentTask != prevTask) {
            SH_DBG("[SH_AUDIO] TaskPoolExecute task=%d", g_Sd_CurrentTask);
            prevTask = g_Sd_CurrentTask;
        }
    }
#endif
    switch (g_Sd_CurrentTask)
    {
        case 0:
            break;

        case 1:
#ifdef SH_PC_PORT
            /* Use xaAudioIdxCheck_2 (the queued-track-id, persistent across Stop)
             * instead of xaAudioIdx_4 (the active-track-id, cleared by Stop).
             * Mirrors PSX Sd_XaAudioPlay's XaLoadState_Initialize using check_2
             * and XaLoadState_EnableAudio re-setting xaAudioIdx_4. Lets the
             * Stop-then-Play sequence in Sd_XaAudioPlayTaskAdd work correctly
             * even when Stop fully clears the streaming-state flags. */
            XaPlayer_Play(g_Sd_AudioWork.xaAudioIdxCheck_2);
            g_Sd_AudioWork.xaAudioIdx_4 = g_Sd_AudioWork.xaAudioIdxCheck_2;
            Sd_TaskPoolUpdate();
#else
            Sd_XaAudioPlay();
#endif
            break;

        case 2:
#ifdef SH_PC_PORT
            /* XaPlayer_Stop calls Xa_SignalPlaybackFinished which clears
             * xaAudioIdx_4 + D_800C37DC. Safe because the upcoming Play in
             * Sd_XaAudioPlayTaskAdd's queue uses xaAudioIdxCheck_2 (still set). */
            XaPlayer_Stop();
            Sd_TaskPoolUpdate();
#else
            Sd_XaAudioStop();
#endif
            break;

        case 6:
            Sd_XaPreLoadAudio();
            break;

        case 7:
            func_80046A70();
            break;

        case 8:
            Sd_StopBgm();
            break;

        default:
            if (g_Sd_CurrentTask >= 160)
            {
                Sd_VabLoad();
            }
            else if (g_Sd_CurrentTask >= 32)
            {
                Sd_KdtLoad();
            }
            else
            {
                Sd_TaskPoolUpdate();
            }
            break;
    }

    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
    {
        D_800C1688.field_4 = VSync(SyncMode_Count) - D_800C1688.field_8;
    }

    // Fade out background music.
    if (g_Sd_AudioWork.bgmFadeSpeed_14 != 0)
    {
        g_Sd_AudioWork.field_E = NO_VALUE;

        if (gSDVolConfig.volumeBgm_8 <= 0)
        {
            gSDVolConfig.volumeBgm_8 = 0;
            Sd_StopBgmStep();
        }
        else
        {
            gSDVolConfig.volumeBgm_8 -= g_Sd_AudioWork.bgmFadeSpeed_14;

            if ((gSDVolConfig.volumeBgm_8 << 16) <= 0)
            {
                gSDVolConfig.volumeBgm_8 = 0;
                Sd_StopBgmStep();
            }
        }

        gSDVolConfig.volumeBgm_6 = gSDVolConfig.volumeBgm_8;

        Sd_SetVolBgm(gSDVolConfig.volumeBgm_8, gSDVolConfig.volumeBgm_8);
    }
    else if (gSDVolConfig.volumeBgm_6 != gSDVolConfig.volumeBgm_8)
    {
        if (gSDVolConfig.volumeBgm_8 < gSDVolConfig.volumeBgm_6)
        {
            gSDVolConfig.volumeBgm_8++;
            if (ABS(gSDVolConfig.volumeBgm_8 - gSDVolConfig.volumeBgm_6) < 2)
            {
                gSDVolConfig.volumeBgm_8 = gSDVolConfig.volumeBgm_6;
            }
        }
        else
        {
            gSDVolConfig.volumeBgm_8--;
            if (ABS(gSDVolConfig.volumeBgm_8 - gSDVolConfig.volumeBgm_6) < 2)
            {
                gSDVolConfig.volumeBgm_8 = gSDVolConfig.volumeBgm_6;
            }
        }

        Sd_SetVolBgm(gSDVolConfig.volumeBgm_8, gSDVolConfig.volumeBgm_8);
    }

    if ((u32)D_800C1688.field_4 > (u32)D_800C1688.field_0)
    {
        if (g_Sd_CurrentTask == 0)
        {
            if (g_Sd_AudioWork.isXaNotPlaying_16 == false)
            {
                Sd_TaskPoolAdd(2);
            }

            D_800C1688.field_8 = VSync(SyncMode_Count);
            D_800C1688.field_4 = 0;
        }
    }

    // Slowly fade in/out game audio based if `g_Sd_AudioWork.muteGame_17` is enabled.
    if (g_Sd_AudioWork.muteGame_17 == true)
    {
        if (gSDVolConfig.volumeGlobal_A > 0)
        {
            gSDVolConfig.volumeGlobal_A -= 8;
            if ((gSDVolConfig.volumeGlobal_A << 16) <= 0)
            {
                gSDVolConfig.volumeGlobal_A = 0;
            }

            SdSetMVol(gSDVolConfig.volumeGlobal_A, gSDVolConfig.volumeGlobal_A);
        }
    }
    else
    {
        if (gSDVolConfig.volumeGlobal_A < (OPT_SOUND_VOLUME_MAX - 1))
        {
            gSDVolConfig.volumeGlobal_A += 4;
            if (gSDVolConfig.volumeGlobal_A >= (OPT_SOUND_VOLUME_MAX - 1))
            {
                gSDVolConfig.volumeGlobal_A = OPT_SOUND_VOLUME_MAX - 1;
            }

            SdSetMVol(gSDVolConfig.volumeGlobal_A, gSDVolConfig.volumeGlobal_A);
        }
    }

    // Reset audio streaming system if failed.
    if (g_Sd_AudioWork.cdErrorCount_0 > CD_ERROR_LIMIT)
    {
        CdReset(0);
        CdControlB(CdlNop, NULL, NULL);

        if (g_Sd_AudioStreamingStates.audioLoadState_0 != AudioLoadState_Reset)
        {
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
        }

        g_Sd_AudioStreamingStates.xaLoadState_1    = XaLoadState_Initialize;
        g_Sd_AudioStreamingStates.xaStopState_2    = XaStopState_FadeOut;
        g_Sd_AudioStreamingStates.xaPreLoadState_3 = 0;
        g_Sd_AudioWork.cdErrorCount_0              = 0;
    }
}

#ifdef SH_PC_PORT
/* PC-only boot speedup: VAB/KDT loaders advance one sub-state per call on
 * PSX because each was gated by async DMA / SPU upload. On PC the I/O is
 * synchronous (PsyCross CdSync always returns CdlComplete; CdReadSync
 * reads one sector per call out of an in-memory disc image), so without
 * batching the boot sits at 30Hz × ~13 states × ~20 VABs = the 15+ s
 * pause between warning screen and Konami logo (and again at every load
 * screen).
 *
 * Earlier version of this drain bailed when audioLoadState_0 stayed the
 * same across two iterations, on the theory that "no state change" meant
 * the loader was blocked on real I/O. That was wrong: state 5
 * (AudioLoadState_CheckLoad) intentionally sits there draining the
 * sector queue one sector per Sd_VabLoad call — count goes N→N-1→…→0,
 * but state stays 5 the whole time. Bailing on first match meant we
 * only drained ~5 sectors per frame, leaving multi-MB VABs to crawl
 * over many frames.
 *
 * Now: keep ticking the pool until task slot goes idle or the
 * iteration cap hits. The cap is generous because each iteration
 * either advances state or drains one 2KB sector, so 4096 covers an
 * 8 MB VAB. A truly stuck state machine still gets cut off.
 *
 * Skipped while XA voice is playing so we don't fast-forward fade
 * timers. Lives here (not game_main.c) because g_Sd_TaskPool /
 * g_Sd_AudioWork are static to this file. */
void Sd_TaskPoolDrain(void)
{
    int drain;

    if (g_Sd_AudioWork.xaAudioIdx_4 != 0)
        return;

    for (drain = 0; drain < 512; drain++)
    {
        if (g_Sd_TaskPool[0] == 0)
            return;
        Sd_TaskPoolExecute();
    }
}
#endif

u8 Sd_CdPrimitiveCmdTry(s32 com, u8* param, u8* res) // 0x80048954
{
    u8 syncRes;
    u8 comCopy;

    comCopy = com;

    if (CdSync(1, &syncRes) == CdlComplete && CdControl(comCopy, param, res))
    {
        g_Sd_AudioWork.cdErrorCount_0 = 0;
        return false;
    }

    g_Sd_AudioWork.cdErrorCount_0++;
    if (g_Sd_AudioWork.cdErrorCount_0 > CD_ERROR_LIMIT)
    {
        CdReset(0);
        CdControlB(CdlNop, NULL, NULL);
        VSync(SyncMode_Wait3);

        if (g_Sd_AudioStreamingStates.audioLoadState_0 != AudioLoadState_Reset)
        {
            g_Sd_AudioStreamingStates.audioLoadState_0 = AudioLoadState_Stop;
        }

        g_Sd_AudioStreamingStates.xaLoadState_1    = XaLoadState_Initialize;
        g_Sd_AudioStreamingStates.xaStopState_2    = XaStopState_FadeOut;
        g_Sd_AudioStreamingStates.xaPreLoadState_3 = 0;
        g_Sd_AudioWork.cdErrorCount_0              = 0;
    }

    return true;
}

#ifdef SH_PC_PORT
/* Called by XA player when playback finishes naturally OR is stopped.
 * Mimics the cleanup the PSX state machine in Sd_XaAudioPlay /
 * Sd_XaAudioStop performs at end-of-stream / stop:
 *   - xaAudioIdx_4   = 0   (Sd_AudioStreamingCheck case 1 → 0)
 *   - D_800C37DC     = 0   (Sd_AudioStreamingCheck case 4 → 0)
 *   - isXaStopping_13 = 0  (matches PSX cleanup phase)
 * Without these clears, Sd_AudioStreamingCheck() returns 4 forever and
 * Bgm_Init() blocks the loading-screen state machine at scene transitions
 * (manifests as a black screen after the alley gate).
 */
void Xa_SignalPlaybackFinished(void) {
    g_Sd_AudioWork.xaAudioIdx_4    = 0;
    g_Sd_AudioWork.isXaStopping_13 = false;
    D_800C37DC                     = false;
}

// Public accessor for the static g_Sd_AudioWork.xaAudioIdx_4.
u16 Sd_GetXaAudioIdx(void) {
    return g_Sd_AudioWork.xaAudioIdx_4;
}

// Watchdog helper: force-clear all XA streaming state.
void Sd_ForceClearXaAudioIdx(void) {
    g_Sd_AudioWork.xaAudioIdx_4    = 0;
    g_Sd_AudioWork.isXaStopping_13 = false;
    D_800C37DC                     = false;
}
#endif

#undef BSS_HACK_SD_CALL_C
#undef VAB_BUFFER_LIMIT
