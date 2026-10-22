/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_VOICE_CLIP_H
#define PC_VOICE_CLIP_H

/* One-shot PCM clips on their own OpenAL sources (pc_voice_clip.c), apart
 * from the game's single voice stream. */

#ifdef __cplusplus
extern "C" {
#endif

/* Play interleaved 16-bit PCM once. The data is copied; the caller may free
 * it afterwards. gain 0..1. Returns 1 when it started. */
int  Pc_VoiceClip_Play(const short* pcm, unsigned int samples, int rate, int stereo, float gain);
/* As Play, with a pitch multiplier. Returns a handle for the calls below, or 0.
 * A handle goes stale once its clip finishes or is displaced; stale handles are
 * ignored. */
unsigned int Pc_VoiceClip_PlayEx(const short* pcm, unsigned int samples, int rate, int stereo,
                                 float gain, float pitch);
void Pc_VoiceClip_SetGain(unsigned int handle, float gain);
int  Pc_VoiceClip_Playing(unsigned int handle);
/* 0..1 through the clip; 1 once it has finished or the handle is stale. */
float Pc_VoiceClip_Progress(unsigned int handle);
void Pc_VoiceClip_Stop(unsigned int handle);
/* Once a frame: recycle finished sources. */
void Pc_VoiceClip_Tick(void);

#ifdef __cplusplus
}
#endif

#endif
