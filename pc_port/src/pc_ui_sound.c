/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ui_sound.c - one-shot WAV cues for port UI.
 *
 * Extracted from pc_ra_toast.c when the browser needed its own cues and the
 * unlock sound became configurable. The OpenAL-vs-SDL split and the
 * alcGetCurrentContext() gate are that file's, unchanged: with
 * spu_renderer=authentic|high_precision|modern OpenAL is never initialised, so
 * a cue must fall back to its own SDL device or it is silently lost.
 */

#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "pc_ui_sound.h"
#include "sh_log.h"

enum { SND_AL = 1, SND_SDL };

struct PcUiSound
{
    int              kind;
    ALuint           alBuf, alSrc;
    SDL_AudioDeviceID dev;
    Uint8*           wav;   /* kept for every replay on the SDL path */
    Uint32           len;
    Uint8*           scaled;      /* one cached attenuated copy (SDL path) */
    float            scaledGain;
};

PcUiSound* PcUiSound_Load(const char* path)
{
    SDL_AudioSpec spec;
    Uint8*        wav = NULL;
    Uint32        len = 0;
    PcUiSound*    snd;

    if (!path || !path[0])
        return NULL;

    if (!SDL_LoadWAV(path, &spec, &wav, &len))
    {
        SH_DBG("[UISND] %s: %s - cue disabled", path, SDL_GetError());
        return NULL;
    }
    if (spec.format != AUDIO_S16LSB || spec.channels < 1 || spec.channels > 2)
    {
        SH_DBG("[UISND] %s must be 16-bit PCM mono/stereo - cue disabled", path);
        SDL_FreeWAV(wav);
        return NULL;
    }

    snd = (PcUiSound*)calloc(1, sizeof(*snd));
    if (!snd)
    {
        SDL_FreeWAV(wav);
        return NULL;
    }

    if (alcGetCurrentContext())
    {
        alGenBuffers(1, &snd->alBuf);
        alGenSources(1, &snd->alSrc);
        alBufferData(snd->alBuf, spec.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                     wav, (ALsizei)len, spec.freq);
        SDL_FreeWAV(wav);                    /* AL copied it */
        alSourcei(snd->alSrc, AL_BUFFER, (ALint)snd->alBuf);
        /* Relative + at the listener, so a cue is never attenuated or panned by
         * wherever the 3D listener happens to be. */
        alSourcei(snd->alSrc, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(snd->alSrc, AL_POSITION, 0.0f, 0.0f, 0.0f);
        snd->kind = SND_AL;
    }
    else
    {
        SDL_AudioSpec want = spec;
        want.samples  = 1024;
        want.callback = NULL;
        if (!SDL_WasInit(SDL_INIT_AUDIO))
            SDL_InitSubSystem(SDL_INIT_AUDIO);
        snd->dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (!snd->dev)
        {
            SH_DBG("[UISND] SDL audio for %s: %s", path, SDL_GetError());
            SDL_FreeWAV(wav);
            free(snd);
            return NULL;
        }
        snd->wav = wav;
        snd->len = len;
        SDL_PauseAudioDevice(snd->dev, 0);
        snd->kind = SND_SDL;
    }

    SH_DBG("[UISND] loaded %s", path);
    return snd;
}

void PcUiSound_PlayGain(PcUiSound* snd, float gain)
{
    if (!snd)
        return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;

    if (snd->kind == SND_AL)
    {
        alSourcef(snd->alSrc, AL_GAIN, gain);
        alSourceStop(snd->alSrc);
        alSourcePlay(snd->alSrc);
    }
    else if (snd->kind == SND_SDL)
    {
        const Uint8* play = snd->wav;

        /* No per-source gain on a raw queue, so an attenuated copy is scaled
         * once and kept. Only one alternate gain is cached, which is all a
         * cue needs: callers use a fixed value per site. */
        if (gain < 0.999f)
        {
            if (!snd->scaled || snd->scaledGain != gain)
            {
                Uint32 i, samples = snd->len / 2;   /* validated AUDIO_S16LSB */
                if (!snd->scaled)
                    snd->scaled = (Uint8*)malloc(snd->len);
                if (snd->scaled)
                {
                    const Sint16* src = (const Sint16*)snd->wav;
                    Sint16*       dst = (Sint16*)snd->scaled;
                    for (i = 0; i < samples; i++)
                        dst[i] = (Sint16)((float)src[i] * gain);
                    snd->scaledGain = gain;
                }
            }
            if (snd->scaled)
                play = snd->scaled;
        }
        SDL_ClearQueuedAudio(snd->dev);
        SDL_QueueAudio(snd->dev, play, snd->len);
    }
}

void PcUiSound_Play(PcUiSound* snd)
{
    PcUiSound_PlayGain(snd, 1.0f);
}
