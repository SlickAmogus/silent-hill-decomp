#include "main/fileinfo.h"

#ifdef SH_PC_PORT
#include <string.h> /* memcpy */
#include "bodyprog/sound/sound_system.h" /* s_AudioItemData g_AudioData */
#include "pc_config.h"                   /* g_PcConfig.language */
#define AUDIO_DATA_COUNT 127
#endif

#define NAME_PART_CHARS  4
#define NAME_CHAR_BITS   6
#define NAME_CHAR_MASK   0x3F
#define NAME_CHAR_OFFSET 0x20

/** Convenience macros to convert constant filenames to `name0123` and `name4567`. */
#define FA2N(c) (((u8)(c) - 0x20) & 0x3f)
#define FNP(c0, c1, c2, c3) (FA2N(c0) | (FA2N(c1) << 6) | (FA2N(c2) << 12) | (FA2N(c3) << 18))
#define FN(c0, c1, c2, c3, c4, c5, c6, c7) FNP(c0, c1, c2, c3), FNP(c4, c5, c6, c7)

#ifdef SH_PC_PORT
/* PC port: one executable supports multiple disc regions. Both region tables are
 * compiled in; g_FileTable is the ACTIVE table, kept in US-canonical shape so all
 * FILE_* enum indices resolve unchanged. Fs_InitFileTableForRegion() fills it from
 * the detected disc. (The upstream decomp/matching build keeps the #else path.) */
static const s_FileInfo s_FileTable_USA[] = {
    #include "filetable.c.USA.inc"
};
static const s_FileInfo s_FileTable_EUR[] = {
    #include "filetable.c.EUR.inc"
};
#define FS_FILE_COUNT_EUR ((s32)(sizeof(s_FileTable_EUR) / sizeof(s_FileInfo)))

s_FileInfo   g_FileTable[FS_FILE_COUNT];
e_GameRegion g_GameRegion = Region_USA;

/* US pathIdx -> EUR pathIdx. EUR inserts VIN2..VIN5 (localized maps), so XA moves
 * from 10 to 14; every other path index is identical. */
static const u8 s_UsaToEurPath[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 14 };

/* XA stream base sectors, runtime-selected. EUR[1] = 0x0B847 is the PAL HILL.
 * container start, mirroring US[1] = 0x099BF. */
static const u32 s_FileXaLoc_USA[] = {
    0x00000, 0x099BF, 0x0A227, 0x0B377, 0x0D0BF, 0x0EA57,
    0x0F997, 0x1096F, 0x16F07, 0x19797, 0x00000
};
static const u32 s_FileXaLoc_EUR[] = {
    0x00000, 0x0B847, 0x0C0AF, 0x0D1FF, 0x0EF47, 0x108DF,
    0x1181F, 0x127F7, 0x18D8F, 0x1B61F, 0x00000
};
u32 g_FileXaLoc[11];

void Fs_InitFileTableForRegion(e_GameRegion region)
{
    s32 i, j;

    g_GameRegion = region;

    memcpy(g_FileXaLoc, (region == Region_EUR) ? s_FileXaLoc_EUR : s_FileXaLoc_USA,
           sizeof(g_FileXaLoc));

    memcpy(g_FileTable, s_FileTable_USA, sizeof(s_FileTable_USA));
    if (region != Region_EUR)
    {
        return;
    }

    /* EUR: keep each US entry's name/path/type but take the same-named EUR file's
     * actual disc sector + size (US sector stays as a fallback for the handful of
     * US-only files that have no EUR equivalent — they are not loaded on PAL). */
    for (i = 0; i < FS_FILE_COUNT; i++)
    {
        const s_FileInfo* u = &s_FileTable_USA[i];
        u8 wantPath = (u->pathIdx < (u8)(sizeof(s_UsaToEurPath) / sizeof(s_UsaToEurPath[0])))
                          ? s_UsaToEurPath[u->pathIdx]
                          : (u8)u->pathIdx;

        for (j = 0; j < FS_FILE_COUNT_EUR; j++)
        {
            const s_FileInfo* e = &s_FileTable_EUR[j];
            if (e->name0123 == u->name0123 && e->name4567 == u->name4567 &&
                e->type == u->type && e->pathIdx == wantPath)
            {
                g_FileTable[i].startSector = e->startSector;
                g_FileTable[i].blockCount  = e->blockCount;
                break;
            }
        }
    }

    Fs_ApplyLanguageRedirects();

    /* The sound system seeks VAB/KDT (BGM/SFX) by absolute disc sector from its
     * own table g_AudioData[].fileOffset_8 — a US-baked parallel to g_FileTable.
     * Re-point each entry from its US sector to the active (EUR) sector of the
     * same file so BGM/SFX load on PAL. (XA streaming uses g_FileXaLoc, already
     * region-selected; its in-container offsets are identical since the HILL.
     * archive is the same size on both discs.) */
    {
        extern s_AudioItemData g_AudioData[];
        s32 a;
        for (a = 0; a < AUDIO_DATA_COUNT; a++)
        {
            s32 us = g_AudioData[a].fileOffset_8;
            for (j = 0; j < FS_FILE_COUNT; j++)
            {
                if ((s32)s_FileTable_USA[j].startSector == us)
                {
                    g_AudioData[a].fileOffset_8 = (s32)g_FileTable[j].startSector;
                    break;
                }
            }
        }
    }
}
/* Language redirect (config `language`, EUR discs only): PAL carries the
 * DE/FR/ES/IT map overlays in VIN2..VIN5 (EUR pathIdx 10..13) with the
 * language digit baked into name char 6 (MAP0_S00 -> MAP0_S10 DE / S20 FR
 * / S30 ES / S40 IT), and per-language death-hint TIMs with the language
 * letter at name char 5 (TIPS_E01 -> TIPS_G01 DE / R FR / S ES / T IT).
 * Rebind the US-canonical entries' sector/size to the chosen language's EUR
 * entry. Names and pathIdx in the ACTIVE table stay US-canonical —
 * g_FilePaths has 11 entries, EUR path indices must never land in it.
 * Idempotent and re-runnable (language 0 rebinds the EN values), so the
 * title-screen options menu can switch languages live. */
void Fs_ApplyLanguageRedirects(void)
{
    s32 lang       = (g_PcConfig.language >= 1 && g_PcConfig.language <= 4) ? g_PcConfig.language : 0;
    u32 mapPrefix  = FNP('M', 'A', 'P', ' ') & 0x3FFFF;               /* chars 0-2 */
    u32 mapSuffix  = FNP('_', 'S', ' ', ' ') & 0xFFF;                 /* chars 4-5 */
    u32 tipsName   = FNP('T', 'I', 'P', 'S');
    u32 tipsSuffix = FNP('_', 'E', ' ', ' ') & 0xFFF;                 /* chars 4-5 */
    static const u8 tipsLetter[5] = { 'E' - 0x20, 'G' - 0x20, 'R' - 0x20, 'S' - 0x20, 'T' - 0x20 };
    s32 i, j;

    if (g_GameRegion != Region_EUR)
    {
        return;
    }

    for (i = 0; i < FS_FILE_COUNT; i++)
    {
        const s_FileInfo* u = &s_FileTable_USA[i];
        u32 wantName4567;
        u8  wantPath;

        if (u->pathIdx == 9 &&
            (u->name0123 & 0x3FFFF) == mapPrefix && (u->name4567 & 0xFFF) == mapSuffix)
        {
            /* VIN/MAPx_Syz -> VINn/MAPx_S<lang>z: language digit at char 6. */
            wantName4567 = ((u32)u->name4567 & ~(0x3Fu << 12)) | ((u32)(0x10 + lang) << 12);
            wantPath     = (u8)(9 + lang);
        }
        else if (u->pathIdx == 8 &&
                 u->name0123 == tipsName && (u->name4567 & 0xFFF) == tipsSuffix)
        {
            /* TIM/TIPS_Exy -> TIM/TIPS_<letter>xy: language letter at char 5. */
            wantName4567 = ((u32)u->name4567 & ~(0x3Fu << 6)) | ((u32)tipsLetter[lang] << 6);
            wantPath     = 8;
        }
        else
        {
            continue;
        }

        for (j = 0; j < FS_FILE_COUNT_EUR; j++)
        {
            const s_FileInfo* e = &s_FileTable_EUR[j];
            if (e->name0123 == u->name0123 && e->name4567 == wantName4567 &&
                e->type == u->type && e->pathIdx == wantPath)
            {
                g_FileTable[i].startSector = e->startSector;
                g_FileTable[i].blockCount  = e->blockCount;
                break;
            }
        }
    }
}

/* Locate an EUR-only file (one with no US-canonical g_FileTable slot, e.g.
 * VIN/ITEM_GER.BIN) in the compiled EUR table. `name8` is the bare 8-char
 * name, no extension. Returns 1 and fills sector/blockCount on a hit. */
int Fs_EurFileLookup(const char* name8, s32 pathIdx, s32 type, u32* outSector, u32* outBlocks)
{
    s32 name0123;
    s32 name4567;
    s32 j;

    Fs_EncodeFileName(&name0123, &name4567, name8);

    for (j = 0; j < FS_FILE_COUNT_EUR; j++)
    {
        const s_FileInfo* e = &s_FileTable_EUR[j];
        if (e->name0123 == (u32)name0123 && e->name4567 == (u32)name4567 &&
            e->type == type && e->pathIdx == pathIdx)
        {
            *outSector = e->startSector;
            *outBlocks = e->blockCount;
            return 1;
        }
    }
    return 0;
}
#else
s_FileInfo g_FileTable[FS_FILE_COUNT] = {
#if VERSION_IS(USA)
    #include "filetable.c.USA.inc"
#elif VERSION_IS(JAP0)
    #include "filetable.c.JAP0.inc"
#elif VERSION_IS(JAP1) || VERSION_IS(JAP2)
    #include "filetable.c.JAP1.inc"
#endif
};
#endif

char* g_FilePaths[] = {
    "\\1ST\\",
    "\\ANIM\\",
    "\\BG\\",
    "\\CHARA\\",
    "\\ITEM\\",
    "\\MISC\\",
    "\\SND\\",
    "\\TEST\\",
    "\\TIM\\",
    "\\VIN\\",
    "\\XA\\"
};

char* g_FileExts[] = {
    ".TIM",
    ".VAB",
    ".BIN",
    ".DMS",
    ".ANM",
    ".PLM",
    ".IPD",
    ".ILM",
    ".TMD",
    ".DAT",
    ".KDT",
    ".CMP"
};

#ifndef SH_PC_PORT
u32 g_FileXaLoc[] = {
#if VERSION_IS(USA)
    0x00000,
    0x099BF,
    0x0A227,
    0x0B377,
    0x0D0BF,
    0x0EA57,
    0x0F997,
    0x1096F,
    0x16F07,
    0x19797,
    0x00000
#elif VERSION_IS(JAP0)
    0x00000,
    0x099C3,
    0x0A22B,
    0x0B37B,
    0x0D0C3,
    0x0EA5B,
    0x0F99B,
    0x10973,
    0x16F0B,
    0x1979B,
    0x00000
#elif VERSION_IS(JAP1) || VERSION_IS(JAP2)
    0x00000,
    0x099C4,
    0x0A22C,
    0x0B37C,
    0x0D0C4,
    0x0EA5C,
    0x0F99C,
    0x10974,
    0x16F0C,
    0x1979C,
    0x00000
#endif
};
#endif /* SH_PC_PORT */

void Fs_DecryptOverlay(s32* dst, const s32* src, s32 size)
{
    s32 i    = 0;
    s32 seed = 0;

    i = 0;
    while (i < (size >> 2))
    {
        seed = (seed + 0x01309125) * 0x03A452F7;
        *dst = *src ^ seed;

        i++;
        src++;
        dst++;
    }
}

s32 Fs_GetFileSize(s32 fileIdx)
{
    return g_FileTable[fileIdx].blockCount * FS_BLOCK_SIZE;
}

void Fs_GetFileName(char* outName, s32 fileIdx)
{
    Fs_GetFileInfoName(outName, &g_FileTable[fileIdx]);
}

void Fs_GetFileInfoName(char* outName, const s_FileInfo* const fileEntry)
{
    s32   i = 0;
    char  decoded;
    u32   namePart;
    char  fileType;
    char* fileExt;

    namePart = fileEntry->name0123;

    while (i < FS_NAME_CHAR_MAX)
    {
        if (i == NAME_PART_CHARS)
        {
            namePart = fileEntry->name4567;
        }

        decoded = namePart & NAME_CHAR_MASK;

        if (decoded == 0)
        {
            break;
        }

        outName[i] = decoded + NAME_CHAR_OFFSET;
        namePart >>= NAME_CHAR_BITS;
        i++;
    }

    fileType = fileEntry->type;

    if (fileType == FS_INVALID_TYPE)
    {
        outName[i] = '\0';
        return;
    }

    fileExt    = g_FileExts[fileType];
    outName[i] = *fileExt;

    while (*fileExt)
    {
        fileExt++;
        i++;
        outName[i] = *fileExt;
    }
}

void Fs_EncodeFileName(s32* outName0123, s32* outName4567, const char* srcName)
{
    s32 i;
    s32 currentShift;
    s32 srcChar;
    s32 encoded;
    s32 name0123;
    s32 name4567;

    name0123     = 0;
    name4567     = 0;
    currentShift = 0;
    for (i = 0; i < FS_NAME_CHAR_MAX; i++)
    {
        srcChar = srcName[i];

        if (i == NAME_PART_CHARS)
            currentShift = 0;

        if (srcChar == '\0' || srcChar == '.')
            break;

        encoded = (srcChar - NAME_CHAR_OFFSET) << currentShift;

        if (i < 4)
        {
            name0123 |= encoded;
        }
        else
        {
            name4567 |= encoded;
        }

        currentShift += NAME_CHAR_BITS;
    }

    *outName0123 = name0123;
    *outName4567 = name4567;
}

s32 Fs_GetFileSectorAlignedSize(s32 fileIdx)
{
    return ALIGN(g_FileTable[fileIdx].blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE);
}

s32 Fs_FindNextFileOfType(s32 fileType, s32 startIdx, s32 dir)
{
    s32 i;
    u32 currentIdx;
    s32 inc;

    inc = (dir < 0) ? -1 : 1;

    i = 0;
    currentIdx = startIdx + inc;
    while (i < FS_FILE_COUNT)
    {
        if (currentIdx >= FS_FILE_COUNT)
        {
            currentIdx = (dir < 0) ? (FS_FILE_COUNT - 1) : 0;
        }

        if (g_FileTable[currentIdx].type == fileType)
        {
            return currentIdx;
        }

        currentIdx += inc;
        i++;
    }

    return NO_VALUE;
}

s32 Fs_FindNextFile(const char* name, s32 fileType, s32 startIdx)
{
    s_FileInfo* fileEntry;
    s32         name0123;
    s32         name4567;
    s32         i        = startIdx;
    s32         foundIdx = NO_VALUE;

    Fs_EncodeFileName(&name0123, &name4567, name);

    fileEntry = &g_FileTable[i];
    while (i < FS_FILE_COUNT)
    {
        if (fileEntry->name4567 == name4567 &&
            fileEntry->name0123 == name0123 &&
            fileEntry->type    == fileType)
        {
            foundIdx = i;
            break;
        }

        i++;
        fileEntry++;
    }

    return foundIdx;
}
