/*
 * lm_reformat.c - Reformat PSX 32-bit binary structs to 64-bit PC layout
 *
 * On PSX, structs like s_LmHeader, s_ModelHeader, s_MeshHeader, and s_Material
 * have 4-byte pointers. When loaded from disc on a 64-bit PC, the binary data
 * uses the 32-bit layout but the C structs have 8-byte pointers + padding.
 *
 * This module parses the raw PSX binary data and writes properly formatted
 * 64-bit struct fields, with offsets converted to real pointers.
 *
 * PSX struct sizes (all pointers are 4 bytes):
 *   s_LmHeader:    20 bytes (64-bit: 40 bytes)
 *   s_ModelHeader: 16 bytes (64-bit: 24 bytes)
 *   s_MeshHeader:  24 bytes (64-bit: 48 bytes)
 *   s_Material:    24 bytes (64-bit: 32 bytes)
 */

#include "game.h"
#include "sh_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/bodyprog.h"

#define PSX_SIZEOF_LM_HEADER    20
#define PSX_SIZEOF_MODEL_HEADER 16
#define PSX_SIZEOF_MESH_HEADER  24
#define PSX_SIZEOF_MATERIAL     24

static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static inline u16 rd16(const u8* p) { return p[0] | (p[1] << 8); }

static void ParseMeshHeader(s_MeshHeader* dst, const u8* src, u8* base)
{
    dst->primitiveCount_0 = src[0];
    dst->vertexCount_1    = src[1];
    dst->normalCount_2    = src[2];
    dst->unkCount_3       = src[3];
    dst->primitives_4     = (s_Primitive*)(base + rd32(&src[4]));
    dst->verticesXy_8     = (DVECTOR*)(base + rd32(&src[8]));
    dst->verticesZ_C      = (s16*)(base + rd32(&src[12]));
    dst->normals_10       = (s_Normal*)(base + rd32(&src[16]));
    dst->unkPtr_14        = base + rd32(&src[20]);
}

static void ParseModelHeader(s_ModelHeader* dst, const u8* src, u8* base)
{
    memcpy(&dst->name_0, src, 8);
    dst->meshCount_8     = src[8];
    dst->vertexOffset_9  = src[9];
    dst->normalOffset_A  = src[10];

    u8 bf = src[11];
    dst->field_B_0 = bf & 1;
    dst->field_B_1 = (bf >> 1) & 7;
    dst->field_B_4 = (bf >> 4) & 3;
    dst->unk_B_6   = (bf >> 6) & 3;

    u32 meshOff = rd32(&src[12]);
    if (dst->meshCount_8 > 0)
    {
        s_MeshHeader* meshes = (s_MeshHeader*)calloc(dst->meshCount_8, sizeof(s_MeshHeader));
        for (int j = 0; j < dst->meshCount_8; j++)
        {
            ParseMeshHeader(&meshes[j], base + meshOff + j * PSX_SIZEOF_MESH_HEADER, base);
        }
        dst->meshHdrs_C = meshes;
    }
    else
    {
        dst->meshHdrs_C = NULL;
    }
}

static void ParseMaterial(s_Material* dst, const u8* src)
{
    memcpy(&dst->name_0, src, 8);
    dst->texture_8    = NULL; /* set later by Lm_MaterialFileIdxApply */
    dst->field_C      = src[12];
    dst->unk_D[0]     = src[13];
    dst->field_E      = src[14];
    dst->field_F      = src[15];
    dst->field_10     = rd16(&src[16]);
    dst->field_12     = rd16(&src[18]);
    dst->field_14.u16 = rd16(&src[20]);
    dst->field_16.u16 = rd16(&src[22]);
}

/**
 * PC replacement for LmHeader_FixOffsets + ModelHeader_FixOffsets.
 *
 * Reads the raw PSX 32-bit binary data at lmHdr's address, parses it into
 * properly formatted 64-bit structs (heap-allocated for sub-structures),
 * then overwrites the s_LmHeader at lmHdr with the correct 64-bit values.
 *
 * After this call, lmHdr->materials_4 etc. are valid 64-bit pointers.
 */
void LmHeader_FixOffsets_PC(s_LmHeader* lmHdr)
{
    u8* raw = (u8*)lmHdr;

    /* Log every call — this fires for ALL callers including map overlay DLL code */
    SH_DBG("[REFORM] LmFixOffsets_PC called: lmHdr=%p raw[0]=0x%02x raw[1]=0x%02x raw[2]=%d",
           (void*)lmHdr, raw[0], raw[1], raw[2]);

    /* The isLoaded flag is at byte 2 in BOTH PSX and 64-bit layouts */
    if (raw[2] == 1)
    {
        SH_DBG("[REFORM] already loaded, skip");
        return; /* already reformatted */
    }

    /* Read all PSX header fields from raw bytes before we overwrite anything */
    u8  magic       = raw[0];
    u8  version     = raw[1];
    u8  matCount    = raw[3];
    u32 matOff      = rd32(&raw[4]);
    u8  modelCount  = raw[8];
    u32 modelHdrsOff  = rd32(&raw[12]);
    u32 modelOrderOff = rd32(&raw[16]);

    SH_DBG("[SH] LmFixOffsets_PC: magic=0x%x ver=%d mats=%d models=%d matOff=0x%x mdlOff=0x%x ordOff=0x%x",
            magic, version, matCount, modelCount, matOff, modelHdrsOff, modelOrderOff);

    /* Reject invalid LM headers (garbage data from unloaded IPD chunks etc.) */
    if (magic != LM_HEADER_MAGIC)
    {
        SH_DBG("[SH] LmFixOffsets_PC: invalid magic 0x%x, skipping", magic);
        lmHdr->isLoaded_2 = 1; /* prevent re-entry */
        return;
    }

    /* Parse materials (PSX stride = 24 bytes) into heap allocation */
    s_Material* mats = NULL;
    if (matCount > 0)
    {
        mats = (s_Material*)calloc(matCount, sizeof(s_Material));
        for (int i = 0; i < matCount; i++)
        {
            ParseMaterial(&mats[i], raw + matOff + i * PSX_SIZEOF_MATERIAL);
        }
    }

    /* Parse model headers (PSX stride = 16) + their mesh headers (PSX stride = 24) */
    s_ModelHeader* models = NULL;
    if (modelCount > 0 && magic == LM_HEADER_MAGIC)
    {
        models = (s_ModelHeader*)calloc(modelCount, sizeof(s_ModelHeader));
        for (int i = 0; i < modelCount; i++)
        {
            ParseModelHeader(&models[i], raw + modelHdrsOff + i * PSX_SIZEOF_MODEL_HEADER, raw);
            SH_DBG("  model[%d] name=%c%c%c%c meshCnt=%d vertOff=%d normOff=%d fB0=%d fB1=%d fB4=%d meshHdrs=%p",
                i, models[i].name_0.str[0], models[i].name_0.str[1],
                models[i].name_0.str[2], models[i].name_0.str[3],
                models[i].meshCount_8, models[i].vertexOffset_9, models[i].normalOffset_A,
                models[i].field_B_0, models[i].field_B_1, models[i].field_B_4,
                (void*)models[i].meshHdrs_C);
        }
    }

    /*
     * Now overwrite the first sizeof(s_LmHeader) bytes (40 on 64-bit) with
     * the properly formatted struct. This clobbers raw bytes 20-39 which
     * contained material/model data, but we already parsed those above.
     */
    memset(lmHdr, 0, sizeof(s_LmHeader));
    lmHdr->magic_0          = magic;
    lmHdr->version_1        = version;
    lmHdr->isLoaded_2       = 1;
    lmHdr->materialCount_3  = matCount;
    lmHdr->materials_4      = mats;
    lmHdr->modelCount_8     = modelCount;
    lmHdr->modelHdrs_C      = models;
    lmHdr->modelOrder_10    = raw + modelOrderOff;

    /* Log model order (rendering order) */
    {
        char _ordBuf[256] = {0};
        int _ordPos = 0;
        for (int i = 0; i < modelCount && _ordPos < 250; i++) {
            _ordPos += snprintf(_ordBuf + _ordPos, sizeof(_ordBuf) - _ordPos, " %d", lmHdr->modelOrder_10[i]);
        }
        SH_DBG("  modelOrder:%s", _ordBuf);
    }

    SH_DBG("[SH] LmFixOffsets_PC: done");
}
