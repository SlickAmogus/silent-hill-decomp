/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_mic.c - default microphone, cut into utterances.
 *
 * 16 kHz mono 16-bit, the format Morpheus records for Scribe. The device is
 * opened without a callback and drained with SDL_DequeueAudio from the frame
 * hook, so there is no audio thread to synchronise with.
 *
 * While the mic is on it listens continuously: an utterance starts when the
 * level crosses the threshold (keeping a short pre-roll so the first syllable
 * is not clipped) and ends after vad_hang_seconds below it. Each finished
 * utterance becomes one WAV for speech-to-text. Turning the mic off ends the
 * utterance in progress rather than throwing it away.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "pc_conscious.h"
#include "sh_log.h"

#define MIC_RATE      16000
#define MIC_BLOCK     320                    /* 20 ms */
#define MIC_PREROLL   (MIC_RATE * 3 / 10)
#define MIC_MAX       (MIC_RATE * 25)        /* one utterance, then it is cut */
#define MIC_MIN_VOICE (MIC_RATE * 3 / 10)    /* shorter bursts are clicks and coughs */

static SDL_AudioDeviceID s_dev;
static int               s_tried, s_failed;
static int               s_on;
static int               s_inSpeech;
static float             s_level;

static short s_pre[MIC_PREROLL];
static int   s_preLen;
static short* s_utt;
static int    s_uttLen;
static int    s_voiced;      /* samples above threshold in this utterance */
static int    s_quiet;       /* trailing samples below threshold */

static short s_blk[MIC_BLOCK];
static int   s_blkLen;

static int Mic_Open(void)
{
    SDL_AudioSpec want, have;

    if (s_dev) return 1;
    if (s_tried) return 0;
    s_tried = 1;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        SH_DBG("[CONSCIOUS] SDL audio init failed: %s", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    SDL_zero(want);
    want.freq     = MIC_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;
    s_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (!s_dev)
    {
        SH_DBG("[CONSCIOUS] no microphone: %s", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    s_utt = (short*)malloc(sizeof(short) * MIC_MAX);
    SH_DBG("[CONSCIOUS] microphone open (%s)", SDL_GetAudioDeviceName(0, 1) ? SDL_GetAudioDeviceName(0, 1) : "default");
    return s_utt != NULL;
}

static unsigned char* Mic_Wav(const short* pcm, int n, unsigned long* outLen)
{
    const unsigned long data = (unsigned long)n * 2;
    unsigned char*      w    = (unsigned char*)malloc(44 + data);
    unsigned int        v;
    if (!w) return NULL;
#define PUT32(o, x) do { v = (unsigned int)(x); w[o] = (unsigned char)v; w[o+1] = (unsigned char)(v >> 8); w[o+2] = (unsigned char)(v >> 16); w[o+3] = (unsigned char)(v >> 24); } while (0)
#define PUT16(o, x) do { w[o] = (unsigned char)(x); w[o+1] = (unsigned char)((x) >> 8); } while (0)
    memcpy(w, "RIFF", 4);      PUT32(4, 36 + data);
    memcpy(w + 8, "WAVEfmt ", 8); PUT32(16, 16);
    PUT16(20, 1);              PUT16(22, 1);
    PUT32(24, MIC_RATE);       PUT32(28, MIC_RATE * 2);
    PUT16(32, 2);              PUT16(34, 16);
    memcpy(w + 36, "data", 4); PUT32(40, data);
#undef PUT32
#undef PUT16
    memcpy(w + 44, pcm, data);
    *outLen = 44 + data;
    return w;
}

static int Mic_Finish(unsigned char** wav, unsigned long* wavLen)
{
    int ok = 0;
    if (s_inSpeech && s_voiced >= MIC_MIN_VOICE)
    {
        *wav = Mic_Wav(s_utt, s_uttLen, wavLen);
        ok   = (*wav != NULL);
    }
    s_inSpeech = 0;
    s_uttLen   = 0;
    s_voiced   = 0;
    s_quiet    = 0;
    s_preLen   = 0;
    return ok;
}

static void Mic_PreRoll(const short* b, int n)
{
    if (n >= MIC_PREROLL)
    {
        memcpy(s_pre, b + n - MIC_PREROLL, sizeof(short) * MIC_PREROLL);
        s_preLen = MIC_PREROLL;
        return;
    }
    if (s_preLen + n > MIC_PREROLL)
    {
        int drop = s_preLen + n - MIC_PREROLL;
        memmove(s_pre, s_pre + drop, sizeof(short) * (size_t)(s_preLen - drop));
        s_preLen -= drop;
    }
    memcpy(s_pre + s_preLen, b, sizeof(short) * (size_t)n);
    s_preLen += n;
}

/* One 20 ms block. Returns 1 when it ended an utterance. */
static int Mic_Block(const short* b, unsigned char** wav, unsigned long* wavLen)
{
    double acc = 0.0;
    float  rms;
    int    i, loud;

    for (i = 0; i < MIC_BLOCK; i++)
        acc += (double)b[i] * (double)b[i];
    rms  = (float)(sqrt(acc / MIC_BLOCK) / 32768.0);
    s_level = s_level * 0.8f + rms * 0.2f;
    loud = rms >= g_PcCon.vadThreshold;

    if (!s_inSpeech)
    {
        if (!loud)
        {
            Mic_PreRoll(b, MIC_BLOCK);
            return 0;
        }
        s_inSpeech = 1;
        memcpy(s_utt, s_pre, sizeof(short) * (size_t)s_preLen);
        s_uttLen = s_preLen;
        s_voiced = 0;
        s_quiet  = 0;
    }

    if (s_uttLen + MIC_BLOCK <= MIC_MAX)
    {
        memcpy(s_utt + s_uttLen, b, sizeof(short) * MIC_BLOCK);
        s_uttLen += MIC_BLOCK;
    }
    if (loud) { s_voiced += MIC_BLOCK; s_quiet = 0; }
    else      { s_quiet  += MIC_BLOCK; }

    if (s_quiet >= (int)(g_PcCon.vadHangSec * MIC_RATE) || s_uttLen + MIC_BLOCK > MIC_MAX)
        return Mic_Finish(wav, wavLen);
    return 0;
}

void PcConMic_SetOn(int on)
{
    on = on ? 1 : 0;
    if (on == s_on)
        return;
    if (on && !Mic_Open())
        return;
    s_on = on;
    if (s_dev)
    {
        SDL_ClearQueuedAudio(s_dev);
        SDL_PauseAudioDevice(s_dev, on ? 0 : 1);
    }
    s_blkLen = 0;
    if (on)
    {
        s_inSpeech = 0;
        s_preLen   = 0;
    }
    SH_DBG("[CONSCIOUS] mic %s", on ? "ON" : "OFF");
}

void PcConMic_Discard(void)
{
    if (s_dev)
        SDL_ClearQueuedAudio(s_dev);
    s_blkLen   = 0;
    s_inSpeech = 0;
    s_uttLen   = 0;
    s_voiced   = 0;
    s_quiet    = 0;
    s_preLen   = 0;
}

int PcConMic_On(void)       { return s_on; }
int PcConMic_Speaking(void) { return s_on && s_inSpeech; }
int PcConMic_Failed(void)   { return s_failed; }

float PcConMic_Level(void)
{
    return s_on ? s_level : 0.0f;
}

int PcConMic_Pump(unsigned char** wav, unsigned long* wavLen)
{
    short buf[2048];
    int   got;

    *wav    = NULL;
    *wavLen = 0;
    if (!s_dev)
        return 0;
    if (!s_on)
    {
        s_level = 0.0f;
        /* Switched off mid-sentence: what was said so far still counts. */
        return s_inSpeech ? Mic_Finish(wav, wavLen) : 0;
    }

    while ((got = (int)SDL_DequeueAudio(s_dev, buf, sizeof(buf))) > 0)
    {
        int n = got / 2, i = 0;
        while (i < n)
        {
            int take = MIC_BLOCK - s_blkLen;
            if (take > n - i) take = n - i;
            memcpy(s_blk + s_blkLen, buf + i, sizeof(short) * (size_t)take);
            s_blkLen += take;
            i += take;
            if (s_blkLen == MIC_BLOCK)
            {
                s_blkLen = 0;
                if (Mic_Block(s_blk, wav, wavLen))
                {
                    /* One utterance per frame; the rest waits in SDL's queue. */
                    return 1;
                }
            }
        }
    }
    return 0;
}
