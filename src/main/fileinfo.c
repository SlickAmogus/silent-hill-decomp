#include "main/fileinfo.h"

#ifdef SH_PC_PORT
#include <stdlib.h> /* malloc, free */
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
/* NTSC-J Rev 1/Rev 2 (SLPM-86192). Index-aligned with the USA table — all 2074
 * entries share name/path/type per index (verified against both discs) — so
 * unlike EUR it drops straight into the US-canonical shape. */
static const s_FileInfo s_FileTable_JAP[] = {
    #include "filetable.c.JAP1.inc"
};
#define FS_FILE_COUNT_EUR ((s32)(sizeof(s_FileTable_EUR) / sizeof(s_FileInfo)))

s_FileInfo   g_FileTable[FS_FILE_COUNT];
e_GameRegion g_GameRegion = Region_USA;

/* US pathIdx -> EUR pathIdx. EUR inserts VIN2..VIN5 (localized maps), so XA moves
 * from 10 to 14; every other path index is identical. */
static const u8 s_UsaToEurPath[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 14 };

/* EUR pathIdx -> bare directory name. An EUR pathIdx is NOT a g_FilePaths index
 * (that array has 11 entries and stops at XA); it must be resolved here. */
static const char* const s_EurFilePathNames[] = {
    "1ST", "ANIM", "BG", "CHARA", "ITEM", "MISC", "SND", "TEST",
    "TIM", "VIN", "VIN2", "VIN3", "VIN4", "VIN5", "XA"
};

/* The name/dir each US-canonical index ACTUALLY has on the mounted disc, held
 * in that disc's own pathIdx space. startSector/blockCount are deliberately
 * left zero — only the identity is meaningful here, g_FileTable owns location.
 * Diverges from g_FileTable only for the PAL language redirects below, so
 * Fs_GetRegionFilePath reports "same as canonical" for USA, JPN and English PAL. */
static s_FileInfo s_ActiveDiscName[FS_FILE_COUNT];

static void Fs_ResetActiveDiscNames(void)
{
    s32 i;

    for (i = 0; i < FS_FILE_COUNT; i++)
    {
        const s_FileInfo* g = &g_FileTable[i];

        s_ActiveDiscName[i].startSector = 0;
        s_ActiveDiscName[i].blockCount  = 0;
        s_ActiveDiscName[i].name0123    = g->name0123;
        s_ActiveDiscName[i].name4567    = g->name4567;
        s_ActiveDiscName[i].type        = g->type;
        s_ActiveDiscName[i].pathIdx =
            (g_GameRegion == Region_EUR &&
             g->pathIdx < (u32)(sizeof(s_UsaToEurPath) / sizeof(s_UsaToEurPath[0])))
                ? s_UsaToEurPath[g->pathIdx]
                : g->pathIdx;
    }
}

/* XA stream base sectors, runtime-selected. EUR[1] = 0x0B847 is the PAL HILL.
 * container start, mirroring US[1] = 0x099BF; JAP (Rev 1/2) is US shifted +5. */
static const u32 s_FileXaLoc_USA[] = {
    0x00000, 0x099BF, 0x0A227, 0x0B377, 0x0D0BF, 0x0EA57,
    0x0F997, 0x1096F, 0x16F07, 0x19797, 0x00000
};
static const u32 s_FileXaLoc_EUR[] = {
    0x00000, 0x0B847, 0x0C0AF, 0x0D1FF, 0x0EF47, 0x108DF,
    0x1181F, 0x127F7, 0x18D8F, 0x1B61F, 0x00000
};
static const u32 s_FileXaLoc_JAP[] = {
    0x00000, 0x099C4, 0x0A22C, 0x0B37C, 0x0D0C4, 0x0EA5C,
    0x0F99C, 0x10974, 0x16F0C, 0x1979C, 0x00000
};
u32 g_FileXaLoc[11];

/* Retained copy of a rearranged disc's file table (NULL for every stock disc).
 * A live language switch re-binds the VIN/TIPS slots from the BAKED EUR table,
 * which would silently undo Fs_RemapFromDiscTable for exactly those slots. */
static s_FileInfo* s_DiscTable      = NULL;
static s32         s_DiscTableCount = 0;

void Fs_InitFileTableForRegion(e_GameRegion region)
{
    s32 i, j;

    g_GameRegion = region;

    /* Back to the baked tables: a previously remapped disc no longer describes
     * what is in g_FileTable. */
    if (s_DiscTable != NULL)
    {
        free(s_DiscTable);
        s_DiscTable      = NULL;
        s_DiscTableCount = 0;
    }

    memcpy(g_FileXaLoc, (region == Region_EUR) ? s_FileXaLoc_EUR
                      : (region == Region_JPN) ? s_FileXaLoc_JAP
                                               : s_FileXaLoc_USA,
           sizeof(g_FileXaLoc));

    memcpy(g_FileTable, s_FileTable_USA, sizeof(s_FileTable_USA));
    if (region == Region_USA)
    {
        Fs_ResetActiveDiscNames();
        return;
    }

    if (region == Region_JPN)
    {
        /* Index-aligned with USA — sectors/sizes drop straight in. */
        memcpy(g_FileTable, s_FileTable_JAP, sizeof(s_FileTable_JAP));
        Fs_ResetActiveDiscNames();
    }
    else
    {
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
    }

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

/* The name + pathIdx a US-canonical slot carries ON A PAL DISC. Two things
 * differ there: EUR inserts VIN2..VIN5 into its path list (so XA is pathIdx 14,
 * not 10), and the config language rebinds the VIN map overlays / TIPS TIMs onto
 * differently-named files. Returns 1 when the slot is language-redirected.
 * Language 0 (English) yields the canonical name, so the result is always the
 * file the slot is currently bound to. */
static s32 Fs_EurDiscKey(const s_FileInfo* u, u32* outName4567, u32* outPath)
{
    s32             lang       = (g_PcConfig.language >= 1 && g_PcConfig.language <= 4) ? g_PcConfig.language : 0;
    u32             mapPrefix  = FNP('M', 'A', 'P', ' ') & 0x3FFFF; /* chars 0-2 */
    u32             mapSuffix  = FNP('_', 'S', ' ', ' ') & 0xFFF;   /* chars 4-5 */
    u32             tipsName   = FNP('T', 'I', 'P', 'S');
    u32             tipsSuffix = FNP('_', 'E', ' ', ' ') & 0xFFF;   /* chars 4-5 */
    static const u8 tipsLetter[5] = { 'E' - 0x20, 'G' - 0x20, 'R' - 0x20, 'S' - 0x20, 'T' - 0x20 };

    if (u->pathIdx == 9 &&
        (u->name0123 & 0x3FFFF) == mapPrefix && (u->name4567 & 0xFFF) == mapSuffix)
    {
        *outName4567 = ((u32)u->name4567 & ~(0x3Fu << 12)) | ((u32)(0x10 + lang) << 12);
        *outPath     = (u32)(9 + lang);
        return 1;
    }

    if (u->pathIdx == 8 && u->name0123 == tipsName && (u->name4567 & 0xFFF) == tipsSuffix)
    {
        *outName4567 = ((u32)u->name4567 & ~(0x3Fu << 6)) | ((u32)tipsLetter[lang] << 6);
        *outPath     = 8;
        return 1;
    }

    *outName4567 = u->name4567;
    *outPath     = (u->pathIdx < (u32)(sizeof(s_UsaToEurPath) / sizeof(s_UsaToEurPath[0])))
                       ? s_UsaToEurPath[u->pathIdx]
                       : u->pathIdx;
    return 0;
}

/* Take one slot's sector/size from a disc's own file table, matched on the name
 * and path THAT DISC uses. Returns 1 when the slot actually moved. */
static s32 Fs_BindSlotFromDisc(const s_FileInfo* disc, s32 count, s32 slot, u32 wantName4567, u32 wantPath)
{
    s_FileInfo* g = &g_FileTable[slot];
    s32         j;

    for (j = 0; j < count; j++)
    {
        const s_FileInfo* d = &disc[j];
        if (d->name0123 == g->name0123 && d->name4567 == wantName4567 &&
            d->type == g->type && d->pathIdx == wantPath)
        {
            if (g->startSector != d->startSector || g->blockCount != d->blockCount)
            {
                g->startSector = d->startSector;
                g->blockCount  = d->blockCount;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

/* Fan-disc sector correction (PC port). A fan re-translation (e.g. the Brazilian
 * patch) rebuilds the CD: every file keeps its name/type/path but moves to a new
 * sector, and the disc's own in-executable file table records the new sectors. We
 * baked the vanilla sectors, so read the mounted disc's own table and overwrite
 * each same-named entry's sector/size from it. On PAL the slot's name/path are
 * US-canonical while the disc's table is in EUR shape (and may be language-
 * redirected), so match through Fs_EurDiscKey rather than on raw equality. Files
 * with no disc match (pruned vanilla backups: B_KONAMI, *_SAFE*; the US-only
 * TIPS_J* on PAL) keep their baked sector. Returns the number of entries changed;
 * 0 means the disc matches the baked table (stock or in-place-patched), in which
 * case nothing — not even the audio/XA offset tables — is touched, so a
 * non-rearranged disc is never disturbed. */
s32 Fs_RemapFromDiscTable(const s_FileInfo* disc, s32 count)
{
    s32  i, j, changed = 0;
    u32* oldSector;

    if (disc == NULL || count <= 0)
        return 0;

    /* The audio/XA rebinds below map an OLD sector to its file, so the pre-remap
     * active sectors have to survive the overwrite. They are not the US ones
     * outside Region_USA. */
    oldSector = (u32*)malloc(sizeof(u32) * FS_FILE_COUNT);
    if (oldSector == NULL)
        return 0;

    for (i = 0; i < FS_FILE_COUNT; i++)
    {
        u32 wantName4567 = g_FileTable[i].name4567;
        u32 wantPath     = g_FileTable[i].pathIdx;

        oldSector[i] = g_FileTable[i].startSector;

        if (g_GameRegion == Region_EUR)
        {
            Fs_EurDiscKey(&s_FileTable_USA[i], &wantName4567, &wantPath);
        }

        changed += Fs_BindSlotFromDisc(disc, count, i, wantName4567, wantPath);
    }

    if (changed == 0)
    {
        free(oldSector);
        return 0;
    }

    if (s_DiscTable != NULL)
    {
        free(s_DiscTable);
        s_DiscTable      = NULL;
        s_DiscTableCount = 0;
    }
    s_DiscTable = (s_FileInfo*)malloc(sizeof(s_FileInfo) * (size_t)count);
    if (s_DiscTable != NULL)
    {
        memcpy(s_DiscTable, disc, sizeof(s_FileInfo) * (size_t)count);
        s_DiscTableCount = count;
    }

    /* Re-point the audio sector table and the XA container bases the same way
     * the EUR path does: old sector -> same file -> its new sector. */
    {
        extern s_AudioItemData g_AudioData[];
        s32 a;
        for (a = 0; a < AUDIO_DATA_COUNT; a++)
        {
            s32 old = g_AudioData[a].fileOffset_8;
            for (j = 0; j < FS_FILE_COUNT; j++)
            {
                if ((s32)oldSector[j] == old)
                {
                    g_AudioData[a].fileOffset_8 = (s32)g_FileTable[j].startSector;
                    break;
                }
            }
        }
    }
    for (i = 1; i < 10; i++) /* g_FileXaLoc[0] and [10] are 0 sentinels */
    {
        u32 base = g_FileXaLoc[i];
        if (base == 0)
            continue;
        for (j = 0; j < FS_FILE_COUNT; j++)
        {
            if (oldSector[j] == base)
            {
                g_FileXaLoc[i] = g_FileTable[j].startSector;
                break;
            }
        }
    }

    free(oldSector);
    return changed;
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

    /* Rebuilt from scratch on every pass so a live language switch can never
     * leave the previous language's disc name behind on an entry this pass
     * fails to match. */
    Fs_ResetActiveDiscNames();

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
                /* The entry keeps its US-canonical name in g_FileTable, so the
                 * localized file's REAL name is only recorded here — it is what
                 * anything addressing the file on disk by name has to use. */
                s_ActiveDiscName[i].name0123 = e->name0123;
                s_ActiveDiscName[i].name4567 = e->name4567;
                s_ActiveDiscName[i].pathIdx  = e->pathIdx;
                break;
            }
        }
    }

    /* The sectors just written are the BAKED (vanilla PAL) ones. On a rearranged
     * fan disc that is stale, so re-take the redirected slots from the disc's own
     * table. s_DiscTable is NULL for every stock disc, making this a no-op. */
    if (s_DiscTable != NULL)
    {
        for (i = 0; i < FS_FILE_COUNT; i++)
        {
            u32 discName4567;
            u32 discPath;

            if (Fs_EurDiscKey(&s_FileTable_USA[i], &discName4567, &discPath))
            {
                Fs_BindSlotFromDisc(s_DiscTable, s_DiscTableCount, i, discName4567, discPath);
            }
        }
    }
}

int Fs_GetRegionFilePath(s32 fileIdx, const char** outDir, char* outName)
{
    const s_FileInfo* disc;
    const s_FileInfo* canon;
    s_FileInfo        named;
    u32               canonPath;

    if (outDir == NULL || outName == NULL || fileIdx < 0 || fileIdx >= FS_FILE_COUNT)
    {
        return 0;
    }

    /* USA and JPN name their files exactly as the active table does (the JAP
     * table is name/path/type-identical to USA at every index), so there is
     * never a second name to try. */
    if (g_GameRegion != Region_EUR)
    {
        return 0;
    }

    disc      = &s_ActiveDiscName[fileIdx];
    canon     = &g_FileTable[fileIdx];
    canonPath = (canon->pathIdx < (u32)(sizeof(s_UsaToEurPath) / sizeof(s_UsaToEurPath[0])))
                    ? s_UsaToEurPath[canon->pathIdx]
                    : canon->pathIdx;

    if (disc->name0123 == canon->name0123 && disc->name4567 == canon->name4567 &&
        disc->pathIdx == canonPath)
    {
        return 0;
    }

    if (disc->pathIdx >= (u32)(sizeof(s_EurFilePathNames) / sizeof(s_EurFilePathNames[0])))
    {
        return 0;
    }

    named             = *disc;
    named.type        = canon->type;
    named.startSector = 0;
    named.blockCount  = 0;
    Fs_GetFileInfoName(outName, &named);

    *outDir = s_EurFilePathNames[disc->pathIdx];
    return 1;
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
