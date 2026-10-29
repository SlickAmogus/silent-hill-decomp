/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lm_endian.c - byte-swap an LM's mesh payload in place.
 *
 * lm_reformat.c converts the structs that CONTAIN POINTERS (s_LmHeader,
 * s_ModelHeader, s_MeshHeader, s_Material) from the PSX 32-bit layout to the
 * host one. It does not touch the payload those structs point at -- the vertex,
 * normal and primitive arrays are aimed straight into the raw disc buffer:
 *
 *     dst->primitives = (s_Primitive*)(base + rd32(&src[4]));
 *     dst->verticesXy = (DVECTOR*)   (base + rd32(&src[8]));
 *
 * On a little-endian host that is free and correct. On PS3/360 every s16 in it
 * is byte-reversed, and this is what kept the PS3 world invisible: the OT
 * contained ONLY code 0x38 (POLY_G4, untextured) with 83% of them thrown out by
 * the >=1024px oversize rule, because both the coordinates and the material
 * index were garbage. gt=0 and texbind=0 for the whole frame.
 *
 * WHAT MOVES, AND WHAT MUST NOT:
 *   verticesXy   vertexCount * (s16 vx, s16 vy)      swap each s16
 *   verticesZ    vertexCount * s16                   swap each s16
 *   primitives   primitiveCount * 20 bytes           swap the six u16s
 *   normals      s8 nx,ny,nz + u8 count              LEAVE -- all bytes
 *   unkPtr_14    per-vertex bytes                    LEAVE -- all bytes
 *
 * The six u16s in s_Primitive are swapped even though four of them (field_0,
 * field_4, field_8, field_A) are really PAIRS OF UV BYTES. That is deliberate:
 * after the swap every u16 member holds the same NUMERIC VALUE it holds on a
 * little-endian host, so all the existing field_6.flags bit tests (& 0x8000,
 * >> 15) keep working untouched, and the punned wide stores that feed the UVs
 * into POLY_GT4 become the ordinary PSX_ST_UV shape with an LE-composed word.
 * The alternative -- leaving the bytes alone -- would have made every one of
 * those call sites endian-specific instead.
 *
 * The bitfield group inside field_6 is a separate fix and lives in the header
 * (model.h), reversed under big-endian like the other groups in this project.
 */
#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "bodyprog/bodyprog.h"   /* s_MeshHeader / s_Primitive */
#include "lm_endian.h"
#include "sh_log.h"

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

/* One LM can hold a few hundred meshes; 4096 is far past any retail file and
 * costs 32KB of bss. On overflow we keep swapping rather than silently leaving
 * geometry in disc order -- a duplicate is possible in theory, missing geometry
 * is certain otherwise -- and say so once. */
#define LM_SEEN_MAX 4096
static const void* s_seen[LM_SEEN_MAX];
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

    if (s_seenN < LM_SEEN_MAX)
    {
        s_seen[s_seenN++] = p;
    }
    else if (!s_overflowLogged)
    {
        s_overflowLogged = 1;
        SH_DBG("[LM-ENDIAN] seen-set overflow at %d meshes; duplicates possible", LM_SEEN_MAX);
    }

    return 0;
}

void Lm_SwapPayloadBegin(void)
{
    s_seenN = 0;
}

void Lm_SwapMeshPayload(struct _MeshHeader* meshRaw)
{
    s_MeshHeader* mesh = (s_MeshHeader*)meshRaw;

    if (mesh == NULL)
        return;

    /* [LM-SWAP] probe: raw disc bytes of the first vertices BEFORE this pass
     * touches them, for the 81-vertex alley mesh the 360 draws first
     * (v0=(-1030,0) z0=1030 after the swap). Disc order is little-endian, so
     * -1030 must read "fa fb" here; "fb fa" would mean the array was already
     * swapped once (a double swap puts it back in disc order). */
    {
        static int s_probeN = 0;
        if (mesh->vertexCount == 81 && s_probeN < 4 && mesh->verticesXy != NULL && mesh->verticesZ != NULL)
        {
            const uint8_t* b = (const uint8_t*)mesh->verticesXy;
            const uint8_t* z = (const uint8_t*)mesh->verticesZ;
            int i, seenXy = 0, seenZ = 0;
            for (i = 0; i < s_seenN; i++) { if (s_seen[i] == mesh->verticesXy) seenXy = 1; if (s_seen[i] == mesh->verticesZ) seenZ = 1; }
            s_probeN++;
            SH_DBG("[LM-SWAP] n=81 xy=%p raw %02x%02x %02x%02x %02x%02x %02x%02x seen=%d | z=%p raw %02x%02x %02x%02x seen=%d prims=%d",
                   (void*)b, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], seenXy,
                   (void*)z, z[0], z[1], z[2], z[3], seenZ, (int)mesh->primitiveCount);
        }
    }

    if (mesh->verticesXy != NULL && !SeenOrAdd(mesh->verticesXy))
        SwapU16Array(mesh->verticesXy, mesh->vertexCount * 2);

    if (mesh->verticesZ != NULL && !SeenOrAdd(mesh->verticesZ))
        SwapU16Array(mesh->verticesZ, mesh->vertexCount);

    if (mesh->primitives != NULL && !SeenOrAdd(mesh->primitives))
    {
        uint8_t* p = (uint8_t*)mesh->primitives;
        int      i;

        for (i = 0; i < mesh->primitiveCount; i++)
        {
            /* Six u16s at 0x0..0xB; the two u8[4] tails at 0xC and 0x10 stay. */
            SwapU16Array(p + i * (int)sizeof(s_Primitive), 6);
        }
    }

    /* normals and unkPtr_14 are byte data -- nothing to convert. */
}

#else /* little-endian: the disc format is already host order */

void Lm_SwapPayloadBegin(void)                        { }
void Lm_SwapMeshPayload(struct _MeshHeader* meshRaw)  { (void)meshRaw; }

#endif
