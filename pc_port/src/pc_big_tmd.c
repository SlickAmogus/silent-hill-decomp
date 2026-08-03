/* Oversized loose ITEM TMDs — the item-model twin of pc_big_lm.c.
 *
 * GameFs_UniqueItemModelLoad (item_screens_3.c) reads EVERY unique
 * inventory item model into the single fixed PSX-RAM slab FS_BUFFER_5. The
 * slab is sized by the disc file table, and the next allocated slot sits
 * 0x1000 bytes above it, so an enlarged replacement model has nowhere to go:
 * fsqueue_3.c's loose byte-replace path measures the file against the slot,
 * finds it too big for a non-TIM read, and logs
 *   [LOOSE/WARN] ... > buf ... for non-TIM read; ignoring loose file
 * before falling back to the disc.
 *
 * This module lifts that ceiling the same way pc_big_lm.c does for character
 * ILMs: probe the loose file, validate it, hand out a calloc'd PC-owned buffer
 * keyed by g_FileTable index, and substitute it for FS_BUFFER_5 at the single
 * Fs_QueueStartRead seam. Fs_QueueTickRead's gate then rises to that buffer's
 * real capacity via Pc_BigTmd_DestCapacity, and the consumer side resolves the
 * same substitution so GsMapModelingData parses the lifted buffer.
 *
 * ---------------------------------------------------------------------------
 * STOCK-UNCHANGED GUARANTEE
 * ---------------------------------------------------------------------------
 * Pc_BigTmd_Redirect returns its `dest` argument unchanged unless ALL of:
 *   1. g_PcConfig.allowLooseFiles is set                (default 0),
 *   2. gamedata/load/ITEM/<NAME>.TMD opens,
 *   3. its size is STRICTLY GREATER than the native sector-aligned slot,
 *   4. it passes BigTmd_Validate.
 * Condition 1 alone means an unmodified install never allocates, never probes
 * past the one config test, and never logs. With loose files ON but no
 * oversized item file present, BigTmd_Ensure returns NULL at the `fileSize <=
 * tableSize` test — the ordinary byte-replace path keeps ownership exactly as
 * before. Every remaining failure path (path too long, registry full, calloc
 * failure, malformed TMD) also returns `dest`. The only pointer the engine can
 * ever receive other than FS_BUFFER_5 is a validated, correctly sized buffer.
 * Pc_BigTmd_DestCapacity returns 0 for FS_BUFFER_5, so the FS gate is
 * untouched on the stock path as well.
 *
 * ---------------------------------------------------------------------------
 * STALE-POINTER POLICY  (pc_big_lm.c's lesson)
 * ---------------------------------------------------------------------------
 * All ~78 UNQ*.TMD ids share FS_BUFFER_5, so switching items MUST NOT leave
 * the previous item's buffer standing in. BigTmd_SetActive is therefore called
 * on EVERY Redirect, including misses, where it CLEARS the record. A stock
 * item loaded after a modded one resolves back to the slab. Buffers are never
 * freed while the process lives (a grown replacement retires the old buffer
 * into a bounded table instead), so no resolve can ever return freed memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"

#include "pc_big_tmd.h"
#include "pc_config.h"
#include "sh_log.h"

/* Draw-time guard in func_8004BD74 (item_screens_cam.c) skips any
 * object with vern==0 || vern>0x2000 || primn==0 || primn>0x2000. Reject at
 * load time instead of letting a bad file reach GsSortObject4J. */
#define BIGTMD_MAX_VERT 0x2000
#define BIGTMD_MAX_PRIM 0x2000

/* GsMapModelingData (libgs_stub.c) bails on nobj > GS_TMD_MAX_OBJS. */
#define BIGTMD_MAX_OBJ  48

/* File layout: u32 id, u32 flags, u32 nobj, then nobj * 28-byte entries.
 * Object-table offsets are relative to the table itself (file + 12). */
#define BIGTMD_HDR_SIZE  12
#define BIGTMD_OBJ_SIZE  28

/* Only the two TMD ids the SH1 item path produces. */
#define BIGTMD_ID_FIXP   0x41
#define BIGTMD_ID_PLAIN  0x40

/* Absolute sanity cap so a wrong/huge file can never drive a giant calloc. */
#define BIGTMD_MAX_FILE  (16 * 1024 * 1024)

enum
{
    BIGTMD_STATE_ACCEPTED = 1,
    BIGTMD_STATE_REJECTED = 2
};

typedef struct
{
    s16    fileIdx;  /* g_FileTable index — THE key */
    u8*    buf;      /* calloc'd substitute; NULL until Redirect allocates */
    size_t cap;
    long   fileSize; /* validated loose size this verdict refers to */
    size_t needBytes;/* GsMapModelingData's worst-case read extent */
    u8     state;
} PcBigTmd;

/* Only oversized files (accepted or rejected) take a slot, so ordinary play
 * — where every probe misses or fits — never fills the table. */
#define PC_BIGTMD_MAX 64
static PcBigTmd s_bigTmd[PC_BIGTMD_MAX];
static int      s_bigTmdCount;

/* A buffer displaced by a mid-session file-size change stays tracked and
 * allocated: a queue entry may still hold it as entry->data. Freeing it would
 * be exactly the stale-pointer class of bug that produced wild GTE reads in
 * the character path. */
#define PC_BIGTMD_RETIRED_MAX 32
static struct
{
    u8*    buf;
    size_t cap;
} s_retired[PC_BIGTMD_RETIRED_MAX];
static int s_retiredCount;

/* Active substitutions, keyed by the NATIVE destination the engine asked for
 * (FS_BUFFER_5 in practice). Tiny because only the item path uses this. */
#define PC_BIGTMD_ACTIVE_MAX 4
static struct
{
    void* nativeDest;
    u8*   buf;
} s_active[PC_BIGTMD_ACTIVE_MAX];

/* fsqueue_3.c's loose fopen, so this probe and the FS probe see exactly the
 * same on-disk namespace (including the Linux case-insensitive fallback). */
extern FILE* Pc_LooseFOpen(const char* path, const char* mode);

/* Built exactly as Fs_QueueTickRead builds its probe path. */
static int BigTmd_LoosePath(s32 fileIdx, char* out, size_t outSize)
{
    const s_FileInfo* file   = &g_FileTable[fileIdx];
    const char*       folder = g_FilePaths[file->pathIdx];
    char              strippedFolder[16];
    char              nameBuf[32];
    size_t            fi;
    size_t            fl;

    if (folder[0] == '\\')
    {
        folder++;
    }
    fl = strlen(folder);
    while (fl > 0 && folder[fl - 1] == '\\')
    {
        fl--;
    }
    if (fl >= sizeof(strippedFolder))
    {
        fl = sizeof(strippedFolder) - 1;
    }
    for (fi = 0; fi < fl; fi++)
    {
        strippedFolder[fi] = folder[fi];
    }
    strippedFolder[fl] = '\0';

    Fs_GetFileName(nameBuf, fileIdx);

    return snprintf(out, outSize, "gamedata/load/%s/%s", strippedFolder, nameBuf) < (int)outSize;
}

static long BigTmd_ProbeSize(const char* path)
{
    long  sz = -1;
    FILE* f  = Pc_LooseFOpen(path, "rb");

    if (f == NULL)
    {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) == 0)
    {
        sz = ftell(f);
    }
    fclose(f);
    return sz;
}

static u8* BigTmd_Slurp(const char* path, long expectSize)
{
    u8*   buf = NULL;
    FILE* f   = Pc_LooseFOpen(path, "rb");

    if (f == NULL)
    {
        return NULL;
    }
    buf = (u8*)malloc((size_t)expectSize);
    if (buf != NULL && fread(buf, 1, (size_t)expectSize, f) != (size_t)expectSize)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static u32 BigTmd_Rd32(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Validate the raw file and compute the buffer extent GsMapModelingData will
 * actually touch.
 *
 * Two separate jobs, both required:
 *
 *  (a) REJECT malformed data before it can reach GsSortObject4J. Header id and
 *      nobj, every object's vern/primn against the draw-time guard's own
 *      limits, every vertex/normal array inside the file, and a full walk of
 *      the primitive packet chain (each packet is a 4-byte header whose [1]
 *      byte is ilen, the payload length in words) so a truncated or lying
 *      primn cannot run the drawer off the end.
 *
 *  (b) SIZE the buffer. The stub bounds a primitive at 64 bytes when it
 *      computes the block extent it memcpy's out of the load buffer, which
 *      over-reads past EOF for every real TMD (3368 bytes past the end of
 *      stock UNQ21.TMD). Inside the 2 MB PSX RAM slab that is harmless; out of
 *      a tight malloc it is a heap over-read. Reproduce the stub's arithmetic
 *      exactly and size the allocation for it. */
static int BigTmd_Validate(const u8* b, long fileSize, size_t* outNeed, char* err, size_t errCap)
{
    u32    id;
    u32    nobj;
    u32    i;
    u32    dataStart = 0xFFFFFFFFu;
    u32    dataEnd   = 0;
    size_t copySize;
    size_t need;

    if (fileSize < BIGTMD_HDR_SIZE + BIGTMD_OBJ_SIZE)
    {
        snprintf(err, errCap, "too small (%ld B)", fileSize);
        return 0;
    }

    id   = BigTmd_Rd32(b + 0);
    nobj = BigTmd_Rd32(b + 8);

    if (id != BIGTMD_ID_FIXP && id != BIGTMD_ID_PLAIN)
    {
        snprintf(err, errCap, "bad TMD id 0x%x", (unsigned)id);
        return 0;
    }
    if (nobj == 0 || nobj > BIGTMD_MAX_OBJ)
    {
        /* nobj > GS_TMD_MAX_OBJS makes GsMapModelingData return early, so the
         * item would link NULL and silently not draw. Reject loudly instead. */
        snprintf(err, errCap, "nobj=%u outside (0,%d]", (unsigned)nobj, BIGTMD_MAX_OBJ);
        return 0;
    }
    if ((long)(BIGTMD_HDR_SIZE + (size_t)nobj * BIGTMD_OBJ_SIZE) > fileSize)
    {
        snprintf(err, errCap, "object table (%u objs) past EOF", (unsigned)nobj);
        return 0;
    }

    for (i = 0; i < nobj; i++)
    {
        const u8* o = b + BIGTMD_HDR_SIZE + (size_t)i * BIGTMD_OBJ_SIZE;
        u32       vertop = BigTmd_Rd32(o + 0);
        u32       vern   = BigTmd_Rd32(o + 4);
        u32       nortop = BigTmd_Rd32(o + 8);
        u32       norn   = BigTmd_Rd32(o + 12);
        u32       primtop= BigTmd_Rd32(o + 16);
        u32       primn  = BigTmd_Rd32(o + 20);
        u32       p;
        u32       k;

        /* Mirror the draw-time guard so nothing it would skip ever loads. */
        if (vern == 0 || vern > BIGTMD_MAX_VERT)
        {
            snprintf(err, errCap, "obj%u vern=%u outside (0,%d]",
                     (unsigned)i, (unsigned)vern, BIGTMD_MAX_VERT);
            return 0;
        }
        if (primn == 0 || primn > BIGTMD_MAX_PRIM)
        {
            snprintf(err, errCap, "obj%u primn=%u outside (0,%d]",
                     (unsigned)i, (unsigned)primn, BIGTMD_MAX_PRIM);
            return 0;
        }
        if (norn > BIGTMD_MAX_VERT)
        {
            snprintf(err, errCap, "obj%u norn=%u > %d", (unsigned)i, (unsigned)norn, BIGTMD_MAX_VERT);
            return 0;
        }

        /* Arrays must lie inside the file. Offsets are from the object table
         * (file + 12), matching the stub's `base + raw[n]` resolution. */
        if ((long)BIGTMD_HDR_SIZE + (long)vertop + (long)vern * 8 > fileSize ||
            (long)BIGTMD_HDR_SIZE + (long)nortop + (long)norn * 8 > fileSize)
        {
            snprintf(err, errCap, "obj%u vert/norm array past EOF", (unsigned)i);
            return 0;
        }

        /* Walk the packet chain: [olen][ilen][flag][mode] then ilen words. */
        p = (u32)BIGTMD_HDR_SIZE + primtop;
        for (k = 0; k < primn; k++)
        {
            u32 ilen;
            u32 total;

            if ((long)p + 4 > fileSize)
            {
                snprintf(err, errCap, "obj%u prim %u header past EOF", (unsigned)i, (unsigned)k);
                return 0;
            }
            ilen  = b[p + 1];
            total = 4u + ilen * 4u;
            if (ilen == 0 || (long)p + (long)total > fileSize)
            {
                snprintf(err, errCap, "obj%u prim %u (ilen=%u) past EOF",
                         (unsigned)i, (unsigned)k, (unsigned)ilen);
                return 0;
            }

            /* Packet SHAPE must match a handler the port's GsSortObject4J
             * dispatch actually strides correctly. Handlers advance by their
             * own struct size, so a lit-flagged packet — or a standard-layout
             * PSX export whose ilen differs from SH1's custom packet shapes —
             * mis-strides inside a batch and RotTransPers reads wild vertices
             * (the crash SHModelViewer's flag preprocessor works around).
             * Accept exactly the shapes shipped on disc: no-light (flag bit0
             * clear, bit1 double-sided allowed), modes 0x30/0x34/0x36 tris and
             * 0x38/0x3C quads at their SH1 ilens. */
            {
                u8  flag = b[p + 2];
                u8  mode = b[p + 3];
                u32 wantIlen;

                switch (mode)
                {
                    case 0x30: wantIlen = 4; break; /* untextured gouraud tri  */
                    case 0x34:                      /* textured gouraud tri    */
                    case 0x36: wantIlen = 6; break; /*   + ABE semi-transparent */
                    case 0x38: wantIlen = 5; break; /* untextured gouraud quad */
                    case 0x3C: wantIlen = 8; break; /* textured gouraud quad   */
                    default:   wantIlen = 0; break;
                }
                if ((flag & ~0x02u) != 0 || wantIlen == 0 || ilen != wantIlen)
                {
                    snprintf(err, errCap,
                             "obj%u prim %u: flag=0x%02x mode=0x%02x ilen=%u not an SH1 "
                             "no-light packet shape — re-export with light calc OFF",
                             (unsigned)i, (unsigned)k, (unsigned)flag, (unsigned)mode,
                             (unsigned)ilen);
                    return 0;
                }
            }
            p += total;
        }

        /* (b) the stub's own extent arithmetic, verbatim. */
        if (vertop  < dataStart) dataStart = vertop;
        if (nortop  < dataStart) dataStart = nortop;
        if (primtop < dataStart) dataStart = primtop;
        if (vertop  + vern  * 8u  > dataEnd) dataEnd = vertop  + vern  * 8u;
        if (nortop  + norn  * 8u  > dataEnd) dataEnd = nortop  + norn  * 8u;
        if (primtop + primn * 64u > dataEnd) dataEnd = primtop + primn * 64u;
    }

    copySize = (dataStart < dataEnd) ? (size_t)(dataEnd - dataStart) : 0;
    copySize += (size_t)nobj * BIGTMD_OBJ_SIZE;
    if (copySize == 0)
    {
        copySize = 4096;
    }

    /* The stub memcpy's copySize bytes starting at file+12, and separately
     * resolves pointers up to file+12+dataEnd. Cover both. */
    need = (size_t)BIGTMD_HDR_SIZE + (copySize > (size_t)dataEnd ? copySize : (size_t)dataEnd);
    if (need < (size_t)fileSize)
    {
        need = (size_t)fileSize;
    }
    *outNeed = need;
    return 1;
}

static PcBigTmd* BigTmd_Find(s32 fileIdx)
{
    int i;

    for (i = 0; i < s_bigTmdCount; i++)
    {
        if (s_bigTmd[i].fileIdx == (s16)fileIdx)
        {
            return &s_bigTmd[i];
        }
    }
    return NULL;
}

/* Probe + validate an oversized loose file for fileIdx, caching the verdict.
 * Returns NULL when the file is absent, fits its slot, or the table is full —
 * every one of those is a "leave the native path alone" answer. Never
 * allocates the substitute buffer; Redirect does that. */
static PcBigTmd* BigTmd_Ensure(s32 fileIdx, const char* path)
{
    long      fileSize;
    long      tableSize;
    PcBigTmd* e;
    char      err[160];
    u8*       tmp;
    size_t    need = 0;
    int       ok;

    fileSize  = BigTmd_ProbeSize(path);
    tableSize = (long)Fs_GetFileSectorAlignedSize(fileIdx);

    if (fileSize <= tableSize)
    {
        /* Absent, or fits the slot: the ordinary byte-replace path owns it and
         * behaviour is identical to today. THIS is the stock-path exit. */
        return NULL;
    }
    if (fileSize > BIGTMD_MAX_FILE)
    {
        SH_DBG("[BIGTMD] %s: %ld B exceeds %d B cap — native item model",
               path, fileSize, BIGTMD_MAX_FILE);
        return NULL;
    }

    e = BigTmd_Find(fileIdx);
    if (e != NULL && e->fileSize == fileSize)
    {
        return e; /* cached verdict, accepted or rejected */
    }

    /* New or size-changed file: re-validate before anything trusts it. */
    tmp = BigTmd_Slurp(path, fileSize);
    if (tmp == NULL)
    {
        snprintf(err, sizeof(err), "read failed");
        ok = 0;
    }
    else
    {
        ok = BigTmd_Validate(tmp, fileSize, &need, err, sizeof(err));
    }
    free(tmp);

    if (e == NULL)
    {
        if (s_bigTmdCount >= PC_BIGTMD_MAX)
        {
            SH_DBG("[BIGTMD] registry full (%d), ignoring %s — native item model",
                   PC_BIGTMD_MAX, path);
            return NULL;
        }
        e = &s_bigTmd[s_bigTmdCount++];
        memset(e, 0, sizeof(*e));
        e->fileIdx = (s16)fileIdx;
    }
    e->fileSize  = fileSize;
    e->needBytes = need;

    if (!ok)
    {
        e->state = BIGTMD_STATE_REJECTED;
        SH_DBG("[BIGTMD] reject: %s (%ld B): %s — native item model", path, fileSize, err);
        return e;
    }

    if (e->buf != NULL)
    {
        if (e->cap < need + PC_BIGTMD_TAIL_SLACK)
        {
            /* Grew: retire (do not free) the old buffer — a queue entry may
             * still name it as entry->data. */
            if (s_retiredCount < PC_BIGTMD_RETIRED_MAX)
            {
                s_retired[s_retiredCount].buf = e->buf;
                s_retired[s_retiredCount].cap = e->cap;
                s_retiredCount++;
            }
            else
            {
                SH_DBG("[BIGTMD] retired-buffer table full; %p untracked", (void*)e->buf);
            }
            e->buf = NULL;
            e->cap = 0;
        }
        else
        {
            /* Reused buffer: re-zero so the slack beyond the new file stays
             * zero after the queue's fread writes only fileSize bytes. */
            memset(e->buf, 0, e->cap);
        }
    }

    e->state = BIGTMD_STATE_ACCEPTED;
    SH_DBG("[BIGTMD] ACCEPT %s: %ld B (slot %ld B, need %zu B)",
           path, fileSize, tableSize, need);
    return e;
}

static void BigTmd_SetActive(void* nativeDest, u8* buf)
{
    int i;
    int freeSlot = -1;

    for (i = 0; i < PC_BIGTMD_ACTIVE_MAX; i++)
    {
        if (s_active[i].nativeDest == nativeDest)
        {
            s_active[i].buf = buf; /* buf == NULL clears the substitution */
            return;
        }
        if (s_active[i].nativeDest == NULL && freeSlot < 0)
        {
            freeSlot = i;
        }
    }
    if (buf == NULL)
    {
        return; /* nothing recorded, nothing to clear */
    }
    if (freeSlot < 0)
    {
        SH_DBG("[BIGTMD] active table full; %p not substituted", nativeDest);
        return;
    }
    s_active[freeSlot].nativeDest = nativeDest;
    s_active[freeSlot].buf        = buf;
}

void* Pc_BigTmd_Redirect(s32 fileIdx, void* dest)
{
    char      path[176];
    PcBigTmd* e;

    if (dest == NULL)
    {
        return dest;
    }

    /* Fast, allocation-free stock exit: loose files off (the default) means
     * this function is a pure identity with one boolean test. */
    if (!g_PcConfig.allowLooseFiles || fileIdx < 0 || fileIdx >= FS_FILE_COUNT)
    {
        BigTmd_SetActive(dest, NULL);
        return dest;
    }
    if (!BigTmd_LoosePath(fileIdx, path, sizeof(path)))
    {
        BigTmd_SetActive(dest, NULL);
        return dest;
    }

    e = BigTmd_Ensure(fileIdx, path);
    if (e == NULL || e->state != BIGTMD_STATE_ACCEPTED)
    {
        /* CLEAR: the previous item may have been redirected, and all ~78
         * UNQ*.TMD ids share this destination. Without this a stock item after
         * a modded one would resolve to the modded buffer and draw the wrong
         * (or a since-overwritten) model. */
        BigTmd_SetActive(dest, NULL);
        return dest;
    }

    if (e->buf == NULL)
    {
        size_t cap = e->needBytes + PC_BIGTMD_TAIL_SLACK;
        u8*    buf = (u8*)calloc(1, cap);

        if (buf == NULL)
        {
            SH_DBG("[BIGTMD] %s: calloc(%zu) failed — native item model", path, cap);
            BigTmd_SetActive(dest, NULL);
            return dest;
        }
        e->buf = buf;
        e->cap = cap;
        SH_DBG("[BIGTMD] REDIRECT %s (file %d) -> %p cap %zu", path, (int)fileIdx,
               (void*)e->buf, e->cap);
    }

    BigTmd_SetActive(dest, e->buf);
    return e->buf;
}

void* Pc_BigTmd_Resolve(void* nativeDest)
{
    int i;

    if (nativeDest == NULL)
    {
        return nativeDest;
    }
    for (i = 0; i < PC_BIGTMD_ACTIVE_MAX; i++)
    {
        if (s_active[i].nativeDest == nativeDest && s_active[i].buf != NULL)
        {
            return s_active[i].buf;
        }
    }
    return nativeDest;
}

size_t Pc_BigTmd_DestCapacity(const void* dest)
{
    int i;

    if (dest == NULL)
    {
        return 0;
    }
    for (i = 0; i < s_bigTmdCount; i++)
    {
        if (s_bigTmd[i].buf == dest && s_bigTmd[i].state == BIGTMD_STATE_ACCEPTED)
        {
            return s_bigTmd[i].cap;
        }
    }
    return 0;
}
