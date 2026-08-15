/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pc_sfx_override.h for the naming convention and the rate contract. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "main/fileinfo.h"
#include "pc_config.h"
#include "pc_loose_files.h"
#include "pc_sfx_override.h"
#include "sh_log.h"
#include "PsyX/PsyX_public.h" /* g_PsyX_SfxOverride */

/* A bank can hold up to 255 samples and the game keeps four slots live, but
 * only overridden samples occupy an entry, so this is a cap on how many sounds
 * a mod may replace at once rather than on bank size. */
#define SFX_OVERRIDE_MAX 256

typedef struct
{
    int    spuAddr;      /* address the voice will be pointed at */
    short* pcm;          /* PC-owned, any length */
    int    sampleCount;
    int    rate;         /* the WAV's own rate; 0 if it could not be read */
    int    spuBase;      /* base of the slot this entry belongs to */
} PcSfxOverride;

static PcSfxOverride s_overrides[SFX_OVERRIDE_MAX];
static int           s_count;

/* Invalidate by SLOT, not by bank: the game reuses one SPU address per bank
 * slot, so every weapon bank lands at the same base (0x21490 in a US build).
 * Keyed on the bank instead, a previous occupant's entries survive the swap and
 * keep matching by address — the shotgun would play the pistol's replacement. */
static void SfxOverride_DropSlot(int spuBase)
{
    int i, w = 0;

    for (i = 0; i < s_count; i++)
    {
        if (s_overrides[i].spuBase == spuBase)
        {
            free(s_overrides[i].pcm);
            continue;
        }
        if (w != i)
        {
            s_overrides[w] = s_overrides[i];
        }
        w++;
    }
    s_count = w;
}

void Pc_SfxOverride_Reset(void)
{
    int i;

    for (i = 0; i < s_count; i++)
    {
        free(s_overrides[i].pcm);
    }
    s_count = 0;
}

/* Minimal RIFF/PCM reader. Deliberately strict: a wrong guess about the data
 * chunk is inaudible as an error and audible as noise, so anything unexpected
 * is refused with a log line rather than played. */
static short* SfxOverride_LoadWav(const char* path, int* outCount, int* outRate)
{
    unsigned char* d;
    long           size = 0;
    int            fmtOff = -1, dataOff = -1, dataLen = 0;
    long           p;
    int            channels, bits, format, frames, i, c;
    short*         pcm;

    *outCount = 0;
    if (outRate != NULL) *outRate = 0;

    d = Pc_LooseSlurp(path, &size);
    if (d == NULL)
    {
        return NULL;
    }

    if (size < 44 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
    {
        SH_DBG("[SFXMOD] %s: not a RIFF/WAVE file", path);
        free(d);
        return NULL;
    }

    for (p = 12; p + 8 <= size;)
    {
        int len = (int)(d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | ((unsigned)d[p + 7] << 24));

        if (len < 0 || p + 8 + len > size)
        {
            len = (int)(size - p - 8);
        }
        if (memcmp(d + p, "fmt ", 4) == 0)
        {
            fmtOff = (int)p + 8;
        }
        else if (memcmp(d + p, "data", 4) == 0)
        {
            dataOff = (int)p + 8;
            dataLen = len;
        }
        p += 8 + len + (len & 1);
    }

    if (fmtOff < 0 || dataOff < 0)
    {
        SH_DBG("[SFXMOD] %s: missing fmt or data chunk", path);
        free(d);
        return NULL;
    }

    format   = d[fmtOff] | (d[fmtOff + 1] << 8);
    channels = d[fmtOff + 2] | (d[fmtOff + 3] << 8);
    bits     = d[fmtOff + 14] | (d[fmtOff + 15] << 8);
    if (outRate != NULL)
    {
        *outRate = (int)(d[fmtOff + 4] | (d[fmtOff + 5] << 8) |
                         (d[fmtOff + 6] << 16) | ((unsigned)d[fmtOff + 7] << 24));
    }

    if (format != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2)
    {
        SH_DBG("[SFXMOD] %s: need uncompressed 8/16-bit mono or stereo PCM (format=%d bits=%d ch=%d)",
               path, format, bits, channels);
        free(d);
        return NULL;
    }

    frames = dataLen / ((bits / 8) * channels);
    if (frames <= 0)
    {
        SH_DBG("[SFXMOD] %s: no samples", path);
        free(d);
        return NULL;
    }

    pcm = (short*)malloc((size_t)frames * sizeof(short));
    if (pcm == NULL)
    {
        free(d);
        return NULL;
    }

    for (i = 0; i < frames; i++)
    {
        int sum = 0;

        for (c = 0; c < channels; c++)
        {
            int off = dataOff + (i * channels + c) * (bits / 8);

            if (bits == 16)
            {
                sum += (short)(d[off] | (d[off + 1] << 8));
            }
            else
            {
                sum += ((int)d[off] - 128) << 8;
            }
        }
        pcm[i] = (short)(sum / channels);
    }

    free(d);
    *outCount = frames;
    return pcm;
}

/* PSX ADPCM -> 16-bit PCM. Mirrors PsyCross's decoder, including the flag bits
 * (LoopEnd = 1) that end a sample: the terminating block's audio is part of it. */
static const int s_adpcmF0[4] = { 0, 60, 115, 98 };
static const int s_adpcmF1[4] = { 0, 0, -52, -55 };

static short* SfxOverride_DecodeAdpcm(const unsigned char* src, int len, int* outCount)
{
    int    blocks = len / 16;
    short* out;
    int    w = 0, b, i, prev1 = 0, prev2 = 0;

    *outCount = 0;
    if (blocks <= 0)
    {
        return NULL;
    }

    out = (short*)malloc((size_t)blocks * 28 * sizeof(short));
    if (out == NULL)
    {
        return NULL;
    }

    for (b = 0; b < blocks; b++)
    {
        const unsigned char* p = src + b * 16;
        int shift = p[0] & 0x0F;
        int filter = (p[0] >> 4) & 0x0F;
        int flag = p[1];
        int mute = shift > 12;

        if (filter > 3)
        {
            filter = 3;
        }

        for (i = 0; i < 14; i++)
        {
            int half;

            for (half = 0; half < 2; half++)
            {
                int nib = (half == 0) ? (p[2 + i] & 0x0F) : (p[2 + i] >> 4);
                int s;

                if (nib > 7)
                {
                    nib -= 16;
                }

                if (mute)
                {
                    s = 0;
                }
                else
                {
                    s = (nib << 12) >> shift;
                    s += (prev1 * s_adpcmF0[filter] + prev2 * s_adpcmF1[filter]) >> 6;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                }

                prev2 = prev1;
                prev1 = s;
                out[w++] = (short)s;
            }
        }

        if (flag & 1)
        {
            break;
        }
    }

    *outCount = w;
    return out;
}

/* Recover the bank's file name from the sector it was read from. g_AudioData
 * seeks by absolute sector rather than by file index, so this is the only handle
 * on the bank's identity at load time. */
static int SfxOverride_BankName(int discSector, char* out, int outSize)
{
    s32 i;

    if (discSector <= 0)
    {
        return 0;
    }

    for (i = 0; i < FS_FILE_COUNT; i++)
    {
        if ((s32)g_FileTable[i].startSector == discSector)
        {
            char name[16];
            int  n;

            memset(name, 0, sizeof(name));
            Fs_GetFileInfoName(name, &g_FileTable[i]);

            /* The table stores "PISTOL  VAB"-style padded names; the loose file
             * is keyed on the stem alone. */
            for (n = 0; n < (int)sizeof(name) && name[n] != '\0'; n++)
            {
                if (name[n] == ' ' || name[n] == '.')
                {
                    break;
                }
            }
            if (n <= 0 || n >= outSize)
            {
                return 0;
            }
            memcpy(out, name, (size_t)n);
            out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Load a whole replacement bank, if the modder installed one. */
static unsigned char* SfxOverride_LoadBank(const char* bank, long* outSize)
{
    char           path[256];
    unsigned char* d;

    *outSize = 0;
    snprintf(path, sizeof(path), "gamedata/load/SND/%s.VAB", bank);

    d = Pc_LooseSlurp(path, outSize);
    if (d == NULL)
    {
        return NULL;
    }

    if (*outSize < 32 + (128 * 16) || d[0] != 'p' || d[1] != 'B' || d[2] != 'A' || d[3] != 'V')
    {
        SH_DBG("[SFXMOD] %s: not a VAB sound bank, ignored", path);
        free(d);
        *outSize = 0;
        return NULL;
    }

    SH_DBG("[SFXMOD] whole-bank replacement: %s (%ld bytes)", path, *outSize);
    return d;
}

/* Body of sample n inside a loose bank. The size table is ONE-BASED and stores
 * length/8, and bodies are packed in order, so an offset is the running sum. */
static const unsigned char* SfxOverride_BankSample(const unsigned char* d, long size, int n, int* outLen)
{
    int programCount = (int)(short)(d[18] | (d[19] << 8));
    int vagCount     = (int)(short)(d[22] | (d[23] << 8));
    int sizeTable, bodies, running, i;

    *outLen = 0;
    if (programCount <= 0 || programCount > 128 || n < 1 || n > vagCount)
    {
        return NULL;
    }

    sizeTable = 32 + (128 * 16) + (programCount * 16 * 32);
    bodies    = sizeTable + 256 * 2;
    if (bodies > size)
    {
        return NULL;
    }

    running = 0;
    for (i = 1; i < n; i++)
    {
        running += (d[sizeTable + i * 2] | (d[sizeTable + i * 2 + 1] << 8)) * 8;
    }

    *outLen = (d[sizeTable + n * 2] | (d[sizeTable + n * 2 + 1] << 8)) * 8;
    if (*outLen <= 0 || bodies + running + *outLen > size)
    {
        *outLen = 0;
        return NULL;
    }
    return d + bodies + running;
}

void Pc_SfxOverride_OnBankLoaded(const void* vabHeader, int spuBase, int discSector)
{
    const unsigned char* vh = (const unsigned char*)vabHeader;
    char                 bank[24];
    int                  programCount, vagCount;
    int                  sizeTable, running, n, installed = 0;
    unsigned char*       looseBank = NULL;
    long                 looseBankSize = 0;

    /* Arm the mixer hook here rather than at startup: this is the earliest point
     * that runs before any voice can play, and it is idempotent. */
    g_PsyX_SfxOverride = Pc_SfxOverride_Lookup;

    if (!g_PcConfig.allowLooseFiles || vh == NULL)
    {
        return;
    }

    /* Same guard the launcher's reader uses: the tag reads "pBAV" in file order
     * because the header stores it as a little-endian word. */
    if (vh[0] != 'p' || vh[1] != 'B' || vh[2] != 'A' || vh[3] != 'V')
    {
        return;
    }

    SfxOverride_DropSlot(spuBase);

    if (!SfxOverride_BankName(discSector, bank, (int)sizeof(bank)))
    {
        return;
    }

    programCount = (int)(short)(vh[18] | (vh[19] << 8));
    vagCount     = (int)(short)(vh[22] | (vh[23] << 8));
    if (programCount <= 0 || programCount > 128 || vagCount <= 0 || vagCount >= 256)
    {
        return;
    }

    /* 32-byte header + 128 program entries of 16 + the tone table. Matches
     * libsd/smf_io.c's own "vh + ps*512 + 2080". */
    sizeTable = 32 + (128 * 16) + (programCount * 16 * 32);

    /* A whole repacked bank dropped in as SND/<BANK>.VAB. The sound system seeks
     * VABs by absolute disc sector, so such a file is invisible to the normal
     * read path — but its samples can be lifted out here and registered exactly
     * like per-sound files, which makes the obvious workflow work with no
     * surgery on the CD path. Addresses still come from the ORIGINAL bank,
     * because that is what is actually resident in SPU RAM. */
    looseBank = SfxOverride_LoadBank(bank, &looseBankSize);

    running = 0;
    for (n = 1; n <= vagCount; n++)
    {
        int  len = (vh[sizeTable + n * 2] | (vh[sizeTable + n * 2 + 1] << 8)) * 8;
        int  addr = spuBase + running;
        char path[256];
        int  count = 0;
        int  wavRate = 0;
        short* pcm = NULL;
        const char* src = NULL;

        running += len;

        if (s_count >= SFX_OVERRIDE_MAX)
        {
            SH_DBG("[SFXMOD] override table full (%d) - %s.%03d and later ignored",
                   SFX_OVERRIDE_MAX, bank, n);
            break;
        }

        /* A per-sound file is the more specific statement, so it wins over the
         * whole-bank file for that one sample. */
        snprintf(path, sizeof(path), "gamedata/load/SND/%s.%03d.wav", bank, n);
        pcm = SfxOverride_LoadWav(path, &count, &wavRate);
        if (pcm == NULL)
        {
            /* BANK_NNN.wav is what the launcher's Audio tool names its exports,
             * so the obvious round trip -- export a sound, edit it, drop it in --
             * produced a file the loader never looked for. Both spellings are
             * built from the bank name we already know rather than parsed, so a
             * bank whose own name contains an underscore (MAP001_2) stays
             * unambiguous either way. */
            snprintf(path, sizeof(path), "gamedata/load/SND/%s_%03d.wav", bank, n);
            pcm = SfxOverride_LoadWav(path, &count, &wavRate);
        }
        if (pcm != NULL)
        {
            src = path;
        }
        else if (looseBank != NULL)
        {
            int adpcmLen = 0;
            const unsigned char* body = SfxOverride_BankSample(looseBank, looseBankSize, n, &adpcmLen);

            if (body != NULL)
            {
                pcm = SfxOverride_DecodeAdpcm(body, adpcmLen, &count);
                src = "loose bank";
            }
        }

        if (pcm == NULL || count <= 0)
        {
            free(pcm);
            continue;
        }

        s_overrides[s_count].spuAddr     = addr;
        s_overrides[s_count].pcm         = pcm;
        s_overrides[s_count].sampleCount = count;
        s_overrides[s_count].rate        = wavRate;
        s_overrides[s_count].spuBase     = spuBase;
        s_count++;
        installed++;

        /* The rate is reported, never acted on: the mixer uploads the
         * replacement at a fixed 44100 and lets the voice pitch scale it exactly
         * as it scaled the original, so a file authored at some other rate plays
         * proportionally faster or slower. Printing it is what lets someone
         * compare against the rate the Audio tool shows for that sample -- the
         * usual cause of a replacement that plays at the wrong speed, and far
         * more noticeable on the low-rate map banks. */
        SH_DBG("[SFXMOD] %s sample %d <- %s (%d samples @ %d Hz, was %d ADPCM bytes) spu=0x%x",
               bank, n, src, count, wavRate, len, addr);
    }

    free(looseBank);

    if (installed > 0)
    {
        SH_DBG("[SFXMOD] bank %s: %d of %d samples replaced", bank, installed, vagCount);
    }
}

int Pc_SfxOverride_Lookup(int spuAddr, const short** outPcm, int* outSampleCount, int* outRate)
{
    int i;

    for (i = 0; i < s_count; i++)
    {
        if (s_overrides[i].spuAddr == spuAddr)
        {
            *outPcm         = s_overrides[i].pcm;
            *outSampleCount = s_overrides[i].sampleCount;
            if (outRate != NULL)
            {
                *outRate = s_overrides[i].rate;
            }
            return 1;
        }
    }
    return 0;
}
