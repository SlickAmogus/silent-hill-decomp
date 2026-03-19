/*
 * ipd_reformat.c - Reformat PSX 32-bit IPD binary to 64-bit PC struct layout
 *
 * On PSX, s_IpdHeader and sub-structs have 4-byte pointers. When loaded
 * from disc on 64-bit, the binary data uses PSX layout but C structs
 * have 8-byte pointers + padding.
 *
 * PSX struct sizes (all pointers are 4 bytes):
 *   s_IpdHeader:         392 bytes (header 84 + collision 308)
 *   s_IpdModelInfo:       16 bytes (64-bit: 20+)
 *   s_IpdModelBuffer:     24 bytes (64-bit: 40+)
 *   s_IpdModelBuffer_C:   36 bytes (64-bit: 40)
 *   s_IpdCollisionData:  308 bytes (64-bit: 348+)
 *
 * PSX IpdHeader layout (offsets from field name suffixes):
 *   0x00: magic(1) isLoaded(1) cellX(1) cellZ(1)
 *   0x04: lmHdr offset (4)
 *   0x08: modelCount(1) modelBufferCount(1) modelOrderCount(1) unk(1)
 *   0x0C: unk_C[8]
 *   0x14: modelInfo offset (4)
 *   0x18: modelBuffers offset (4)
 *   0x1C: subcell table (52 bytes: textureCount+unk_1D+unk_20)
 *   0x50: modelOrderList offset (4)
 *   0x54: collision data (308 bytes)
 */

#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh_log.h"

#include "bodyprog/bodyprog.h"

/* PSX struct sizes */
#define PSX_IPD_HEADER_SIZE       0x54  /* header before collision */
#define PSX_IPD_COLLISION_SIZE    0x134 /* 308 bytes */
#define PSX_IPD_TOTAL_SIZE        0x188 /* 392 bytes */
#define PSX_IPD_MODEL_INFO_SIZE   16
#define PSX_IPD_MODEL_BUFFER_SIZE 24
#define PSX_IPD_MODEL_BUFFER_C_SIZE 36

static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static inline u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static inline s16 rds16(const u8* p) { return (s16)(p[0] | (p[1] << 8)); }
static inline s32 rds32(const u8* p) { return (s32)rd32(p); }

static void ParseIpdModelInfo(s_IpdModelInfo* dst, const u8* src)
{
    dst->isGlobalPlm_0 = src[0];
    dst->unk_1[0]      = src[1];
    dst->unk_1[1]      = src[2];
    dst->unk_1[2]      = src[3];
    memcpy(&dst->modelName_4, &src[4], 8);
    /* modelHdr_C is initially NULL - will be resolved by ModelLinkObjectLists */
    dst->modelHdr_C = NULL;
}

static void ParseIpdModelBufferC(s_IpdModelBuffer_C* dst, const u8* src)
{
    /* modelHdr_0 initially holds a model index (small integer), not a pointer.
     * IpdHeader_ModelBufferLinkObjectLists replaces it with actual pointer. */
    u32 modelIdx = rd32(&src[0]);
    dst->modelHdr_0 = (s_ModelHeader*)(uintptr_t)modelIdx;

    /* MATRIX at offset 4 (32 bytes): short m[3][3], short pad, long t[3] */
    memcpy(&dst->field_4, &src[4], sizeof(MATRIX));
}

static void ParseIpdModelBuffer(s_IpdModelBuffer* dst, const u8* src, u8* base)
{
    dst->field_0 = src[0];
    dst->field_1 = src[1];
    dst->field_2 = src[2];
    dst->unk_3   = (s8)src[3];
    dst->field_4 = rds16(&src[4]);
    dst->field_6 = rds16(&src[6]);
    dst->field_8 = rds16(&src[8]);
    dst->field_A = rds16(&src[10]);

    u32 fieldC_off  = rd32(&src[12]);
    u32 field10_off = rd32(&src[16]);
    u32 field14_off = rd32(&src[20]);

    /* field_C: array of s_IpdModelBuffer_C, field_0 entries */
    if (dst->field_0 > 0)
    {
        dst->field_C = (s_IpdModelBuffer_C*)calloc(dst->field_0, sizeof(s_IpdModelBuffer_C));
        for (int i = 0; i < dst->field_0; i++)
        {
            ParseIpdModelBufferC(&dst->field_C[i],
                base + fieldC_off + i * PSX_IPD_MODEL_BUFFER_C_SIZE);
        }
    }
    else
    {
        dst->field_C = NULL;
    }

    /* field_10: SVECTOR array, field_1 entries - raw data in buffer, no reformatting needed */
    dst->field_10 = (SVECTOR*)(base + field10_off);

    /* field_14: SVECTOR bounding box array, field_2 entries */
    dst->field_14 = (SVECTOR*)(base + field14_off);
}

static void ParseIpdCollisionData(s_IpdCollisionData* dst, const u8* collraw, u8* collbase)
{
    dst->positionX_0 = rds32(&collraw[0x00]);
    dst->positionZ_4 = rds32(&collraw[0x04]);

    /* Bitfield at offset 0x08 - read as u32 and manually set fields */
    u32 bf = rd32(&collraw[0x08]);
    dst->field_8_0  = bf & 0xFF;
    dst->field_8_8  = (bf >> 8) & 0xFF;
    dst->field_8_16 = (bf >> 16) & 0xFF;
    dst->field_8_24 = (bf >> 24) & 0xFF;

    /* Collision sub-pointers: offsets relative to collision data base */
    u32 ptrC_off  = rd32(&collraw[0x0C]);
    u32 ptr10_off = rd32(&collraw[0x10]);
    u32 ptr14_off = rd32(&collraw[0x14]);
    u32 ptr18_off = rd32(&collraw[0x18]);

    SH_DBG("[IPD-COLL] raw ptrs: C=0x%x 10=0x%x 14=0x%x 18=0x%x",
            ptrC_off, ptr10_off, ptr14_off, ptr18_off);
    SH_DBG("[IPD-COLL] field_1C=%d field_1E=%d field_1F=%d",
            rds16(&collraw[0x1C]), collraw[0x1E], collraw[0x1F]);

    dst->ptr_C  = (SVECTOR3*)(collbase + ptrC_off);
    dst->ptr_10 = (s_IpdCollisionData_10*)(collbase + ptr10_off);
    dst->ptr_14 = (s_IpdCollisionData_14*)(collbase + ptr14_off);
    dst->ptr_18 = (s_IpdCollisionData_18*)(collbase + ptr18_off);

    dst->field_1C = rds16(&collraw[0x1C]);
    dst->field_1E = collraw[0x1E];
    dst->field_1F = collraw[0x1F];

    u32 ptr20_off = rd32(&collraw[0x20]);
    dst->ptr_20 = (s_IpdCollisionData_20*)(collbase + ptr20_off);

    dst->field_24 = rd16(&collraw[0x24]);
    dst->field_26 = rd16(&collraw[0x26]);

    u32 ptr28_off = rd32(&collraw[0x28]);
    u32 ptr2C_off = rd32(&collraw[0x2C]);
    dst->ptr_28 = (u8*)(collbase + ptr28_off);
    dst->ptr_2C = (void*)(collbase + ptr2C_off);

    /* Dump first few ptr_10 entries to verify data alignment */
    {
        s_IpdCollisionData_10* p10 = dst->ptr_10;
        SH_DBG("[IPD-COLL] ptr_10[0]: f0=%d f2=%d f4=%d f8=%d fA=%d",
                p10[0].field_0, p10[0].field_2, p10[0].field_4, p10[0].field_8, p10[0].field_A);
        if (dst->field_8_8 > 1)
            SH_DBG("[IPD-COLL] ptr_10[1]: f0=%d f2=%d f4=%d f8=%d fA=%d",
                    p10[1].field_0, p10[1].field_2, p10[1].field_4, p10[1].field_8, p10[1].field_A);
    }

    dst->field_30 = collraw[0x30];
    dst->unk_31[0] = collraw[0x31];
    dst->unk_31[1] = collraw[0x32];
    dst->unk_31[2] = collraw[0x33];

    memcpy(dst->field_34, &collraw[0x34], 256);
}

/**
 * PC replacement for IpdHeader_FixHeaderOffsets + IpdCollData_FixOffsets.
 *
 * Reads raw PSX 32-bit binary data at ipdHdr's address, parses it into
 * properly formatted 64-bit structs (heap-allocated for arrays),
 * then overwrites the s_IpdHeader with correct 64-bit values.
 *
 * After this, the original IpdHeader_FixOffsets flow can call
 * LmHeader_FixOffsets, Ipd_MaterialsLoad, and model link functions.
 */
void IpdHeader_FixOffsets_PC(s_IpdHeader* ipdHdr)
{
    u8* raw = (u8*)ipdHdr;

    /* Check already loaded */
    if (raw[1] == 1)
    {
        return;
    }

    /* Validate magic */
    if (raw[0] != IPD_HEADER_MAGIC)
    {
        fprintf(stderr, "[SH] IpdFixOffsets_PC: invalid magic %d (expected %d), skipping\n",
                raw[0], IPD_HEADER_MAGIC);
        fflush(stderr);
        return;
    }

    /* Read PSX header fields from raw bytes */
    u8  magic            = raw[0];
    u8  cellX            = raw[2];
    u8  cellZ            = raw[3];
    u32 lmHdrOff         = rd32(&raw[0x04]);
    u8  modelCount       = raw[0x08];
    u8  modelBufferCount = raw[0x09];
    u8  modelOrderCount  = raw[0x0A];
    u8  unkB             = raw[0x0B];
    u32 modelInfoOff     = rd32(&raw[0x14]);
    u32 modelBuffersOff  = rd32(&raw[0x18]);
    u32 modelOrderOff    = rd32(&raw[0x50]);

    fprintf(stderr, "[SH] IpdFixOffsets_PC: magic=%d cell=[%d,%d] models=%d bufs=%d orders=%d\n",
            magic, (s8)cellX, (s8)cellZ, modelCount, modelBufferCount, modelOrderCount);
    fprintf(stderr, "[SH]   lmOff=0x%x miOff=0x%x mbOff=0x%x moOff=0x%x\n",
            lmHdrOff, modelInfoOff, modelBuffersOff, modelOrderOff);
    fflush(stderr);

    /* Save subcell table (52 bytes from PSX offset 0x1C to 0x4F) */
    u8 subcellTable[52];
    memcpy(subcellTable, &raw[0x1C], 52);

    /* Save unk_C[8] from PSX offset 0x0C */
    u8 unkC[8];
    memcpy(unkC, &raw[0x0C], 8);

    /* Save collision raw data before overwriting (PSX offset 0x54, 308 bytes) */
    u8 collraw[PSX_IPD_COLLISION_SIZE];
    memcpy(collraw, &raw[0x54], PSX_IPD_COLLISION_SIZE);
    /* The collision sub-data base for pointer fixup is raw + 0x54 (where collData lives in PSX) */
    u8* collbase = raw + 0x54;

    /* Parse model info array (PSX stride = 16 bytes) */
    s_IpdModelInfo* modelInfos = NULL;
    if (modelCount > 0)
    {
        modelInfos = (s_IpdModelInfo*)calloc(modelCount, sizeof(s_IpdModelInfo));
        for (int i = 0; i < modelCount; i++)
        {
            ParseIpdModelInfo(&modelInfos[i],
                raw + modelInfoOff + i * PSX_IPD_MODEL_INFO_SIZE);
        }
    }

    /* Parse model buffers array (PSX stride = 24 bytes) */
    s_IpdModelBuffer* modelBuffers = NULL;
    if (modelBufferCount > 0)
    {
        modelBuffers = (s_IpdModelBuffer*)calloc(modelBufferCount, sizeof(s_IpdModelBuffer));
        for (int i = 0; i < modelBufferCount; i++)
        {
            ParseIpdModelBuffer(&modelBuffers[i],
                raw + modelBuffersOff + i * PSX_IPD_MODEL_BUFFER_SIZE, raw);
        }
    }

    /* Now overwrite the s_IpdHeader with 64-bit struct values.
     * This clobbers raw bytes but we've already parsed everything above. */
    memset(ipdHdr, 0, sizeof(s_IpdHeader));

    ipdHdr->magic_0            = magic;
    ipdHdr->isLoaded_1         = 0; /* Will be set to 1 by caller after all fixups */
    ipdHdr->cellX_2            = (s8)cellX;
    ipdHdr->cellZ_3            = (s8)cellZ;
    ipdHdr->lmHdr_4            = (s_LmHeader*)(raw + lmHdrOff);
    ipdHdr->modelCount_8       = modelCount;
    ipdHdr->modelBufferCount_9 = modelBufferCount;
    ipdHdr->modelOrderCount_A  = modelOrderCount;
    ipdHdr->unk_B[0]           = unkB;
    memcpy(ipdHdr->unk_C, unkC, 8);
    ipdHdr->modelInfo_14       = modelInfos;
    ipdHdr->modelBuffers_18    = modelBuffers;

    /* Restore subcell table: textureCount_1C(1) + unk_1D[3](3) + unk_20[48](48) = 52 bytes */
    ipdHdr->textureCount_1C = subcellTable[0];
    memcpy(ipdHdr->unk_1D, &subcellTable[1], 3);
    memcpy(ipdHdr->unk_20, &subcellTable[4], 48);

    ipdHdr->modelOrderList_50 = raw + modelOrderOff;

    /* Parse collision data into the embedded struct */
    ParseIpdCollisionData(&ipdHdr->collisionData_54, collraw, collbase);

    /* Register as valid collision data for stale-pointer detection */
    {
        extern void PC_CollRegisterValid(s_IpdCollisionData* cd);
        PC_CollRegisterValid(&ipdHdr->collisionData_54);
    }

    fprintf(stderr, "[SH] IpdFixOffsets_PC: done. lmHdr=%p modelInfo=%p[%d] modelBufs=%p[%d]\n",
            (void*)ipdHdr->lmHdr_4, (void*)ipdHdr->modelInfo_14, modelCount,
            (void*)ipdHdr->modelBuffers_18, modelBufferCount);
    fflush(stderr);
}
