/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_lipsync.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL_timer.h>
#include "pc_config.h"
#include "sh_log.h"

#define LIP_WIN_MS  20
#define LIP_RING    4096
#define LIP_PITCH_N 1024

/* The line's median pitch decides. Measured on the cafe scene's lines, Harry
 * speaks at 100-155 Hz and Cybil at 164-400; only Harry's shouts cross. */
#define LIP_FEMALE_HZ 158
#define LIP_HZ_MIN    60
#define LIP_HZ_BINS   200 /* 2 Hz each */

static int    s_active;
static uint16_t s_line;
static Uint32 s_startMs;
static Uint32 s_holdMs;
static int    s_held;
static int    s_rate, s_dec;
static int    s_winFrames;
static float  s_env[LIP_RING];
static uint32_t s_fed;
static double s_acc;
static int    s_accN;
static float  s_peak;
static float  s_pitch[LIP_PITCH_N];
static int    s_pitchN;
static int    s_hist[LIP_HZ_BINS];
static int    s_voiced;

/* Pitch of one window by normalised autocorrelation. The shortest lag close
 * to the best wins: a subharmonic correlates almost as well and would read a
 * female voice an octave low. */
static void Lip_Vote(void)
{
    int n = s_pitchN, er = s_rate / s_dec, lo = er / 400, hi = er / 70, L, best = 0;
    float r[512], top = 0.0f, e0 = 0.0f;
    int i;

    if (hi >= n / 2) hi = n / 2 - 1;
    if (hi - lo + 1 > 512) hi = lo + 511;
    for (i = 0; i < n; i++) e0 += s_pitch[i] * s_pitch[i];
    if (e0 < (float)n * 400.0f * 400.0f)
        return;
    for (L = lo; L <= hi; L++)
    {
        float s = 0.0f, a = 0.0f, b = 0.0f;
        for (i = 0; i + L < n; i++)
        {
            s += s_pitch[i] * s_pitch[i + L];
            a += s_pitch[i] * s_pitch[i];
            b += s_pitch[i + L] * s_pitch[i + L];
        }
        r[L - lo] = (a > 0.0f && b > 0.0f) ? s / sqrtf(a * b) : 0.0f;
        if (r[L - lo] > top) top = r[L - lo];
    }
    if (top < 0.5f)
        return;
    for (L = lo; L <= hi; L++)
        if (r[L - lo] >= 0.85f * top && (L == lo || r[L - lo] >= r[L - lo - 1]) &&
            (L == hi || r[L - lo] >= r[L - lo + 1]))
        {
            best = L;
            break;
        }
    if (best == 0)
        return;
    {
        int bin = (int)((float)er / (float)best - LIP_HZ_MIN) / 2;
        if (bin >= 0 && bin < LIP_HZ_BINS)
        {
            s_hist[bin]++;
            s_voiced++;
        }
    }
}

/* Per-line corrections, "<xaIdx> = <MODEL>" a line, "-" for nobody. */
static char s_override[PC_LIP_LINES][12];

static void Lip_LoadOverrides(void)
{
    static int loaded;
    char line[96];
    FILE* f;
    int n = 0;

    if (loaded)
        return;
    loaded = 1;
    if (!g_PcConfig.allowLooseFiles)
        return;
    f = fopen("gamedata/load/XA/speakers.txt", "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        int idx;
        char who[12];
        if (line[0] == '#' || sscanf(line, " %d = %11s", &idx, who) != 2 || idx <= 0 || idx >= PC_LIP_LINES)
            continue;
        snprintf(s_override[idx], sizeof(s_override[idx]), "%s", who);
        n++;
    }
    fclose(f);
    SH_DBG("[LIP] speakers.txt: %d line(s) reassigned", n);
}

void Pc_Lip_Feed(uint16_t xaIdx, const int16_t* pcm, int frames, int rate, int channels)
{
    int f;

    if (pcm == NULL || frames <= 0 || rate <= 0 || channels <= 0)
        return;
    if (!s_active)
    {
        Lip_LoadOverrides();
        s_active = 1;
        s_line = xaIdx;
        s_startMs = SDL_GetTicks();
        s_held = 0;
        s_rate = rate;
        s_dec = rate > 24000 ? (rate + 9450) / 18900 : 1;
        s_winFrames = rate * LIP_WIN_MS / 1000;
        s_fed = 0;
        s_acc = 0.0;
        s_accN = 0;
        s_peak = 0.0f;
        s_pitchN = 0;
        memset(s_hist, 0, sizeof(s_hist));
        s_voiced = 0;
    }
    for (f = 0; f < frames; f++)
    {
        int c, v = 0;
        for (c = 0; c < channels; c++)
            v += pcm[f * channels + c];
        v /= channels;
        s_acc += (double)v * v;
        if ((f % s_dec) == 0 && s_pitchN < LIP_PITCH_N)
            s_pitch[s_pitchN++] = (float)v;
        if (++s_accN >= s_winFrames)
        {
            float rms = (float)sqrt(s_acc / s_accN);
            if (s_fed < LIP_RING)
                s_env[s_fed] = rms;
            s_fed++;
            if (rms > s_peak)
                s_peak = rms;
            /* Two windows of audio per pitch estimate: one is too short for
             * a low male voice's period to repeat a few times. */
            if ((s_fed & 1) == 0)
            {
                Lip_Vote();
                s_pitchN = 0;
            }
            s_acc = 0.0;
            s_accN = 0;
        }
    }
}

void Pc_Lip_Stop(void)
{
    s_active = 0;
}

void Pc_Lip_Hold(int hold)
{
    if (!s_active || hold == s_held)
        return;
    s_held = hold;
    if (hold)
        s_holdMs = SDL_GetTicks();
    else
        s_startMs += SDL_GetTicks() - s_holdMs;
}

float Pc_Lip_Level(const char** speaker, int* female)
{
    Uint32 now = s_held ? s_holdMs : SDL_GetTicks();
    uint32_t w;
    float ref;

    *female = -1;
    *speaker = NULL;
    if (!s_active)
        return -1.0f;
    if (s_line < PC_LIP_LINES)
        *speaker = s_override[s_line][0] ? s_override[s_line] : g_PcLipSpeaker[s_line];
    if (s_voiced >= 4)
    {
        int i, n = 0;
        for (i = 0; i < LIP_HZ_BINS && n * 2 < s_voiced; i++)
            n += s_hist[i];
        *female = LIP_HZ_MIN + (i - 1) * 2 >= LIP_FEMALE_HZ;
    }
    w = (now - s_startMs) / LIP_WIN_MS;
    if (w >= s_fed || w >= LIP_RING)
        return 0.0f;
    /* Relative to the line's own loudest window so far, so a whispered line
     * still moves the mouth; the floor keeps room noise shut. */
    ref = s_peak * 0.6f;
    if (ref < 1500.0f)
        ref = 1500.0f;
    ref = (s_env[w] - 0.12f * ref) / (0.88f * ref);
    return ref < 0.0f ? 0.0f : (ref > 1.0f ? 1.0f : ref);
}
