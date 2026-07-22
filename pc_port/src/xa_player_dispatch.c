#include "xa_player.h"
#include "pc_audio_config.h"

void PcLegacyXa_PlayWithParams(uint16_t, uint16_t, uint32_t, uint32_t);
void PcLegacyXa_Play(uint16_t);
void PcLegacyXa_Stop(void);
void PcLegacyXa_Update(void);
void PcLegacyXa_SetVolume(int16_t, int16_t);
void PcLegacyXa_SetPauseHold(int);
int PcLegacyXa_IsVoiceAudioDraining(void);
int PcLegacyXa_VoiceGapHold(void);
void PcLegacyXa_SetMasterVolume(float);

void PcSoftwareXa_PlayWithParams(uint16_t, uint16_t, uint32_t, uint32_t);
void PcSoftwareXa_Play(uint16_t);
void PcSoftwareXa_Stop(void);
void PcSoftwareXa_Update(void);
void PcSoftwareXa_SetVolume(int16_t, int16_t);
void PcSoftwareXa_SetPauseHold(int);
int PcSoftwareXa_IsVoiceAudioDraining(void);
int PcSoftwareXa_VoiceGapHold(void);
void PcSoftwareXa_SetMasterVolume(float);

#define XA_DISPATCH_VOID(publicName, legacyName, softwareName, args, callargs) \
    void publicName args { \
        if (PcAudioConfig_UsesSoftwareSpu()) softwareName callargs; \
        else legacyName callargs; \
    }

XA_DISPATCH_VOID(XaPlayer_PlayWithParams, PcLegacyXa_PlayWithParams,
                 PcSoftwareXa_PlayWithParams,
                 (uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors),
                 (xaIdx, fileIdx, sectorOffset, numSectors))
XA_DISPATCH_VOID(XaPlayer_Play, PcLegacyXa_Play, PcSoftwareXa_Play,
                 (uint16_t xaIdx), (xaIdx))
XA_DISPATCH_VOID(XaPlayer_Stop, PcLegacyXa_Stop, PcSoftwareXa_Stop, (void), ())
XA_DISPATCH_VOID(XaPlayer_Update, PcLegacyXa_Update, PcSoftwareXa_Update, (void), ())
XA_DISPATCH_VOID(XaPlayer_SetVolume, PcLegacyXa_SetVolume, PcSoftwareXa_SetVolume,
                 (int16_t left, int16_t right), (left, right))
XA_DISPATCH_VOID(XaPlayer_SetPauseHold, PcLegacyXa_SetPauseHold,
                 PcSoftwareXa_SetPauseHold, (int hold), (hold))
XA_DISPATCH_VOID(XaPlayer_SetMasterVolume, PcLegacyXa_SetMasterVolume,
                 PcSoftwareXa_SetMasterVolume, (float volume), (volume))

int Xa_IsVoiceAudioDraining(void)
{
    return PcAudioConfig_UsesSoftwareSpu()
        ? PcSoftwareXa_IsVoiceAudioDraining()
        : PcLegacyXa_IsVoiceAudioDraining();
}

int Xa_VoiceGapHold(void)
{
    return PcAudioConfig_UsesSoftwareSpu()
        ? PcSoftwareXa_VoiceGapHold()
        : PcLegacyXa_VoiceGapHold();
}
