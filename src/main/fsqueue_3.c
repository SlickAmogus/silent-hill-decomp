
#include "main/fsqueue.h"
#include "main/fsmem.h"
#include "bodyprog/bodyprog.h"
#ifdef SH_PC_PORT
#include <stdio.h>
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
        entry->data = Fs_AllocMem(ALIGN(entry->info->blockCount_0_19 * FS_BLOCK_SIZE, FS_SECTOR_SIZE));
    }
    else
    {
        entry->data = entry->externalData;
    }

#ifdef SH_PC_PORT
    {
        fprintf(stderr, "[SH] FsAlloc: alloc=%d data=%p extData=%p blocks=%d\n",
            entry->allocate, (void*)entry->data, (void*)entry->externalData,
            entry->info->blockCount_0_19);
    }
#endif
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
                                               ALIGN(entry->info->blockCount_0_19 * FS_BLOCK_SIZE, FS_SECTOR_SIZE),
                                               other->data,
                                               other->info->blockCount_0_19 * FS_BLOCK_SIZE);
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
    CdIntToPos(entry->info->startSector_0_0, &cdloc);
#ifdef SH_PC_PORT
    /* PsyCross CdControl returns 0 for CdlSetloc even on success.
     * Call it for the side effect (seeking the file), then return true. */
    {
        static int setlocLog = 0;
        if (setlocLog < 10) {
            printf("[SH] Fs_QueueTickSetLoc: startSector=%d cdloc=(%02x:%02x:%02x)\n",
                entry->info->startSector_0_0,
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
    sectorCount = ((entry->info->blockCount_0_19 * FS_BLOCK_SIZE) + FS_SECTOR_SIZE) - 1;

    // Overflow check?
    if (sectorCount < 0)
    {
        sectorCount += FS_SECTOR_SIZE - 1;
    }

    {
        int result;
#ifdef SH_PC_PORT
        static int readLogCount = 0;
#endif
        result = CdRead(sectorCount >> FS_SECTOR_SHIFT, (u32*)entry->data, CdlModeSpeed);
#ifdef SH_PC_PORT
        printf("[SH] CdRead: sectors=%d data=%p mode=%d result=%d\n",
            sectorCount >> FS_SECTOR_SHIFT, (void*)entry->data, CdlModeSpeed, result);
        fflush(stdout);
#endif
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
    strcat(pathBuf, g_FilePaths[file->pathIdx_4_0]);
    Fs_GetFileInfoName(nameBuf, file);
    strcat(pathBuf, nameBuf);

    for (retry = 0; retry <= 2; retry++)
    {
        handle = open(pathBuf, O_NOBUF | O_RDONLY);
        if (handle == NO_VALUE)
        {
            continue;
        }

        temp = read(handle,entry->data, ALIGN(file->blockCount_0_19 * FS_BLOCK_SIZE, FS_SECTOR_SIZE));
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
    printf("[SH] Fs_QueuePostLoadTim: extData=%p data=%p\n",
        (void*)entry->externalData, (void*)entry->data);
    {
        u8* p = (u8*)entry->externalData;
        printf("[SH]   TIM header bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
#endif
    OpenTIM((u64*)entry->externalData);
    ReadTIM(&tim);
#ifdef SH_PC_PORT
    printf("[SH]   TIM parsed: prect=%p paddr=%p crect=%p caddr=%p mode=0x%x\n",
        (void*)tim.prect, (void*)tim.paddr, (void*)tim.crect, (void*)tim.caddr, tim.mode);
    if (tim.prect) {
        printf("[SH]   prect: x=%d y=%d w=%d h=%d\n",
            tim.prect->x, tim.prect->y, tim.prect->w, tim.prect->h);
    }
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

    LoadImage(&tempRect, tim.paddr);

    if (tim.caddr != NULL)
    {
        tempRect = *tim.crect;
        if (entry->extra.image.clutX != NO_VALUE)
        {
            tempRect.x = entry->extra.image.clutX;
            tempRect.y = entry->extra.image.clutY;
        }

        LoadImage(&tempRect, tim.caddr);
    }

    return true;
}

bool Fs_QueuePostLoadAnm(s_FsQueueEntry* entry)
{
    Fs_CharaAnimInfoUpdate(entry->extra.anm.field_0, entry->extra.anm.charaId_4, entry->externalData, entry->extra.anm.coords_8);
    return true;
}
