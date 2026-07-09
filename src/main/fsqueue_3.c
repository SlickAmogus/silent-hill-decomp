
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
#include "tex_pack.h"
#include "sh_log.h"

/* Forensics for the FS-queue stomp family (SaveLoad.log / SewerCrash*.log):
 * dump the whole corrupted entry so the written byte pattern names the
 * writer (observed so far: 64-bit -1 over info, 16-bit 0x8002 at info+4). */
void FsQueue_DumpEntryHex(const char* tag, int idx, const s_FsQueueEntry* e)
{
    const u8* b = (const u8*)e;
    SH_DBG("[FSQ-CORRUPT] %s idx=%d entry=%p hex="
           "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X "
           "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X "
           "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X "
           "%02X%02X%02X%02X%02X%02X%02X%02X",
           tag, idx, (const void*)e,
           b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
           b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],
           b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23],
           b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31],
           b[32],b[33],b[34],b[35],b[36],b[37],b[38],b[39],
           b[40],b[41],b[42],b[43],b[44],b[45],b[46],b[47],
           b[48],b[49],b[50],b[51],b[52],b[53],b[54],b[55]);
}

#define FSQ_INFO_VALID(p) ((p) >= &g_FileTable[0] && (p) < &g_FileTable[FS_FILE_COUNT])
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
#ifdef SH_PC_PORT
            /* This scan walks EVERY live entry between the postLoad and read
             * cursors — the per-cursor guards in Fs_QueueUpdate never see the
             * ones in the middle, and a stomped info pointer here was the
             * SaveLoad.log crash (read at 0xFFFFFFFFFFFFFFFF). Treat a
             * corrupt entry as non-overlapping and keep going. */
            /* Self corrupt: report "can't read yet" — the read-cursor guard
             * in Fs_QueueUpdate drops it next tick. */
            if (!FSQ_INFO_VALID(entry->info))
            {
                FsQueue_DumpEntryHex("CanRead-self", g_FsQueue.read.idx, entry);
                return false;
            }
            if (!FSQ_INFO_VALID(other->info))
            {
                FsQueue_DumpEntryHex("CanRead-other", (g_FsQueue.postLoad.idx + i) & (FS_QUEUE_LENGTH - 1), other);
                continue;
            }
#endif
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
        SH_DBG("[HIRES] pending table full, dropping %s", path);
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

/* Read a whole loose file. Returns malloc'd bytes (caller frees) or NULL
 * with the failing step logged. 64MB cap. */
static unsigned char* PcFile_Slurp(const char* path, long* outSize)
{
    unsigned char* buf = NULL;
    long           sz  = -1;
    FILE*          f   = fopen(path, "rb");

    if (f == NULL)
    {
        SH_DBG("[LOOSE/WARN] %s: fopen failed (errno=%d %s)", path, errno, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0)
    {
        sz = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0)
        {
            sz = -1;
        }
    }
    if (sz <= 0)
    {
        SH_DBG("[LOOSE/WARN] %s: invalid size %ld", path, sz);
    }
    else if (sz >= 64 * 1024 * 1024)
    {
        SH_DBG("[LOOSE/WARN] %s: too large (%ld bytes, cap 64MB)", path, sz);
    }
    else
    {
        buf = (unsigned char*)malloc((size_t)sz);
        if (buf == NULL)
        {
            SH_DBG("[LOOSE/WARN] %s: malloc(%ld) failed", path, sz);
        }
        else if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        {
            SH_DBG("[LOOSE/WARN] %s: short read of %ld bytes", path, sz);
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf != NULL && outSize != NULL)
    {
        *outSize = sz;
    }
    return buf;
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
            SH_DBG("[LOOSE/INIT] allow_loose_files=%d  base=gamedata/load/  verbose=%s",
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

        /* Hi-res PNG override: "<discname>.png" (e.g. ITEM_M.TIM.png) under
         * gamedata/load/ always registers as a hi-res override — with PNG's
         * true 8-bit alpha — never as a byte-replace. The disc file still
         * loads so the engine picks the native VRAM rect; PostLoadTim then
         * registers the PNG against it. Takes precedence over a same-name
         * loose file. */
        int pngOverride = 0;
        {
            char pngPath[168];
            FILE* pf;
            snprintf(pngPath, sizeof(pngPath), "%s.png", loosePath);
            pf = fopen(pngPath, "rb");
            if (pf != NULL)
            {
                fclose(pf);
                HiresPending_Stash(entry, pngPath);
                pngOverride = 1;
                s_hires++;
                if (s_hires <= 64)
                {
                    SH_DBG("[LOOSE/HIRES] %s: PNG override; deferring to PostLoadTim",
                           pngPath);
                }
            }
        }

        lf = pngOverride ? NULL : fopen(loosePath, "rb");
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
                SH_DBG("[LOOSE/WARN] %s: fseek/ftell failed (errno=%d %s)",
                       loosePath, errno, strerror(errno));
            }

            if (fileSize > 0 && (size_t)fileSize > bufSize)
            {
                fclose(lf);
                HiresPending_Stash(entry, loosePath);
                s_hires++;
                if (s_hires <= 64)
                {
                    SH_DBG("[LOOSE/HIRES] %s (%ld bytes) > buf %u; deferring to PostLoadTim for hi-res override",
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
                    SH_DBG("[LOOSE] hit: %s -> %u/%u bytes (file=%ld)",
                           loosePath, (unsigned)got, (unsigned)bufSize, fileSize);
                }
                if (got == 0)
                {
                    s_warns++;
                    SH_DBG("[LOOSE/WARN] %s: fread returned 0 (errno=%d %s) — falling back to disc",
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
                        SH_DBG("[LOOSE/WARN] %s: short read %u of %ld bytes (errno=%d %s)",
                               loosePath, (unsigned)got, fileSize, errno, strerror(errno));
                    }
                    (void)got;
                    return true;
                }
            }
        }
        else if (!pngOverride)
        {
            s_misses++;
            const char* verb = getenv("SH_LOOSE_VERBOSE");
            int verbose = (verb && verb[0] && verb[0] != '0');
            if (verbose && s_misses <= 256)
            {
                SH_DBG("[LOOSE/MISS] %s (errno=%d %s)",
                       loosePath, errno, strerror(errno));
            }
        }

        /* Periodic summary every 64 probes — survives long sessions
         * without flooding the log. */
        {
            int total = s_hits + s_misses + s_hires + s_warns;
            if (total > 0 && (total % 64) == 0)
            {
                SH_DBG("[LOOSE/SUMMARY] %d hits, %d misses, %d hi-res, %d warnings (cumulative)",
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
    /* Virtual chunk-pool slot (resident_textures; encoding in
     * hires_override.h): clutY names a VRAM row that doesn't exist. Skip
     * both VRAM uploads — the pixel rect aliases a real pool page and would
     * stomp it — and instead decode the TIM straight into the slot's
     * persistent GL texture. */
    bool      pcVirtualSlot = entry->extra.image.u != UCHAR_MAX &&
                              entry->extra.image.clutY >= HIRES_POOL_CLUT_ROW_BASE;
#endif

#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) {
        char _fnm[16] = {0};
        int _fidx = (entry->info >= &g_FileTable[0] && entry->info < &g_FileTable[FS_FILE_COUNT])
                        ? (int)(entry->info - &g_FileTable[0]) : -1;
        if (_fidx >= 0) Fs_GetFileInfoName(_fnm, entry->info);
        fprintf(g_ShDebugLog, "[BOOT0/TIM] PostLoadTim file=%d '%s' ss=0x%x img.u=%u img.v=%u tPage=%u,%u clutX=%d clutY=%d\n",
        _fidx, _fnm, _fidx >= 0 ? (unsigned)entry->info->startSector : 0u,
        (unsigned)entry->extra.image.u, (unsigned)entry->extra.image.v,
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
    if (entry->extra.image.u != UCHAR_MAX)
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

#ifdef SH_PC_PORT
    if (!pcVirtualSlot)
#endif
    {
        LoadImage(&tempRect, tim.paddr);
    }
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

#ifdef SH_PC_PORT
        if (!pcVirtualSlot)
#endif
        {
            LoadImage(&tempRect, tim.caddr);
        }
#ifdef SH_PC_PORT
        clutRect = tempRect;
        haveClut = true;
#endif
    }
#ifdef SH_PC_PORT
    { extern FILE* g_ShDebugLog; if (g_ShDebugLog) { fprintf(g_ShDebugLog, "[BOOT0/TIM] PostLoadTim done\n"); fflush(g_ShDebugLog); } }

    /* Virtual pool slot: decode the TIM (or a loose PNG/TIM replacement)
     * into the slot's persistent GL texture. slotId comes from the synthetic
     * clutY the slot was initialized with; native pixel dims come from the
     * disc TIM so replacement UVs map 0..1 over the original. */
    if (pcVirtualSlot)
    {
        s32 slotId = (s32)entry->extra.image.clutY - HIRES_POOL_CLUT_ROW_BASE;
        int nativeW = (discBitDepth == 4)  ? (int)pixelRect.w * 4 :
                      (discBitDepth == 8)  ? (int)pixelRect.w * 2 :
                      (discBitDepth == 24) ? ((int)pixelRect.w * 2) / 3 :
                                             (int)pixelRect.w;
        int nativeH = (int)pixelRect.h;
        const char* loosePath = HiresPending_PopPath(entry);
        int registered = 0;

        if (discBitDepth <= 0 || !FSQ_INFO_VALID(entry->info))
        {
            SH_DBG("[POOLTEX] slot %d: bad TIM (mode=%u) or invalid info — not registered",
                   slotId, (unsigned)tim.mode);
        }
        else
        {
            if (loosePath != NULL && loosePath[0] != '\0')
            {
                long           lsz  = 0;
                unsigned char* lbuf = PcFile_Slurp(loosePath, &lsz);
                if (lbuf != NULL)
                {
                    SH_DBG("[POOLTEX] slot %d: loose replacement %s", slotId, loosePath);
                    registered = HiresOverride_PoolSlotRegister(
                        slotId, lbuf, (unsigned int)lsz, nativeW, nativeH) == 0;
                    free(lbuf);
                }
                if (!registered)
                {
                    SH_DBG("[POOLTEX] slot %d: loose %s unusable — falling back to disc TIM",
                           slotId, loosePath);
                }
            }

            /* DuckStation texture pack, matched by content hash of the
             * upload. Loose gamedata/load replacements above take priority. */
            if (!registered && TexPack_HasEntries())
            {
                int cw = 0, ch = 0;
                int clutCount = (tim.caddr != NULL && tim.crect != NULL) ? (int)tim.crect->w : 0;
                unsigned char* canvas = TexPack_Compose(
                    (const unsigned char*)tim.paddr, (int)pixelRect.w, (int)pixelRect.h,
                    (const unsigned short*)tim.caddr, clutCount, discBitDepth, &cw, &ch);
                if (canvas != NULL)
                {
                    registered = HiresOverride_PoolSlotRegisterRGBA(
                        slotId, canvas, cw, ch, nativeW, nativeH) == 0;
                    free(canvas);
                }
            }

            if (!registered)
            {
                unsigned int discSize = (unsigned int)ALIGN(
                    entry->info->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE);
                HiresOverride_PoolSlotRegister(slotId, (const unsigned char*)entry->externalData,
                                               discSize, nativeW, nativeH);
            }
        }
    }
    else
    /* Hi-res override: if Fs_QueueTickRead detected a loose TIM bigger than
     * the disc buffer, register it now with the rects we just used for the
     * native upload. Sample-time lookup will key by (tpage, clut), which
     * derive from these same coords. */
    {
        const char* hiresPath = HiresPending_PopPath(entry);
        int         looseHires = 0;

        /* This upload just rewrote VRAM: any rect-keyed override covering
         * those cells now shows the wrong image. */
        {
            extern void Pc_PoolStompProbe(int x, int y, int w, int h);
            Pc_PoolStompProbe((int)pixelRect.x, (int)pixelRect.y,
                              (int)pixelRect.w, (int)pixelRect.h);
        }
        HiresOverride_InvalidateVramRect((int)pixelRect.x, (int)pixelRect.y,
                                         (int)pixelRect.w, (int)pixelRect.h);
        if (haveClut)
        {
            HiresOverride_InvalidateVramRect((int)clutRect.x, (int)clutRect.y,
                                             (int)clutRect.w, (int)clutRect.h);
        }

        if (hiresPath && hiresPath[0])
        {
            if (discBitDepth <= 0)
            {
                SH_DBG("[LOOSE/HIRES/SKIP] %s: disc TIM bit-depth unknown (mode=%u); cannot register override",
                       hiresPath, (unsigned)tim.mode);
            }
            else
            {
                long           sz  = 0;
                unsigned char* buf = PcFile_Slurp(hiresPath, &sz);
                if (buf != NULL)
                {
                    int cx = haveClut ? (int)clutRect.x : -1;
                    int cy = haveClut ? (int)clutRect.y : -1;
                    SH_DBG("[LOOSE/HIRES] registering %s: pixelRect=(%d,%d %dx%d) clut=(%d,%d) discBpp=%d",
                        hiresPath,
                        (int)pixelRect.x, (int)pixelRect.y,
                        (int)pixelRect.w, (int)pixelRect.h,
                        cx, cy, discBitDepth);
                    looseHires = HiresOverride_RegisterFromTim(
                        hiresPath, buf, (unsigned int)sz,
                        (int)pixelRect.x, (int)pixelRect.y,
                        (int)pixelRect.w, (int)pixelRect.h,
                        cx, cy, discBitDepth) == 0;
                    free(buf);
                }
            }
        }

        /* DuckStation texture pack for VRAM-resident TIMs (items, HUD,
         * charas, 2D backgrounds), matched by content hash of the upload.
         * A loose hi-res replacement above takes priority.
         *
         * Character/item TIMs carry multiple CLUT ROWS — palette variants
         * a draw selects with a clut-row offset. Each row hashes to a
         * different pack palette, so match and compose per row and register
         * each under its own clut coordinate; rows without pack entries
         * simply fall back to the native art. */
        if (!looseHires && discBitDepth > 0 && TexPack_HasEntries())
        {
            int clutW = (haveClut && tim.crect != NULL) ? (int)tim.crect->w : 0;
            int rows  = haveClut ? (int)clutRect.h : 1;
            int r;

            if (rows < 1) rows = 1;
            if (rows > 16) rows = 16;

            for (r = 0; r < rows; r++)
            {
                int cw = 0, ch = 0;
                const unsigned short* clutRow = (tim.caddr != NULL)
                    ? (const unsigned short*)tim.caddr + (size_t)r * (size_t)clutW
                    : NULL;
                unsigned char* canvas = TexPack_Compose(
                    (const unsigned char*)tim.paddr, (int)pixelRect.w, (int)pixelRect.h,
                    clutRow, clutW, discBitDepth, &cw, &ch);
                if (canvas != NULL)
                {
                    char packLabel[24];
                    snprintf(packLabel, sizeof(packLabel), "texpack row %d", r);
                    HiresOverride_RegisterRGBA(packLabel, canvas, cw, ch,
                                               (int)pixelRect.x, (int)pixelRect.y,
                                               (int)pixelRect.w, (int)pixelRect.h,
                                               haveClut ? (int)clutRect.x : -1,
                                               haveClut ? ((int)clutRect.y + r) : -1,
                                               discBitDepth);
                    free(canvas);
                }
            }
        }
    }
#endif

    return true;
}

bool Fs_QueuePostLoadAnm(s_FsQueueEntry* entry)
{
    Fs_CharaAnimDataUpdate(entry->extra.anm.field_0, entry->extra.anm.charaId, entry->externalData, entry->extra.anm.coords_8);
    return true;
}
