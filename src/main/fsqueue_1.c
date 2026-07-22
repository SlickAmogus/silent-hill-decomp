#include "gpu.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif
#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "main/fsqueue.h"
#include "main/fsmem.h"

#include <memory.h>
#include <psyq/libetc.h>
#include <psyq/libcd.h>

s_FsQueue g_FsQueue;

#ifdef SH_XBOX_PORT
/* Split the per-frame chunk-streaming cost ([POST] fs=) into disk-READ vs
 * REFORMAT (post-load offset fixups), read+reset each frame by game_main.c.
 * The exterior streams continuously while moving; this says which half to kill. */
unsigned long long g_FsReadCycles     = 0;
unsigned long long g_FsReformatCycles = 0;
unsigned           g_FsReadCount      = 0;
unsigned           g_FsReformatCount  = 0;
static inline unsigned long long ShxFsRdtsc(void)
{
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
#endif

bool Fs_QueueIsEntryLoaded(s32 queueIdx)
{
    return queueIdx < g_FsQueue.postLoad.idx;
}

s32 Fs_QueueGetLength(void)
{
    return (g_FsQueue.last.idx + 1) - g_FsQueue.postLoad.idx;
}

bool Fs_QueueChunksLoad(void)
{
    bool result;

    D_800C48F0 = true;

#ifdef SH_PC_PORT
    /* On PC, CD reads are synchronous — drain the queue HERE instead of
     * requiring it to already be empty. Waiters on this function (item
     * pickup AwaitLoad, BGM init, inventory entry) softlocked whenever
     * background streaming kept re-queuing reads each frame (hospital
     * basement-key report: interior texture churn meant the queue was
     * never empty at event-update time). After a bounded flush the queue
     * IS empty at this instant, which is all "chunks loaded" means with
     * synchronous reads. The Ipd_ChunkInitCheck gate stays bypassed (it
     * can report false while LM post-processing lags and would block the
     * pickup state machine permanently). */
    {
        s32 flushLimit = 500;
        while (Fs_QueueGetLength() > 0 && flushLimit-- > 0)
        {
            Fs_QueueUpdate();
        }
    }
    result = Fs_QueueGetLength() == 0;
#else
    result = false;
    if (Fs_QueueGetLength() == 0)
    {
        result = Ipd_ChunkInitCheck() != false;
    }
#endif

    return result;
}

void Fs_QueueWaitForEmpty(void)
{
    func_800892A4(0);
    func_80089128();

#ifdef SH_PC_PORT
    {
        extern int g_TickCount;
        int waitCount = 0;
#endif
    while (true)
    {
        VSync(SyncMode_Wait);
        if (Fs_QueueGetLength() <= 0)
        {
            break;
        }

#ifdef SH_PC_PORT
        waitCount++;
        if (waitCount > 500) {
            /* Force the queue empty to prevent infinite loop */
            extern FILE* g_ShDebugLog;
            SH_DBG("[FSQ] WaitForEmpty TIMEOUT after %d vsyncs — forcing queue empty (len=%d state=%d)",
                   waitCount, (int)Fs_QueueGetLength(), (int)g_FsQueue.state);
            if (g_ShDebugLog) fflush(g_ShDebugLog);
            g_FsQueue.read.idx = g_FsQueue.last.idx + 1;
            g_FsQueue.postLoad.idx = g_FsQueue.read.idx;
            break;
        }
#endif
        Fs_QueueUpdate();
    }

#ifdef SH_PC_PORT
    }
#endif
    func_800892A4(1);
    DrawSync(SyncMode_Wait);
    VSync(SyncMode_Wait);
}

s32 Fs_QueueStartSeek(e_FsFile fileIdx)
{
    return Fs_QueueEnqueue(fileIdx, FsQueueOp_Seek, FsQueuePostLoadType_None, false, NULL, 0, NULL);
}

s32 Fs_QueueStartRead(e_FsFile fileIdx, void* dest)
{
    return Fs_QueueEnqueue(fileIdx, FsQueueOp_Read, FsQueuePostLoadType_None, false, dest, 0, NULL);
}

s32 Fs_QueueStartReadTim(e_FsFile fileIdx, void* dest, const s_FsImageDesc* image)
{
    s_FsQueueExtra extra;

    if (image != NULL)
    {
        extra.image = *image;
    }
    else
    {
        extra.image.u     = NO_VALUE;
        extra.image.clutX = NO_VALUE;
        extra.image.v     = NO_VALUE;
        extra.image.clutY = NO_VALUE;
    }

    return Fs_QueueEnqueue(fileIdx, FsQueueOp_Read, FsQueuePostLoadType_Tim, false, dest, 0, &extra);
}

s32 Fs_QueueStartReadAnm(s32 idx, s32 charaId, void* dest, GsCOORDINATE2* coords)
{
    s32            fileIdx;
    s_FsQueueExtra extra;

    fileIdx             = CHARA_FILE_INFOS[charaId].animFileIdx;
    extra.anm.charaId = charaId;
    extra.anm.field_0   = idx;
    extra.anm.coords_8  = coords;
    return Fs_QueueEnqueue(fileIdx, FsQueueOp_Read, FsQueuePostLoadType_Anm, false, dest, 0, &extra);
}

s32 Fs_QueueEnqueue(e_FsFile fileIdx, u8 op, u8 postLoad, u8 alloc, void* data, u32 unused0, s_FsQueueExtra* extra)
{
    s_FsQueueEntry* newEntry;
    s_FsQueuePtr*   lastPtr;

#ifdef SH_PC_PORT
    /* Chokepoint guard: an out-of-range fileIdx makes info a wild pointer
     * ((u32)-1 gives the non-canonical &g_FileTable[0xFFFFFFFF] every
     * [FSQ-CORRUPT] log showed). Known producer fixed in func_800566B4
     * (missing material TIM); refuse any others and name them here. */
    if ((u32)fileIdx >= FS_FILE_COUNT)
    {
        SH_DBG("[FSQ] REJECT enqueue fileIdx=%d op=%d postLoad=%d data=%p",
               (int)fileIdx, (int)op, (int)postLoad, data);
        return g_FsQueue.last.idx;
    }
#endif

    // Wait for space in queue.
    while (Fs_QueueGetLength() >= FS_QUEUE_LENGTH)
    {
        Fs_QueueUpdate();
    }

    // This is the reason these pointers and indices are wrapped into structs.
    // If left as they are in the queue struct, this doesn't match unless manually addressed.
    lastPtr = &g_FsQueue.last;
    lastPtr->idx++;
    lastPtr->ptr = &g_FsQueue.entries[lastPtr->idx & (FS_QUEUE_LENGTH - 1)];

    newEntry               = g_FsQueue.last.ptr;
    newEntry->info         = &g_FileTable[fileIdx];
    newEntry->operation    = op;
    newEntry->postLoad     = postLoad;
    newEntry->allocate     = alloc;
    newEntry->externalData = data;
    newEntry->unused1      = unused0;

    if (extra != NULL)
    {
        newEntry->extra = *extra;
    }

    return g_FsQueue.last.idx;
}

void Fs_QueueInitialize(void)
{
    bzero(&g_FsQueue, sizeof(g_FsQueue));
    g_FsQueue.last.idx      = NO_VALUE;
    g_FsQueue.last.ptr      = &g_FsQueue.entries[FS_QUEUE_LENGTH - 1];
    g_FsQueue.read.idx      = 0;
    g_FsQueue.read.ptr      = g_FsQueue.entries;
    g_FsQueue.postLoad.idx  = 0;
    g_FsQueue.postLoad.ptr  = g_FsQueue.entries;
    g_FsQueue.state         = 0;
    g_FsQueue.postLoadState = 0;
    g_FsQueue.resetTimer0   = 0;
    g_FsQueue.resetTimer1   = 0;
    Fs_InitializeMem(FS_MEM_BASE, FS_MEM_SIZE);
}

void Fs_QueueReset(void)
{
    if (Fs_QueueGetLength() <= 0)
    {
        return;
    }

    if (g_FsQueue.read.idx <= g_FsQueue.last.idx)
    {
        g_FsQueue.read.idx = g_FsQueue.read.idx + FS_QUEUE_LENGTH;
        g_FsQueue.read.ptr = g_FsQueue.entries + (g_FsQueue.read.idx & (FS_QUEUE_LENGTH - 1));
        g_FsQueue.last     = g_FsQueue.read;
    }

    g_FsQueue.postLoad           = g_FsQueue.read;
    g_FsQueue.postLoadState      = FsQueuePostLoadState_Init;
    g_FsQueue.read.ptr->postLoad = FsQueuePostLoadType_None;
}

void Fs_QueueUpdate(void)
{
    s_FsQueuePtr*   read;
    s_FsQueueEntry* tick;
    s32             temp = 0;

    // Pending read/seek operations; tick them.
    tick = g_FsQueue.read.ptr;
    if (g_FsQueue.read.idx <= g_FsQueue.last.idx)
    {
#ifdef SH_PC_PORT
        /* Sewer progression crash (SewerCrash*.log): a queue entry's info
         * pointer was found holding 0xFFFFFFFFFFFFFFFF at Creeper spawn on
         * map5_s00 — something stomps the live entry between enqueue and
         * tick (corruptor unidentified; deterministic across users). info
         * is ALWAYS &g_FileTable[idx], so validate before dereferencing.
         * On violation: dump forensics, drop the entry, keep the queue
         * alive — a skipped enemy load beats a hard crash. */
        {
            const s_FileInfo* lo = &g_FileTable[0];
            const s_FileInfo* hi = &g_FileTable[FS_FILE_COUNT];
            if (tick->info < lo || tick->info >= hi)
            {
                extern void FsQueue_DumpEntryHex(const char* tag, int idx, const s_FsQueueEntry* e);
                SH_DBG("[FSQ-CORRUPT] entry idx=%d info=%p op=%d postLoad=%d alloc=%d data=%p ext=%p — dropping",
                       g_FsQueue.read.idx, (void*)tick->info, (int)tick->operation,
                       (int)tick->postLoad, (int)tick->allocate,
                       (void*)tick->data, (void*)tick->externalData);
                FsQueue_DumpEntryHex("read-cursor", g_FsQueue.read.idx, tick);
                g_FsQueue.state       = 0;
                g_FsQueue.resetTimer0 = 0;
                g_FsQueue.resetTimer1 = 0;
                temp                  = ++g_FsQueue.read.idx;
                g_FsQueue.read.ptr    = g_FsQueue.entries + (temp & (FS_QUEUE_LENGTH - 1));
                return;
            }
        }
#endif
        switch (tick->operation)
        {
            case FsQueueOp_Seek:
                temp = Fs_QueueUpdateSeek(tick);
                break;

            case FsQueueOp_Read:
#ifdef SH_XBOX_PORT
                { unsigned long long _t = ShxFsRdtsc();
                  temp = Fs_QueueUpdateRead(tick);
                  g_FsReadCycles += ShxFsRdtsc() - _t; g_FsReadCount++; }
#else
                temp = Fs_QueueUpdateRead(tick);
#endif
                break;
        }

        // Seek or read done, proceed to next one.
        // Alias and `temp` use seem to be required for match for some reason, might be an inline?
        if (temp == 1)
        {
            read                  = &g_FsQueue.read;
            g_FsQueue.state       = 0; // `FsQueueReadState_Allocate` or `FSQS_SEEK_SETLOC`.
            g_FsQueue.resetTimer0 = 0;
            g_FsQueue.resetTimer1 = 0;
            temp                  = ++read->idx;
            read->ptr             = g_FsQueue.entries + (temp & (FS_QUEUE_LENGTH - 1));
        }
    }
    // Nothing to read.
    else
    {
        g_FsQueue.state = 0; // `FsQueueReadState_Allocate` or `FSQS_SEEK_SETLOC`.
    }

    // Preparations to post-load in queue; tick them.
    tick = g_FsQueue.postLoad.ptr;
    if (g_FsQueue.postLoad.idx < g_FsQueue.read.idx)
    {
#ifdef SH_PC_PORT
        /* Same corruption guard for the post-load cursor (see above). */
        if (tick->info < &g_FileTable[0] || tick->info >= &g_FileTable[FS_FILE_COUNT])
        {
            extern void FsQueue_DumpEntryHex(const char* tag, int idx, const s_FsQueueEntry* e);
            SH_DBG("[FSQ-CORRUPT] postload idx=%d info=%p — skipping", g_FsQueue.postLoad.idx, (void*)tick->info);
            FsQueue_DumpEntryHex("postload-cursor", g_FsQueue.postLoad.idx, tick);
            g_FsQueue.postLoadState = FsQueuePostLoadState_Init;
            temp                    = ++g_FsQueue.postLoad.idx;
            g_FsQueue.postLoad.ptr  = g_FsQueue.entries + (temp & (FS_QUEUE_LENGTH - 1));
            return;
        }
#endif
#ifdef SH_XBOX_PORT
        { unsigned long long _t = ShxFsRdtsc();
          temp = Fs_QueueUpdatePostLoad(tick);
          g_FsReformatCycles += ShxFsRdtsc() - _t; g_FsReformatCount++; }
#else
        temp = Fs_QueueUpdatePostLoad(tick);
#endif
        if (temp == true)
        {
            g_FsQueue.postLoadState = FsQueuePostLoadState_Init;
            temp                    = ++g_FsQueue.postLoad.idx;
            g_FsQueue.postLoad.ptr  = g_FsQueue.entries + (temp & (FS_QUEUE_LENGTH - 1));
        }
    }
    // Nothing to post-load.
    else
    {
        g_FsQueue.postLoadState = FsQueuePostLoadState_Init;
    }
}

bool Fs_QueueUpdateSeek(s_FsQueueEntry* entry)
{
    bool result = false;
    s32  state  = g_FsQueue.state;

#ifdef SH_PC_PORT
    /* No CD drive on PC — seeks are meaningless. Complete immediately so
     * subsequent reads in the queue are not blocked. */
    (void)entry;
    (void)state;
    return true;
#endif

    switch (state)
    {
        case FsQueueSeekState_SetLoc:
            switch (Fs_QueueTickSetLoc(entry))
            {
                // CdlSetloc failed, reset and retry.
                case false:
                    g_FsQueue.state = FsQueueSeekState_Reset;
                    break;

                case true:
                    g_FsQueue.state = FsQueueSeekState_SeekL;
                    break;
            }
            break;

        case FsQueueSeekState_SeekL:
            switch (CdControl(CdlSeekL, NULL, NULL))
            {
                // `CdlSeekL` failed, reset and retry.
                case 0:
                    g_FsQueue.state = FsQueueSeekState_Reset;
                    break;

                case 1:
                    g_FsQueue.state = FsQueueSeekState_Sync;
                    break;
            }
            break;

        case FsQueueSeekState_Sync:
            switch (CdSync(1, NULL))
            {
                // Keep waiting, operation in progress.
                case CdlNoIntr:
                    break;

                // Done seeking.
                case CdlComplete:
                    result = true;
                    break;

                // Disk error; reset and retry.
                case CdlDiskError:
                    g_FsQueue.state = FsQueueSeekState_Reset;
                    break;

                // Inknown error, reset and retry.
                default:
                    g_FsQueue.state = FsQueueSeekState_Reset;
                    break;
            }
            break;

        case FsQueueSeekState_Reset:
            switch (Fs_QueueResetTick(entry))
            {
                // Still resetting.
                case 0:
                    break;

                // Reset done, retry from beginning.
                case 1:
                    g_FsQueue.state = FsQueueSeekState_SetLoc;
                    break;
            }
            break;
    }

    return result;
}
