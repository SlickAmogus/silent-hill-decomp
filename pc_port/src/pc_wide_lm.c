/* v7 high-poly ILM — detection + parse + registry + lifetime (no draw yet).
 *
 * Pc_WideLm_Parse is the v7 arm of LmHeader_FixOffsets_PC: it reformats the
 * narrow spine EXACTLY as the v6 path does (calloc'd 64-bit materials +
 * modelHdrs, meshCount forced to 0 so the stock walkers are no-ops), then
 * parses the PC-only u32 geometry blob into malloc'd per-part buffers and
 * registers them keyed by modelFileIdx. Lifetime mirrors pc_big_lm.c: a
 * re-parse of the same key frees the prior geometry first, so a live
 * s_CharaModel never dereferences freed data.
 *
 * On every failure path the spine is left fully formed but UNregistered:
 * meshCount=0 makes the stock walkers draw nothing, and Pc_WideLm_IsWide then
 * returns false — an invisible but stable, non-corrupting model. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_wide_lm.h"
#include "sh_log.h"

/* Wide-blob record layout (little-endian), mirroring _emit_wide_ilm:
 *   part  : ordinal:u32, boneIdx:u8 + 3 pad, meshCount:u32
 *   mesh  : vertexCount:u32, normalCount:u32, primCount:u32,
 *           verts[]:SVECTOR(8), normals[]:SVECTOR(8),
 *           prims[]: corner[4]:u32, normal[4]:u32, uv[4]:u16, clut:u16,
 *                    flags:u16, reserved:u32  (48 bytes total) */
#define WIDE_PART_HDR_SIZE 12
#define WIDE_MESH_HDR_SIZE 12
#define WIDE_SVECTOR_SIZE  8
#define WIDE_PRIM_SIZE     48
#define WIDE_TRI_SENTINEL  0xFFFFFFFFu

#define PSX_SIZEOF_MODEL_HEADER 16
#define PSX_SIZEOF_MATERIAL     24

enum
{
    WIDELM_STATE_ACCEPTED = 1,
    WIDELM_STATE_REJECTED = 2
};

typedef struct
{
    s16            fileIdx;     /* dedup/lifetime key; -1 when unknown */
    s_LmHeader*    lmHdr;       /* the reformatted spine header buffer */
    s_ModelHeader* spineModels; /* calloc'd modelHdrs base — IsWide range-tests this */
    u32            modelCount;
    s_WideLmPart*  parts;       /* [modelCount], indexed by part ordinal */
    u8             state;
} PcWideLm;

#define PC_WIDELM_MAX 64
static PcWideLm s_wideLm[PC_WIDELM_MAX];
static int      s_wideLmCount;

static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static inline u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

static void ParseMaterial(s_Material* dst, const u8* src)
{
    memcpy(&dst->name, src, 8);
    dst->texture    = NULL; /* set later by Lm_MaterialFileIdxApply */
    dst->field_C      = src[12];
    dst->unk_D[0]     = src[13];
    dst->field_E      = src[14];
    dst->field_F      = src[15];
    dst->field_10     = rd16(&src[16]);
    dst->field_12     = rd16(&src[18]);
    dst->field_14.u16 = rd16(&src[20]);
    dst->field_16.u16 = rd16(&src[22]);
}

/* Narrow-spine ModelHeader reformat, identical to lm_reformat.c's
 * ParseModelHeader EXCEPT meshCount is forced to 0: the wide drawer owns this
 * part's geometry, so the stock walkers must find no meshes to draw. */
static void ParseSpineModelHeader(s_ModelHeader* dst, const u8* src)
{
    u8 bf = src[11];

    memcpy(&dst->name, src, 8);
    dst->meshCount     = 0;
    dst->vertexOffset  = src[9];
    dst->normalOffset  = src[10];
    dst->field_B_0 = bf & 1;
    dst->field_B_1 = (bf >> 1) & 7;
    dst->field_B_4 = (bf >> 4) & 3;
    dst->unk_B_6   = (bf >> 6) & 3;
    dst->meshHdrs  = NULL;
}

static void WideLm_FreeParts(s_WideLmPart* parts, u32 count)
{
    u32 i;
    u32 j;

    if (parts == NULL)
    {
        return;
    }
    for (i = 0; i < count; i++)
    {
        s_WideLmPart* p = &parts[i];

        if (p->meshes != NULL)
        {
            for (j = 0; j < p->meshCount; j++)
            {
                free(p->meshes[j].verts);
                free(p->meshes[j].normals);
                free(p->meshes[j].prims);
            }
            free(p->meshes);
        }
    }
    free(parts);
}

static PcWideLm* WideLm_FindByKey(s32 fileIdx, const s_LmHeader* lmHdr)
{
    int i;

    for (i = 0; i < s_wideLmCount; i++)
    {
        if (fileIdx >= 0)
        {
            if (s_wideLm[i].fileIdx == (s16)fileIdx)
            {
                return &s_wideLm[i];
            }
        }
        else if (s_wideLm[i].lmHdr == lmHdr)
        {
            return &s_wideLm[i];
        }
    }
    return NULL;
}

/* Parse the wide u32 geometry blob into a calloc'd parts[modelCount] array,
 * bounds-checking every read against blobEnd. Returns the array on success,
 * NULL on any structural/allocation failure (caller leaves the spine standing).
 * The validator already proved these extents at BigLm_Ensure time; this is
 * defense-in-depth so a corrupt or unvalidated buffer degrades, never OOBs. */
static s_WideLmPart* WideLm_ParseBlob(const u8* raw, u32 blobOff, u32 blobEnd,
                                      u32 partCount, u32 modelCount)
{
    s_WideLmPart* parts;
    u32           c = blobOff;
    u32           pi;

    parts = (s_WideLmPart*)calloc(modelCount, sizeof(s_WideLmPart));
    if (parts == NULL)
    {
        return NULL;
    }

    for (pi = 0; pi < partCount; pi++)
    {
        u32           ordinal;
        u32           meshCount;
        u32           mi;
        s_WideLmPart* part;
        s_WideMesh*   meshes;

        if (c + WIDE_PART_HDR_SIZE > blobEnd)
        {
            goto fail;
        }
        ordinal   = rd32(&raw[c]);
        meshCount = rd32(&raw[c + 8]);

        /* Parts index by ordinal into the spine; converter emits ordinal == the
         * model index, so an out-of-range ordinal is a corrupt file. */
        if (ordinal >= modelCount)
        {
            goto fail;
        }
        part = &parts[ordinal];
        if (part->meshes != NULL)
        {
            goto fail; /* duplicate ordinal */
        }
        part->boneIdx   = raw[c + 4];
        part->meshCount = meshCount;
        c += WIDE_PART_HDR_SIZE;

        if (meshCount == 0)
        {
            part->meshes = NULL;
            continue;
        }
        meshes = (s_WideMesh*)calloc(meshCount, sizeof(s_WideMesh));
        if (meshes == NULL)
        {
            goto fail;
        }
        part->meshes = meshes;

        for (mi = 0; mi < meshCount; mi++)
        {
            s_WideMesh* mesh = &meshes[mi];
            u32         vc;
            u32         nc;
            u32         pc;
            u32         k;

            if (c + WIDE_MESH_HDR_SIZE > blobEnd)
            {
                goto fail;
            }
            vc = rd32(&raw[c]);
            nc = rd32(&raw[c + 4]);
            pc = rd32(&raw[c + 8]);
            c += WIDE_MESH_HDR_SIZE;

            mesh->vertexCount = vc;
            mesh->normalCount = nc;
            mesh->primCount   = pc;

            /* Extent-check each array before allocating, overflow-safe: the
             * divisions can never wrap because c <= blobEnd is an invariant. */
            if (vc > (blobEnd - c) / WIDE_SVECTOR_SIZE)
            {
                goto fail;
            }
            if (vc > 0)
            {
                mesh->verts = (SVECTOR*)malloc(vc * sizeof(SVECTOR));
                if (mesh->verts == NULL)
                {
                    goto fail;
                }
                for (k = 0; k < vc; k++)
                {
                    const u8* s = &raw[c + k * WIDE_SVECTOR_SIZE];
                    mesh->verts[k].vx  = (s16)rd16(&s[0]);
                    mesh->verts[k].vy  = (s16)rd16(&s[2]);
                    mesh->verts[k].vz  = (s16)rd16(&s[4]);
                    mesh->verts[k].pad = (s16)rd16(&s[6]);
                }
                c += vc * WIDE_SVECTOR_SIZE;
            }

            if (nc > (blobEnd - c) / WIDE_SVECTOR_SIZE)
            {
                goto fail;
            }
            if (nc > 0)
            {
                mesh->normals = (SVECTOR*)malloc(nc * sizeof(SVECTOR));
                if (mesh->normals == NULL)
                {
                    goto fail;
                }
                for (k = 0; k < nc; k++)
                {
                    const u8* s = &raw[c + k * WIDE_SVECTOR_SIZE];
                    mesh->normals[k].vx  = (s16)rd16(&s[0]);
                    mesh->normals[k].vy  = (s16)rd16(&s[2]);
                    mesh->normals[k].vz  = (s16)rd16(&s[4]);
                    mesh->normals[k].pad = (s16)rd16(&s[6]);
                }
                c += nc * WIDE_SVECTOR_SIZE;
            }

            if (pc > (blobEnd - c) / WIDE_PRIM_SIZE)
            {
                goto fail;
            }
            if (pc > 0)
            {
                mesh->prims = (s_WidePrim*)malloc(pc * sizeof(s_WidePrim));
                if (mesh->prims == NULL)
                {
                    goto fail;
                }
                for (k = 0; k < pc; k++)
                {
                    const u8*   s = &raw[c + k * WIDE_PRIM_SIZE];
                    s_WidePrim* pr = &mesh->prims[k];
                    u32         corners;
                    u32         ci;

                    pr->corner[0] = rd32(&s[0]);
                    pr->corner[1] = rd32(&s[4]);
                    pr->corner[2] = rd32(&s[8]);
                    pr->corner[3] = rd32(&s[12]);
                    pr->normal[0] = rd32(&s[16]);
                    pr->normal[1] = rd32(&s[20]);
                    pr->normal[2] = rd32(&s[24]);
                    pr->normal[3] = rd32(&s[28]);
                    pr->uv[0] = rd16(&s[32]);
                    pr->uv[1] = rd16(&s[34]);
                    pr->uv[2] = rd16(&s[36]);
                    pr->uv[3] = rd16(&s[38]);
                    pr->clut  = rd16(&s[40]);
                    pr->flags = rd16(&s[42]);

                    /* Corner indices must land in this mesh's own arrays; the
                     * tri sentinel in corner[3] is not an index. */
                    corners = (pr->corner[3] == WIDE_TRI_SENTINEL) ? 3 : 4;
                    for (ci = 0; ci < corners; ci++)
                    {
                        if (pr->corner[ci] >= vc || pr->normal[ci] >= nc)
                        {
                            goto fail;
                        }
                    }
                }
                c += pc * WIDE_PRIM_SIZE;
            }
        }
    }

    return parts;

fail:
    WideLm_FreeParts(parts, modelCount);
    return NULL;
}

void Pc_WideLm_Parse(s_LmHeader* lmHdr, size_t size, s32 modelFileIdx)
{
    u8*            raw = (u8*)lmHdr;
    u8             magic;
    u8             version;
    u8             matCount;
    u32            matOff;
    u8             modelCount;
    u32            modelHdrsOff;
    u32            modelOrderOff;
    u32            wideBlobOff;
    u32            wideBlobSize;
    u32            widePartCount;
    u32            blobEnd;
    s_Material*    mats = NULL;
    s_ModelHeader* models = NULL;
    s_WideLmPart*  parts;
    PcWideLm*      entry;
    int            i;

    /* Read every header field into locals BEFORE the 40-byte header finalize
     * below clobbers bytes 0..39 (the v7 extension at 20..31 included). */
    magic         = raw[0];
    version       = raw[1];
    matCount      = raw[3];
    matOff        = rd32(&raw[4]);
    modelCount    = raw[8];
    modelHdrsOff  = rd32(&raw[12]);
    modelOrderOff = rd32(&raw[16]);
    wideBlobOff   = rd32(&raw[20]);
    wideBlobSize  = rd32(&raw[24]);
    widePartCount = rd32(&raw[28]);

    if (magic != LM_HEADER_MAGIC || version != 7)
    {
        lmHdr->isLoaded = 1; /* not a v7 file after all; prevent re-entry */
        return;
    }

    /* --- Reformat the narrow spine (mirror LmHeader_FixOffsets_PC v6 path,
     *     meshCount forced 0). Materials are parsed BEFORE the header finalize
     *     because the CJ layout places materials at offset 0x20, whose first
     *     bytes the 40-byte memset would otherwise clobber. --- */
    if (matCount > 0)
    {
        mats = (s_Material*)calloc(matCount, sizeof(s_Material));
        if (mats != NULL)
        {
            for (i = 0; i < matCount; i++)
            {
                ParseMaterial(&mats[i], raw + matOff + (u32)i * PSX_SIZEOF_MATERIAL);
            }
        }
    }
    if (modelCount > 0)
    {
        models = (s_ModelHeader*)calloc(modelCount, sizeof(s_ModelHeader));
        if (models != NULL)
        {
            for (i = 0; i < modelCount; i++)
            {
                ParseSpineModelHeader(&models[i], raw + modelHdrsOff + (u32)i * PSX_SIZEOF_MODEL_HEADER);
            }
        }
    }

    memset(lmHdr, 0, sizeof(s_LmHeader));
    lmHdr->magic         = magic;
    lmHdr->version       = version;
    lmHdr->isLoaded      = 1;
    lmHdr->materialCount = matCount;
    lmHdr->materials     = mats;
    lmHdr->modelCount    = modelCount;
    lmHdr->modelHdrs     = models;
    lmHdr->modelOrder    = raw + modelOrderOff;

    /* --- Parse + register the wide geometry blob. Any failure below leaves
     *     the spine standing (invisible, meshCount=0) and does not register. --- */
    if (models == NULL || modelCount == 0)
    {
        SH_DBG("[WIDELM] file %d: spine has no models — not registering wide geometry", modelFileIdx);
        return;
    }

    /* size is the dest buffer capacity (Pc_BigLm_DestCapacity); 0 means the
     * buffer is un-boundable (a plain PSX-RAM slab or a retired buffer that
     * bigLm did not accept). An unknown ceiling is NOT license to skip the
     * check — WideLm_ParseBlob would then trust an attacker-controlled blobEnd
     * as its only read bound and OOB-read. Refuse: leave the spine standing
     * (invisible, non-corrupting), the design's always-fallback discipline. */
    blobEnd = wideBlobOff + wideBlobSize;
    if (size == 0 || wideBlobOff < sizeof(s_LmHeader) || blobEnd < wideBlobOff ||
        blobEnd > (u32)size)
    {
        SH_DBG("[WIDELM] file %d: wide blob out of range (off=0x%X size=0x%X cap=%zu) — spine only",
               modelFileIdx, wideBlobOff, wideBlobSize, size);
        return;
    }

    parts = WideLm_ParseBlob(raw, wideBlobOff, blobEnd, widePartCount, modelCount);
    if (parts == NULL)
    {
        SH_DBG("[WIDELM] file %d: wide blob parse failed — spine only (invisible)", modelFileIdx);
        return;
    }

    /* Dedup / retire: a re-parse of the same key frees the prior geometry so a
     * live s_CharaModel never dereferences freed data (mirrors pc_big_lm.c). */
    entry = WideLm_FindByKey(modelFileIdx, lmHdr);
    if (entry != NULL)
    {
        WideLm_FreeParts(entry->parts, entry->modelCount);
        entry->parts = NULL;
    }
    else
    {
        if (s_wideLmCount >= PC_WIDELM_MAX)
        {
            SH_DBG("[WIDELM] registry full (%d) — file %d spine only", PC_WIDELM_MAX, modelFileIdx);
            WideLm_FreeParts(parts, modelCount);
            return;
        }
        entry = &s_wideLm[s_wideLmCount++];
        memset(entry, 0, sizeof(*entry));
    }

    entry->fileIdx     = (s16)modelFileIdx;
    entry->lmHdr       = lmHdr;
    entry->spineModels = models;
    entry->modelCount  = modelCount;
    entry->parts       = parts;
    entry->state       = WIDELM_STATE_ACCEPTED;

    SH_DBG("[WIDELM] ACCEPT file %d: %u parts, wide blob 0x%X..0x%X (%u B)",
           modelFileIdx, widePartCount, wideBlobOff, blobEnd, wideBlobSize);
}

int Pc_WideLm_IsWide(const s_ModelHeader* modelHdr)
{
    int i;

    if (modelHdr == NULL)
    {
        return 0;
    }
    for (i = 0; i < s_wideLmCount; i++)
    {
        if (s_wideLm[i].state == WIDELM_STATE_ACCEPTED &&
            modelHdr >= s_wideLm[i].spineModels &&
            modelHdr <  s_wideLm[i].spineModels + s_wideLm[i].modelCount)
        {
            return 1;
        }
    }
    return 0;
}

s_WideLmPart* Pc_WideLm_PartOf(const s_ModelHeader* modelHdr)
{
    int i;

    if (modelHdr == NULL)
    {
        return NULL;
    }
    for (i = 0; i < s_wideLmCount; i++)
    {
        if (s_wideLm[i].state == WIDELM_STATE_ACCEPTED &&
            modelHdr >= s_wideLm[i].spineModels &&
            modelHdr <  s_wideLm[i].spineModels + s_wideLm[i].modelCount)
        {
            u32 idx = (u32)(modelHdr - s_wideLm[i].spineModels);
            return &s_wideLm[i].parts[idx];
        }
    }
    return NULL;
}

/* Clone of Model_MaterialFlagsApply (bodyprog_80055028.c:1496) over a wide
 * part's prims. The stock function walks modelHdr->meshHdrs, but a v7 spine has
 * meshCount==0, so the runtime tpage (prim field_6_0 = mat->field_E) and the
 * CLUT/UV rebases were NEVER baked into the wide prims — they kept the file's
 * pre-bake field_6==0, so every wide prim sampled VRAM page 0 (garbage: the
 * body vanished, feet showed noise). Called from Lm_MaterialFlagsApply at the
 * same point + timing (after the texture upload sets mat->field_E) as the stock
 * bake, so the wide prims get the identical material state stock prims do. */
void Pc_WideLm_ApplyMaterial(const s_ModelHeader* modelHdr, s32 matIdx, const s_Material* mat, s32 matFlags)
{
    s_WideLmPart* part = Pc_WideLm_PartOf(modelHdr);
    u32           mi;

    if (part == NULL)
    {
        return;
    }
    for (mi = 0; mi < part->meshCount; mi++)
    {
        s_WideMesh* mesh = &part->meshes[mi];
        u32         pi;

        for (pi = 0; pi < mesh->primCount; pi++)
        {
            s_WidePrim* prim = &mesh->prims[pi];
            s32         pmi  = (prim->flags >> 8) & 0x7F; /* materialIdx (s7) */

            if (pmi >= 0x40) { pmi -= 0x80; } /* sign-extend 7-bit; NO_VALUE == -1 */

            if (pmi == NO_VALUE)
            {
                prim->flags = (u16)((prim->flags & 0xFF00) | 32); /* field_6_0 = 32 */
            }
            if (pmi == matIdx)
            {
                if (matFlags & MaterialFlag_0)
                {
                    prim->flags = (u16)((prim->flags & 0xFF00) | (mat->field_E & 0xFF));
                }
                if (matFlags & MaterialFlag_1)
                {
                    prim->clut = (u16)(mat->field_10 + (prim->clut - mat->field_12));
                }
                if (matFlags & MaterialFlag_2)
                {
                    int k;
                    for (k = 0; k < 4; k++)
                    {
                        prim->uv[k] = (u16)(mat->field_14.u16 + (prim->uv[k] - mat->field_16.u16));
                    }
                }
            }
        }
    }
}

void Pc_WideLm_Free(s32 fileIdx)
{
    PcWideLm* e = WideLm_FindByKey(fileIdx, NULL);

    if (e == NULL)
    {
        return;
    }
    WideLm_FreeParts(e->parts, e->modelCount);
    e->parts = NULL;
    e->state = 0;
}
