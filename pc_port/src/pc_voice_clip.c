/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_voice_clip.c - one-shot PCM clips on their own OpenAL sources.
 *
 * The game's voice channel is one stream (xa_player.c): starting anything on
 * it cuts the cutscene line that is playing and tells the scene script the
 * line finished. Generated lines therefore get their own small set of sources
 * on the same OpenAL context PsyCross owns, fed with already decoded PCM and
 * forgotten when done.
 */

#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "pc_voice_clip.h"
#include "sh_log.h"

#define VC_SOURCES 4

typedef struct
{
    ALuint       source;
    ALuint       buffer;
    int          busy;
    unsigned int serial; /* bumped on every start, so old handles go stale */
} VcSlot;

static VcSlot       s_slots[VC_SOURCES];
static int          s_ready;
static unsigned int s_serial;

/* The game's voice volume slider, so these sit at the level of the XA lines. */
extern float g_PcXaVolume;

/* OpenAL's error flag is one sticky value for the whole context: it holds the
 * last error ANYONE raised until someone reads it, and the SPU emulation does
 * not read its own. Testing alGetError() after our calls therefore blamed us
 * for PsyCross's leftovers -- the sources were never marked ready and every
 * clip failed silently. Drain it first, and judge our own calls by what they
 * produced (valid names, a playing source) rather than by the flag. */
static void Vc_DrainErrors(void)
{
    int n = 0;
    while (alGetError() != AL_NO_ERROR && ++n < 16) {}
}

static int Vc_Ready(void)
{
    int i;
    if (s_ready)
    {
        return 1;
    }
    if (alcGetCurrentContext() == NULL)
    {
        return 0; /* PsyCross has not brought audio up (or runs without it) */
    }
    Vc_DrainErrors();
    for (i = 0; i < VC_SOURCES; i++)
    {
        alGenSources(1, &s_slots[i].source);
        alGenBuffers(1, &s_slots[i].buffer);
        s_slots[i].busy = 0;
        if (!alIsSource(s_slots[i].source) || !alIsBuffer(s_slots[i].buffer))
        {
            SH_DBG("[VOICECLIP] could not create OpenAL source/buffer %d (err 0x%x)", i, alGetError());
            return 0;
        }
    }
    s_ready = 1;
    return 1;
}

static VcSlot* Vc_FromHandle(unsigned int handle)
{
    unsigned int idx = handle & 0xFu;
    if (!s_ready || idx == 0 || idx > VC_SOURCES)
    {
        return NULL;
    }
    if (!s_slots[idx - 1].busy || s_slots[idx - 1].serial != (handle >> 4))
    {
        return NULL;
    }
    return &s_slots[idx - 1];
}

void Pc_VoiceClip_Tick(void)
{
    int i;
    if (!s_ready)
    {
        return;
    }
    for (i = 0; i < VC_SOURCES; i++)
    {
        ALint st = AL_STOPPED;
        if (!s_slots[i].busy) continue;
        alGetSourcei(s_slots[i].source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING && st != AL_PAUSED)
        {
            alSourcei(s_slots[i].source, AL_BUFFER, 0);
            s_slots[i].busy = 0;
        }
    }
}

unsigned int Pc_VoiceClip_PlayEx(const short* pcm, unsigned int samples, int rate, int stereo,
                                 float gain, float pitch)
{
    int i, pick = -1;
    if (!pcm || samples == 0 || rate <= 0 || !Vc_Ready())
    {
        return 0;
    }
    Pc_VoiceClip_Tick();
    for (i = 0; i < VC_SOURCES; i++)
    {
        if (!s_slots[i].busy) { pick = i; break; }
    }
    if (pick < 0)
    {
        /* All talking at once: the oldest gives way. */
        pick = 0;
        alSourceStop(s_slots[0].source);
        alSourcei(s_slots[0].source, AL_BUFFER, 0);
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    if (pitch < 0.5f) pitch = 0.5f;
    if (pitch > 2.0f) pitch = 2.0f;
    Vc_DrainErrors();
    alBufferData(s_slots[pick].buffer, stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 pcm, (ALsizei)(samples * sizeof(short)), rate);
    alSourcei(s_slots[pick].source, AL_BUFFER, (ALint)s_slots[pick].buffer);
    /* Not positioned in 3D: the caller does distance falloff through gain,
     * and a source left at the listener origin would be spatialised by
     * whatever listener state the spatial SPU backend last set. */
    alSourcei(s_slots[pick].source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(s_slots[pick].source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(s_slots[pick].source, AL_GAIN, gain * g_PcXaVolume);
    alSourcef(s_slots[pick].source, AL_PITCH, pitch);
    alSourcePlay(s_slots[pick].source);
    {
        ALint st = AL_STOPPED;
        ALenum err = alGetError();
        alGetSourcei(s_slots[pick].source, AL_SOURCE_STATE, &st);
        s_slots[pick].busy = (st == AL_PLAYING);
        if (!s_slots[pick].busy)
        {
            SH_DBG("[VOICECLIP] clip did not start (%u samples @ %d Hz, state 0x%x, err 0x%x)",
                   samples, rate, st, err);
            return 0;
        }
    }
    s_serial = (s_serial + 1) & 0x0FFFFFFFu;
    if (s_serial == 0) s_serial = 1;
    s_slots[pick].serial = s_serial;
    return (s_serial << 4) | (unsigned int)(pick + 1);
}

int Pc_VoiceClip_Play(const short* pcm, unsigned int samples, int rate, int stereo, float gain)
{
    return Pc_VoiceClip_PlayEx(pcm, samples, rate, stereo, gain, 1.0f) != 0;
}

void Pc_VoiceClip_SetGain(unsigned int handle, float gain)
{
    VcSlot* s = Vc_FromHandle(handle);
    if (s)
    {
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;
        alSourcef(s->source, AL_GAIN, gain * g_PcXaVolume);
    }
}

int Pc_VoiceClip_Playing(unsigned int handle)
{
    VcSlot* s = Vc_FromHandle(handle);
    ALint   st = AL_STOPPED;
    if (!s)
    {
        return 0;
    }
    alGetSourcei(s->source, AL_SOURCE_STATE, &st);
    return st == AL_PLAYING || st == AL_PAUSED;
}

void Pc_VoiceClip_Stop(unsigned int handle)
{
    VcSlot* s = Vc_FromHandle(handle);
    if (s)
    {
        alSourceStop(s->source);
        alSourcei(s->source, AL_BUFFER, 0);
        s->busy = 0;
    }
}
