/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_out_xbox360.c - output pump for the software SPU.
 *
 * The whole PSX SPU (voices, ADSR, VAG decode, reverb) is audio_xbox.c, which is
 * nxdk-free and shared with the Xbox port -- it just needs somebody to call
 * Audio_RenderInto() and hand the samples to hardware. On Xbox that was a
 * DirectSound streaming buffer driven off a Win32 event; on 360 libXenon exposes
 * a far simpler submit/queue model, so this file is the whole of the difference.
 *
 * Pumped from the main loop rather than a thread: xenon_sound_get_unplayed()
 * makes the queue depth directly observable, so topping it up per frame keeps
 * latency bounded without needing a second thread and the locking that implies
 * around a software SPU that is not reentrant.
 */
#include <string.h>
#include <stdint.h>   /* xenon_sound/sound.h uses uint32_t without including it */

#include <xenon_sound/sound.h>

#include "sh_log.h"

/* audio_xbox.c renders at 48 kHz stereo, which is also what the 360 wants, so
 * there is no resampling stage here. */
#define OUT_HZ         48000
#define CHUNK_FRAMES   256                       /* ~5.3 ms per submit */
#define CHUNK_SAMPLES  (CHUNK_FRAMES * 2)        /* interleaved stereo */
/* Keep roughly this many bytes queued. Too little underruns across a long
 * blocking load (the CD-rate file reads stall the main loop for whole frames);
 * too much adds audible latency to SFX. */
#define TARGET_QUEUED_BYTES (CHUNK_SAMPLES * 2 * 6)

extern void Audio_RenderInto(short* out, int frames);

static int   s_ready;
static short s_chunk[CHUNK_SAMPLES];

void Audio360_Init(void)
{
    xenon_sound_init();
    s_ready = 1;
    SH_DBG("[AUD] xenon_sound up: %d Hz stereo, %d-frame chunks", OUT_HZ, CHUNK_FRAMES);
}

/* Call once per frame. Renders and submits until the queue is topped up.
 * Keeps the Xbox port's name: audio_xbox.c already calls Audio_XboxPump. */
void Audio_XboxPump(void)
{
    int guard = 0;

    if (!s_ready)
        return;

    /* Bounded: a stalled or mis-reporting queue must not spin the main loop
     * forever rendering audio nobody is consuming. */
    while (xenon_sound_get_unplayed() < TARGET_QUEUED_BYTES && guard++ < 8) {
        Audio_RenderInto(s_chunk, CHUNK_FRAMES);
        xenon_sound_submit(s_chunk, sizeof(s_chunk));
    }
}

/* XA streaming (the CD audio track mixer) lives in xa_xbox.c, which is nxdk-bound
 * and not ported yet. audio_xbox.c calls this unconditionally from its mixer, so
 * it has to exist; contributing silence means BGM is simply absent rather than
 * the mix being corrupted by an unwritten accumulator. */
void Xa_XboxMixInto(int* accL, int* accR, int* accC)
{
    (void)accL; (void)accR; (void)accC;
}
