
#include "main/fsqueue.h"
#include "main/fsmem.h"
#include "main/fileinfo.h"
#include "bodyprog/bodyprog.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include <string.h>
#include "pc_config.h"
#endif

#include <psyq/libapi.h>
#include <psyq/libcd.h>
#include <psyq/libgte.h>
#include <psyq/libgpu.h>
#include <psyq/string.h>
#include <psyq/sys/file.h>

bool Fs_QueueAllocEntryData(s_FsQueueEntry* entry)
{
    bool result = false;

    if (entry->allocate)
    {
        entry->data = Fs_AllocMem(ALIGN(entry->info->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE));
    }
    else
    {
        entry->data = entry->externalData;
    }

    if (entry->data != 0)
    {
        result = true;
    }
    return result;
}

bool Fs_QueueCanRead(s_FsQueueEntry* entry)
{
    s_FsQueueEntry* other;
    s32             queueLength;
    s32             overlap;
    s32             i;

    queueLength = g_FsQueue.read.idx - g_FsQueue.postLoad.idx;

    for (i = 0; i < queueLength; i++)
    {
        other   = &g_FsQueue.entries[(g_FsQueue.postLoad.idx + i) & (FS_QUEUE_LENGTH - 1)];
        overlap = false;
        if (other->postLoad || other->allocate)
        {
            overlap = Fs_QueueDoBuffersOverlap(entry->data,
                                               ALIGN(entry->info->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE),
                                               other->data,
                                               other->info->blockCount * FS_BLOCK_SIZE);
        }

        if (overlap == true)
        {
            return false;
        }
    }

    return true;
}

bool Fs_QueueDoBuffersOverlap(u8* data0, u32 size0, u8* data1, u32 size1)
{
#ifdef SH_PC_PORT
    /* On 64-bit, use full pointer comparison instead of PSX 24-bit masking */
    uintptr_t d0 = (uintptr_t)data0;
    uintptr_t d1 = (uintptr_t)data1;
    if ((d1 >= d0 + size0) || (d0 >= d1 + size1))
    {
        return false;
    }
    return true;
#else
    u32 data0Low = (u32)data0 & 0xFFFFFF;
    u32 data1Low = (u32)data1 & 0xFFFFFF;
    if ((data1Low >= data0Low + size0) || (data0Low >= data1Low + size1))
    {
        return false;
    }

    return true;
#endif
}

bool Fs_QueueTickSetLoc(s_FsQueueEntry* entry)
{
    CdlLOC cdloc;
    CdIntToPos(entry->info->startSector, &cdloc);
#ifdef SH_PC_PORT
    /* PsyCross CdControl returns 0 for CdlSetloc even on success.
     * Call it for the side effect (seeking the file), then return true. */
    {
        static int setlocLog = 0;
        if (setlocLog < 10) {
            printf("[SH] Fs_QueueTickSetLoc: startSector=%d cdloc=(%02x:%02x:%02x)\n",
                entry->info->startSector,
                cdloc.minute, cdloc.second, cdloc.sector);
            setlocLog++;
        }
    }
    CdControl(CdlSetloc, (u_char*)&cdloc, NULL);
    return true;
#else
    return CdControl(CdlSetloc, (u_char*)&cdloc, NULL);
#endif
}

bool Fs_QueueTickRead(s_FsQueueEntry* entry)
{
    s32 sectorCount;

    // Round up to sector boundary. Masking not needed because of `>> 11` below.
    sectorCount = ((entry->info->blockCount * FS_BLOCK_SIZE) + FS_SECTOR_SIZE) - 1;

    // Overflow check?
    if (sectorCount < 0)
    {
        sectorCount += FS_SECTOR_SIZE - 1;
    }

#ifdef SH_PC_PORT
    /* Loose-file override: when allow_loose_files is enabled, probe
     * gamedata/load/{FOLDER}/{NAME}{EXT} on disk. If present, slurp it
     * directly into the queue entry's buffer and skip the CD read. This
     * is how the texture-mod pipeline replaces individual TIM/TMD/etc.
     * files without rebuilding the .bin image.
     *
     * One fopen("rb") per asset load. If the file isn't there the open
     * fails immediately and we fall through to the original CdRead path.
     * No stat / no caching — the OS handles directory caching for us. */
    if (g_PcConfig.allowLooseFiles && entry->info != NULL)
    {
        s_FileInfo* file = entry->info;
        const char* folder = g_FilePaths[file->pathIdx]; /* e.g. "\\BG\\" */
        char strippedFolder[16];
        char nameBuf[32];
        char loosePath[160];
        size_t fi = 0;
        size_t fl;
        FILE* lf;

        /* Strip leading/trailing backslashes from g_FilePaths entry. */
        if (folder[0] == '\\') folder++;
        fl = strlen(folder);
        while (fl > 0 && folder[fl - 1] == '\\') fl--;
        if (fl >= sizeof(strippedFolder)) fl = sizeof(strippedFolder) - 1;
        for (fi = 0; fi < fl; fi++) strippedFolder[fi] = folder[fi];
        strippedFolder[fl] = '\0';

        Fs_GetFileInfoName(nameBuf, file);

        /* Use forward slashes — fopen on mingw accepts them. */
        snprintf(loosePath, sizeof(loosePath), "gamedata/load/%s/%s",
                 strippedFolder, nameBuf);

        lf = fopen(loosePath, "rb");
        if (lf != NULL)
        {
            size_t bufSize = (size_t)ALIGN(file->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE);
            size_t got = fread(entry->data, 1, bufSize, lf);
            fclose(lf);
            {
                static int looseLog = 0;
                if (looseLog < 32)
                {
                    fprintf(stderr, "[LOOSE] %s -> %u/%u bytes\n",
                            loosePath, (unsigned)got, (unsigned)bufSize);
                    looseLog++;
                }
            }
            (void)got;
            return true;
        }
    }
#endif

    {
        int result;
#ifdef SH_PC_PORT
        static int readLogCount = 0;
#endif
        result = CdRead(sectorCount >> FS_SECTOR_SHIFT, (u32*)entry->data, CdlModeSpeed);
        return result;
    }
}

bool Fs_QueueResetTick(s_FsQueueEntry* entry)
{
    bool result;

    result = false;

    g_FsQueue.resetTimer0++;

    if (g_FsQueue.resetTimer0 >= 8)
    {
        result                = true;
        g_FsQueue.resetTimer0 = 0;
        g_FsQueue.resetTimer1++;

        if (g_FsQueue.resetTimer1 >= 9)
        {
            if (CdReset(0) == 1)
            {
                g_FsQueue.resetTimer1 = 0;
            }
            else
            {
                result = false;
            }
        }
    }

    return result;
}

bool Fs_QueueTickReadPcDrv(s_FsQueueEntry* entry)
{
    s32         handle;
    s32         temp;
    s32         retry;
    bool        result;
    s_FileInfo* file = entry->info;
    char        pathBuf[64];
    char        nameBuf[32];

    result = false;

    strcpy(pathBuf, "sim:.\\DATA");
    strcat(pathBuf, g_FilePaths[file->pathIdx]);
    Fs_GetFileInfoName(nameBuf, file);
    strcat(pathBuf, nameBuf);

    for (retry = 0; retry <= 2; retry++)
    {
        handle = open(pathBuf, O_NOBUF | O_RDONLY);
        if (handle == NO_VALUE)
        {
            continue;
        }

        temp = read(handle,entry->data, ALIGN(file->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE));
        if (temp == NO_VALUE)
        {
            continue;
        }

        do
        {
            temp = close(handle);
        }
        while (temp == NO_VALUE);

        result = true;
        break;
    }

    return result;
}

bool Fs_QueueUpdatePostLoad(s_FsQueueEntry* entry)
{
    bool result;
    s32  state;
    u8   postLoad;

    result = false;
    state  = g_FsQueue.postLoadState;

    switch (state)
    {
        case FsQueuePostLoadState_Init:
            if (entry->allocate)
            {
                g_FsQueue.postLoadState = FsQueuePostLoadState_Skip;
            }
            else
            {
                g_FsQueue.postLoadState = FsQueuePostLoadState_Exec;
            }
            break;

        // Do nothing.
        case FsQueuePostLoadState_Skip:
            break;

        case FsQueuePostLoadState_Exec:
            postLoad = entry->postLoad;

            switch (postLoad)
            {
                case FsQueuePostLoadType_None:
                    result = true;
                    break;

                case FsQueuePostLoadType_Tim:
                    result = Fs_QueuePostLoadTim(entry);
                    break;

                case FsQueuePostLoadType_Anm:
                    result = Fs_QueuePostLoadAnm(entry);
                    break;

                default:
                    break;
            }
            break;

        default:
            break;
    }

    return result;
}

bool Fs_QueuePostLoadTim(s_FsQueueEntry* entry)
{
    TIM_IMAGE tim;
    RECT      tempRect;

#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] PostLoadTim entry: externalData=%p img.u=%u img.v=%u tPage=%u,%u clutX=%d clutY=%d\n",
        entry->externalData, (unsigned)entry->extra.image.u, (unsigned)entry->extra.image.v,
        (unsigned)entry->extra.image.tPage[0], (unsigned)entry->extra.image.tPage[1],
        (int)entry->extra.image.clutX, (int)entry->extra.image.clutY); fflush(g_ShDebugLog); } }
#endif
    OpenTIM((u64*)entry->externalData);
    ReadTIM(&tim);
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] post ReadTIM: prect=%p caddr=%p paddr=%p mode=%u\n",
        (void*)tim.prect, (void*)tim.caddr, (void*)tim.paddr, (unsigned)tim.mode); fflush(g_ShDebugLog); } }
#endif

    tempRect = *tim.prect;
    if (entry->extra.image.u != 0xFF)
    {
        // This contraption simply extracts XY from tPage value.
        // For some reason it seems to be byte swapped, or maybe tPage is stored as u8[2]?
        // Same as `tempRect.x = (entry->extra.image.tPage & 0x0F) * 64` for normal tPage.
        tempRect.x = entry->extra.image.u + ((entry->extra.image.tPage[1] & 0xF) << 6);

        // Same as `tempRect.y = (entry->extra.image.tPage & 0x10) * 16` for normal tPage.
        tempRect.y = entry->extra.image.v + ((entry->extra.image.tPage[1] << 4) & 0x100);
    }
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] pre pixel LoadImage rect=(%d,%d %dx%d)\n",
        (int)tempRect.x, (int)tempRect.y, (int)tempRect.w, (int)tempRect.h); fflush(g_ShDebugLog); } }
#endif

    LoadImage(&tempRect, tim.paddr);

    if (tim.caddr != NULL)
    {
        tempRect = *tim.crect;
        if (entry->extra.image.clutX != NO_VALUE)
        {
            tempRect.x = entry->extra.image.clutX;
            tempRect.y = entry->extra.image.clutY;
        }
#ifdef SH_PC_PORT
        { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] pre CLUT LoadImage rect=(%d,%d %dx%d)\n",
            (int)tempRect.x, (int)tempRect.y, (int)tempRect.w, (int)tempRect.h); fflush(g_ShDebugLog); } }
#endif

        LoadImage(&tempRect, tim.caddr);
    }
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] PostLoadTim done\n"); fflush(g_ShDebugLog); } }
#endif

    return true;
}

bool Fs_QueuePostLoadAnm(s_FsQueueEntry* entry)
{
    Fs_CharaAnimInfoUpdate(entry->extra.anm.field_0, entry->extra.anm.charaId_4, entry->externalData, entry->extra.anm.coords_8);
    return true;
}
