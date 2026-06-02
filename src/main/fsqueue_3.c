
#include "main/fsqueue.h"
#include "main/fsmem.h"
#include "main/fileinfo.h"
#include "bodyprog/bodyprog.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pc_config.h"
#include "hires_override.h"
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

#ifdef SH_PC_PORT
/* Hi-res override pending table.
 * When Fs_QueueTickRead detects a loose file LARGER than the disc-image
 * buffer slot, we cannot slurp it into entry->data without overflowing.
 * Instead, stash the loose file's path here keyed by entry pointer; the
 * disc CdRead happens normally so VRAM still receives the original native
 * TIM. Then Fs_QueuePostLoadTim — which has already parsed the disc TIM
 * and computed the engine's target VRAM rect — uses that path to register
 * a hi-res GL texture override. */
#define HIRES_PENDING_MAX 32
typedef struct {
    s_FsQueueEntry* entry;
    char path[160];
} HiresPending;
static HiresPending s_hiresPending[HIRES_PENDING_MAX];

static void HiresPending_Stash(s_FsQueueEntry* entry, const char* path)
{
    int free = -1;
    for (int i = 0; i < HIRES_PENDING_MAX; i++)
    {
        if (s_hiresPending[i].entry == entry) { free = i; break; }
        if (s_hiresPending[i].entry == NULL && free < 0) free = i;
    }
    if (free < 0)
    {
        fprintf(stderr, "[HIRES] pending table full, dropping %s\n", path);
        return;
    }
    s_hiresPending[free].entry = entry;
    strncpy(s_hiresPending[free].path, path,
            sizeof(s_hiresPending[free].path) - 1);
    s_hiresPending[free].path[sizeof(s_hiresPending[free].path) - 1] = '\0';
}

static const char* HiresPending_PopPath(s_FsQueueEntry* entry)
{
    static char buf[160];
    for (int i = 0; i < HIRES_PENDING_MAX; i++)
    {
        if (s_hiresPending[i].entry == entry)
        {
            strncpy(buf, s_hiresPending[i].path, sizeof(buf));
            buf[sizeof(buf) - 1] = '\0';
            s_hiresPending[i].entry = NULL;
            s_hiresPending[i].path[0] = '\0';
            return buf;
        }
    }
    return NULL;
}
#endif

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
     * No stat / no caching — the OS handles directory caching for us.
     *
     * Logging:
     *   [LOOSE/INIT]  one-shot config status
     *   [LOOSE]       hit, byte-replace
     *   [LOOSE/HIRES] hit, deferred to PostLoadTim for hi-res override
     *   [LOOSE/MISS]  fopen failed (gated, capped at 32 entries)
     *   [LOOSE/WARN]  unexpected: short read, fseek failure, etc.
     *   [LOOSE/SUMMARY] periodic: hits/misses/hires/warnings counts
     * Set env var SH_LOOSE_VERBOSE=1 to log every miss (useful when
     * debugging which exact paths the engine is probing). */
    {
        static int s_looseInitLogged = 0;
        if (!s_looseInitLogged)
        {
            s_looseInitLogged = 1;
            const char* verb = getenv("SH_LOOSE_VERBOSE");
            fprintf(stderr,
                "[LOOSE/INIT] allow_loose_files=%d  base=gamedata/load/  verbose=%s\n",
                g_PcConfig.allowLooseFiles,
                (verb && verb[0] && verb[0] != '0') ? "yes" : "no");
        }
    }
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

        static int s_hits = 0;
        static int s_misses = 0;
        static int s_hires = 0;
        static int s_warns = 0;

        lf = fopen(loosePath, "rb");
        if (lf != NULL)
        {
            size_t bufSize = (size_t)ALIGN(file->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE);
            /* Probe loose file size. If it overflows the disc-image buffer
             * slot, we cannot byte-replace; treat it as a hi-res TIM and
             * defer to PostLoadTim. */
            long fileSize = 0;
            int seekFailed = 0;
            if (fseek(lf, 0, SEEK_END) == 0)
            {
                fileSize = ftell(lf);
                if (fseek(lf, 0, SEEK_SET) != 0) seekFailed = 1;
            }
            else
            {
                seekFailed = 1;
            }
            if (seekFailed)
            {
                s_warns++;
                fprintf(stderr, "[LOOSE/WARN] %s: fseek/ftell failed (errno=%d %s)\n",
                        loosePath, errno, strerror(errno));
            }

            if (fileSize > 0 && (size_t)fileSize > bufSize)
            {
                fclose(lf);
                HiresPending_Stash(entry, loosePath);
                s_hires++;
                if (s_hires <= 64)
                {
                    fprintf(stderr, "[LOOSE/HIRES] %s (%ld bytes) > buf %u; deferring to PostLoadTim for hi-res override\n",
                            loosePath, fileSize, (unsigned)bufSize);
                }
                /* Fall through to CdRead so the disc TIM populates entry->data
                 * for native VRAM upload (hi-res override is registered later
                 * with the engine's chosen target rect). */
            }
            else
            {
                size_t got = fread(entry->data, 1, bufSize, lf);
                fclose(lf);
                s_hits++;
                if (s_hits <= 64)
                {
                    fprintf(stderr, "[LOOSE] hit: %s -> %u/%u bytes (file=%ld)\n",
                            loosePath, (unsigned)got, (unsigned)bufSize, fileSize);
                }
                if (got == 0)
                {
                    s_warns++;
                    fprintf(stderr, "[LOOSE/WARN] %s: fread returned 0 (errno=%d %s) — falling back to disc\n",
                            loosePath, errno, strerror(errno));
                    /* zero-byte read means the loose file is empty/unreadable;
                     * don't return — fall through to CdRead so we don't render
                     * uninitialized buffer contents. */
                }
                else
                {
                    if (got < bufSize && fileSize > 0 && (long)got < fileSize)
                    {
                        s_warns++;
                        fprintf(stderr, "[LOOSE/WARN] %s: short read %u of %ld bytes (errno=%d %s)\n",
                                loosePath, (unsigned)got, fileSize, errno, strerror(errno));
                    }
                    (void)got;
                    return true;
                }
            }
        }
        else
        {
            s_misses++;
            const char* verb = getenv("SH_LOOSE_VERBOSE");
            int verbose = (verb && verb[0] && verb[0] != '0');
            if (verbose && s_misses <= 256)
            {
                fprintf(stderr, "[LOOSE/MISS] %s (errno=%d %s)\n",
                        loosePath, errno, strerror(errno));
            }
        }

        /* Periodic summary every 64 probes — survives long sessions
         * without flooding the log. */
        {
            int total = s_hits + s_misses + s_hires + s_warns;
            if (total > 0 && (total % 64) == 0)
            {
                fprintf(stderr, "[LOOSE/SUMMARY] %d hits, %d misses, %d hi-res, %d warnings (cumulative)\n",
                        s_hits, s_misses, s_hires, s_warns);
            }
        }
    }
#endif

    return CdRead(sectorCount >> FS_SECTOR_SHIFT, (u32*)entry->data, CdlModeSpeed);
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
    RECT      pixelRect = {0};
    RECT      clutRect = {0};
    bool      haveClut = false;
    int       discBitDepth = 0;
#endif

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
#ifdef SH_PC_PORT
    pixelRect = tempRect;
    /* tim.mode bits 0-2: 0=4bpp, 1=8bpp, 2=16bpp, 3=24bpp. */
    {
        int code = (int)(tim.mode & 0x7);
        discBitDepth = (code == 0) ? 4 : (code == 1) ? 8 :
                       (code == 2) ? 16 : (code == 3) ? 24 : 0;
    }
#endif

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
#ifdef SH_PC_PORT
        clutRect = tempRect;
        haveClut = true;
#endif
    }
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] PostLoadTim done\n"); fflush(g_ShDebugLog); } }

    /* Hi-res override: if Fs_QueueTickRead detected a loose TIM bigger than
     * the disc buffer, register it now with the rects we just used for the
     * native upload. Sample-time lookup will key by (tpage, clut), which
     * derive from these same coords. */
    {
        const char* hiresPath = HiresPending_PopPath(entry);
        if (hiresPath && hiresPath[0])
        {
            if (discBitDepth <= 0)
            {
                fprintf(stderr, "[LOOSE/HIRES/SKIP] %s: disc TIM bit-depth unknown (mode=%u); cannot register override\n",
                        hiresPath, (unsigned)tim.mode);
            }
            else
            {
                FILE* hf = fopen(hiresPath, "rb");
                if (!hf)
                {
                    fprintf(stderr, "[LOOSE/HIRES/ERR] %s: fopen failed at PostLoad (errno=%d %s)\n",
                            hiresPath, errno, strerror(errno));
                }
                else
                {
                    int seekOk = (fseek(hf, 0, SEEK_END) == 0);
                    long sz = seekOk ? ftell(hf) : -1;
                    if (seekOk) fseek(hf, 0, SEEK_SET);
                    if (sz <= 0)
                    {
                        fprintf(stderr, "[LOOSE/HIRES/ERR] %s: invalid size %ld\n", hiresPath, sz);
                    }
                    else if (sz >= 64 * 1024 * 1024)
                    {
                        fprintf(stderr, "[LOOSE/HIRES/ERR] %s: too large (%ld bytes, cap 64MB)\n",
                                hiresPath, sz);
                    }
                    else
                    {
                        unsigned char* buf = (unsigned char*)malloc((size_t)sz);
                        if (!buf)
                        {
                            fprintf(stderr, "[LOOSE/HIRES/ERR] %s: malloc(%ld) failed\n",
                                    hiresPath, sz);
                        }
                        else
                        {
                            size_t got = fread(buf, 1, (size_t)sz, hf);
                            if (got != (size_t)sz)
                            {
                                fprintf(stderr, "[LOOSE/HIRES/ERR] %s: short read %u of %ld bytes\n",
                                        hiresPath, (unsigned)got, sz);
                            }
                            else
                            {
                                int cx = haveClut ? (int)clutRect.x : -1;
                                int cy = haveClut ? (int)clutRect.y : -1;
                                fprintf(stderr,
                                    "[LOOSE/HIRES] registering %s: pixelRect=(%d,%d %dx%d) clut=(%d,%d) discBpp=%d\n",
                                    hiresPath,
                                    (int)pixelRect.x, (int)pixelRect.y,
                                    (int)pixelRect.w, (int)pixelRect.h,
                                    cx, cy, discBitDepth);
                                HiresOverride_RegisterFromTim(
                                    hiresPath, buf, (unsigned int)sz,
                                    (int)pixelRect.x, (int)pixelRect.y,
                                    (int)pixelRect.w, (int)pixelRect.h,
                                    cx, cy, discBitDepth);
                            }
                            free(buf);
                        }
                    }
                    fclose(hf);
                }
            }
        }
    }
#endif

    return true;
}

bool Fs_QueuePostLoadAnm(s_FsQueueEntry* entry)
{
    Fs_CharaAnimInfoUpdate(entry->extra.anm.field_0, entry->extra.anm.charaId_4, entry->externalData, entry->extra.anm.coords_8);
    return true;
}
