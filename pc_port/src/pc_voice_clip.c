/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_voice_clip.c - one-shot PCM clips, apart from the game's voice stream.
 *
 * The game's voice channel is one stream (xa_player.c): starting anything on
 * it cuts the cutscene line that is playing and tells the scene script the
 * line finished. Generated lines therefore get their own small set of voices.
 *
 * Two ways out, picked on first use:
 *   - OpenAL sources on PsyCross's context, when the legacy SPU made one;
 *   - otherwise our own SDL output device, mixed here. The software SPU never
 *     creates an OpenAL context (it renders through its own backend, and its
 *     only way in is the XA queue the cutscenes own), so without this every
 *     clip was silent there. Shared-mode output mixes the two streams.
 */

#include <stdlib.h>
#include <string.h>
#include <SDL.h>
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

    /* SDL mixer */
    short*       pcm;
    unsigned int frames;
    int          stereo;
    double       pos, step;
    float        gain;
} VcSlot;

enum { VC_NONE = 0, VC_OPENAL, VC_SDL, VC_FAILED };

static VcSlot            s_slots[VC_SOURCES];
static int               s_backend;
static unsigned int      s_serial;
static SDL_AudioDeviceID s_dev;
static int               s_devRate;

/* The game's voice volume slider, so these sit at the level of the XA lines. */
extern float g_PcXaVolume;

/* OpenAL's error flag is one sticky value for the whole context and the SPU
 * code leaves its own errors in it, so our calls are judged by what they
 * produced, never by the flag. */
static void Vc_DrainErrors(void)
{
    int n = 0;
    while (alGetError() != AL_NO_ERROR && ++n < 16) {}
}

static void SDLCALL Vc_Mix(void* user, Uint8* stream, int len)
{
    float* out    = (float*)stream;
    int    frames = len / (int)(2 * sizeof(float));
    int    i, f;
    (void)user;

    memset(stream, 0, (size_t)len);
    for (i = 0; i < VC_SOURCES; i++)
    {
        VcSlot* s = &s_slots[i];
        float   g;
        if (!s->busy || !s->pcm)
            continue;
        g = s->gain * g_PcXaVolume / 32768.0f;
        for (f = 0; f < frames; f++)
        {
            unsigned int k = (unsigned int)s->pos;
            float        t = (float)(s->pos - (double)k);
            float        l, r;
            if (k + 1 >= s->frames)
            {
                s->busy = 0;
                break;
            }
            if (s->stereo)
            {
                l = s->pcm[k * 2]     + (s->pcm[k * 2 + 2] - s->pcm[k * 2])     * t;
                r = s->pcm[k * 2 + 1] + (s->pcm[k * 2 + 3] - s->pcm[k * 2 + 1]) * t;
            }
            else
            {
                l = r = s->pcm[k] + (s->pcm[k + 1] - s->pcm[k]) * t;
            }
            out[f * 2]     += l * g;
            out[f * 2 + 1] += r * g;
            s->pos += s->step;
        }
    }
    for (f = 0; f < frames * 2; f++)
    {
        if (out[f] > 1.0f)  out[f] = 1.0f;
        if (out[f] < -1.0f) out[f] = -1.0f;
    }
}

static int Vc_OpenSdl(void)
{
    SDL_AudioSpec want, have;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        SH_DBG("[VOICECLIP] SDL audio init failed: %s", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq     = 48000;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = Vc_Mix;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s_dev)
    {
        SH_DBG("[VOICECLIP] no output device: %s", SDL_GetError());
        return 0;
    }
    s_devRate = have.freq;
    SDL_PauseAudioDevice(s_dev, 0);
    SH_DBG("[VOICECLIP] own SDL output open (%d Hz) -- no OpenAL context to share", s_devRate);
    return 1;
}

static int Vc_Ready(void)
{
    int i;
    if (s_backend == VC_OPENAL || s_backend == VC_SDL)
        return 1;
    if (s_backend == VC_FAILED)
        return 0;

    if (alcGetCurrentContext() != NULL)
    {
        Vc_DrainErrors();
        for (i = 0; i < VC_SOURCES; i++)
        {
            alGenSources(1, &s_slots[i].source);
            alGenBuffers(1, &s_slots[i].buffer);
            if (!alIsSource(s_slots[i].source) || !alIsBuffer(s_slots[i].buffer))
                break;
        }
        if (i == VC_SOURCES)
        {
            s_backend = VC_OPENAL;
            SH_DBG("[VOICECLIP] using OpenAL sources on the SPU's context");
            return 1;
        }
        SH_DBG("[VOICECLIP] OpenAL context present but sources failed; trying SDL output");
    }
    s_backend = Vc_OpenSdl() ? VC_SDL : VC_FAILED;
    return s_backend == VC_SDL;
}

static VcSlot* Vc_FromHandle(unsigned int handle)
{
    unsigned int idx = handle & 0xFu;
    if (idx == 0 || idx > VC_SOURCES)
        return NULL;
    if (!s_slots[idx - 1].busy || s_slots[idx - 1].serial != (handle >> 4))
        return NULL;
    return &s_slots[idx - 1];
}

void Pc_VoiceClip_Tick(void)
{
    int i;
    if (s_backend != VC_OPENAL)
        return;
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

static unsigned int Vc_Handle(int pick)
{
    s_serial = (s_serial + 1) & 0x0FFFFFFFu;
    if (s_serial == 0) s_serial = 1;
    s_slots[pick].serial = s_serial;
    return (s_serial << 4) | (unsigned int)(pick + 1);
}

unsigned int Pc_VoiceClip_PlayEx(const short* pcm, unsigned int samples, int rate, int stereo,
                                 float gain, float pitch)
{
    int i, pick = -1;
    if (!pcm || samples < 4 || rate <= 0 || !Vc_Ready())
        return 0;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    if (pitch < 0.5f) pitch = 0.5f;
    if (pitch > 2.0f) pitch = 2.0f;

    if (s_backend == VC_SDL)
    {
        short* copy = (short*)malloc(samples * sizeof(short));
        short* old  = NULL;
        if (!copy)
            return 0;
        memcpy(copy, pcm, samples * sizeof(short));
        SDL_LockAudioDevice(s_dev);
        for (i = 0; i < VC_SOURCES; i++)
            if (!s_slots[i].busy) { pick = i; break; }
        if (pick < 0)
            pick = 0; /* all talking at once: the oldest gives way */
        old = s_slots[pick].pcm;
        s_slots[pick].pcm    = copy;
        s_slots[pick].frames = stereo ? samples / 2 : samples;
        s_slots[pick].stereo = stereo;
        s_slots[pick].pos    = 0.0;
        s_slots[pick].step   = (double)rate * pitch / (double)s_devRate;
        s_slots[pick].gain   = gain;
        s_slots[pick].busy   = 1;
        SDL_UnlockAudioDevice(s_dev);
        free(old);
        return Vc_Handle(pick);
    }

    Pc_VoiceClip_Tick();
    for (i = 0; i < VC_SOURCES; i++)
        if (!s_slots[i].busy) { pick = i; break; }
    if (pick < 0)
    {
        pick = 0;
        alSourceStop(s_slots[0].source);
        alSourcei(s_slots[0].source, AL_BUFFER, 0);
    }
    Vc_DrainErrors();
    alBufferData(s_slots[pick].buffer, stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 pcm, (ALsizei)(samples * sizeof(short)), rate);
    alSourcei(s_slots[pick].source, AL_BUFFER, (ALint)s_slots[pick].buffer);
    /* Not placed in 3D: distance falloff is the caller's gain, and a source at
     * the listener origin would pick up whatever listener state the SPU set. */
    alSourcei(s_slots[pick].source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(s_slots[pick].source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(s_slots[pick].source, AL_GAIN, gain * g_PcXaVolume);
    alSourcef(s_slots[pick].source, AL_PITCH, pitch);
    alSourcePlay(s_slots[pick].source);
    {
        ALint  st  = AL_STOPPED;
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
    return Vc_Handle(pick);
}

int Pc_VoiceClip_Play(const short* pcm, unsigned int samples, int rate, int stereo, float gain)
{
    return Pc_VoiceClip_PlayEx(pcm, samples, rate, stereo, gain, 1.0f) != 0;
}

void Pc_VoiceClip_SetGain(unsigned int handle, float gain)
{
    VcSlot* s = Vc_FromHandle(handle);
    if (!s)
        return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    if (s_backend == VC_SDL)
        s->gain = gain; /* one aligned float: the mixer may read either value */
    else
        alSourcef(s->source, AL_GAIN, gain * g_PcXaVolume);
}

int Pc_VoiceClip_Playing(unsigned int handle)
{
    VcSlot* s = Vc_FromHandle(handle);
    ALint   st = AL_STOPPED;
    if (!s)
        return 0;
    if (s_backend == VC_SDL)
        return s->busy;
    alGetSourcei(s->source, AL_SOURCE_STATE, &st);
    return st == AL_PLAYING || st == AL_PAUSED;
}

float Pc_VoiceClip_Progress(unsigned int handle)
{
    VcSlot* s = Vc_FromHandle(handle);
    if (!s)
        return 1.0f;
    if (s_backend == VC_SDL)
        return s->frames ? (float)(s->pos / (double)s->frames) : 1.0f;
    {
        ALint off = 0, size = 0, bits = 16, ch = 1;
        alGetSourcei(s->source, AL_SAMPLE_OFFSET, &off);
        alGetBufferi(s->buffer, AL_SIZE, &size);
        alGetBufferi(s->buffer, AL_BITS, &bits);
        alGetBufferi(s->buffer, AL_CHANNELS, &ch);
        size = size / ((bits / 8) * (ch ? ch : 1));
        return size > 0 ? (float)off / (float)size : 1.0f;
    }
}

void Pc_VoiceClip_Stop(unsigned int handle)
{
    VcSlot* s = Vc_FromHandle(handle);
    if (!s)
        return;
    if (s_backend == VC_SDL)
    {
        SDL_LockAudioDevice(s_dev);
        s->busy = 0;
        SDL_UnlockAudioDevice(s_dev);
        return;
    }
    alSourceStop(s->source);
    alSourcei(s->source, AL_BUFFER, 0);
    s->busy = 0;
}
