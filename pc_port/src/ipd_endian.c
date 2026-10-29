/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ipd_endian.c - byte-swap the IPD arrays ipd_reformat.c does not parse.
 *
 * Same split as lm_endian.c: ipd_reformat.c converts the structs that CONTAIN
 * POINTERS and aims the leaf arrays straight into the raw disc buffer --
 *
 *     dst->field_10         = (SVECTOR*)(base + field10_off);
 *     dst->subcellPositions = (SVECTOR*)(base + field14_off);
 *
 * -- with the comment "raw data in buffer, no reformatting needed", which is
 * true only on a little-endian host.
 *
 * THIS IS WHAT KEPT THE PS3 STREET INVISIBLE. subcellPositions is the rectangle
 * Gfx_ChunkSubcellVisibleCheck tests the viewer against:
 *
 *     if (p->vx < subcellX && subcellX < p->vy && p->vz < subcellZ && subcellZ < p->pad)
 *
 * With every s16 byte-reversed those comparisons cannot succeed, so the check
 * returned false for EVERY model buffer the PVS selected. Measured as
 * "[IPD-DRAW] pvsRun=20 pvsVisible=0" -- twenty buffers offered per second,
 * none of them accepted -- while the chunks themselves were loaded and matched
 * (loaded=4 cellMatch=4 drawn=4). Only Harry and the snow reached the screen.
 *
 * field_10 is the billboard position array Ipd_ChunkDraw walks alongside it,
 * and has the same shape and the same problem.
 *
 * NOT handled here: the collision sub-arrays in ParseIpdCollisionData
 * (splitVertices, surfaces, subcells, subcellRanges, ptr_18/28/2C) are aimed at
 * the raw buffer in exactly the same way and need the same treatment before
 * collision is trustworthy. They are a separate, larger surface and are
 * deliberately left for their own pass rather than bundled in blind.
 */
#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "bodyprog/bodyprog.h"   /* s_IpdModelBuffer */
#include "ipd_endian.h"
#include "sh_log.h"

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

/* One IPD holds a few hundred model buffers at most; 4096 is far past any
 * retail file. On overflow keep converting rather than silently leaving
 * geometry in disc order -- a duplicate is possible in theory, invisible world
 * is certain otherwise -- and say so once. */
#define IPD_SEEN_MAX 4096
static const void* s_seen[IPD_SEEN_MAX];
static int         s_seenN;
static int         s_overflowLogged;

static void SwapU16Array(void* p, int n)
{
    uint8_t* b = (uint8_t*)p;
    int      i;

    for (i = 0; i < n; i++)
    {
        uint8_t t = b[i * 2];
        b[i * 2]     = b[i * 2 + 1];
        b[i * 2 + 1] = t;
    }
}

/* Returns 1 if this pointer was already converted in the current window. */
static int SeenOrAdd(const void* p)
{
    int i;

    if (p == NULL)
        return 1;

    for (i = 0; i < s_seenN; i++)
    {
        if (s_seen[i] == p)
            return 1;
    }

    if (s_seenN < IPD_SEEN_MAX)
    {
        s_seen[s_seenN++] = p;
    }
    else if (!s_overflowLogged)
    {
        s_overflowLogged = 1;
        SH_DBG("[IPD-ENDIAN] seen-set overflow at %d buffers; duplicates possible", IPD_SEEN_MAX);
    }

    return 0;
}

void Ipd_SwapPayloadBegin(void)
{
    s_seenN = 0;
}

void Ipd_SwapModelBufferPayload(struct _IpdModelBuffer* bufRaw)
{
    s_IpdModelBuffer* buf = (s_IpdModelBuffer*)bufRaw;

    if (buf == NULL)
        return;

    /* Both are SVECTOR arrays: four s16 each. subcellPositions uses them as a
     * rectangle (vx,vy = x range, vz,pad = z range), field_10 as a position
     * plus a type tag in `pad` -- the tag is a byte read through an s8 cast, so
     * swapping the halfword it lives in is what puts it back where the switch
     * in Ipd_ChunkDraw reads it. */
    if (buf->subcellPositions != NULL && buf->subcellCount > 0 &&
        !SeenOrAdd(buf->subcellPositions))
    {
        SwapU16Array(buf->subcellPositions, buf->subcellCount * 4);
    }

    if (buf->field_10 != NULL && buf->field_1 > 0 && !SeenOrAdd(buf->field_10))
    {
        SwapU16Array(buf->field_10, buf->field_1 * 4);
    }
}

static uint32_t Rd32LE(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* `count` elements of `stride` bytes; the first `words` u16s of each swapped.
 * Per element, never as a flat span: the structs end in u8 indices. */
static void SwapElems(void* base, int count, int stride, int words)
{
    uint8_t* p = (uint8_t*)base;
    int      i, w;

    if (base == NULL || count <= 0 || SeenOrAdd(base))
        return;
    for (i = 0; i < count; i++, p += stride)
    {
        for (w = 0; w < words; w++)
        {
            uint8_t t = p[w * 2]; p[w * 2] = p[w * 2 + 1]; p[w * 2 + 1] = t;
        }
    }
}

/* Elements of `stride` that fit between array k and the nearest array after
 * it (or the LM header): a header count can never swap into a neighbour. */
static int SpanElems(const uint32_t* offs, int n, int k, uint32_t cap, int stride)
{
    uint32_t end = cap;
    int      j;

    for (j = 0; j < n; j++)
    {
        if (j != k && offs[j] > offs[k] && offs[j] < end)
            end = offs[j];
    }
    if (end <= offs[k])
        return 0x7FFFFFFF;
    return (int)((end - offs[k]) / (uint32_t)stride);
}

/* Ported from the N64 port's IpdColl_SwapBigEndian. Without this Harry's
 * floor and walls are read byte-reversed: [COLL-MISS] everywhere, falling
 * through the alley ("fell off nothing"), phantom walls. ptr_28 / ptr_2C /
 * subcellCheckIdx are u8 arrays and stay. */
void Ipd_SwapCollisionPayload(struct _IpdCollisionData* collRaw, const unsigned char* collraw, unsigned lmHdrOff)
{
    s_IpdCollisionData* coll = (s_IpdCollisionData*)collRaw;
    uint32_t            offs[7];
    uint32_t            cap = (lmHdrOff > 0x54) ? (lmHdrOff - 0x54) : 0xFFFFFFFFu;
    int                 n, m;

    if (coll == NULL || collraw == NULL)
        return;

    offs[0] = Rd32LE(&collraw[0x0C]);
    offs[1] = Rd32LE(&collraw[0x10]);
    offs[2] = Rd32LE(&collraw[0x14]);
    offs[3] = Rd32LE(&collraw[0x18]);
    offs[4] = Rd32LE(&collraw[0x20]);
    offs[5] = Rd32LE(&collraw[0x28]);
    offs[6] = Rd32LE(&collraw[0x2C]);

    /* Offset 0 = absent array (the pointer would be the struct itself). */
    if (offs[0])
    {
        n = (int)coll->splitVertexCount; m = SpanElems(offs, 7, 0, cap, 6);
        SwapElems(coll->splitVertices, n < m ? n : m, 6, 3);
    }
    if (offs[1])
    {
        n = (int)coll->surfaceCount; m = SpanElems(offs, 7, 1, cap, 12);
        SwapElems(coll->surfaces, n < m ? n : m, 12, 6);
    }
    if (offs[2])
    {
        n = (int)coll->subcellCount; m = SpanElems(offs, 7, 2, cap, 10);
        SwapElems(coll->subcells, n < m ? n : m, 10, 3);
    }
    if (offs[3])
    {
        n = (int)coll->field_8_24; m = SpanElems(offs, 7, 3, cap, 10);
        SwapElems(coll->ptr_18, n < m ? n : m, 10, 5);
    }
    if (offs[4])
    {
        /* The grid walk reads one range past the last cell (func_8006B1C8
         * looks at [1] of the current cell). */
        n = (int)coll->subcellCountX * (int)coll->subcellCountZ + 1;
        m = SpanElems(offs, 7, 4, cap, 4);
        SwapElems(coll->subcellRanges, n < m ? n : m, 4, 2);
    }

    {
        static int s_logged;
        if (s_logged < 2 && coll->surfaceCount > 0 && coll->surfaces != NULL)
        {
            s_IpdCollSurface* s = coll->surfaces;
            s_logged++;
            SH_DBG("[IPD-COLL] swapped: verts=%d surfaces=%d subcells=%d round=%d grid=%dx%d | surf0 x=%d h=%d z=%d type=%d tilt=(%d,%d)",
                   (int)coll->splitVertexCount, (int)coll->surfaceCount, (int)coll->subcellCount,
                   (int)coll->field_8_24, (int)coll->subcellCountX, (int)coll->subcellCountZ,
                   (int)s->field_0, (int)s->baseGroundHeight, (int)s->field_4, (int)s->groundType,
                   (int)s->tiltAngleX, (int)s->tiltAngleZ);
        }
    }
}

#else /* little-endian: the disc format is already host order */

void Ipd_SwapPayloadBegin(void)                                  { }
void Ipd_SwapModelBufferPayload(struct _IpdModelBuffer* bufRaw)  { (void)bufRaw; }
void Ipd_SwapCollisionPayload(struct _IpdCollisionData* coll, const unsigned char* collraw, unsigned lmHdrOff)
{
    (void)coll; (void)collraw; (void)lmHdrOff;
}

#endif
