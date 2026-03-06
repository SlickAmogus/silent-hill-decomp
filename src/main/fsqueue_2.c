#include "main/fsqueue.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#endif

#include <psyq/libcd.h>

bool Fs_QueueUpdateRead(s_FsQueueEntry* entry)
{
    bool result;

    result = false;
#ifdef SH_PC_PORT
    {
        static int prevState = -1;
        if (g_FsQueue.state == 0 && prevState != 0) {
            printf("[SH] FsRead NEW: data=%p sector=%d blocks=%d alloc=%d\n",
                (void*)entry->data,
                entry->info->startSector_0_0,
                entry->info->blockCount_0_19,
                entry->allocate);
            fflush(stdout);
        }
        prevState = g_FsQueue.state;
    }
#endif
    switch (g_FsQueue.state)
    {
        case FsQueueReadState_Allocate:
            switch (Fs_QueueAllocEntryData(entry))
            {
                // Retry until memory is allocated?
                case 0:
                    break;

                case 1:
                    g_FsQueue.state = FsQueueReadState_Check;
                    break;
            }

            break;

        case FsQueueReadState_Check:
            switch (Fs_QueueCanRead(entry))
            {
                // Can't read yet, memory in use by another operation. Wait until next tick.
                case 0:
                    break;

                case 1:
                    g_FsQueue.state = FsQueueReadState_SetLoc;
                    break;
            }

            break;

        case FsQueueReadState_SetLoc:
            switch (Fs_QueueTickSetLoc(entry))
            {
                // `CdlSetloc` failed, reset CD.
                case false:
                    g_FsQueue.state = FsQueueReadState_Reset;
                    break;

                case true:
                    g_FsQueue.state = FsQueueReadState_Read;
                    break;
            }

            break;

        case FsQueueReadState_Read:
            switch (Fs_QueueTickRead(entry))
            {
                // `CdRead` failed, reset CD and retry.
                case 0:
                    g_FsQueue.state = FsQueueReadState_Reset;
                    break;

                case 1:
                    g_FsQueue.state = FsQueueReadState_Sync;
                    break;
            }

            break;

        // Check how read is going.
        case FsQueueReadState_Sync:
        {
#ifdef SH_PC_PORT
            int syncResult = CdReadSync(1, NULL);
            {
                static int syncDbg = 0;
                if (syncDbg < 200) {
                    printf("[SH] CdReadSync result=%d\n", syncResult);
                    syncDbg++;
                }
            }
            switch (syncResult)
#else
            switch (CdReadSync(1, NULL))
#endif
            {
                // `CdReadSync` failed, reset CD.
                case NO_VALUE:
                    g_FsQueue.state = FsQueueReadState_Reset;
                    break;

                // Done reading and no state transition, let caller know that it's done.
                case 0:
#ifdef SH_PC_PORT
                    {
                        static int doneDbg = 0;
                        if (doneDbg < 5) {
                            u8* p = (u8*)entry->data;
                            printf("[SH] Read done! data[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                            doneDbg++;
                        }
                    }
#endif
                    result = true;
                    break;
            }

            break;
        }

        case FsQueueReadState_Reset:
            switch (Fs_QueueResetTick(entry))
            {
                // Still resetting.
                case 0:
                    break;

                // Reset done, retry from `setloc`.
                case 1:
                    g_FsQueue.state = FsQueueReadState_SetLoc;
                    break;
            }

            break;
    }

    return result;
}
