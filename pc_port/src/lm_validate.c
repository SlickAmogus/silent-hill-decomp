/* lm_validate.c - raw ILM/PLM validator shared by pc_big_lm.c, lm_reformat.c,
 * the offline harness and the converter's Python twin (the rule table in the
 * Stage 1 spec is normative for both languages).
 *
 * Parses the PSX 20/16/24/20-byte structs itself with the exact byte reads
 * lm_reformat.c uses: the caller's buffer is pre-fix-up file bytes, and the
 * offline harness only ever has raw files. No game headers, no logging —
 * failures go into the caller's message buffer.
 *
 * The V9 slack rule is buffer slack, not in-file slack: stock files pack
 * arrays back-to-back and may end an array flush at EOF (BFLU.ILM), which
 * shipped hardware over-read every frame. The only real hazard is the strided
 * GTE copies crossing the destination BUFFER end, so the rule is
 * fileSize + tailSlackBytes - arrayEnd, with runtime callers passing the F1
 * +16 malloc tail and authoring tools passing 0 (they emit real pad bytes). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lm_validate.h"

#define LMV_SIZEOF_HEADER   20
#define LMV_SIZEOF_MATERIAL 24
#define LMV_SIZEOF_MODEL    16
#define LMV_SIZEOF_MESH     24
#define LMV_SIZEOF_PRIM     20
#define LMV_SIZEOF_NORMAL   4

#define LMV_RD32(p) ((u32)(p)[0] | ((u32)(p)[1] << 8) | ((u32)(p)[2] << 16) | ((u32)(p)[3] << 24))

#define LMV_FAIL(id, ...)                    \
    do                                       \
    {                                        \
        if (err != NULL && errCap > 0)       \
        {                                    \
            snprintf(err, errCap, __VA_ARGS__); \
        }                                    \
        return (id);                         \
    } while (0)

/* Overflow-safe extent test: a crafted 0xFFFFFFF0 offset must not wrap.
 * count is a u8 and elemSize <= 24, so len itself cannot overflow. */
static int LmvExtentOk(u32 off, u32 count, u32 elemSize, u32 fileSize)
{
    u32 len = count * elemSize;

    return off <= fileSize && len <= fileSize - off;
}

/* Per-mesh array descriptors for the V2 extent/alignment sweep and V9 slack. */
typedef struct
{
    const char* name;
    u32         off;
    u32         count;
    u32         elemSize;
    u32         minSlack; /* V9; 0 = exempt (prims are not stride-copied) */
} LmvArray;

static u32 LmvMeshArrays(const u8* mesh, LmvArray out[5])
{
    out[0].name = "primitives"; out[0].off = LMV_RD32(&mesh[4]);  out[0].count = mesh[0]; out[0].elemSize = LMV_SIZEOF_PRIM;   out[0].minSlack = 0;
    out[1].name = "verticesXy"; out[1].off = LMV_RD32(&mesh[8]);  out[1].count = mesh[1]; out[1].elemSize = 4;                 out[1].minSlack = LM_VALIDATE_ARRAY_SLACK;
    out[2].name = "verticesZ";  out[2].off = LMV_RD32(&mesh[12]); out[2].count = mesh[1]; out[2].elemSize = 2;                 out[2].minSlack = LM_VALIDATE_ARRAY_SLACK;
    out[3].name = "normals";    out[3].off = LMV_RD32(&mesh[16]); out[3].count = mesh[2]; out[3].elemSize = LMV_SIZEOF_NORMAL; out[3].minSlack = LM_VALIDATE_ARRAY_SLACK;
    out[4].name = "shade";      out[4].off = LMV_RD32(&mesh[20]); out[4].count = mesh[3]; out[4].elemSize = 1;                 out[4].minSlack = LM_VALIDATE_SHADE_SLACK;
    return 5;
}

/* v7 wide-blob record sizes (little-endian), mirroring _emit_wide_ilm. */
#define LMV_V7_HEADER      32 /* LmHeader(20) + v7 extension(12) */
#define LMV_V7_PART_HDR    12 /* ordinal:u32, bone:u8+3pad, meshCount:u32 */
#define LMV_V7_MESH_HDR    12 /* vertexCount/normalCount/primCount u32 */
#define LMV_V7_SVECTOR     8
#define LMV_V7_PRIM        48
#define LMV_V7_TRI_SENT    0xFFFFFFFFu
#define LMV_V7_ANIM_BONES  32 /* activeBones u32 mask cap (skeletal only) */

/* v7 high-poly ruleset. Structural + the rules that still apply to wide parts:
 * keeps V3 (<=56 parts), V4 (modelOrder in range), V13 (bone-prefix < ANM
 * bones) and the <=32 animatable-bones mask; drops the <=256 pool-window rules
 * (V5-V8/V11 — wide parts never enter the T8 scratch pool); adds a per-prim
 * corner < mesh count check (the wide equivalent of V11's dangling-corner
 * guard, but bounded by the part's OWN u32 vertex/normal arrays). */
static int LmvValidateV7(const u8* file, u32 fileSize, const s_LmValidateParams* p,
                         char* err, u32 errCap)
{
    u32 matCount;
    u32 matOff;
    u32 modelCount;
    u32 modelHdrsOff;
    u32 modelOrderOff;
    u32 wideBlobOff;
    u32 wideBlobSize;
    u32 widePartCount;
    u32 blobEnd;
    u32 c;
    u32 i;

    if (fileSize < LMV_V7_HEADER || file[0] != 0x30 || file[1] != 7 || file[2] != 0)
    {
        LMV_FAIL(1, "V1: not a raw v7 LM file (magic=0x%02X ver=%d isLoaded=%d size=%u)",
                 fileSize > 0 ? file[0] : 0,
                 fileSize > 1 ? file[1] : 0,
                 fileSize > 2 ? file[2] : 0,
                 fileSize);
    }

    matCount      = file[3];
    matOff        = LMV_RD32(&file[4]);
    modelCount    = file[8];
    modelHdrsOff  = LMV_RD32(&file[12]);
    modelOrderOff = LMV_RD32(&file[16]);
    wideBlobOff   = LMV_RD32(&file[20]);
    wideBlobSize  = LMV_RD32(&file[24]);
    widePartCount = LMV_RD32(&file[28]);

    /* V2 — spine sections + the wide blob within the file. Spine strides are
     * the stock 24/16/1 byte layouts (the game binds the spine unchanged). */
    if (!LmvExtentOk(matOff, matCount, LMV_SIZEOF_MATERIAL, fileSize))
    {
        LMV_FAIL(2, "V2: materials section out of file (off=0x%X len=0x%X size=0x%X)",
                 matOff, matCount * LMV_SIZEOF_MATERIAL, fileSize);
    }
    if (!LmvExtentOk(modelHdrsOff, modelCount, LMV_SIZEOF_MODEL, fileSize))
    {
        LMV_FAIL(2, "V2: modelHdrs section out of file (off=0x%X len=0x%X size=0x%X)",
                 modelHdrsOff, modelCount * LMV_SIZEOF_MODEL, fileSize);
    }
    if (!LmvExtentOk(modelOrderOff, modelCount, 1, fileSize))
    {
        LMV_FAIL(2, "V2: modelOrder section out of file (off=0x%X len=0x%X size=0x%X)",
                 modelOrderOff, modelCount, fileSize);
    }
    if (wideBlobOff > fileSize || wideBlobSize > fileSize - wideBlobOff)
    {
        LMV_FAIL(2, "V2: wide blob out of file (off=0x%X size=0x%X file=0x%X)",
                 wideBlobOff, wideBlobSize, fileSize);
    }

    /* V3 — Skeleton_BoneModelAssign writes bones_C[56] unchecked. */
    if (p->skeletal && modelCount > LM_VALIDATE_MAX_PARTS)
    {
        LMV_FAIL(3, "V3: %u parts exceeds skeleton cap %d", modelCount, LM_VALIDATE_MAX_PARTS);
    }

    /* V4 — every modelOrder byte indexes modelHdrs. */
    for (i = 0; i < modelCount; i++)
    {
        u32 order = file[modelOrderOff + i];

        if (order >= modelCount)
        {
            LMV_FAIL(4, "V4: modelOrder[%u]=%u out of range (%u parts)", i, order, modelCount);
        }
    }

    /* V12 — the wide drawer implements only the LIT chain (field_B_0==1 &&
     * field_B_4==0). A part selecting the flat chain (field_B_0==0) or the
     * third chain (field_B_4!=0) would be drawn lit with wrong shading, so
     * reject it and fall back to the invisible-but-stable spine. Mirrors the
     * v6 V12 offset rule, gating the same draw-chain bits the drawer assumes. */
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       fieldB0 = mh[11] & 1;
        u32       fieldB4 = (mh[11] >> 4) & 3;

        if (fieldB0 == 0 || fieldB4 != 0)
        {
            LMV_FAIL(12, "V12: part %.8s field_B_0=%u field_B_4=%u — wide drawer implements only the lit chain",
                     (const char*)mh, fieldB0, fieldB4);
        }
    }

    /* V13 — bone-prefix bound to the ANM (skipped when unknown, as v6 does),
     * plus the <=32 animatable-bones mask cap that holds regardless. */
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;

        if (mh[0] >= '0' && mh[0] <= '9' && mh[1] >= '0' && mh[1] <= '9')
        {
            s32 boneIdx = (mh[0] - '0') * 10 + (mh[1] - '0');

            if (p->skeletal && boneIdx >= LMV_V7_ANIM_BONES)
            {
                LMV_FAIL(13, "V13: part %.8s binds bone %d but the activeBones mask holds %d",
                         (const char*)mh, boneIdx, LMV_V7_ANIM_BONES);
            }
            if (p->anmBoneCount > 0 && boneIdx >= p->anmBoneCount)
            {
                LMV_FAIL(13, "V13: part %.8s binds bone %d but the ANM has %d bones",
                         (const char*)mh, boneIdx, p->anmBoneCount);
            }
        }
    }

    /* V14 — walk the wide blob: every record fits, every prim corner indexes
     * its own mesh's u32 vertex/normal arrays. */
    c = wideBlobOff;
    blobEnd = wideBlobOff + wideBlobSize;
    for (i = 0; i < widePartCount; i++)
    {
        u32 ordinal;
        u32 meshCount;
        u32 mi;

        if (c + LMV_V7_PART_HDR > blobEnd)
        {
            LMV_FAIL(14, "V14: part %u header runs past wide blob (off=0x%X end=0x%X)", i, c, blobEnd);
        }
        ordinal   = LMV_RD32(&file[c]);
        meshCount = LMV_RD32(&file[c + 8]);
        if (ordinal >= modelCount)
        {
            LMV_FAIL(14, "V14: wide part %u ordinal %u out of range (%u parts)", i, ordinal, modelCount);
        }
        c += LMV_V7_PART_HDR;

        for (mi = 0; mi < meshCount; mi++)
        {
            u32 vc;
            u32 nc;
            u32 pc;
            u32 k;

            if (c + LMV_V7_MESH_HDR > blobEnd)
            {
                LMV_FAIL(14, "V14: part %u mesh %u header past wide blob", ordinal, mi);
            }
            vc = LMV_RD32(&file[c]);
            nc = LMV_RD32(&file[c + 4]);
            pc = LMV_RD32(&file[c + 8]);
            c += LMV_V7_MESH_HDR;

            if (vc > (blobEnd - c) / LMV_V7_SVECTOR)
            {
                LMV_FAIL(14, "V14: part %u mesh %u verts (%u) past wide blob", ordinal, mi, vc);
            }
            c += vc * LMV_V7_SVECTOR;
            if (nc > (blobEnd - c) / LMV_V7_SVECTOR)
            {
                LMV_FAIL(14, "V14: part %u mesh %u normals (%u) past wide blob", ordinal, mi, nc);
            }
            c += nc * LMV_V7_SVECTOR;
            if (pc > (blobEnd - c) / LMV_V7_PRIM)
            {
                LMV_FAIL(14, "V14: part %u mesh %u prims (%u) past wide blob", ordinal, mi, pc);
            }
            for (k = 0; k < pc; k++)
            {
                const u8* prim    = file + c + k * LMV_V7_PRIM;
                u32       corners = LMV_RD32(&prim[12]) == LMV_V7_TRI_SENT ? 3 : 4;
                u32       ci;

                for (ci = 0; ci < corners; ci++)
                {
                    u32 corner = LMV_RD32(&prim[ci * 4]);
                    u32 normal = LMV_RD32(&prim[16 + ci * 4]);

                    if (corner >= vc)
                    {
                        LMV_FAIL(14, "V14: part %u prim %u corner %u reads vertex %u of %u",
                                 ordinal, k, ci, corner, vc);
                    }
                    if (normal >= nc)
                    {
                        LMV_FAIL(14, "V14: part %u prim %u corner %u reads normal %u of %u",
                                 ordinal, k, ci, normal, nc);
                    }
                }
            }
            c += pc * LMV_V7_PRIM;
        }
    }

    return 0;
}

int Lm_Validate(const u8* file, u32 fileSize, const s_LmValidateParams* p,
                char* err, u32 errCap)
{
    u32 matCount;
    u32 matOff;
    u32 modelCount;
    u32 modelHdrsOff;
    u32 modelOrderOff;
    u32 i;
    u32 j;
    u32 k;
    u32 vertSlotCap;

    /* v7 high-poly ILMs have an entirely different geometry layout; stock v6
     * (version 0 defaults here too) falls through to the pool-window ruleset. */
    if (p->version == 7)
    {
        return LmvValidateV7(file, fileSize, p, err, errCap);
    }

    /* V1 — header. isLoaded must be 0: lm_reformat.c:106-109 skips fix-up on 1
     * and the runtime would then dereference file offsets as pointers. */
    if (fileSize < LMV_SIZEOF_HEADER || file[0] != 0x30 || file[1] != 6 || file[2] != 0)
    {
        LMV_FAIL(1, "V1: not a raw LM file (magic=0x%02X ver=%d isLoaded=%d size=%u)",
                 fileSize > 0 ? file[0] : 0,
                 fileSize > 1 ? file[1] : 0,
                 fileSize > 2 ? file[2] : 0,
                 fileSize);
    }

    matCount      = file[3];
    matOff        = LMV_RD32(&file[4]);
    modelCount    = file[8];
    modelHdrsOff  = LMV_RD32(&file[12]);
    modelOrderOff = LMV_RD32(&file[16]);

    /* V2 — every section extent within the file, strided arrays 4-aligned
     * (the walkers copy them with u32 strides). Zero-count arrays are exempt:
     * their pointers are never dereferenced. Checked file-level sections
     * first so the per-part sweep below reads only proven-in-file bytes. */
    if (!LmvExtentOk(matOff, matCount, LMV_SIZEOF_MATERIAL, fileSize))
    {
        LMV_FAIL(2, "V2: materials section out of file (off=0x%X len=0x%X size=0x%X)",
                 matOff, matCount * LMV_SIZEOF_MATERIAL, fileSize);
    }
    if (!LmvExtentOk(modelHdrsOff, modelCount, LMV_SIZEOF_MODEL, fileSize))
    {
        LMV_FAIL(2, "V2: modelHdrs section out of file (off=0x%X len=0x%X size=0x%X)",
                 modelHdrsOff, modelCount * LMV_SIZEOF_MODEL, fileSize);
    }
    if (!LmvExtentOk(modelOrderOff, modelCount, 1, fileSize))
    {
        LMV_FAIL(2, "V2: modelOrder section out of file (off=0x%X len=0x%X size=0x%X)",
                 modelOrderOff, modelCount, fileSize);
    }
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh        = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       meshCount = mh[8];
        u32       meshOff   = LMV_RD32(&mh[12]);

        if (!LmvExtentOk(meshOff, meshCount, LMV_SIZEOF_MESH, fileSize))
        {
            LMV_FAIL(2, "V2: part %.8s meshHdrs section out of file (off=0x%X len=0x%X size=0x%X)",
                     (const char*)mh, meshOff, meshCount * LMV_SIZEOF_MESH, fileSize);
        }
        for (j = 0; j < meshCount; j++)
        {
            LmvArray  arr[5];
            u32       arrCount = LmvMeshArrays(file + meshOff + j * LMV_SIZEOF_MESH, arr);

            for (k = 0; k < arrCount; k++)
            {
                if (arr[k].count == 0)
                {
                    continue;
                }
                if (!LmvExtentOk(arr[k].off, arr[k].count, arr[k].elemSize, fileSize))
                {
                    LMV_FAIL(2, "V2: part %.8s mesh %u %s section out of file (off=0x%X len=0x%X size=0x%X)",
                             (const char*)mh, j, arr[k].name,
                             arr[k].off, arr[k].count * arr[k].elemSize, fileSize);
                }
                if ((arr[k].off & 3) != 0)
                {
                    LMV_FAIL(2, "V2: part %.8s mesh %u %s section misaligned (off=0x%X)",
                             (const char*)mh, j, arr[k].name, arr[k].off);
                }
            }
        }
    }

    /* V3 — Skeleton_BoneModelAssign writes bones_C[56] unchecked (F7); props
     * never enter Skeleton_Init (RSR_GLB.PLM legally has 61 parts). */
    if (p->skeletal && modelCount > LM_VALIDATE_MAX_PARTS)
    {
        LMV_FAIL(3, "V3: %u parts exceeds skeleton cap %d", modelCount, LM_VALIDATE_MAX_PARTS);
    }

    /* V4 — every modelOrder byte indexes modelHdrs. Repeats are tolerated by
     * the runtime (a repeat draws a part twice), so no permutation check. */
    for (i = 0; i < modelCount; i++)
    {
        u32 order = file[modelOrderOff + i];

        if (order >= modelCount)
        {
            LMV_FAIL(4, "V4: modelOrder[%u]=%u out of range (%u parts)", i, order, modelCount);
        }
    }

    /* V5-V8 — window rules keeping every pool write inside the T8 scratch
     * CAPs (3-stride writes <= 256+2 = 258; 4-stride shade copy <= 255+3 <=
     * 261). Skeletal V5 caps at 255: a file that never writes slot 255 and
     * has no dangling refs (V11) cannot reference it, which is what makes the
     * quad-corner-3 0xFF sentinel collision structurally impossible.
     * Separate passes per rule: first failure wins in NUMERIC rule order,
     * so every V5 site is reported before any V6 site, and so on. */
    vertSlotCap = p->skeletal ? LM_VALIDATE_VERT_SLOT_CAP : LM_VALIDATE_POOL_SLOTS;
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       vertOff = mh[9];
        u32       meshOff = LMV_RD32(&mh[12]);

        for (j = 0; j < mh[8]; j++)
        {
            u32 vc = (file + meshOff + j * LMV_SIZEOF_MESH)[1];

            if (vertOff + vc > vertSlotCap)
            {
                LMV_FAIL(5, "V5: part %.8s mesh %u vertex window %u+%u exceeds %u",
                         (const char*)mh, j, vertOff, vc, vertSlotCap);
            }
        }
    }
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       normOff = mh[10];
        u32       meshOff = LMV_RD32(&mh[12]);

        for (j = 0; j < mh[8]; j++)
        {
            u32 nc = (file + meshOff + j * LMV_SIZEOF_MESH)[2];

            if (normOff + nc > LM_VALIDATE_POOL_SLOTS)
            {
                LMV_FAIL(6, "V6: part %.8s mesh %u normal window %u+%u exceeds %d",
                         (const char*)mh, j, normOff, nc, LM_VALIDATE_POOL_SLOTS);
            }
        }
    }
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       normOff = mh[10];
        u32       meshOff = LMV_RD32(&mh[12]);

        for (j = 0; j < mh[8]; j++)
        {
            u32 shadeCount = (file + meshOff + j * LMV_SIZEOF_MESH)[3];

            if (normOff + shadeCount > LM_VALIDATE_POOL_SLOTS)
            {
                LMV_FAIL(7, "V7: part %.8s mesh %u shade window %u+%u exceeds %d",
                         (const char*)mh, j, normOff, shadeCount, LM_VALIDATE_POOL_SLOTS);
            }
        }
    }
    /* V8 checked independently of V7: unkCount_3==0 with a nonzero count-byte
     * sum is legal, shipped and universal (all of HERO) — the env-mode-1/2
     * walk then consumes stale pool bytes, which is stock behavior. Shade
     * byte VALUES need no rule: any u8 read of the 258-slot pool is in
     * bounds. */
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       normOff = mh[10];
        u32       meshOff = LMV_RD32(&mh[12]);

        for (j = 0; j < mh[8]; j++)
        {
            const u8* mesh      = file + meshOff + j * LMV_SIZEOF_MESH;
            u32       normArr   = LMV_RD32(&mesh[16]);
            u32       shadeWalk = 0;

            for (k = 0; k < mesh[2]; k++)
            {
                /* env mode 1 (func_80057658) advances a slot even for a
                 * count-byte 0: `while (++var_t0 < end)` skips the body but
                 * the post-loop `*(var_t0 - 1)` still writes. Per-normal
                 * advance is max(count,1), so a 0 costs a slot, not none. */
                u32 count = file[normArr + k * LMV_SIZEOF_NORMAL + 3];

                shadeWalk += count ? count : 1;
            }
            if (normOff + shadeWalk > LM_VALIDATE_POOL_SLOTS)
            {
                LMV_FAIL(8, "V8: part %.8s mesh %u shade walk %u+%u exceeds %d",
                         (const char*)mh, j, normOff, shadeWalk, LM_VALIDATE_POOL_SLOTS);
            }
        }
    }

    /* V9 — buffer slack for the strided over-reads (see file comment). */
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh        = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       meshCount = mh[8];
        u32       meshOff   = LMV_RD32(&mh[12]);

        for (j = 0; j < meshCount; j++)
        {
            LmvArray arr[5];
            u32      arrCount = LmvMeshArrays(file + meshOff + j * LMV_SIZEOF_MESH, arr);

            for (k = 0; k < arrCount; k++)
            {
                u32 end;
                u32 slack;

                if (arr[k].count == 0 || arr[k].minSlack == 0)
                {
                    continue;
                }
                end   = arr[k].off + arr[k].count * arr[k].elemSize; /* <= fileSize by V2 */
                slack = fileSize + p->tailSlackBytes - end;
                if (slack < arr[k].minSlack)
                {
                    LMV_FAIL(9, "V9: part %.8s mesh %u %s array needs %u slack bytes, has %u",
                             (const char*)mh, j, arr[k].name, arr[k].minSlack, slack);
                }
            }
        }
    }

    /* V10 — structural, no scan possible (see header comment). */

    /* V11 — skeletal only: replay modelOrder, stamp each part's windows, then
     * require its prim corners to read stamped slots (the walker copies then
     * draws; the window does not advance between meshes). Held-item PLMs
     * resolve corners against the WIELDER's pool left in scratch at draw time
     * (HANDGUN.PLM has 5 dangling corners by design), hence the gate. */
    if (p->skeletal)
    {
        u8 vstamp[LM_VALIDATE_POOL_SLOTS];
        u8 nstamp[LM_VALIDATE_POOL_SLOTS];

        memset(vstamp, 0, sizeof(vstamp));
        memset(nstamp, 0, sizeof(nstamp));

        for (i = 0; i < modelCount; i++)
        {
            const u8* mh = file + modelHdrsOff + file[modelOrderOff + i] * LMV_SIZEOF_MODEL;
            u32       meshCount;
            u32       vertOff;
            u32       normOff;
            u32       meshOff;
            u32       primIdx;

            meshCount = mh[8];
            vertOff   = mh[9];
            normOff   = mh[10];
            meshOff   = LMV_RD32(&mh[12]);

            for (j = 0; j < meshCount; j++)
            {
                const u8* mesh = file + meshOff + j * LMV_SIZEOF_MESH;

                /* Windows proven <= 256 by V5/V6, so the stamps stay in bounds. */
                memset(&vstamp[vertOff], 1, mesh[1]);
                memset(&nstamp[normOff], 1, mesh[2]);
            }

            primIdx = 0;
            for (j = 0; j < meshCount; j++)
            {
                const u8* mesh      = file + meshOff + j * LMV_SIZEOF_MESH;
                u32       primCount = mesh[0];
                u32       primsOff  = LMV_RD32(&mesh[4]);

                for (k = 0; k < primCount; k++, primIdx++)
                {
                    const u8* prim    = file + primsOff + k * LMV_SIZEOF_PRIM;
                    const u8* vtx     = &prim[0xC];
                    const u8* nrm     = &prim[0x10];
                    u32       corners = vtx[3] == 0xFF ? 3 : 4;
                    u32       c;

                    for (c = 0; c < corners; c++)
                    {
                        if (!vstamp[vtx[c]])
                        {
                            LMV_FAIL(11, "V11: part %.8s prim %u corner %u reads unwritten vertex slot %u",
                                     (const char*)mh, primIdx, c, vtx[c]);
                        }
                        if (!nstamp[nrm[c]])
                        {
                            LMV_FAIL(11, "V11: part %.8s prim %u corner %u reads unwritten normal slot %u",
                                     (const char*)mh, primIdx, c, nrm[c]);
                        }
                    }
                }
            }
        }
    }

    /* V12 — F2: the third chain (func_80059D50) feeds func_800574D4 +
     * func_80057B7C with offset 0 always while func_80059E34 reads corners
     * absolutely, so a field_B_4 part must sit at offset 0. */
    for (i = 0; i < modelCount; i++)
    {
        const u8* mh      = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;
        u32       fieldB4 = (mh[11] >> 4) & 3;

        if (fieldB4 != 0 && (mh[9] != 0 || mh[10] != 0))
        {
            LMV_FAIL(12, "V12: part %.8s field_B_4=%u requires vertexOffset==0 && normalOffset==0 (got %u/%u)",
                     (const char*)mh, fieldB4, mh[9], mh[10]);
        }
    }

    /* V13 — skipped when the ANM boneCount is unknown: an out-of-range bone
     * degrades (never-initialized GsCOORDINATE2 placement), it does not
     * corrupt, and the converter enforces this rule strictly at author time.
     * Non-digit prefixes bind bone 0 by hardware rule — legal, no check. */
    if (p->anmBoneCount > 0)
    {
        for (i = 0; i < modelCount; i++)
        {
            const u8* mh = file + modelHdrsOff + i * LMV_SIZEOF_MODEL;

            if (mh[0] >= '0' && mh[0] <= '9' && mh[1] >= '0' && mh[1] <= '9')
            {
                s32 boneIdx = (mh[0] - '0') * 10 + (mh[1] - '0');

                if (boneIdx >= p->anmBoneCount)
                {
                    LMV_FAIL(13, "V13: part %.8s binds bone %d but the ANM has %d bones",
                             (const char*)mh, boneIdx, p->anmBoneCount);
                }
            }
        }
    }

    return 0;
}
