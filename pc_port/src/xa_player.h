/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PORT_XA_PLAYER_H
#define PC_PORT_XA_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by game to start XA playback
// xaIdx: index into g_XaItemData
// fileIdx: which raw file (1-9, maps to 05_02152 through 45_28784)
// sectorOffset: starting sector within that file
// numSectors: how many sectors to read (duration in sectors)
void XaPlayer_PlayWithParams(uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors);

// Legacy interface (used by Sd_TaskPoolExecute)
void XaPlayer_Play(uint16_t xaIdx);

void XaPlayer_Stop(void);
void XaPlayer_Update(void);

/* Play a loose PCM WAV (8/16-bit, mono/stereo) through the voice source, same
 * pacing as a disc line of the WAV's length. While a disc line is still
 * producing audio the file is mixed into it instead, and plays on alone if it
 * outlasts that line. Returns 0 (nothing changed) when the file is
 * missing/unsupported or loose files are off. */
int XaPlayer_PlayFile(const char* path);
/* Stop only a loose file (playing or mixed); a disc line is left alone. */
void XaPlayer_StopFile(void);
/* True while a loose file started by XaPlayer_PlayFile is still being heard. */
int XaPlayer_FileAudible(void);
void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight);

/* Console-freeze hold: pauses the OpenAL voice source and freezes the PSX
 * end-of-voice pacing clock while the game clock is force-zeroed, so the
 * voice can't run ahead of the frozen scene. Idempotent per state. */
void XaPlayer_SetPauseHold(int hold);

/* True only while a voice is actually producing audio, EXCLUDING the
 * end-of-voice pad tail (unlike Sd_AudioStreamingCheck()==1). The subtitle
 * page-advance gate uses this so voiced cutscene pages advance at real audio
 * drain instead of pad-end — see the definition for the map6_s04 rationale. */
int Xa_IsVoiceAudioDraining(void);

/* Master XA (FMV/voice) volume multiplier in [0,1], applied on top of the
 * game-driven per-track gain. Set from config / console / options menu. */
extern float g_PcXaVolume;
void XaPlayer_SetMasterVolume(float v);

/* PSX voice-start delay model (xa_seek_model.c). The sound task pool reports
 * its Stop and Play tasks; the players hold each disc line's audio start by
 * the modelled CD seek + task-state time, consumed once per line. */
void     Pc_XaSeek_OnStopTask(void);
void     Pc_XaSeek_OnPlayTask(uint16_t xaIdx, int xaVol);
uint32_t Pc_XaSeek_LineDelayMs(void);

#ifdef __cplusplus
}
#endif

#endif // PC_PORT_XA_PLAYER_H
