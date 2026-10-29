/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_xaudio2.cpp - output pump for the software SPU, through XAudio2.
 *
 * The whole PSX SPU (voices, ADSR, VAG decode, reverb) is audio_xbox.c, shared
 * with the Original Xbox and libXenon builds; it only needs somebody to call
 * Audio_RenderInto() and hand the samples to hardware. XAudio2's source voice
 * is a submit queue, the same model libXenon's xenon_sound had, so this file
 * is the same shape as audio_out_xbox360.c.
 *
 * Pumped from the main loop rather than a thread: BuffersQueued makes the
 * queue depth directly observable, so topping it up per frame keeps latency
 * bounded without a second thread around a software SPU that is not
 * reentrant.
 *
 * Samples are host-order 16-bit, which on this machine is big-endian -- what
 * the 360's XAudio2 expects for WAVE_FORMAT_PCM. (The usual 360 gotcha is
 * little-endian WAV data from a PC needing a byte swap; rendered-in-place
 * audio has no such step.)
 */
#include <xtl.h>
#include <xaudio2.h>
#include <string.h>

extern "C" {
#include "sh_log.h"
void Audio_RenderInto(short* out, int frames);
void Xa_XboxPump(void);   /* xa_xbox.c: decode XA ahead of the mixer */
}

#define OUT_HZ        48000
#define CHUNK_FRAMES  256                      /* ~5.3 ms per submit */
#define CHUNK_SAMPLES (CHUNK_FRAMES * 2)       /* interleaved stereo */
/* chunks kept in flight: ~85 ms. The queue is topped up once per game frame,
 * and a cutscene frame on hardware can take 30+ ms (run 026 logged queued=2,
 * i.e. ~10 ms left); at the old 6 (~32 ms) the voice lines crackled. */
#define TARGET_QUEUED 16
/* XAudio2 reads a submitted buffer asynchronously, so a chunk must outlive
 * its submission: a ring deeper than the queue target is reused only after
 * the voice has consumed it. */
#define RING          24

static IXAudio2*               s_xa;
static IXAudio2MasteringVoice* s_master;
static IXAudio2SourceVoice*    s_voice;
static short                   s_ring[RING][CHUNK_SAMPLES];
static int                     s_ringPos;
static int                     s_ready;

extern "C" void Audio360_Init(void)
{
    WAVEFORMATEX wf;
    HRESULT      hr;

    hr = XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !s_xa) {
        SH_DBG("[AUD] XAudio2Create failed 0x%08x - no audio", (unsigned)hr);
        return;
    }
    hr = s_xa->CreateMasteringVoice(&s_master, 2, OUT_HZ, 0, 0, NULL);
    if (FAILED(hr)) {
        SH_DBG("[AUD] CreateMasteringVoice failed 0x%08x - no audio", (unsigned)hr);
        return;
    }
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = OUT_HZ;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 4;
    wf.nAvgBytesPerSec = OUT_HZ * 4;
    hr = s_xa->CreateSourceVoice(&s_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        SH_DBG("[AUD] CreateSourceVoice failed 0x%08x - no audio", (unsigned)hr);
        return;
    }
    s_voice->Start(0, XAUDIO2_COMMIT_NOW);
    s_ready = 1;
    SH_DBG("[AUD] XAudio2 up: %d Hz stereo, %d-frame chunks", OUT_HZ, CHUNK_FRAMES);
}

/* Call once per frame. Renders and submits until the queue is topped up.
 * Keeps the Xbox port's name: audio_xbox.c already calls Audio_XboxPump. */
extern "C" void Audio_XboxPump(void)
{
    XAUDIO2_VOICE_STATE st;
    int queued, guard = 0;

    if (!s_ready)
        return;

    /* Decode XA (voice lines / FMV audio) ahead of the render below; the
     * mixer pulls it through Xa_XboxMixInto. Same place dsound_xbox.c did it. */
    Xa_XboxPump();

    s_voice->GetState(&st);
    queued = (int)st.BuffersQueued;
    /* Bounded: a stalled or mis-reporting queue must not spin the main loop
     * forever rendering audio nobody is consuming. */
    while (queued < TARGET_QUEUED && guard++ < TARGET_QUEUED) {
        XAUDIO2_BUFFER b;
        short* chunk = s_ring[s_ringPos];
        s_ringPos = (s_ringPos + 1) % RING;

        Audio_RenderInto(chunk, CHUNK_FRAMES);
        {
            /* Is the software SPU producing anything at all? Peak sample per
             * ~10 s window, logged with the queue depth, so a silent console
             * can be told apart as "nothing rendered" vs "rendered, not heard". */
            static int s_renders = 0, s_peak = 0;
            int i;
            for (i = 0; i < CHUNK_SAMPLES; i++) {
                int v = chunk[i] < 0 ? -chunk[i] : chunk[i];
                if (v > s_peak) s_peak = v;
            }
            if ((++s_renders % 1800) == 0) {
                SH_DBG("[AUD] renders=%d peak=%d queued=%d", s_renders, s_peak, queued);
                s_peak = 0;
            }
        }
        memset(&b, 0, sizeof(b));
        b.AudioBytes = CHUNK_SAMPLES * sizeof(short);
        b.pAudioData = (const BYTE*)chunk;
        if (FAILED(s_voice->SubmitSourceBuffer(&b))) {
            static int s_failLogged = 0;
            if (!s_failLogged++) SH_DBG("[AUD] SubmitSourceBuffer FAILED");
            break;
        }
        queued++;
    }
}

/* XaPlayer_Stop: drop what is already rendered but not yet heard, or the tail
 * of the stopped line plays over the start of the next one (doubled cutscene
 * voices). The decode ring is cleared by the caller; this is the ~32 ms of
 * chunks sitting in XAudio2's queue. */
extern "C" void Audio360_DropQueued(void)
{
    if (s_ready)
        s_voice->FlushSourceBuffers();
}
