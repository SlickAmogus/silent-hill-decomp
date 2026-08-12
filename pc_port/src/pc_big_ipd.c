/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Oversized loose map chunks (the .IPD files under BG).
 *
 * A chunk slot is a fixed destination buffer: either a slice of the shared
 * 0x2C000 chunk buffer or a per-slot calloc (Ipd_ActiveChunksClear). A loose
 * IPD bigger than that slot cannot be byte-replaced — Fs_QueueTickRead's size
 * gate rejects it and the disc file loads instead, so an edited map silently
 * does nothing. That is the ceiling the level editor hits the moment it adds
 * geometry rather than only moving it.
 *
 * This grows the destination instead. Before a chunk read is enqueued, probe
 * the loose file for that file index; if it exceeds the slot's capacity, swap
 * chunk->ipdHdr for a PC-owned buffer big enough and register that capacity so
 * the queue's gate lets the whole file through and the reformatter bounds-checks
 * against the real buffer. Grown buffers are cached against the slot pointer
 * that needed them, so a map change resetting the slot to its default neither
 * leaks nor re-allocates.
 *
 * Growing also tightens safety: an oversized read into a shared slice would
 * have run into the neighbouring slice, and Fs_QueueDoBuffersOverlap sizes
 * entries from the file table, so it could not have seen that coming. A grown
 * buffer overlaps nothing.
 *
 * Everything here is additive: with no oversized loose file present every
 * lookup misses and the stock path runs untouched. Structural validation is
 * unchanged and still happens after load in IpdHeader_FixOffsets_PC, which
 * rejects a bad chunk into the caller's designed retry path. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "main/fileinfo.h"

#include "pc_big_ipd.h"
#include "pc_big_lm.h" /* Pc_LooseFOpen — same loose-file open semantics */
#include "sh_log.h"

/* One entry per distinct destination buffer. PC_MAX_IPD_CHUNKS (256) owned
 * slots, plus the handful of shared-buffer slice pointers, which differ per
 * active-chunk count (a 2-way slice and a 4-way slice put slot 1 at different
 * offsets), so leave headroom above 256. */
#define PC_BIGIPD_MAX_SLOTS 288

typedef struct
{
    const void* slot;     /* buffer Ipd_ActiveChunksClear assigned to the slot */
    size_t      slotCap;  /* what that buffer holds */
    void*       grown;    /* PC-owned replacement, NULL until one is needed */
    size_t      grownCap;
} PcBigIpdSlot;

static PcBigIpdSlot s_slots[PC_BIGIPD_MAX_SLOTS];
static s32          s_slotCount;

static PcBigIpdSlot* BigIpd_Find(const void* p)
{
    s32 i;

    if (p == NULL)
    {
        return NULL;
    }
    for (i = 0; i < s_slotCount; i++)
    {
        if (s_slots[i].slot == p)
        {
            return &s_slots[i];
        }
    }
    return NULL;
}

/* A grown buffer is itself a valid destination: once Ipd_LoadStart swaps it in,
 * every later lookup (the queue's size gate, the reformatter's bounds check)
 * arrives with the grown pointer, not the slot's original. */
static PcBigIpdSlot* BigIpd_FindGrown(const void* p)
{
    s32 i;

    if (p == NULL)
    {
        return NULL;
    }
    for (i = 0; i < s_slotCount; i++)
    {
        if (s_slots[i].grown == p)
        {
            return &s_slots[i];
        }
    }
    return NULL;
}

void Pc_BigIpd_RegisterSlot(const void* slot, size_t cap)
{
    PcBigIpdSlot* e;

    if (slot == NULL || cap == 0)
    {
        return;
    }

    e = BigIpd_Find(slot);
    if (e != NULL)
    {
        /* Same buffer, possibly re-sliced by a map with a different active
         * count: keep the larger capacity claim so the gate never over-reports
         * room that a smaller slice does not have. */
        if (cap < e->slotCap)
        {
            e->slotCap = cap;
        }
        return;
    }

    if (s_slotCount >= PC_BIGIPD_MAX_SLOTS)
    {
        return; /* stock behavior for the rest — no oversized support, no harm */
    }

    e           = &s_slots[s_slotCount++];
    e->slot     = slot;
    e->slotCap  = cap;
    e->grown    = NULL;
    e->grownCap = 0;
}

/* Build the loose probe path exactly as Fs_QueueTickRead does (its builder is
 * static to that TU; pc_big_lm.c duplicates it for the same reason). */
static int BigIpd_LoosePath(s32 fileIdx, char* out, size_t outSize)
{
    const s_FileInfo* file;
    const char*       folder;
    char              strippedFolder[16];
    char              nameBuf[32];
    size_t            fi;
    size_t            fl;

    if (fileIdx < 0 || fileIdx >= FS_FILE_COUNT)
    {
        return 0;
    }

    file   = &g_FileTable[fileIdx];
    folder = g_FilePaths[file->pathIdx];
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

static long BigIpd_ProbeSize(const char* path)
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

void Pc_BigIpd_EnsureCapacity(void** pDest, s32 fileIdx)
{
    char          path[176];
    long          fileSize;
    size_t        need;
    size_t        have;
    PcBigIpdSlot* e;

    if (pDest == NULL || *pDest == NULL)
    {
        return;
    }

    e = BigIpd_Find(*pDest);
    if (e == NULL)
    {
        /* Chunk slots are recycled by the streamer without going through
         * Ipd_ActiveChunksClear, so this slot may already be pointing at a
         * buffer grown for a previous cell. Resolve through that too — a
         * later, even larger cell in the same slot still has to grow. */
        e = BigIpd_FindGrown(*pDest);
        if (e == NULL)
        {
            return; /* not a chunk destination we know about */
        }
    }

    if (!BigIpd_LoosePath(fileIdx, path, sizeof(path)))
    {
        return;
    }

    /* Capacity of wherever the chunk currently points, which is the grown
     * buffer once one is in use. */
    have = (*pDest == e->grown) ? e->grownCap : e->slotCap;

    fileSize = BigIpd_ProbeSize(path);
    if (fileSize <= 0 || (size_t)fileSize <= have)
    {
        /* Absent, unreadable, or it fits where we already are — the stock
         * byte-replace path owns it. A smaller cell reusing a grown buffer is
         * fine: every offset it uses comes from its own freshly-read header. */
        return;
    }

    need = (size_t)fileSize + PC_BIGIPD_TAIL_SLACK;

    if (e->grown != NULL && e->grownCap >= need)
    {
        /* Reuse: re-zero so the slack past this (possibly smaller) file is not
         * the previous chunk's tail, which the reformatter could read. */
        memset(e->grown, 0, e->grownCap);
        *pDest = e->grown;
        return;
    }

    if (e->grown != NULL)
    {
        free(e->grown);
        e->grown    = NULL;
        e->grownCap = 0;
    }

    e->grown = calloc(1, need);
    if (e->grown == NULL)
    {
        SH_DBG("[BIGIPD] calloc %u B failed for %s — native slot, file ignored",
               (unsigned)need, path);
        return;
    }
    e->grownCap = need;
    *pDest      = e->grown;

    SH_DBG("[BIGIPD] grew chunk slot for %s: %ld B (slot held %u B)",
           path, fileSize, (unsigned)e->slotCap);
}

size_t Pc_BigIpd_DestCapacity(const void* dest)
{
    PcBigIpdSlot* e = BigIpd_FindGrown(dest);

    if (e != NULL)
    {
        return e->grownCap;
    }

    e = BigIpd_Find(dest);
    return (e != NULL) ? e->slotCap : 0;
}
