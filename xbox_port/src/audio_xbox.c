/*
 * audio_xbox.c - PSX SPU emulation (software mixer) for the Silent Hill Xbox port.
 *
 * Replaces the no-op Spu* stubs (xbox_compat_stubs.c) with a real 24-voice
 * software SPU: a 512 KB SPU-RAM byte buffer the game fills via
 * SpuSetTransferStartAddr/SpuWrite, 24 voices keyed on/off and parameterised by
 * SpuSetVoiceAttr (start/loop address, L/R volume, pitch), and a mixer that
 * decodes PSX VAG ADPCM and sums the voices into 16-bit stereo PCM.
 *
 * OUTPUT-AGNOSTIC: this file only produces PCM via Audio_RenderInto(); the
 * DirectSound HAL (dsound_xbox.c) owns the hardware APU streaming buffer and
 * pumps Audio_RenderInto() into it on the main thread. Because DirectSound's
 * looping secondary buffer is played by the APU in hardware and refilled by a
 * main-thread pump (no audio DPC/IRQ callback), all SPU voice state and the
 * float ADPCM math run on the main thread — no cross-thread/FPU-in-interrupt
 * hazard, so no locking is needed.
 *
 * The VAG ADPCM decoder (vagToPcm/decodeVag) is ported from PsyCross
 * PsyX_SPUAL.cpp; the voice-attribute semantics mirror PsyX_SPUAL_SetVoiceAttr.
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <psyq/libspu.h>   /* the SAME header the game's libsd compiles against —
                            * guarantees SpuVoiceAttr layout / SPU_VOICE_* masks
                            * match what the caller fills in. */

#include "sh_log.h"

/* Debug hook the DirectSound bridge/driver (dsound_bridge.c) calls — forward it
 * to the SH debug log so RXDK init diagnostics land in D:\silenthill.log. */
void xbox_log(const char* fmt, ...)
{
    extern FILE* g_ShDebugLog;
    va_list ap;
    if (!g_ShDebugLog) return;
    va_start(ap, fmt);
    vfprintf(g_ShDebugLog, fmt, ap);
    va_end(ap);
}

#define SPU_RAM_SIZE    0x80000        /* 512 KB PSX SPU RAM */
#define SPU_VOICES      24
#define OUT_HZ          48000          /* AC97/DirectSound output rate */
#define SRC_HZ          44100          /* PSX VAG native rate at pitch=4096 */
#define VOICE_PCM_CAP   0x10000        /* max decoded samples per voice (~1.49s) */
#define SPU_ALLOC_BASE  0x1010         /* PSX reserves 0..0x100F */

static unsigned char  s_spuRam[SPU_RAM_SIZE];
static unsigned char* s_xferPtr = s_spuRam;     /* SpuSetTransferStartAddr cursor */
static int            s_allocTop = SPU_ALLOC_BASE;
static int            s_inited   = 0;

/* Master volume (SdSetMVol -> SpuSetCommonAttr), Q14. Applied to the final mix. */
static int            s_masterL  = 0x3FFF;
static int            s_masterR  = 0x3FFF;

/* PSX SPU ADSR envelope phase. */
typedef enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvPhase;

/* One software voice. pos/step are in decoded-PCM samples (fractional for
 * resampling). pcm[] holds the voice's sample, pre-decoded on key-on. */
typedef struct {
    unsigned int   addr;        /* waveform start (byte offset in SPU RAM) */
    unsigned int   loopAddr;    /* loop start (byte offset); 0 = none set */
    int            volL, volR;  /* per-voice volume, Q14 (0..0x3FFF) */
    unsigned short pitch;        /* raw pitch (4096 = native) */
    double         step;        /* decoded samples advanced per output frame */
    double         pos;         /* fractional play position in pcm[] */
    int            pcmLen;      /* decoded sample count */
    int            loopStart;   /* loop region [loopStart,loopEnd); -1 = no loop */
    int            loopEnd;
    int            looping;
    volatile int   active;
    short*         pcm;         /* VOICE_PCM_CAP decoded samples */

    /* ADSR envelope (ported from PsyCross PsyX_SPUAL.cpp / nocash psx-spx). The
     * game programs adsr1/adsr2 on every key-on (smf_io.c key_on mask 0x6019F),
     * so hasEnv is set for all sequenced/SFX voices; without it notes clicked
     * on/off with no attack ramp or release tail. */
    int            hasEnv;      /* this voice programmed a real ADSR envelope */
    EnvPhase       envPhase;
    int            envLevel;    /* 0..0x7FFF */
    int            envCounter;  /* cycles accumulator */
    double         envTickAcc;  /* fractional 44100Hz-tick accumulator */
    unsigned short adsr1, adsr2;
} Voice;

static Voice s_v[SPU_VOICES];
static short s_voicePcm[SPU_VOICES][VOICE_PCM_CAP];  /* ~3 MB BSS, CPU-only */

/* --- PSX VAG ADPCM decode (ported from PsyCross PsyX_SPUAL.cpp) ------------ */

static const float K0[5] = { 0.0f, 0.9375f, 1.796875f, 1.53125f, 1.90625f };
static const float K1[5] = { 0.0f, 0.0f, -0.8125f, -0.859375f, -0.9375f };

/* 2^(12 - shift) for shift 0..15 — the per-block sample scale, precomputed so
 * the inner decode has no pow() call. */
static const float SHIFT_FACTOR[16] = {
    4096.0f, 2048.0f, 1024.0f, 512.0f, 256.0f, 128.0f, 64.0f, 32.0f,
    16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f
};

static short vagToPcm(unsigned char sp, int sd, float* p1, float* p2)
{
    float out;
    int   r;
    int   filt = (sp >> 4) & 0x0F;
    if (filt > 4) filt = 4;             /* guard the K0/K1 tables (5 entries) */

    if (sd > 7) sd -= 16;               /* sign-extend the 4-bit nibble */

    out = (float)sd * SHIFT_FACTOR[sp & 0x0F] + (*p1) * K0[filt] + (*p2) * K1[filt];
    *p2 = *p1;
    *p1 = out;

    r = (int)(out >= 0.0f ? out + 0.5f : out - 0.5f);
    if (r > 32767)  r = 32767;
    if (r < -32768) r = -32768;
    return (short)r;
}

/* PSX ADPCM block flag bits (byte 1 of each 16-byte block). */
#define ADPCM_LOOP_END   0x01           /* terminating block (stop or loop back) */
#define ADPCM_REPEAT     0x02           /* with LOOP_END: loop instead of stop */
#define ADPCM_LOOP_START 0x04           /* mark this block as the loop point */

/* Decode the VAG at byteAddr into out[] (up to cap samples). On a Repeat loop,
 * writes loopStart/loopEnd (sample indices); otherwise both -1. Stops after the
 * terminating LOOP_END block. Returns the decoded sample count. */
static int decodeVag(unsigned int byteAddr, short* out, int cap,
                     int* loopStart, int* loopEnd)
{
    float        p1 = 0.0f, p2 = 0.0f;
    int          k = 0, ls = -1;
    unsigned int i = byteAddr;

    *loopStart = -1;
    *loopEnd   = -1;

    while (i + 16 <= SPU_RAM_SIZE && k + 28 <= cap) {
        unsigned char sp   = s_spuRam[i];
        unsigned char flag = s_spuRam[i + 1];
        int           j;

        if (flag & ADPCM_LOOP_START)
            ls = k;                      /* loop point = start of this block */

        for (j = 2; j < 16; j++) {
            int b = s_spuRam[i + j];
            out[k++] = vagToPcm(sp, b & 0x0F, &p1, &p2);
            out[k++] = vagToPcm(sp, (b >> 4) & 0x0F, &p1, &p2);
        }

        if (flag & ADPCM_LOOP_END) {
            if (flag & ADPCM_REPEAT) {
                *loopStart = (ls >= 0) ? ls : 0;
                *loopEnd   = k;
            }
            break;                       /* end of the sample data */
        }
        i += 16;
    }
    return k;
}

/* --- mixer ---------------------------------------------------------------- */

/* --- PSX SPU ADSR envelope (ported from PsyCross PsyX_SPUAL.cpp) ----------- */

#define ADSR_MAX 0x7FFF

static int adsr_sustain_level(unsigned short adsr1)
{
    int lvl = ((adsr1 & 0x0F) + 1) << 11;          /* (N+1)*0x800 */
    return lvl > ADSR_MAX ? ADSR_MAX : lvl;
}

/* Decompose a 0..0x7F rate into (cycles, step) at the current level. */
static void adsr_step_params(int rate, int increase, int exponential, int level,
                             int* outCycles, int* outStep)
{
    int shift  = (rate >> 2) & 0x1F;
    int sel    = rate & 3;
    int step   = increase ? (7 - sel) : (-8 + sel);
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);

    if (shift < 11)
        step <<= (11 - shift);

    if (exponential) {
        if (increase && level > 0x6000) cycles <<= 2;      /* attack slows near top */
        if (!increase)                  step = (step * level) >> 15;
    }
    if (cycles < 1) cycles = 1;
    *outCycles = cycles;
    *outStep   = step;
}

/* Advance a voice's envelope by `ticks` 44100Hz samples; returns 0..0x7FFF. */
static int EnvelopeAdvance(Voice* v, int ticks)
{
    const int sustainLevel = adsr_sustain_level(v->adsr1);
    int guard;

    v->envCounter += ticks;

    for (guard = 0; guard < 4096 && v->envPhase != ENV_OFF; guard++) {
        int rate, increase, exponential, cycles, step;

        switch (v->envPhase) {
        case ENV_ATTACK:  rate = (v->adsr1 >> 8) & 0x7F; increase = 1; exponential = (v->adsr1 >> 15) & 1; break;
        case ENV_DECAY:   rate = ((v->adsr1 >> 4) & 0x0F) << 2; increase = 0; exponential = 1; break;
        case ENV_SUSTAIN: rate = (v->adsr2 >> 6) & 0x7F; increase = !((v->adsr2 >> 14) & 1); exponential = (v->adsr2 >> 15) & 1; break;
        case ENV_RELEASE:
        default:          rate = (v->adsr2 & 0x1F) << 2; increase = 0; exponential = (v->adsr2 >> 5) & 1; break;
        }

        adsr_step_params(rate, increase, exponential, v->envLevel, &cycles, &step);
        if (v->envCounter < cycles)
            break;
        v->envCounter -= cycles;
        v->envLevel   += step;
        if (v->envLevel > ADSR_MAX) v->envLevel = ADSR_MAX;
        if (v->envLevel < 0)        v->envLevel = 0;

        switch (v->envPhase) {
        case ENV_ATTACK:  if (v->envLevel >= ADSR_MAX)     v->envPhase = ENV_DECAY;   break;
        case ENV_DECAY:   if (v->envLevel <= sustainLevel) { v->envLevel = sustainLevel; v->envPhase = ENV_SUSTAIN; } break;
        case ENV_SUSTAIN: break;
        case ENV_RELEASE: if (v->envLevel <= 0)            v->envPhase = ENV_OFF;     break;
        default: break;
        }
    }
    return v->envLevel;
}

/* Fill out[] with `frames` stereo (L,R) 16-bit samples, summing active voices.
 * Called by the DirectSound pump on the main thread. */
void Audio_RenderInto(short* out, int frames)
{
    int f, i;

    if (!s_inited) {
        memset(out, 0, (size_t)frames * 4);
        return;
    }

    const double envRate = (double)SRC_HZ / (double)OUT_HZ;

    for (f = 0; f < frames; f++) {
        int L = 0, R = 0;

        for (i = 0; i < SPU_VOICES; i++) {
            Voice* v = &s_v[i];
            int    idx, s0, s1, s;
            double frac;

            if (!v->active)
                continue;

            idx = (int)v->pos;
            if (idx < 0 || idx >= v->pcmLen) { v->active = 0; continue; }

            /* Linear interpolation between the two nearest samples (was
             * nearest-neighbour -> aliasing on every non-unity pitch). */
            s0 = v->pcm[idx];
            if (v->looping && idx + 1 >= v->loopEnd) s1 = v->pcm[v->loopStart];
            else if (idx + 1 < v->pcmLen)            s1 = v->pcm[idx + 1];
            else                                     s1 = s0;
            frac = v->pos - (double)idx;
            s = s0 + (int)((s1 - s0) * frac);

            /* ADSR: scale by the envelope level (Q15), advanced at 44100Hz. */
            if (v->hasEnv) {
                int ticks;
                v->envTickAcc += envRate;
                ticks = (int)v->envTickAcc;
                v->envTickAcc -= ticks;
                if (ticks) EnvelopeAdvance(v, ticks);
                if (v->envPhase == ENV_OFF) { v->active = 0; continue; }
                s = (s * v->envLevel) >> 15;
            }

            L += (s * v->volL) >> 14;
            R += (s * v->volR) >> 14;

            v->pos += v->step;
            if (v->looping) {
                if (v->pos >= (double)v->loopEnd)
                    v->pos -= (double)(v->loopEnd - v->loopStart);
            } else if ((int)v->pos >= v->pcmLen) {
                v->active = 0;
            }
        }

        /* Master volume (Q14); 64-bit intermediate — the summed mix can exceed
         * int16 before scaling. */
        L = (int)(((long long)L * s_masterL) >> 14);
        R = (int)(((long long)R * s_masterR) >> 14);

        if (L > 32767)  L = 32767;
        if (L < -32768) L = -32768;
        if (R > 32767)  R = 32767;
        if (R < -32768) R = -32768;
        out[f * 2]     = (short)L;
        out[f * 2 + 1] = (short)R;
    }
}

/* --- PSX libspu API (replaces the no-op stubs) ---------------------------- */

void SpuInit(void)
{
    int i;
    /* Idempotent: the game calls SpuInit twice (main_xbox before MainLoop, then
     * SdInit inside it) — don't wipe live voices/RAM on the second call. */
    if (s_inited)
        return;

    memset(s_spuRam, 0, sizeof(s_spuRam));
    memset(s_v, 0, sizeof(s_v));
    for (i = 0; i < SPU_VOICES; i++) {
        s_v[i].pcm       = s_voicePcm[i];
        s_v[i].loopStart = -1;
        s_v[i].loopEnd   = -1;
        s_v[i].volL      = 0x3FFF;
        s_v[i].volR      = 0x3FFF;
        s_v[i].pitch     = 4096;
        s_v[i].step      = (double)SRC_HZ / (double)OUT_HZ;
        s_v[i].envPhase  = ENV_OFF;
    }
    s_masterL = 0x3FFF;
    s_masterR = 0x3FFF;
    s_xferPtr = s_spuRam;
    s_allocTop = SPU_ALLOC_BASE;
    s_inited = 1;
    SH_DBG("[SH_AUDIO] SPU mixer init (%d voices, out=%dHz, ADSR+interp)", SPU_VOICES, OUT_HZ);
}

void SpuQuit(void) { s_inited = 0; }

int SpuInitMalloc(int num, char* top) { (void)num; (void)top; s_allocTop = SPU_ALLOC_BASE; return 0; }

unsigned int SpuSetTransferStartAddr(unsigned int addr)
{
    if (addr >= SPU_RAM_SIZE) addr = SPU_RAM_SIZE - 1;
    s_xferPtr = s_spuRam + addr;
    return addr;
}

int SpuSetTransferMode(int mode) { return mode; }

unsigned int SpuWrite(unsigned char* addr, unsigned int size)
{
    size_t off = (size_t)(s_xferPtr - s_spuRam);
    if (!addr) return 0;
    if (off + size > SPU_RAM_SIZE) size = (unsigned int)(SPU_RAM_SIZE - off);
    memcpy(s_xferPtr, addr, size);
    s_xferPtr += size;
    return size;
}

int SpuIsTransferCompleted(int flag) { (void)flag; return 1; }   /* synchronous */

void SpuSetVoiceAttr(SpuVoiceAttr* a)
{
    int i;
    if (!a) return;

    for (i = 0; i < SPU_VOICES; i++) {
        Voice* v;
        if ((a->voice & SPU_VOICECH(i)) == 0)
            continue;
        v = &s_v[i];

        if (a->mask & SPU_VOICE_WDSA)  v->addr     = a->addr;
        if (a->mask & SPU_VOICE_LSAX)  v->loopAddr = a->loop_addr;
        if (a->mask & SPU_VOICE_VOLL)  v->volL     = a->volume.left  & 0x3FFF;
        if (a->mask & SPU_VOICE_VOLR)  v->volR     = a->volume.right & 0x3FFF;
        if (a->mask & SPU_VOICE_PITCH) {
            v->pitch = a->pitch;
            v->step  = ((double)SRC_HZ / (double)OUT_HZ) * ((double)a->pitch / 4096.0);
        }
        /* Capture the ADSR registers the sequencer programs (key_on mask 0x6019F,
         * adsr_set, rr_off). A voice with a programmed envelope runs the ADSR. */
        if (a->mask & SPU_VOICE_ADSR_ADSR1) v->adsr1 = a->adsr1;
        if (a->mask & SPU_VOICE_ADSR_ADSR2) v->adsr2 = a->adsr2;
        if (a->mask & (SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2))
            v->hasEnv = (v->adsr1 || v->adsr2);
    }
}

void SpuGetVoiceAttr(SpuVoiceAttr* a)
{
    int i;
    if (!a) return;
    for (i = 0; i < SPU_VOICES; i++) {
        Voice* v;
        if ((a->voice & SPU_VOICECH(i)) == 0)
            continue;
        v = &s_v[i];
        a->volume.left  = (short)v->volL;
        a->volume.right = (short)v->volR;
        a->volumex      = a->volume;
        a->pitch        = v->pitch;
        a->addr         = v->addr;
        a->loop_addr    = v->loopAddr;
        a->envx         = 0;
        return;                          /* one voice per Get */
    }
}

void SpuSetKey(int on_off, unsigned int voice_bit)
{
    int i;
    for (i = 0; i < SPU_VOICES; i++) {
        Voice* v;
        if ((voice_bit & SPU_VOICECH(i)) == 0)
            continue;
        v = &s_v[i];

        if (on_off == SPU_ON) {
            int ls, le;
            v->pcmLen = decodeVag(v->addr, v->pcm, VOICE_PCM_CAP, &ls, &le);
            v->loopStart = ls;
            v->loopEnd   = le;
            v->looping   = (le > 0 && ls >= 0 && le <= v->pcmLen);
            v->pos       = 0.0;
            v->active    = (v->pcmLen > 0);
            if (v->hasEnv) {                 /* start the ADSR from silence (attack) */
                v->envPhase   = ENV_ATTACK;
                v->envLevel   = 0;
                v->envCounter = 0;
                v->envTickAcc = 0.0;
            }
        } else if (v->hasEnv && v->envPhase != ENV_OFF) {
            /* Key-off with an envelope: enter RELEASE and keep rendering (and
             * looping) so the tail rings out; the mixer deactivates at ENV_OFF. */
            v->envPhase = ENV_RELEASE;
        } else {
            /* No envelope: hard stop. Clear looping so a Repeat sample doesn't
             * ring forever. */
            v->looping = 0;
            v->active  = 0;
        }
    }
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr* a)
{
    if (!a) return;
    SpuSetVoiceAttr(a);
    SpuSetKey(SPU_ON, a->voice);
}

int SpuGetKeyStatus(unsigned int voice_bit)
{
    int i;
    for (i = 0; i < SPU_VOICES; i++) {
        Voice* v;
        if ((voice_bit & SPU_VOICECH(i)) == 0)
            continue;
        v = &s_v[i];
        if (!v->active)
            return SPU_OFF;
        if (v->hasEnv) {
            if (v->envPhase == ENV_RELEASE) return SPU_OFF_ENV_ON;  /* keyed off, ringing out */
            if (v->envLevel == 0)           return SPU_ON_ENV_OFF;  /* on, envelope at zero */
        }
        return SPU_ON;
    }
    return SPU_OFF;
}

/* Master volume (SdSetMVol): the final global gain stage — menu volume, scene
 * fade-to-silence, pause ducking. Applied to the summed mix in Audio_RenderInto. */
void SpuSetCommonAttr(SpuCommonAttr* a)
{
    if (!a) return;
    if (a->mask & SPU_COMMON_MVOLL) { int m = a->mvol.left;  if (m < 0) m = -m; s_masterL = m & 0x3FFF; }
    if (a->mask & SPU_COMMON_MVOLR) { int m = a->mvol.right; if (m < 0) m = -m; s_masterR = m & 0x3FFF; }
}

/* --- reverb: no-op for now (software reverb is the next build) ------------- */
int          SpuSetReverb(int on_off)                       { (void)on_off; return 0; }
int          SpuSetReverbModeParam(SpuReverbAttr* a)        { (void)a; return 0; }
unsigned int SpuSetReverbVoice(int on_off, unsigned int v) { (void)on_off; (void)v; return 0; }
int          SpuReserveReverbWorkArea(int on_off)           { (void)on_off; return 1; }
int          SpuClearReverbWorkArea(int mode)              { (void)mode; return 0; }
