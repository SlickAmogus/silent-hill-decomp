#include "main/fileinfo.h"

#ifdef SH_PC_PORT
#include <string.h> /* memcpy */
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
