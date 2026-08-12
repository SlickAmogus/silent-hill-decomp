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
    int    bankSector;   /* so a slot reload can drop just its own entries */
} PcSfxOverride;

static PcSfxOverride s_overrides[SFX_OVERRIDE_MAX];
static int           s_count;

static void SfxOverride_DropBank(int bankSector)
{
    int i, w = 0;

    for (i = 0; i < s_count; i++)
    {
        if (s_overrides[i].bankSector == bankSector)
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
static short* SfxOverride_LoadWav(const char* path, int* outCount)
{
    unsigned char* d;
    long           size = 0;
    int            fmtOff = -1, dataOff = -1, dataLen = 0;
    long           p;
    int            channels, bits, format, frames, i, c;
    short*         pcm;

    *outCount = 0;

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

void Pc_SfxOverride_OnBankLoaded(const void* vabHeader, int spuBase, int discSector)
{
    const unsigned char* vh = (const unsigned char*)vabHeader;
    char                 bank[24];
    int                  programCount, vagCount;
    int                  sizeTable, running, n, installed = 0;

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

    SfxOverride_DropBank(discSector);

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

    running = 0;
    for (n = 1; n <= vagCount; n++)
    {
        int  len = (vh[sizeTable + n * 2] | (vh[sizeTable + n * 2 + 1] << 8)) * 8;
        int  addr = spuBase + running;
        char path[256];
        int  count = 0;
        short* pcm;

        running += len;

        if (s_count >= SFX_OVERRIDE_MAX)
        {
            SH_DBG("[SFXMOD] override table full (%d) — %s.%03d and later ignored",
                   SFX_OVERRIDE_MAX, bank, n);
            break;
        }

        snprintf(path, sizeof(path), "gamedata/load/SND/%s.%03d.wav", bank, n);

        pcm = SfxOverride_LoadWav(path, &count);
        if (pcm == NULL)
        {
            continue;
        }

        s_overrides[s_count].spuAddr     = addr;
        s_overrides[s_count].pcm         = pcm;
        s_overrides[s_count].sampleCount = count;
        s_overrides[s_count].bankSector  = discSector;
        s_count++;
        installed++;

        SH_DBG("[SFXMOD] %s sample %d <- %s (%d samples, was %d ADPCM bytes) spu=0x%x",
               bank, n, path, count, len, addr);
    }

    if (installed > 0)
    {
        SH_DBG("[SFXMOD] bank %s: %d of %d samples replaced", bank, installed, vagCount);
    }
}

int Pc_SfxOverride_Lookup(int spuAddr, const short** outPcm, int* outSampleCount)
{
    int i;

    for (i = 0; i < s_count; i++)
    {
        if (s_overrides[i].spuAddr == spuAddr)
        {
            *outPcm         = s_overrides[i].pcm;
            *outSampleCount = s_overrides[i].sampleCount;
            return 1;
        }
    }
    return 0;
}
