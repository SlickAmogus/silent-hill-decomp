#include "main/fsqueue.h"
#ifdef SH_PC_PORT
#include <stdio.h>
extern int g_FsLooseReadComplete; /* fsqueue_3.c — set when a loose read fully populated entry->data */
#endif

#include <psyq/libcd.h>

bool Fs_QueueUpdateRead(s_FsQueueEntry* entry)
{
    bool result;

    result = false;
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
#ifdef SH_PC_PORT
                    /* A byte-replace loose read already has the whole file in
                     * entry->data and issued no CdRead — there is no command
                     * for CdReadSync to complete, so entering Sync would return
                     * NO_VALUE and spin in Reset forever. Report done now; the
                     * caller resets state and advances (post-load runs off its
                     * own cursor). */
                    if (g_FsLooseReadComplete)
                    {
                        g_FsLooseReadComplete = 0;
                        result = true;
                        break;
                    }
#endif
                    g_FsQueue.state = FsQueueReadState_Sync;
                    break;
            }

            break;

        // Check how read is going.
        case FsQueueReadState_Sync:
        {
            switch (CdReadSync(1, NULL))
            {
                // `CdReadSync` failed, reset CD.
                case NO_VALUE:
                    g_FsQueue.state = FsQueueReadState_Reset;
                    break;

                // Done reading and no state transition, let caller know that it's done.
                case 0:
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
