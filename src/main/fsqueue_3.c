
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

#ifndef _WIN32
/* Loose-file paths are built from the disc file table's UPPERCASE folder/names
 * (gamedata/load/CHARA/DOB.TIM). Windows/macOS open those case-insensitively;
 * Linux (ext4) does not, so a mod authored on Windows or with a lowercased
 * folder silently fails to load. These enable a case-insensitive fallback. */
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#endif

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

#ifndef _WIN32
/* Resolve a relative path case-insensitively for case-sensitive filesystems.
 * Walks each component: a verbatim hit wins (stat), else the parent directory
 * is scanned for a strcasecmp match, so "gamedata/load/CHARA/DOB.TIM.png"
 * still finds an on-disk "gamedata/load/chara/dob.tim.png". Fills `out` with
 * the real path and returns 1 only if EVERY component resolved. Called only
 * after an exact open already missed. */
static int Loose_ResolveCase(const char* path, char* out, size_t outSize)
{
    const char* p      = path;
    size_t      outLen = 0;

    if (path == NULL || path[0] == '\0') return 0;
    out[0] = '\0';

    while (*p != '\0')
    {
        char        comp[128];
        char        cand[300];
        const char* slash   = strchr(p, '/');
        size_t      compLen = slash ? (size_t)(slash - p) : strlen(p);
        struct stat st;
        int         found = 0;

        if (compLen == 0) { p = slash + 1; continue; } /* skip // */
        if (compLen >= sizeof(comp)) return 0;
        memcpy(comp, p, compLen);
        comp[compLen] = '\0';

        if (outLen == 0) snprintf(cand, sizeof(cand), "%s", comp);
        else             snprintf(cand, sizeof(cand), "%s/%s", out, comp);

        if (stat(cand, &st) == 0)
        {
            found = 1; /* component exists verbatim */
        }
        else
        {
            DIR* d = opendir(outLen == 0 ? "." : out);
            if (d != NULL)
            {
                struct dirent* de;
                while ((de = readdir(d)) != NULL)
                {
                    if (strcasecmp(de->d_name, comp) == 0)
                    {
                        if (outLen == 0) snprintf(cand, sizeof(cand), "%s", de->d_name);
                        else             snprintf(cand, sizeof(cand), "%s/%s", out, de->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
        }

        if (!found) return 0;
        if ((size_t)snprintf(out, outSize, "%s", cand) >= outSize) return 0;
        outLen = strlen(out);

        if (!slash) break;
        p = slash + 1;
    }
    return out[0] != '\0';
}
#endif

/* fopen a loose file, retrying case-insensitively on a miss (Linux). On
 * Windows/macOS the first fopen already matches any case, so the fallback is
 * compiled out / never taken. */
static FILE* Loose_FOpen(const char* path, const char* mode)
{
    FILE* f = fopen(path, mode);
#ifndef _WIN32
    if (f == NULL)
    {
        char resolved[300];
        if (Loose_ResolveCase(path, resolved, sizeof(resolved)))
            f = fopen(resolved, mode);
    }
#endif
    return f;
}

/* Read a whole loose file. Returns malloc'd bytes (caller frees) or NULL
 * with the failing step logged. 64MB cap. */
static unsigned char* PcFile_Slurp(const char* path, long* outSize)
{
    unsigned char* buf = NULL;
    long           sz  = -1;
    FILE*          f   = Loose_FOpen(path, "rb");

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

/* True when a per-CLUT-row loose override set is present. A multi-CLUT chara/BG
 * TIM draws different body regions through different palette rows; a modder can
 * replace one region with "{base}.pNN.png" (NN zero-padded, from the launcher's
 * per-palette extraction). Row 0 always ships, so probing p00 is the cheap
 * gate that keeps the common no-mod path from doing 16 fopens per TIM. */
static int Loose_HasPerRow(const char* base)
{
    char  p[176];
    FILE* f;
    if (base == NULL || base[0] == '\0') return 0;
    snprintf(p, sizeof(p), "%s.p00.png", base);
    f = Loose_FOpen(p, "rb");
    if (f != NULL) { fclose(f); return 1; }
    return 0;
}

/* Resolve a WHOLE-image loose replacement for a base disc path, in priority
 * order: "{base}.png" (e.g. CHARA/DOB.TIM.png), "{stem}.png" (DOB.png), then
 * the base file itself (an oversized loose .TIM handed straight to the
 * decoder). Returns 1 and fills `out` with the first that exists. */
static int Loose_ResolveWhole(const char* base, char* out, size_t outSize)
{
    FILE* f;

    snprintf(out, outSize, "%s.png", base);
    f = Loose_FOpen(out, "rb");
    if (f != NULL) { fclose(f); return 1; }

    {
        const char* slash = strrchr(base, '/');
        const char* fname = slash ? slash + 1 : base;
        const char* dot   = strchr(fname, '.');
        if (dot != NULL && dot != fname)
        {
            size_t stemLen = (size_t)(dot - base);
            char   stem[160];
            if (stemLen >= sizeof(stem)) stemLen = sizeof(stem) - 1;
            memcpy(stem, base, stemLen);
            stem[stemLen] = '\0';
            snprintf(out, outSize, "%s.png", stem);
            f = Loose_FOpen(out, "rb");
            if (f != NULL) { fclose(f); return 1; }
        }
    }

    {
        size_t n = strlen(base);
        if (n >= outSize) return 0;
        memcpy(out, base, n + 1);
        f = Loose_FOpen(out, "rb");
        if (f != NULL) { fclose(f); return 1; }
    }
    return 0;
}
#endif

#ifdef SH_PC_PORT
/* Set to 1 by the byte-replace loose path below when it fully populates
 * entry->data from disk without issuing a CdRead. Fs_QueueUpdateRead reads
 * this to skip the Sync state — with no CD command enqueued, CdReadSync
 * would return NO_VALUE and drop the read into a Reset loop that never
 * completes (see fsqueue_2.c). Cleared on every entry so disc/hi-res reads
 * (which DO issue a CdRead and must Sync) are unaffected. */
int g_FsLooseReadComplete = 0;
#endif

bool Fs_QueueTickRead(s_FsQueueEntry* entry)
{
    s32 sectorCount;

#ifdef SH_PC_PORT
    g_FsLooseReadComplete = 0;
#endif

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

        /* Hi-res PNG override under gamedata/load/ — registers as a hi-res
         * override (PNG's true 8-bit alpha), never a byte-replace. The disc
         * file still loads so the engine picks the native VRAM rect; PostLoadTim
         * then registers the PNG against it. Takes precedence over a same-name
         * loose file. Two accepted names, in priority order:
         *   1. "<discname>.png"   e.g. DRU02F.TIM.png  (full disc name + .png)
         *   2. "<basename>.png"   e.g. DRU02F.png       (extension replaced —
         *      the intuitive name for replacing DRU02F.TIM with a PNG). */
        int pngOverride = 0;
        {
            char pngPath[176];
            FILE* pf;

            /* Probe (cheapest first) any override form. A per-CLUT-row set
             * always ships row 0, so "{base}.p00.png" is the per-row gate;
             * then the two whole-image forms "{base}.png" and "{stem}.png".
             * If any exists, stash the BASE disc path — PostLoadTim resolves
             * the exact form (per-row overlay vs whole-image replace). */
            snprintf(pngPath, sizeof(pngPath), "%s.p00.png", loosePath);
            pf = Loose_FOpen(pngPath, "rb");

            if (pf == NULL)
            {
                snprintf(pngPath, sizeof(pngPath), "%s.png", loosePath);
                pf = Loose_FOpen(pngPath, "rb");
            }

            if (pf == NULL)
            {
                char baseName[32];
                size_t bn = 0;
                while (bn < sizeof(baseName) - 1 && nameBuf[bn] != '\0' && nameBuf[bn] != '.')
                {
                    baseName[bn] = nameBuf[bn];
                    bn++;
                }
                baseName[bn] = '\0';
                snprintf(pngPath, sizeof(pngPath), "gamedata/load/%s/%s.png",
                         strippedFolder, baseName);
                pf = Loose_FOpen(pngPath, "rb");
            }

            if (pf != NULL)
            {
                fclose(pf);
                HiresPending_Stash(entry, loosePath);
                pngOverride = 1;
                s_hires++;
                if (s_hires <= 64)
                {
                    SH_DBG("[LOOSE/HIRES] %s: override present; deferring to PostLoadTim",
                           loosePath);
                }
            }
        }

        lf = pngOverride ? NULL : Loose_FOpen(loosePath, "rb");
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
                    g_FsLooseReadComplete = 1;
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

    /* Point-sample the font atlas: an HD font pack paints gutterless glyph cells
     * edge-to-edge, so bilinear sampling bleeds the neighbour glyph's ink across
     * the cell boundary ("ghost text" beside A/O). Force GL_NEAREST for the font
     * TIMs only (FONT16 = the 12x16 menu/UI/dialogue font; FONT24 = credits) —
     * every other override keeps bilinear. Cleared before return. */
    {
        int fidx = FSQ_INFO_VALID(entry->info) ? (int)(entry->info - &g_FileTable[0]) : -1;
        HiresOverride_SetForceNearestUpload(
            fidx == FILE_1ST_FONT16_TIM || fidx == FILE_TIM_FONT24_TIM);
    }

    /* Virtual pool slot: decode the TIM (or a loose PNG/TIM replacement)
     * into the slot's persistent GL texture. slotId comes from the synthetic
     * clutY the slot was initialized with; native pixel dims come from the
     * disc TIM so replacement UVs map 0..1 over the original. */
    if (pcVirtualSlot)
    {
        /* Inverse of the slot-id encoding in hires_override.h: the id is
         * split across 16-row-spaced clutY groups and the clutX cell bits. */
        s32 slotId = (((s32)entry->extra.image.clutY - HIRES_POOL_CLUT_ROW_BASE)
                      / HIRES_POOL_MAX_ROWS) * 64
                   + ((s32)entry->extra.image.clutX / 16);
        int nativeW = (discBitDepth == 4)  ? (int)pixelRect.w * 4 :
                      (discBitDepth == 8)  ? (int)pixelRect.w * 2 :
                      (discBitDepth == 24) ? ((int)pixelRect.w * 2) / 3 :
                                             (int)pixelRect.w;
        int nativeH = (int)pixelRect.h;
        const char* loosePath = HiresPending_PopPath(entry);
        int registered = 0;
        int perRow;
        /* The pending-path table is keyed on the s_FsQueueEntry* pointer, which
         * the FS queue recycles. A stash that wasn't popped for its own load can
         * be popped HERE by a different file that reused the pointer — e.g. a
         * building chunk (THR2501F) inheriting a stale CHARA/BFLU stash and
         * painting BFLU across the wall. The stashed path's basename is the disc
         * name it was stashed for, so require it to match THIS entry's disc name;
         * on mismatch drop it so the base disc TIM (below) renders instead. */
        if (loosePath != NULL && FSQ_INFO_VALID(entry->info))
        {
            char        curName[16] = {0};
            const char* base = strrchr(loosePath, '/');
            base = (base != NULL) ? base + 1 : loosePath;
            Fs_GetFileInfoName(curName, entry->info);
            if (curName[0] != '\0' && strcmp(base, curName) != 0)
            {
                SH_DBG("[POOLTEX] slot %d: stale stash '%s' != loading '%s' — discarding",
                       slotId, loosePath, curName);
                loosePath = NULL;
            }
        }
        perRow = Loose_HasPerRow(loosePath);

        if (discBitDepth <= 0 || !FSQ_INFO_VALID(entry->info))
        {
            SH_DBG("[POOLTEX] slot %d: bad TIM (mode=%u) or invalid info — not registered",
                   slotId, (unsigned)tim.mode);
        }
        else
        {
            /* Whole-image loose replacement (no per-row set): a single palette
             * covers the slot — ideal for the many single-CLUT-row monsters
             * (CLD1/ICU/...). A per-row set instead overlays onto the disc base
             * below, so untouched palette rows keep the native art. */
            if (!perRow && loosePath != NULL && loosePath[0] != '\0')
            {
                char whole[176];
                if (Loose_ResolveWhole(loosePath, whole, sizeof(whole)))
                {
                    long           lsz  = 0;
                    unsigned char* lbuf = PcFile_Slurp(whole, &lsz);
                    if (lbuf != NULL)
                    {
                        SH_DBG("[POOLTEX] slot %d: loose replacement %s", slotId, whole);
                        registered = HiresOverride_PoolSlotRegister(
                            slotId, lbuf, (unsigned int)lsz, nativeW, nativeH) == 0;
                        free(lbuf);
                    }
                    if (!registered)
                    {
                        SH_DBG("[POOLTEX] slot %d: loose %s unusable — falling back to disc TIM",
                               slotId, whole);
                    }
                }
            }

            /* Base content first — the disc TIM, one texture per CLUT row
             * (prims select palette rows with baked clut deltas). A whole-image
             * loose replacement above fully covers the slot instead. */
            if (!registered)
            {
                unsigned int discSize = (unsigned int)ALIGN(
                    entry->info->blockCount * FS_BLOCK_SIZE, FS_SECTOR_SIZE);
                HiresOverride_PoolSlotRegister(slotId, (const unsigned char*)entry->externalData,
                                               discSize, nativeW, nativeH);

                /* Per-CLUT-row loose overlay: replace only the palette rows the
                 * modder supplied ("{base}.pNN.png"); untouched rows keep the
                 * disc art. This is what recolours a whole multi-CLUT monster
                 * region-by-region. */
                int looseRowsApplied = 0;
                if (perRow)
                {
                    int rows = (haveClut && tim.crect != NULL) ? (int)clutRect.h : 1;
                    int r;

                    if (rows < 1) rows = 1;
                    if (rows > HIRES_POOL_MAX_ROWS) rows = HIRES_POOL_MAX_ROWS;

                    for (r = 0; r < rows; r++)
                    {
                        char  pr[176];
                        FILE* chk;
                        snprintf(pr, sizeof(pr), "%s.p%02d.png", loosePath, r);
                        chk = Loose_FOpen(pr, "rb");
                        if (chk == NULL) continue; /* row not supplied — keep disc art */
                        fclose(chk);
                        {
                            long           psz  = 0;
                            unsigned char* pbuf = PcFile_Slurp(pr, &psz);
                            if (pbuf != NULL)
                            {
                                if (HiresOverride_PoolSlotLoosePngRow(
                                        slotId, r, pbuf, (unsigned int)psz, nativeW, nativeH) == 0)
                                    looseRowsApplied++;
                                free(pbuf);
                            }
                        }
                    }
                    if (looseRowsApplied > 0)
                        SH_DBG("[POOLTEX] slot %d: %d loose CLUT-row override(s) from %s",
                               slotId, looseRowsApplied, loosePath);
                }

                /* DuckStation texture pack: per-row palette match, composed rows
                 * overwrite that row's texture. An explicit loose per-row override
                 * wins over a pack for this slot (as on the VRAM path). */
                if (!looseRowsApplied && TexPack_HasEntries())
                {
                    int clutW = (tim.caddr != NULL && tim.crect != NULL) ? (int)tim.crect->w : 0;
                    int rows  = (haveClut && tim.crect != NULL) ? (int)clutRect.h : 1;
                    int r;

                    if (rows < 1) rows = 1;
                    if (rows > HIRES_POOL_MAX_ROWS) rows = HIRES_POOL_MAX_ROWS;

                    for (r = 0; r < rows; r++)
                    {
                        int cw = 0, ch = 0;
                        const unsigned short* clutRow;
                        const unsigned char* canvas;

                        if (HiresOverride_PackBudgetExceeded())
                        {
                            static int s_budgetLog = 0;
                            if (!s_budgetLog)
                            {
                                s_budgetLog = 1;
                                SH_DBG("[TEXPACK] GL byte budget reached — further pool rows keep native art");
                            }
                            break;
                        }

                        clutRow = (tim.caddr != NULL)
                            ? (const unsigned short*)tim.caddr + (size_t)r * (size_t)clutW
                            : NULL;
                        canvas = TexPack_Compose(
                            (const unsigned char*)tim.paddr, (int)pixelRect.w, (int)pixelRect.h,
                            clutRow, clutW, discBitDepth, &cw, &ch);
                        if (canvas != NULL)
                        {
                            /* canvas is owned by the compose cache — no free. Key
                             * the upload on the compose content hash so an
                             * unchanged re-upload skips the glTexImage2D churn. */
                            HiresOverride_PoolSlotRegisterRGBAKeyed(
                                slotId, r, canvas, cw, ch, nativeW, nativeH,
                                TexPack_LastComposeHash());
                        }
                    }
                }
            }
        }
    }
    else
    /* Hi-res override: if Fs_QueueTickRead detected a loose TIM bigger than
     * the disc buffer, register it now with the rects we just used for the
     * native upload. Sample-time lookup will key by (tpage, clut), which
     * derive from these same coords. */
    {
        const char* base = HiresPending_PopPath(entry);
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

        if (base && base[0])
        {
            if (discBitDepth <= 0)
            {
                SH_DBG("[LOOSE/HIRES/SKIP] %s: disc TIM bit-depth unknown (mode=%u); cannot register override",
                       base, (unsigned)tim.mode);
            }
            else if (Loose_HasPerRow(base))
            {
                /* Per-CLUT-row overlay: register each supplied palette row at
                 * its own clut cell (clutRect.y + r), so every body region a
                 * prim draws through row r samples the override; unsupplied
                 * rows fall through to native VRAM. */
                int rows = haveClut ? (int)clutRect.h : 1;
                int r, applied = 0;

                if (rows < 1) rows = 1;
                if (rows > 16) rows = 16;

                for (r = 0; r < rows; r++)
                {
                    char  pr[176];
                    FILE* chk;
                    snprintf(pr, sizeof(pr), "%s.p%02d.png", base, r);
                    chk = Loose_FOpen(pr, "rb");
                    if (chk == NULL) continue;
                    fclose(chk);
                    {
                        long           sz  = 0;
                        unsigned char* buf = PcFile_Slurp(pr, &sz);
                        if (buf != NULL)
                        {
                            char label[24];
                            snprintf(label, sizeof(label), "loose row %d", r);
                            if (HiresOverride_RegisterLoosePngRow(
                                    label, buf, (unsigned int)sz,
                                    (int)pixelRect.x, (int)pixelRect.y,
                                    (int)pixelRect.w, (int)pixelRect.h,
                                    haveClut ? (int)clutRect.x : -1,
                                    haveClut ? ((int)clutRect.y + r) : -1,
                                    discBitDepth) == 0)
                                applied++;
                            free(buf);
                        }
                    }
                }
                if (applied > 0)
                {
                    looseHires = 1;
                    SH_DBG("[LOOSE/HIRES] %s: %d loose CLUT-row override(s)", base, applied);
                }
            }
            else
            {
                /* One whole-image PNG (e.g. BOS.png) with no per-row set replaces
                 * EVERY palette of this texture: register it across all the disc
                 * TIM's clut rows so any palette a prim selects samples it. */
                char whole[176];
                if (Loose_ResolveWhole(base, whole, sizeof(whole)))
                {
                    long           sz  = 0;
                    unsigned char* buf = PcFile_Slurp(whole, &sz);
                    if (buf != NULL)
                    {
                        int cx   = haveClut ? (int)clutRect.x : -1;
                        int cy   = haveClut ? (int)clutRect.y : -1;
                        int rows = haveClut ? (int)clutRect.h : 1;

                        if (rows < 1) rows = 1;
                        if (rows > 16) rows = 16;
                        SH_DBG("[LOOSE/HIRES] registering %s across %d clut row(s): pixelRect=(%d,%d %dx%d) clut=(%d,%d) discBpp=%d",
                            whole, rows,
                            (int)pixelRect.x, (int)pixelRect.y,
                            (int)pixelRect.w, (int)pixelRect.h,
                            cx, cy, discBitDepth);
                        looseHires = HiresOverride_RegisterLoosePngAllRows(
                            whole, buf, (unsigned int)sz,
                            (int)pixelRect.x, (int)pixelRect.y,
                            (int)pixelRect.w, (int)pixelRect.h,
                            cx, cy, rows, discBitDepth) == 0;
                        free(buf);
                    }
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
                const unsigned short* clutRow;
                const unsigned char* canvas;

                if (HiresOverride_PackBudgetExceeded())
                {
                    static int s_budgetLog2 = 0;
                    if (!s_budgetLog2)
                    {
                        s_budgetLog2 = 1;
                        SH_DBG("[TEXPACK] GL byte budget reached — further VRAM rows keep native art");
                    }
                    break;
                }

                clutRow = (tim.caddr != NULL)
                    ? (const unsigned short*)tim.caddr + (size_t)r * (size_t)clutW
                    : NULL;
                canvas = TexPack_Compose(
                    (const unsigned char*)tim.paddr, (int)pixelRect.w, (int)pixelRect.h,
                    clutRow, clutW, discBitDepth, &cw, &ch);
                if (canvas != NULL)
                {
                    char packLabel[24];
                    snprintf(packLabel, sizeof(packLabel), "texpack row %d", r);
                    /* canvas is owned by the compose cache — no free. Key on the
                     * compose content hash so an unchanged re-upload of this VRAM
                     * rect skips the glTexImage2D churn. */
                    HiresOverride_RegisterRGBAKeyed(packLabel, canvas, cw, ch,
                                               (int)pixelRect.x, (int)pixelRect.y,
                                               (int)pixelRect.w, (int)pixelRect.h,
                                               haveClut ? (int)clutRect.x : -1,
                                               haveClut ? ((int)clutRect.y + r) : -1,
                                               discBitDepth, TexPack_LastComposeHash());
                }
            }
        }
    }

    HiresOverride_SetForceNearestUpload(0);
#endif

    return true;
}

bool Fs_QueuePostLoadAnm(s_FsQueueEntry* entry)
{
    Fs_CharaAnimDataUpdate(entry->extra.anm.field_0, entry->extra.anm.charaId, entry->externalData, entry->extra.anm.coords_8);
    return true;
}
