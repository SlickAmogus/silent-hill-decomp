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
