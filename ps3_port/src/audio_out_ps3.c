/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * audio_out_ps3.c - output pump for the software SPU.
 *
 * The whole PSX SPU (voices, ADSR, VAG decode, reverb) is audio_xbox.c, which is
 * nxdk-free and shared with both Xbox ports -- it just needs somebody to call
 * Audio_RenderInto() and hand the samples to hardware. On Xbox that was a
 * DirectSound streaming buffer; on 360 a libXenon submit/queue; here it is an
 * lv2 audio port, which is a ring of fixed-size blocks the hardware walks on its
 * own while we write ahead of its read cursor.
 *
 * Two things differ from both Xbox ports and neither is optional:
 *
 * 1. lv2 audio is FLOAT, not s16. The port buffer holds normalised f32 samples,
 *    so the SPU's s16 output is converted on the way out. There is no format
 *    negotiation to avoid this.
 * 2. The block size is fixed at AUDIO_BLOCK_SAMPLES (256 frames) and the rate is
 *    fixed at 48 kHz. audio_xbox.c already renders 48 kHz stereo, so no
 *    resampling stage is needed -- only the block quantisation.
 *
 * Pumped from the main loop rather than a thread, matching the other ports: the
 * software SPU is not reentrant, and driving it from the audio event thread
 * would need locking around every voice update. Writing one block ahead of
 * readIndex keeps latency at roughly a block and a half without that.
 */
#include <string.h>

#include <audio/audio.h>

#include "sh_log.h"

#define OUT_CHANNELS   2
#define BLOCK_FRAMES   AUDIO_BLOCK_SAMPLES              /* 256, fixed by lv2 */
#define BLOCK_SAMPLES  (BLOCK_FRAMES * OUT_CHANNELS)

extern void Audio_RenderInto(short* out, int frames);

static int             s_ready;
static u32             s_port;
static audioPortConfig s_cfg;
static u32             s_writeBlock;
static short           s_pcm[BLOCK_SAMPLES];

void Audio360_Init(void)
{
    audioPortParam param;

    if (audioInit() != 0) {
        SH_DBG("[AUD] audioInit FAILED - running silent");
        return;
    }

    memset(&param, 0, sizeof(param));
    param.numChannels = AUDIO_PORT_2CH;
    param.numBlocks   = AUDIO_BLOCK_8;
    param.attrib      = 0;
    param.level       = 1.0f;

    if (audioPortOpen(&param, &s_port) != 0) {
        SH_DBG("[AUD] audioPortOpen FAILED - running silent");
        return;
    }
    if (audioGetPortConfig(s_port, &s_cfg) != 0) {
        SH_DBG("[AUD] audioGetPortConfig FAILED - running silent");
        return;
    }
    audioPortStart(s_port);

    s_writeBlock = 0;
    s_ready      = 1;
    SH_DBG("[AUD] lv2 port %u up: %llu ch, %llu blocks, buf=0x%08x",
           (unsigned)s_port, (unsigned long long)s_cfg.channelCount,
           (unsigned long long)s_cfg.numBlocks, (unsigned)s_cfg.audioDataStart);
}

/* Call once per frame. Fills whatever blocks the hardware has already consumed,
 * staying one block ahead of the read cursor.
 *
 * Keeps the Xbox port's name: audio_xbox.c already calls Audio_XboxPump. */
void Audio_XboxPump(void)
{
    float* base;
    u32    blocks;
    int    guard = 0;

    if (!s_ready)
        return;

    base   = (float*)(uintptr_t)s_cfg.audioDataStart;
    blocks = (u32)s_cfg.numBlocks;

    /* Refresh the read cursor; without it we would write blindly and either
     * overrun the block the DAC is reading or fall permanently behind. */
    if (audioGetPortConfig(s_port, &s_cfg) != 0)
        return;

    /* Bounded, like the 360's: a stalled or mis-reporting cursor must not spin
     * the main loop rendering audio nobody is consuming. The bound is one full
     * ring, which is the most that can legitimately be owed in a frame. */
    while (s_writeBlock != s_cfg.readIndex && guard++ < (int)blocks) {
        float* dst = base + (size_t)s_writeBlock * BLOCK_SAMPLES;
        int    i;

        Audio_RenderInto(s_pcm, BLOCK_FRAMES);
        /* s16 -> normalised f32. 1/32768 rather than 1/32767 so the conversion
         * is an exact binary scale and full-negative does not clip. */
        for (i = 0; i < BLOCK_SAMPLES; i++)
            dst[i] = (float)s_pcm[i] * (1.0f / 32768.0f);

        s_writeBlock = (s_writeBlock + 1) % blocks;
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
