#include "game.h"
#include "inline_no_dmpsx.h"
#ifdef SH_PC_PORT
#include <stdlib.h>
#include "pc_config.h"
#include "sh_log.h"
/* Max IPD chunk slots on PC. Largest maps (map0_s00) have ~129 chunks.
 * PSX uses 1-4 slots with streaming. PC can hold all chunks in memory. */
#define PC_MAX_IPD_CHUNKS 256
#endif

#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/math/math.h"
#include "main/fsqueue.h"
#include "types.h"

/** Known contents:
 * - Map loading funcs
 * - Animation funcs
 */

// ========================================
// CHARACTERS
// ========================================

bool func_80040B74(e_CharacterId charaId) // 0x80040B74
{
    s32 i;

    for (i = 0; i < ARRAY_SIZE(g_WorldGfxWork.charaModels); i++)
    {
        if (g_WorldGfxWork.charaModels[i].charaId == charaId)
        {
            return true;
        }
    }

    return false;
}

// ========================================
// WORLD RENDERING
// ========================================

PACKET D_800BFBF0[2][D_800BFBF0_STRIDE];

#ifdef SH_PC_PORT
s_IpdCollisionData* D_800C1010[256];
#else
s_IpdCollisionData* D_800C1010[4];
#endif

s_Map g_Map;

void func_80040BAC(void) // 0x80040BAC
{
    DVECTOR   posTable[17];
    POLY_G4*  poly_g4;
    POLY_F4*  poly_f4;
    PACKET*   packet;
    POLY_G3*  poly_g3;
    DR_TPAGE* page;
    s32       i;
    s32       k;
    s32       j;
    s32*      ptr;

    for (i = 0; i < 17; i++)
    {
        if (i < 2)
        {
            posTable[i].vx = g_GameWork.gsScreenWidth / 2;
            posTable[i].vy = (g_GameWork.gsScreenHeight / 4) * i;
        }
        else if (i < 6)
        {
            posTable[i].vx = (g_GameWork.gsScreenWidth >> 1) - (((g_GameWork.gsScreenWidth >> 1) >> 1) * (i - 2));
            posTable[i].vy = g_GameWork.gsScreenHeight / 2;
        }
        else if (i < 10)
        {
            posTable[i].vx = -g_GameWork.gsScreenWidth / 2;
            posTable[i].vy = (g_GameWork.gsScreenHeight >> 1) - (((g_GameWork.gsScreenHeight >> 1) >> 1) * (i - 6));
        }
        else if (i < 14)
        {
            posTable[i].vx = (-g_GameWork.gsScreenWidth / 2) + ((g_GameWork.gsScreenWidth >> 2) * (i - 10));
            posTable[i].vy = -g_GameWork.gsScreenHeight / 2;
        }
        else
        {
            posTable[i].vx = g_GameWork.gsScreenWidth / 2;
            posTable[i].vy = -g_GameWork.gsScreenHeight / 2 + ((g_GameWork.gsScreenHeight >> 2) * (i - 14));
        }
    }

    for (j = 0, ptr = &posTable[0], packet = &D_800BFBF0; j < 2; j++,
        packet += sizeof(DR_TPAGE) * 2 + sizeof(POLY_G4) * 16 * 3 + sizeof(POLY_G3) * 16 + sizeof(POLY_F4) * 16)
    {
        page = (DR_TPAGE*)packet;

        SetDrawTPage(page, 1, 1, 0x40);
        page++;
        SetDrawTPage(page, 1, 1, 0x20);

        poly_g3 = packet + sizeof(DR_TPAGE) * 2;
        poly_f4 = packet + (sizeof(DR_TPAGE) * 2) + ((sizeof(POLY_G4) * 16) * 3) + (sizeof(POLY_G3) * 16);

        for (i = 0; i < 16; i++, poly_g3++, poly_f4++)
        {
            SetPolyG3(poly_g3);
            setSemiTrans(poly_g3, true);
            SetPolyF4(poly_f4);
            setSemiTrans(poly_f4, true);

            *(s32*)&poly_f4->x2 = ptr[i % 16];
            *(s32*)&poly_f4->x3 = ptr[i % 16 + 1];
        }

        poly_g4 = packet + (sizeof(DR_TPAGE) * 2) + (sizeof(POLY_G3) * 16);

        for (k = 0; k < 3; k++)
        {
            for (i = 0; i < 16; i++, poly_g4++)
            {
                SetPolyG4(poly_g4);
                setSemiTrans(poly_g4, true);
            }
        }
    }
}

void func_80040E7C(u8 arg0, u8 arg1, u8 arg2, u8 arg3, u8 arg4, u8 arg5) // 0x80040E7C
{
    #define SET_RGB(r, g, b) \
        (((r) & 0xFF) | ((g) & 0xFF) << 8 | ((b) & 0xFF) << 16)

    u32      colorTable[4];
    s32      j;
    s32      i;
    s32      k;
    u32      color;
    POLY_G3* poly_g3;
    POLY_F4* poly_f4;
    POLY_G4* poly_g4;
    PACKET*  packet;

    color = SET_RGB(arg0, arg1, arg2);

    packet = &D_800BFBF0;

    colorTable[0] = SET_RGB(0, 0, 0);
    colorTable[1] = SET_RGB(arg3 / 3, arg4 / 3, arg5 / 3);
    colorTable[2] = SET_RGB(arg3 * 2 / 3, arg4 * 2 / 3, arg5 * 2 / 3);
    colorTable[3] = SET_RGB(arg3, arg4, arg5);

    for (i = 0; i < 2; i++,
        packet += (sizeof(DR_TPAGE) * 2) + ((sizeof(POLY_G4) * 16) * 3) + (sizeof(POLY_G3) * 16) + (sizeof(POLY_F4) * 16))
    {
        poly_g3 = packet + (sizeof(DR_TPAGE) * 2);
        poly_f4 = packet + (sizeof(DR_TPAGE) * 2) + ((sizeof(POLY_G4) * 16) * 3) + (sizeof(POLY_G3) * 16);

        for (j = 0; j < 16; j++, poly_g3++, poly_f4++)
        {
            *(s32*)&poly_g3->r0 = color + (poly_g3->code << 24);
            *(s32*)&poly_g3->r1 = colorTable[0];
            *(s32*)&poly_g3->r2 = colorTable[0];
            *(s32*)&poly_f4->r0 = colorTable[3] + (poly_f4->code << 24);
        }

        poly_g4 = packet + (sizeof(DR_TPAGE) * 2) + (sizeof(POLY_G3) * 16);

        for (k = 0; k < 3; k++)
        {
            for (j = 0; j < 16; j++, poly_g4++)
            {
                *(s32*)&poly_g4->r0 = colorTable[k] + (poly_g4->code << 24);
                *(s32*)&poly_g4->r1 = colorTable[k];
                *(s32*)&poly_g4->r2 = colorTable[k + 1];
                *(s32*)&poly_g4->r3 = colorTable[k + 1];
            }
        }
    }
}

void func_80041074(GsOT* ot, s32 arg1, SVECTOR* rot, const VECTOR3* pos) // 0x80041074
{
    VECTOR3 pos0; // Q19.12
    q19_12  rotY;
    q19_12  rotX;

    func_800410D8(&pos0, &rotY, &rotX, rot, pos);
    func_800414E0(ot, &pos0, arg1, rotY, rotX);
}

void func_800410D8(VECTOR3* pos0, q19_12* azimuthAngle, q19_12* altitudeAngle, SVECTOR* rot, const VECTOR3* pos1) // 0x800410D8
{
    MATRIX        transformMat;
    SVECTOR       vec0;
    GsCOORDINATE2 coord;
    VECTOR        offset0; // Q19.12
    VECTOR        offset1; // Q19.12
    s32           flag;

    memset(&vec0, 0, sizeof(SVECTOR));

    // Compute coord.
    coord.super      = NULL;
    coord.workm      = GsIDMATRIX;
    coord.workm.t[0] = Q12_TO_Q8(pos1->vx);
    coord.workm.t[1] = Q12_TO_Q8(pos1->vy);
    coord.workm.t[2] = Q12_TO_Q8(pos1->vz);
    coord.flg        = true;

    Vw_CoordToViewSpaceMatrix(&coord, &transformMat);
    SetRotMatrix(&transformMat);
    SetTransMatrix(&transformMat);
    RotTrans(&vec0, &offset0, &flag);

    ApplyRotMatrix(rot, &offset1);

    Math_RelativeRotationGet(azimuthAngle, altitudeAngle, &offset0, &offset1);
    func_8004137C(pos0, &offset0, &offset1, ReadGeomScreen());
}

void Math_RelativeRotationGet(q19_12* azimuthAngle, q19_12* altitudeAngle, const VECTOR* offsetFrom, const VECTOR* offsetTo) // 0x8004122C
{
    VECTOR  dir0;        // Q19.12
    VECTOR  dir1;        // Q19.12
    VECTOR  planeNormal; // Q19.12
    SVECTOR cosTheta;
    SVECTOR sinTheta;
    VECTOR  sideStep;    // Q19.12

    // Compute vector from cross product of directions.
    VectorNormal(offsetFrom, &dir0);
    VectorNormal(offsetTo, &dir1);
    OuterProduct12(&dir0, &dir1, &planeNormal);
    VectorNormal(&planeNormal, &planeNormal);

    // Compute Y rotation (azimuth).
    *azimuthAngle = Q12_ANGLE_NORM_U(ratan2(planeNormal.vy, planeNormal.vx) - Q12_ANGLE(90.0f));

    // Compute vector from ??? TODO.
    cosTheta.vx = Q12_MULT(dir0.vx, dir1.vx) +
                  Q12_MULT(dir0.vy, dir1.vy) +
                  Q12_MULT(dir0.vz, dir1.vz);
    cosTheta.vx = FP_FROM((dir0.vx * dir1.vx) +
                          (dir0.vy * dir1.vy) +
                          (dir0.vz * dir1.vz), Q12_SHIFT); // @hack Duplicate operation required for match.
    OuterProduct12(&planeNormal, &dir0, &sideStep);
    sinTheta.vx = FP_FROM((dir1.vx * sideStep.vx) +
                          (dir1.vy * sideStep.vy) +
                          (dir1.vz * sideStep.vz), Q12_SHIFT);

    // Compute X rotation (altitude).
    *altitudeAngle = Q12_ANGLE_NORM_U(ratan2(sinTheta.vx, cosTheta.vx));
}

void func_8004137C(VECTOR3* result, const VECTOR* offset0, const VECTOR* offset1, s32 screenDist)
{
    VECTOR vec;
    s32    offsetX;
    s32    offsetY;
    s32    screenDistHalf;
    s32    z;

    screenDistHalf = screenDist / 2;

    if (screenDistHalf < offset0->vz)
    {
        vec = *offset0;
    }
    else
    {
        z = 1;
        if (offset1->vz != 0)
        {
            z = offset1->vz;
        }

        vec.vz = screenDistHalf;
        vec.vx = (((screenDistHalf - offset0->vz) * offset1->vx) / z) + offset0->vx;
        vec.vy = (((screenDistHalf - offset0->vz) * offset1->vy) / z) + offset0->vy;
    }

    ReadGeomOffset(&offsetX, &offsetY);

    result->vz = vec.vz;
    result->vx = ((vec.vx * screenDist) / vec.vz) + offsetX;
    result->vy = ((vec.vy * screenDist) / vec.vz) + offsetY;
}

void func_800414E0(GsOT* arg0, VECTOR3* arg1, s32 arg2, q19_12 angle0, q19_12 angle1) // 0x800414E0
{
    s32      sp10[4];
    DVECTOR  sp20[4];
    s32      temp_a1;
    s32      temp_a3;
    s32      temp_lo;
    s32      temp_s0_3;
    s32      temp_s1;
    s32      temp_s2;
    s32      var_a0;
    s32      var_s0;
    s32      var_s1;
    s32      i;
    s32      j;
    s32      var_v0;
    s32      var_v1;
    s32      var_v1_2;
    DVECTOR* var_s1_2;
    u32*     var_a1_3;
    u32*     var_t0;
    u32*     var_t1;
    u32*     var_t4;
    POLY_G3* poly_g3;
    POLY_G4* poly_g4;
    POLY_F4* poly_f4;

    if (arg1->vz < Q12(0.25f))
    {
        var_s1 = Q12(1.0f);
    }
    else
    {
        var_s1 = Q12(1024.0f) / arg1->vz;
    }

    var_s0 = (Q12_MULT_ALT(arg2, 500) << 10) / arg1->vz;
    var_v1 = var_s0 * (Q12(3.0f) - Q12_MULT(var_s1, Math_Cos(angle1)));
    var_s0 = var_v1 / ((SHRT_MAX / 2) + 1);

    sp10[0] = (var_s0 / 5);
    sp10[1] = (var_s0 * 4) / 10;
    sp10[2] = (var_s0 * 7) / 10;
    sp10[3] = var_s0;

    temp_s1 = Math_Sin(angle1);
    temp_s2 = Math_Cos(angle0);
    temp_a3 = Math_Sin(angle0);

    for (i = 0; i < 4; i++)
    {
        temp_lo = Q12(var_s0 - (sp10[i] >> 1)) / var_s0;
        sp20[i].vx = arg1->vx + Q12_MULT_ALT(Q12_MULT_ALT(Q12_MULT_ALT(sp10[i], temp_lo), temp_s2), temp_s1);
        sp20[i].vy = arg1->vy + Q12_MULT_ALT(Q12_MULT_ALT(Q12_MULT_ALT(sp10[i], temp_lo), temp_a3), temp_s1);
    }

    var_t4 = PSX_SCRATCH;
    for (i = 0; i < 4; i++)
    {
        var_s1_2 = var_t4 + (i * 17);
        for (j = 0; j < 17; j++, var_s1_2++)
        {
            temp_s0_3 = Math_Cos(j << 8);
            temp_a1   = Math_Sin(j << 8);

            var_s1_2->vx = Q12_MULT_ALT(sp10[i], temp_s0_3) + sp20[i].vx;
            var_s1_2->vx = CLAMP(var_s1_2->vx, Q12(-0.25f), Q12(0.25f) - 1);

            var_s1_2->vy = Q12_MULT_ALT(sp10[i], temp_a1) + sp20[i].vy;
            var_s1_2->vy = CLAMP(var_s1_2->vy, Q12(-0.25f), Q12(0.25f) - 1);
        }
    }

    var_t0 = (u32*)PSX_SCRATCH;

    poly_g3 = &D_800BFBF0[g_ActiveBufferIdx][sizeof(DR_TPAGE) * 2];
    poly_f4 = &D_800BFBF0[g_ActiveBufferIdx][(sizeof(DR_TPAGE) * 2) +
                                             ((sizeof(POLY_G4) * 16) * 3) +
                                             (sizeof(POLY_G3) * 16)];

    for (j = 0; j < 16; j++, poly_g3++, poly_f4++)
    {
        poly_g3->x0         = arg1->vx;
        poly_g3->y0         = arg1->vy;
        *(s32*)&poly_g3->x1 = var_t0[j];
        *(s32*)&poly_g3->x2 = var_t0[j + 1];

        addPrim(arg0->org, poly_g3);

        *(s32*)&poly_f4->x0 = var_t0[j + 51];
        *(s32*)&poly_f4->x1 = var_t0[j + 52];

        addPrim(&arg0->org[1], poly_f4);
    }

    var_t1  = (u32*)PSX_SCRATCH;
    poly_g4 = &D_800BFBF0[g_ActiveBufferIdx][(sizeof(DR_TPAGE) * 2) + (sizeof(POLY_G3) * 16)];

    for (i = 0; i < 3; i++)
    {
        var_a1_3 = var_t1 + (i * 17);

        for (j = 0; j < 16; j++, poly_g4++)
        {
            *(s32*)&poly_g4->x0 = var_a1_3[j];
            *(s32*)&poly_g4->x1 = var_a1_3[j + 1];
            *(s32*)&poly_g4->x2 = var_a1_3[17 + j];
            *(s32*)&poly_g4->x3 = var_a1_3[(17 + j) + 1];

            addPrim(&arg0->org[1], poly_g4);
        }
    }

    AddPrim(arg0->org, &D_800BFBF0[g_ActiveBufferIdx][sizeof(DR_TPAGE)]);
    AddPrim(&arg0->org[1], &D_800BFBF0[g_ActiveBufferIdx]);
}

// ========================================
// WORLD INITIALIZATION 1
// ========================================

u32 Fs_QueueEntryLoadStatusGet(s32 queueIdx) // 0x80041ADC
{
    if (queueIdx == NO_VALUE)
    {
        return FsQueueEntryLoadStatus_Invalid;
    }
    else if (!Fs_QueueIsEntryLoaded(queueIdx))
    {
        return FsQueueEntryLoadStatus_Unloaded;
    }

    return FsQueueEntryLoadStatus_Loaded;
}

u32 IpdHeader_LoadStateGet(s_IpdChunk* chunk) // 0x80041B1C
{
    s32 queueState;
    s32 queueStateCpy;

    queueState    = Fs_QueueEntryLoadStatusGet(chunk->queueIdx);
    queueStateCpy = queueState;

    if (queueStateCpy == FsQueueEntryLoadStatus_Unloaded)
    {
        return StaticModelLoadState_Unloaded;
    }
    else if (queueStateCpy == FsQueueEntryLoadStatus_Invalid ||
             queueState != FsQueueEntryLoadStatus_Loaded)
    {
        return StaticModelLoadState_Invalid;
    }
    else if (chunk->ipdHdr->isLoaded && IpdHeader_IsTextureLoaded(chunk->ipdHdr))
    {
        return StaticModelLoadState_Loaded;
    }

    return StaticModelLoadState_Corrupted;
}

u32 LmHeader_LoadStateGet(s_GlobalLm* globalLm) // 0x80041BA0
{
    s32 queueState;
    s32 queueStateCpy;

    queueState    = Fs_QueueEntryLoadStatusGet(globalLm->queueIdx);
    queueStateCpy = queueState;

    if (queueStateCpy == FsQueueEntryLoadStatus_Unloaded)
    {
        return StaticModelLoadState_Unloaded;
    }
    else if (queueStateCpy == FsQueueEntryLoadStatus_Invalid ||
             queueState    != FsQueueEntryLoadStatus_Loaded)
    {
        return StaticModelLoadState_Invalid;
    }
    else if (globalLm->lmHdr->isLoaded && LmHeader_IsTextureLoaded(globalLm->lmHdr))
    {
        return StaticModelLoadState_Loaded;
    }

    return StaticModelLoadState_Corrupted;
}

void Map_Init(s_LmHeader* lmHdr, s_IpdHeader* ipdBuf, s32 ipdBufSize) // 0x80041C24
{
    bzero(&g_Map, sizeof(s_Map));
    Lm_Init(&g_Map.globalLm_138, lmHdr);

    g_Map.ipdBuffer_150     = ipdBuf;
    g_Map.ipdBufferSize_154 = ipdBufSize;
    g_Map.ipdActiveSize_158 = 0;
    g_Map.isExterior_588    = true;

    Ipd_ActiveChunksQueueIdxClear(g_Map.ipdActive_15C, 4);
    Ipd_TexturesInit();
    Map_IpdCollisionDataInit();
}

void Lm_Init(s_GlobalLm* globalLm, s_LmHeader* lmHdr) // 0x80041CB4
{
    globalLm->lmHdr = lmHdr;
    LmHeader_Init(lmHdr);

    globalLm->queueIdx = 0;
    globalLm->fileIdx  = NO_VALUE;
}

void LmHeader_Init(s_LmHeader* lmHdr) // 0x80041CEC
{
    lmHdr->magic         = LM_HEADER_MAGIC;
    lmHdr->version       = LM_VERSION;
    lmHdr->isLoaded      = true;
    lmHdr->materialCount = 0;
    lmHdr->modelCount    = 0;
}

void Ipd_ActiveChunksQueueIdxClear(s_IpdChunk* chunks, s32 chunkCount) // 0x80041D10
{
    s_IpdChunk* curChunk;

    for (curChunk = &chunks[0]; curChunk < &chunks[chunkCount]; curChunk++)
    {
        curChunk->queueIdx = NO_VALUE;
    }
}

void Ipd_TexturesInit(void) // 0x80041D48
{
    s32 i;
    s16 j;
    s16 x;
    s32 y;

    for (i = 0, y = 8, x = 0, j = 0; i < 8; i++, y++, x += 16)
    {
        if (y == 11)
        {
            y = 21;
        }

        Texture_Init(&g_Map.ipdTextures_430.fullPageTextures_58[i], 0, 0, y, 0, 0, x, j);
    }

    Textures_ActiveTex_CountReset(&g_Map.ipdTextures_430.fullPage_0);
    Textures_ActiveTex_PutTextures(&g_Map.ipdTextures_430.fullPage_0, g_Map.ipdTextures_430.fullPageTextures_58, 8);

    for (i = 0, y = 26, j = 0; i < 2; i++, x += 16)
    {
        Texture_Init(&g_Map.ipdTextures_430.halfPageTextures_118[i], 0, 0, y, (i & 0x1) * 32, 0, x, j);
        if (i & 0x1)
        {
            y++;
        }
    }

    Textures_ActiveTex_CountReset(&g_Map.ipdTextures_430.halfPage_2C);
    Textures_ActiveTex_PutTextures(&g_Map.ipdTextures_430.halfPage_2C, g_Map.ipdTextures_430.halfPageTextures_118, 2);
}

void Map_IpdCollisionDataInit(void) // 0x80041E98
{
    bzero(&g_Map.collisionData_0, sizeof(s_IpdCollisionData));
    g_Map.collisionData_0.field_1C = 512;
}

void Map_PlaceIpdAtCell(s16 ipdFileIdx, s32 cellX, s32 cellZ) // 0x80041ED0
{
    s_IpdChunk*  curChunk;
    s_IpdHeader* ipdHdr;

    ((s16*)&g_Map.ipdGridCenter_42C[cellZ])[cellX] = ipdFileIdx;

    for (curChunk = g_Map.ipdActive_15C; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
    {
        if (curChunk->cellX != cellX || curChunk->cellZ != cellZ)
        {
            continue;
        }

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            ipdHdr = curChunk->ipdHdr;
            if (ipdHdr->isLoaded)
            {
                Lm_MaterialRefCountDec(ipdHdr->lmHdr);
            }
        }

        curChunk->queueIdx = NO_VALUE;
    }
}

void Ipd_ActiveMapChunksClear(void) // 0x80041FF0
{
    Ipd_ActiveChunksClear(&g_Map, g_Map.ipdActiveSize_158);
}

void Ipd_TexturesRefClear(void) // 0x8004201C
{
    s_Texture* curTex;

    // TODO: Will these match as for loops?
    curTex = &g_Map.ipdTextures_430.fullPageTextures_58[0];
    while (curTex < (&g_Map.ipdTextures_430.fullPageTextures_58[8]))
    {
        if (curTex->refCount_14 == 0)
        {
            Texture_RefClear(curTex);
        }

        curTex++;
    }

    curTex = &g_Map.ipdTextures_430.halfPageTextures_118[0];
    while (curTex < (&g_Map.ipdTextures_430.halfPageTextures_118[2]))
    {
        if (curTex->refCount_14 == 0)
        {
            Texture_RefClear(curTex);
        }

        curTex++;
    }
}

void Map_WorldClearReset(void) // 0x800420C0
{
    Map_GlobalLmFree();
    Ipd_ActiveChunksClear(&g_Map, g_Map.ipdActiveSize_158);
    Ipd_TexturesInit();
}

void Map_GlobalLmFree(void) // 0x800420FC
{
    s_GlobalLm* globalLm;

    globalLm = &g_Map.globalLm_138;

    if (Fs_QueueEntryLoadStatusGet(globalLm->queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
        globalLm->lmHdr->isLoaded)
    {
        Lm_MaterialRefCountDec(g_Map.globalLm_138.lmHdr);
    }

    Lm_Init(&g_Map.globalLm_138, g_Map.globalLm_138.lmHdr);
}

s_Texture* Texture_InfoGet(char* texName) // 0x80042178
{
    s_Texture* tex;

    tex = Textures_ActiveTex_FindTexture(texName, &g_Map.ipdTextures_430.fullPage_0);
    if (tex != NULL)
    {
        return tex;
    }

    tex = Textures_ActiveTex_FindTexture(texName, &g_Map.ipdTextures_430.halfPage_2C);
    if (tex != NULL)
    {
        return tex;
    }

    return NULL;
}

void Ipd_MapFileInfoSet(char* mapTag, e_FsFile plmIdx, s32 activeIpdCount, bool isExterior, e_FsFile ipdFileIdx, e_FsFile texFileIdx) // 0x800421D8
{
#ifdef SH_PC_PORT
    {
        bool willRebuild = (g_Map.ipdActiveSize_158 != activeIpdCount || strcmp(mapTag, g_Map.mapTag_144) != 0);
        SH_DBG("[IPD-INIT] Ipd_MapFileInfoSet: mapTag='%s' activeIpdCount=%d isExterior=%d plmIdx=%d ipdFileIdx=%d texFileIdx=%d curSize=%d willRebuildGrid=%d",
               mapTag, activeIpdCount, isExterior, plmIdx, ipdFileIdx, texFileIdx, g_Map.ipdActiveSize_158, willRebuild);
    }
#endif
    g_Map.isExterior_588 = isExterior;
    g_Map.texFileIdx_134 = texFileIdx;

    if (plmIdx != NO_VALUE)
    {
        if (plmIdx != g_Map.globalLm_138.fileIdx)
        {
            if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm_138.queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
                g_Map.globalLm_138.lmHdr->isLoaded)
            {
                Lm_MaterialRefCountDec(g_Map.globalLm_138.lmHdr);
            }

            g_Map.globalLm_138.fileIdx  = plmIdx;
            g_Map.globalLm_138.queueIdx = NO_VALUE;
        }
    }

#ifdef SH_PC_PORT
    {
    /* Preload only applies to exterior maps — interior maps use PSX streaming. */
    bool _usePreload = g_PcConfig.preloadChunks && isExterior;
    /* On PC with preloading, only clear+rebuild when the map TAG changes
     * (actual map transition). Don't clear on same-map calls — our
     * ipdActiveSize (256) != PSX activeIpdCount (2-4) would always trigger. */
    if ((_usePreload && strcmp(mapTag, g_Map.mapTag_144) != 0) ||
        (!_usePreload && (g_Map.ipdActiveSize_158 != activeIpdCount || strcmp(mapTag, g_Map.mapTag_144) != 0)))
#else
    if (g_Map.ipdActiveSize_158 != activeIpdCount || strcmp(mapTag, g_Map.mapTag_144) != 0)
#endif
    {
        Ipd_ActiveChunksClear(&g_Map, activeIpdCount);

#ifdef SH_PC_PORT
        g_Map.ipdActiveSize_158 = _usePreload ? PC_MAX_IPD_CHUNKS : activeIpdCount;
#else
        g_Map.ipdActiveSize_158 = activeIpdCount;
#endif
        g_Map.ipdFileIdx_14C    = ipdFileIdx;
        strcpy(g_Map.mapTag_144, mapTag);

        g_Map.mapTagSize_148 = strlen(mapTag);
        Map_MakeIpdGrid(&g_Map, mapTag, ipdFileIdx);
    }
#ifdef SH_PC_PORT
    }
#endif
}

void Ipd_ActiveChunksClear(s_Map* map, s32 arg1) // 0x80042300
{
    s32          step;
    s32          i;
    s_IpdChunk*  curChunk;
    s_IpdHeader* ipdHdr0;
    s_IpdHeader* ipdHdr1;

#ifdef SH_PC_PORT
    {
        s32 activeCount = 0;
        for (i = 0; i < map->ipdActiveSize_158; i++) {
            if (map->ipdActive_15C[i].queueIdx != NO_VALUE) activeCount++;
        }
        SH_DBG("[IPD-CLEAR] Ipd_ActiveChunksClear: clearing %d active chunks (total slots=%d, newSize=%d)",
               activeCount, map->ipdActiveSize_158, arg1);
    }
#endif

    ipdHdr0 = map->ipdBuffer_150;
    step    = (map->ipdBufferSize_154 / arg1) & ~0x3;

#ifdef SH_PC_PORT
    /* On PC, initialize ALL 64 slots. First `arg1` slots get shared buffer,
       extra slots get individually malloc'd buffers for debug mode. */
    for (i = 0; i < PC_MAX_IPD_CHUNKS; i++)
    {
        curChunk = &map->ipdActive_15C[i];

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            ipdHdr1 = curChunk->ipdHdr;
            if (ipdHdr1 != NULL && ipdHdr1->isLoaded)
            {
                Lm_MaterialRefCountDec(ipdHdr1->lmHdr);
            }
        }

        curChunk->queueIdx      = NO_VALUE;
        curChunk->distance1    = INT_MAX;
        curChunk->outsideCount = 0;

        if (i < arg1)
        {
            curChunk->ipdHdr = ipdHdr0;
            *(u8**)&ipdHdr0 += step;
        }
        else if (i < PC_MAX_IPD_CHUNKS)
        {
            /* Allocate individual buffers for extra debug slots */
            if (curChunk->ipdHdr == NULL)
            {
                curChunk->ipdHdr = (s_IpdHeader*)calloc(1, step > 0 ? step : 65536);
            }
        }
    }
#else
    for (i = 0; i < 4; i++, *(u8**)&ipdHdr0 += step)
    {
        curChunk = &map->ipdActive_15C[i];

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            ipdHdr1 = curChunk->ipdHdr;
            if (ipdHdr1->isLoaded)
            {
                Lm_MaterialRefCountDec(ipdHdr1->lmHdr);
            }
        }

        curChunk->queueIdx      = NO_VALUE;
        curChunk->distance1    = INT_MAX;
        curChunk->outsideCount = 0;

        if (i < arg1)
        {
            curChunk->ipdHdr = ipdHdr0;
        }
        else
        {
            curChunk->ipdHdr = NULL;
        }
    }
#endif
}

void Map_MakeIpdGrid(s_Map* map, char* mapTag, e_FsFile fileIdxStart) // 0x800423F4
{
    char            sp10[256];
    s32             x;
    s32             z;
    s32             i;
    char*           filenameSuffix;
    s_IpdColumn*    col;

    map->ipdGridCenter_42C = (s_IpdColumn*)(&map->ipdGrid_1CC[8].idx[8]);

    for (z = -8; z < 11; z++)
    {
        for (x = -8; x < 8; x++)
        {
            ((s16*)&map->ipdGridCenter_42C[z])[x] = NO_VALUE;
        }
    }

    // Run through all game files.
    for (i = fileIdxStart; i < FS_FILE_COUNT; i++)
    {
        if (g_FileTable[i].type == FileType_Ipd)
        {
            Fs_GetFileName(sp10, i);

            if (strncmp(sp10, map->mapTag_144, map->mapTagSize_148) == 0)
            {
                filenameSuffix = &sp10[map->mapTagSize_148];
                if (ConvertHexToS8(&x, filenameSuffix[0], filenameSuffix[1]) &&
                    ConvertHexToS8(&z, filenameSuffix[2], filenameSuffix[3]))
                {
                    col         = &map->ipdGridCenter_42C[z];
                    col->idx[x] = i;
                }
            }
        }
    }
#ifdef SH_PC_PORT
    {
        s32 gridCount = 0;
        s32 gz, gx;
        for (gz = -8; gz < 11; gz++) {
            for (gx = -8; gx < 8; gx++) {
                if (((s16*)&map->ipdGridCenter_42C[gz])[gx] != NO_VALUE) gridCount++;
            }
        }
        SH_DBG("[IPD-GRID] Map_MakeIpdGrid complete: mapTag='%s' totalCells=%d", map->mapTag_144, gridCount);
    }
#endif
}

bool ConvertHexToS8(s32* out, char hex0, char hex1) // 0x8004255C
{
    char low;
    char high;
    char letterIdx;
    char hexVal;
    bool isNumber;

    high     = hex0 - '0';
    isNumber = high < 10;

    hexVal   = high;
    hexVal <<= 4;
    if (!isNumber)
    {
        letterIdx = hex0 - 'A';
        if (letterIdx > 5)
        {
            return false;
        }

        hexVal = (hex0 + 201) << 4;
    }

    low      = hex1 - '0';
    isNumber = low < 10;
    if (isNumber)
    {
        hexVal |= low;
    }
    else
    {
        letterIdx = hex1 - 'A';
        if (letterIdx > 5)
        {
            return false;
        }

        hexVal |= hex1 + 201;
    }

    *out = (hexVal << 24) >> 24; // Sign extend.
    return true;
}

s_IpdCollisionData** func_800425D8(s32* collDataIdx) // 0x800425D8
{
    s_IpdChunk*         ptr;
    s_IpdCollisionData* collData;
    s_IpdHeader*        ipdHdr;

    ptr          = g_Map.ipdActive_15C;
    *collDataIdx = 0;

    while (ptr < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158])
    {
        if (Fs_QueueEntryLoadStatusGet(ptr->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            ipdHdr = ptr->ipdHdr;
            if (ipdHdr->isLoaded)
            {
                collData = IpdHeader_CollisionDataGet(ipdHdr);
                if (collData != NULL)
                {
                    D_800C1010[(*collDataIdx)++] = collData;
                }
            }
        }

        ptr++;
    }

    return &D_800C1010[0];
}

s_IpdCollisionData* func_800426E4(s32 posX, s32 posZ) // 0x800426E4
{
    s32          geomX;
    s32          geomZ;
    s32          cellX;
    s32          cellZ;
    s_IpdHeader* ipdHdr;
    s_IpdChunk*  curChunk;

    // Convert position to geometry space.
    geomX = Q12_TO_Q8(posX);
    geomZ = Q12_TO_Q8(posZ);

    // Compute cell coordinates.
    cellX = FLOOR_TO_STEP(geomX, Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(geomZ, Q12_TO_Q8(CHUNK_CELL_SIZE));

    // Run through active chunks.
    for (curChunk = g_Map.ipdActive_15C; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
    {
        // Check if chunk is loaded.
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < FsQueueEntryLoadStatus_Loaded)
        {
            continue;
        }

        // Check if chunk matches cell coordinates.
        ipdHdr = curChunk->ipdHdr;
        if (ipdHdr->isLoaded &&
            curChunk->cellX == cellX && curChunk->cellZ == cellZ)
        {
            return &ipdHdr->collisionData;
        }
    }

    // Fallback.
    if (((s16*)(&g_Map.ipdGridCenter_42C[cellZ]))[cellX] != NO_VALUE)
    {
        return NULL;
    }
    else
    {
        return &g_Map.collisionData_0;
    }
}

s32 func_8004287C(s_WorldObjectModel* model, s_WorldObjectMetadata* metadata, q19_12 posX, q19_12 posZ) // 0x8004287C
{
    s_IpdChunk* chunks[4];
    q19_12      distsToEdges[4];
    q23_8       geomX;
    q23_8       geomZ;
    s32         cellX;
    s32         cellZ;
    q19_12      distToEdge;
    s32         i;
    s32         j;
    s32         k;
    s32         chunkIdx;
    s_IpdChunk* curChunk;
    s_GlobalLm* globalLm;

    globalLm = &g_Map.globalLm_138;

    // Convert position to geometry space.
    geomX = Q12_TO_Q8(posX);
    geomZ = Q12_TO_Q8(posZ);

#ifdef SH_PC_PORT
    /* PC: belt-and-suspenders null guards. Crash dump 2026-05-02 02:08
     * (FAILURE_BUCKET_ID INVALID_POINTER_READ at func_8004287C+0x337,
     * `mov rax, qword ptr [rax]`) hit during GameBoot_InGameInit on
     * the new map after door transition. The chunk list is still
     * populating at this point — some slots have queueIdx set but
     * ipdHdr / ipdHdr->lmHdr / etc. not yet ready. */
    if (globalLm->lmHdr == NULL) {
        return 0;
    }
#endif
    if (Fs_QueueEntryLoadStatusGet(globalLm->queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
        globalLm->lmHdr->isLoaded &&
        Lm_ModelFind(model, g_Map.globalLm_138.lmHdr, metadata))
    {
        return 2;
    }

    cellX = FLOOR_TO_STEP(geomX, Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(geomZ, Q12_TO_Q8(CHUNK_CELL_SIZE));

    for (curChunk = g_Map.ipdActive_15C, chunkIdx = 0;
         curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158];
         curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < FsQueueEntryLoadStatus_Loaded)
        {
            continue;
        }

#ifdef SH_PC_PORT
        /* Same guard as above: a queueIdx >= Loaded doesn't yet
         * imply ipdHdr was populated by IpdHeader_FixOffsets — there's
         * a small window during multi-stage loads where the queue
         * thinks the read is done but the post-read fixup hasn't
         * run yet. */
        if (curChunk->ipdHdr == NULL) {
            continue;
        }
#endif
        if (!curChunk->ipdHdr->isLoaded)
        {
            continue;
        }

        if (!g_Map.isExterior_588)
        {
            if (curChunk->cellX == cellX && curChunk->cellZ == cellZ)
            {
                chunks[chunkIdx] = curChunk;
                chunkIdx++;
                break;
            }
        }
        else
        {
            if (curChunk->cellX >= (cellX - 1) && (cellX + 1) >= curChunk->cellX &&
                curChunk->cellZ >= (cellZ - 1) && (cellZ + 1) >= curChunk->cellZ)
            {
#ifdef SH_PC_PORT
                /* `chunks[4]` is a fixed stack array. With PC's chunk
                 * count bump (PC_MAX_IPD_CHUNKS=256, vs PSX's 4), more
                 * loaded chunks can pass the ±1 cell-range check and
                 * the loop OOB-writes past chunks[4]/distsToEdges[4],
                 * stomping adjacent stack — that's the source of the
                 * "NHS." filename bytes appearing in OT addr fields
                 * (chunks[] holds s_IpdChunk* whose names begin with
                 * "BG_*.PLM" etc.) and the func_8004287C+0x422 crash.
                 *
                 * Cap chunkIdx at 4 (the original PSX max). Sorting
                 * inserts only when there's room; we drop further
                 * matches outside the 4 nearest. */
                if (chunkIdx >= 4) {
                    continue;
                }
#endif
                distToEdge = Ipd_DistanceToEdgeGet(geomX, geomZ, curChunk->cellX, curChunk->cellZ);
                for (i = 0; i < chunkIdx; i++)
                {
                    if (distToEdge < distsToEdges[i])
                    {
                        break;
                    }
                }

                for (j = chunkIdx; j >= (i + 1); j--)
                {
                    distsToEdges[j] = distsToEdges[j - 1];
                    chunks[j]       = chunks[j - 1];
                }

                chunkIdx++;
                distsToEdges[j] = distToEdge;
                chunks[j]       = curChunk;
            }
        }
    }

    for (k = 0; k < chunkIdx; k++)
    {
        curChunk = chunks[k];
#ifdef SH_PC_PORT
        /* Same load-order race as the earlier guards in this function:
         * curChunk->ipdHdr passed the null check above (else it
         * wouldn't be in chunks[]), but its ipdHdr->lmHdr can still be
         * NULL during PC's multi-stage post-load fixup window. Crash
         * dump 2026-05-02 03:36 hit func_8004287C+0x422 here with
         * `mov rax, qword ptr [rax]` reading lmHdr through a NULL. */
        if (curChunk->ipdHdr == NULL || curChunk->ipdHdr->lmHdr == NULL) {
            continue;
        }
#endif
        if (Lm_ModelFind(model, curChunk->ipdHdr->lmHdr, metadata))
        {
            return (curChunk - g_Map.ipdActive_15C) + 3;
        }
    }

    return 0;
}

bool IpdHeader_IsLoaded(s32 ipdIdx) // 0x80042C04
{
    return IpdHeader_LoadStateGet(&g_Map.ipdActive_15C[ipdIdx]) >= StaticModelLoadState_Loaded;
}

void Ipd_ChunkInit(q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1) // 0x80042C3C
{
    s32         fullPageTexCount;
    s_IpdChunk* curChunk;

#ifdef SH_PC_PORT
    {
        static s32 chunkInitCallCount = 0;
        if (chunkInitCallCount < 20 || (chunkInitCallCount % 300) == 0) {
            SH_DBG("[IPD-INIT] Ipd_ChunkInit #%d: pos0=(%d,%d) pos1=(%d,%d) globalLm.queueIdx=%d globalLm.fileIdx=%d",
                   chunkInitCallCount, posX0, posZ0, posX1, posZ1,
                   g_Map.globalLm_138.queueIdx, g_Map.globalLm_138.fileIdx);
        }
        chunkInitCallCount++;
    }
#endif

    g_Map.positionX_578 = posX1;
    g_Map.positionX_57C = posZ1;

    if (g_Map.globalLm_138.queueIdx == NO_VALUE)
    {
        g_Map.globalLm_138.queueIdx = Fs_QueueStartRead(g_Map.globalLm_138.fileIdx, g_Map.globalLm_138.lmHdr);
    }

#ifdef SH_PC_PORT
    if (g_PcConfig.preloadChunks && g_Map.isExterior_588)
    {
        /* PC: Load ALL IPD chunks for the entire map at once.
         * Scan the full grid (-8..+10 in Z, -8..+7 in X) and load every
         * IPD file that exists. This eliminates PSX CD streaming entirely. */
        static int _preloaded = 0;
        s32 px, pz, pFileIdx;
        s_IpdChunk* pChunk;

        /* First, flush the FS queue to load the global LM */
        {
            int flushCount = 0;
            while (Fs_QueueGetLength() > 0 && flushCount < 500)
            {
                Fs_QueueUpdate();
                flushCount++;
            }
        }

        /* Fix up global LM if just loaded */
        if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm_138.queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
            !g_Map.globalLm_138.lmHdr->isLoaded)
        {
            fullPageTexCount                         = g_Map.ipdTextures_430.fullPage_0.count_0;
            g_Map.ipdTextures_430.fullPage_0.count_0 = 4;

            LmHeader_FixOffsets(g_Map.globalLm_138.lmHdr);
#ifdef SH_PC_PORT
            SH_DBG("[GLOBAL-LM-A] FixOffsets done: matCnt=%d modelCnt=%d texCount=%d", g_Map.globalLm_138.lmHdr->materialCount, g_Map.globalLm_138.lmHdr->modelCount, g_Map.ipdTextures_430.fullPage_0.count_0);
#endif
            Lm_MaterialsLoadWithFilter(g_Map.globalLm_138.lmHdr, &g_Map.ipdTextures_430.fullPage_0, NULL, g_Map.texFileIdx_134, BlendMode_Additive);
#ifdef SH_PC_PORT
            SH_DBG("[GLOBAL-LM-A] MaterialsLoadWithFilter done");
#endif
            Lm_MaterialFlagsApply(g_Map.globalLm_138.lmHdr);

            g_Map.ipdTextures_430.fullPage_0.count_0 = fullPageTexCount;
        }

        /* Scan entire grid and load all IPD files into sequential slots */
        {
            s32 nextSlot = 0;
            for (pz = -8; pz < 11; pz++)
            {
                for (px = -8; px < 8; px++)
                {
                    pFileIdx = Map_IpdIdxGet(px, pz);
                    if (pFileIdx == NO_VALUE)
                        continue;

                    /* Check if already loaded */
                    if (Map_IsIpdPresent(g_Map.ipdActive_15C, px, pz))
                        continue;

                    /* Find next unused slot */
                    while (nextSlot < PC_MAX_IPD_CHUNKS &&
                           Fs_QueueEntryLoadStatusGet(g_Map.ipdActive_15C[nextSlot].queueIdx) >= FsQueueEntryLoadStatus_Loaded)
                    {
                        nextSlot++;
                    }

                    if (nextSlot >= PC_MAX_IPD_CHUNKS)
                    {
                        SH_DBG("[PRELOAD] Ran out of chunk slots at cell (%d,%d)!", px, pz);
                        break;
                    }

                    pChunk = &g_Map.ipdActive_15C[nextSlot];
                    pChunk->materialCount = 0;

                    /* Start loading */
                    Ipd_LoadStart(pChunk, pFileIdx, px, pz, posX0, posZ0, posX1, posZ1, g_Map.isExterior_588);

                    /* Immediately flush the read */
                    {
                        int flushCount = 0;
                        while (Fs_QueueGetLength() > 0 && flushCount < 500)
                        {
                            Fs_QueueUpdate();
                            flushCount++;
                        }
                    }

                    /* Fix up the chunk if loaded */
                    if (Fs_QueueEntryLoadStatusGet(pChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
                    {
                        IpdHeader_FixOffsets(pChunk->ipdHdr, &g_Map.globalLm_138.lmHdr, 1,
                                            &g_Map.ipdTextures_430.fullPage_0,
                                            &g_Map.ipdTextures_430.halfPage_2C,
                                            g_Map.texFileIdx_134);
                        func_80044044(pChunk->ipdHdr, pChunk->cellX, pChunk->cellZ);
                    }

                    nextSlot++;
                }
                if (nextSlot >= PC_MAX_IPD_CHUNKS) break;
            }
        }

        if (!_preloaded) {
            /* Count loaded chunks */
            s32 loadedCount = 0;
            for (curChunk = g_Map.ipdActive_15C; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
            {
                if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
                    loadedCount++;
            }
            SH_DBG("[PRELOAD] Loaded %d chunks for map '%s'", loadedCount, g_Map.mapTag_144);
            _preloaded = 1;
        }

        /* Update cell position for rendering */
        {
            s32 cellX1 = FLOOR_TO_STEP(Q12_TO_Q8(posX1), Q12_TO_Q8(CHUNK_CELL_SIZE));
            s32 cellZ1 = FLOOR_TO_STEP(Q12_TO_Q8(posZ1), Q12_TO_Q8(CHUNK_CELL_SIZE));
            g_Map.cellX_580 = cellX1;
            g_Map.cellZ_584 = cellZ1;
        }

        /* Update distance samples for all chunks */
        Ipd_ActiveChunksSample(&g_Map, posX0, posZ0, posX1, posZ1, g_Map.isExterior_588);
        Ipd_ChunkMaterialsApply(&g_Map);
    }
    else
#endif
    {
        Map_ChunkLoad(&g_Map, posX0, posZ0, posX1, posZ1);
    }

    if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm_138.queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
        !g_Map.globalLm_138.lmHdr->isLoaded)
    {
        fullPageTexCount                         = g_Map.ipdTextures_430.fullPage_0.count_0;
        g_Map.ipdTextures_430.fullPage_0.count_0 = 4;

        LmHeader_FixOffsets(g_Map.globalLm_138.lmHdr);
#ifdef SH_PC_PORT
        SH_DBG("[GLOBAL-LM-B] FixOffsets done: matCnt=%d modelCnt=%d texCount=%d", g_Map.globalLm_138.lmHdr->materialCount, g_Map.globalLm_138.lmHdr->modelCount, g_Map.ipdTextures_430.fullPage_0.count_0);
#endif
        Lm_MaterialsLoadWithFilter(g_Map.globalLm_138.lmHdr, &g_Map.ipdTextures_430.fullPage_0, NULL, g_Map.texFileIdx_134, BlendMode_Additive);
#ifdef SH_PC_PORT
        SH_DBG("[GLOBAL-LM-B] MaterialsLoadWithFilter done");
#endif
        Lm_MaterialFlagsApply(g_Map.globalLm_138.lmHdr);

        g_Map.ipdTextures_430.fullPage_0.count_0 = fullPageTexCount;
    }

    for (curChunk = g_Map.ipdActive_15C; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            IpdHeader_FixOffsets(curChunk->ipdHdr, &g_Map.globalLm_138.lmHdr, 1, &g_Map.ipdTextures_430.fullPage_0, &g_Map.ipdTextures_430.halfPage_2C, g_Map.texFileIdx_134);
            func_80044044(curChunk->ipdHdr, curChunk->cellX, curChunk->cellZ);
        }
    }
}

q19_12 Ipd_PaddedDistanceToEdgeGet(q19_12 posX, q19_12 posZ, s32 cellX, s32 cellZ, bool isExterior) // 0x80042DE8
{
    q19_12 dist;

    dist = Ipd_DistanceToEdgeGet(Q12_TO_Q8(posX), Q12_TO_Q8(posZ), cellX, cellZ);
    if (isExterior)
    {
        dist -= Q12(1.0f);
        if (dist < Q12(0.0f))
        {
            dist = Q12(0.0f);
        }
    }

    return dist;
}

q19_12 Ipd_DistanceToEdgeGet(q19_12 posX, q19_12 posZ, s32 cellX, s32 cellZ) // 0x80042E2C
{
    #define OUTSIDE_DIST(val, min, max) \
        (((val) < (min)) ? ((min) - (val)) : (((max) <= (val)) ? ((val) - (max)) : 0))

    s32 cellBoundX;
    s32 cellBoundZ;
    s32 x;
    s32 z;

    // Compute cell boundary position.
    cellBoundX = cellX * Q12_TO_Q8(CHUNK_CELL_SIZE);
    cellBoundZ = cellZ * Q12_TO_Q8(CHUNK_CELL_SIZE);

    x = OUTSIDE_DIST(posX, cellBoundX, cellBoundX + Q12_TO_Q8(CHUNK_CELL_SIZE));
    z = OUTSIDE_DIST(posZ, cellBoundZ, cellBoundZ + Q12_TO_Q8(CHUNK_CELL_SIZE));
    return Vc_VectorMagnitudeCalc(x, 0, z);
}

#ifdef SH_PC_PORT
static bool s_chunkScanShouldLog = false;
#endif

s32 Map_ChunkLoad(s_Map* map, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1) // 0x80042EBC
{
    s32          cellX0;
    s32          cellZ0;
    s32          cellZ1;
    s32          cellX1;
    s32          queueIdx;
    s32          projCellX;
    s32          projCellZ;
    s32          chunkIdx;
    s32          curQueueIdx;
    s32          x;
    s32          z;
    s_IpdChunk*  chunk;
    s_IpdHeader* ipdHdr;

    queueIdx = NO_VALUE;

    cellX0 = FLOOR_TO_STEP(Q12_TO_Q8(posX0), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ0 = FLOOR_TO_STEP(Q12_TO_Q8(posZ0), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellX1 = FLOOR_TO_STEP(Q12_TO_Q8(posX1), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ1 = FLOOR_TO_STEP(Q12_TO_Q8(posZ1), Q12_TO_Q8(CHUNK_CELL_SIZE));

    map->cellX_580 = cellX1;
    map->cellZ_584 = cellZ1;

#ifdef SH_PC_PORT
    {
        static int chunkLoadCallsSinceCellChange = 0;
        static int prevCellX = 999, prevCellZ = 999;
        static int totalCalls = 0;
        bool cellChanged = (cellX0 != prevCellX || cellZ0 != prevCellZ);
        s_chunkScanShouldLog = cellChanged || (chunkLoadCallsSinceCellChange < 10) || ((totalCalls % 300) == 0);
        totalCalls++;
        if (cellChanged) {
            chunkLoadCallsSinceCellChange = 0;
            prevCellX = cellX0;
            prevCellZ = cellZ0;
        }
        chunkLoadCallsSinceCellChange++;
        if (s_chunkScanShouldLog) {
            SH_DBG("[CHUNK-SCAN] Map_ChunkLoad #%d: playerCell=(%d,%d) ext=%d posQ12=(%d,%d) cellChanged=%d",
                   totalCalls, cellX0, cellZ0, map->isExterior_588, posX0, posZ0, cellChanged);
        }
    }
#endif

    Ipd_ActiveChunksSample(map, posX0, posZ0, posX1, posZ1, map->isExterior_588);
    Ipd_ChunkMaterialsApply(map);

    {
#ifdef SH_PC_PORT
    s32 scanMin = g_DebugCamEnabled ? -4 : -1;
    s32 scanMax = g_DebugCamEnabled ? 5 : 1;
    s32 loadsThisFrame = 0;
    s32 maxLoadsPerFrame = g_DebugCamEnabled ? 2 : 9;
#else
    s32 scanMin = -1;
    s32 scanMax = 1;
#endif
    for (z = scanMin; z <= scanMax; z++)
    {
        for (x = scanMin; x <= scanMax; x++)
        {
#ifdef SH_PC_PORT
            if (g_DebugCamEnabled || map->isExterior_588 || (x == 0 && z == 0))
#else
            if (map->isExterior_588 || (x == 0 && z == 0))
#endif
            {
                projCellZ = cellZ0 + z;
                projCellX = cellX0 + x;

                chunkIdx = Map_IpdIdxGet(projCellX, projCellZ);
                if (chunkIdx != NO_VALUE &&
#ifdef SH_PC_PORT
                    (g_DebugCamEnabled || Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, projCellX, projCellZ, map->isExterior_588) <= Q12(0.0f)) &&
#else
                    Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, projCellX, projCellZ, map->isExterior_588) <= Q12(0.0f) &&
#endif
                    !Map_IsIpdPresent(map->ipdActive_15C, projCellX, projCellZ))
                {
#ifdef SH_PC_PORT
                    if (loadsThisFrame >= maxLoadsPerFrame)
                    {
                        continue;
                    }
#endif
                    chunk = Ipd_FreeChunkFind(map->ipdActive_15C, map->isExterior_588);

#ifdef SH_PC_PORT
                    if (chunk == NULL)
                    {
                        continue;
                    }
#endif

                    if (Fs_QueueEntryLoadStatusGet(chunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
                    {
                        ipdHdr = chunk->ipdHdr;
                        if (ipdHdr->isLoaded)
                        {
                            Lm_MaterialRefCountDec(ipdHdr->lmHdr);
                        }
                    }

#ifdef SH_PC_PORT
                    /* Reset material count so this chunk isn't re-evicted in
                       the same frame (stale materialCount from pre-eviction
                       would make it the top eviction candidate again). */
                    chunk->materialCount = 0;
#endif

                    curQueueIdx = Ipd_LoadStart(chunk, chunkIdx, projCellX, projCellZ, posX0, posZ0, posX1, posZ1, map->isExterior_588);
                    if (curQueueIdx != NO_VALUE)
                    {
                        queueIdx = curQueueIdx;
#ifdef SH_PC_PORT
                        loadsThisFrame++;
#endif
                    }
                }
            }
        }
    }
    } /* close scanMin/scanMax block */

    return queueIdx;
}

#ifdef SH_PC_PORT
/* Registry of headers IpdHeader_FixOffsets has reformatted, keyed by the
 * resulting heap lmHdr. The IPD file's byte 1 lands on the isLoaded field but is
 * NOT a reliable "reformatted" flag (some IPDs store 1, and overlapping PSX-RAM
 * buffer reuse leaves it stale), so isLoaded alone cannot gate lmHdr derefs.
 * A fresh file load overwrites lmHdr with raw bytes, so a registry mismatch
 * means the header has not been reformatted yet. */
static struct { s_IpdHeader* hdr; s_LmHeader* lm; } s_pcFixedIpd[64];
static s32 s_pcFixedIpdCount = 0;

bool IpdHeader_PC_IsReformatted(s_IpdHeader* ipdHdr)
{
    s32 i;
    for (i = 0; i < s_pcFixedIpdCount; i++)
    {
        if (s_pcFixedIpd[i].hdr == ipdHdr)
        {
            return s_pcFixedIpd[i].lm == ipdHdr->lmHdr;
        }
    }
    return false;
}
#endif

void Ipd_ActiveChunksSample(s_Map* map, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x800431E4
{
    s_IpdChunk* curChunk;

    // Run through active chunks.
    for (curChunk = map->ipdActive_15C; curChunk < &map->ipdActive_15C[map->ipdActiveSize_158]; curChunk++)
    {
        if (curChunk->queueIdx == NO_VALUE)
        {
            curChunk->distance0  = INT_MAX;
            curChunk->distance1 = INT_MAX;
        }
        else
        {
            Ipd_DistanceToEdgeCalc(curChunk, posX0, posZ0, posX1, posZ1, isExterior);
        }

#ifdef SH_PC_PORT
        /* A just-loaded chunk's isLoaded byte comes straight from the IPD file
         * (or stale overlapping RAM) and can read true before IpdHeader_FixOffsets
         * has reformatted it — the fixup runs only after Map_ChunkLoad returns.
         * Correct it here so this sample (and every other isLoaded-gated lmHdr
         * deref this frame) skips the chunk until it is actually reformatted,
         * matching PSX where isLoaded stays false until the fixup. */
        if (curChunk->queueIdx != NO_VALUE && curChunk->ipdHdr != NULL &&
            Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded &&
            !IpdHeader_PC_IsReformatted(curChunk->ipdHdr))
        {
            curChunk->ipdHdr->isLoaded = false;
        }
#endif

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < FsQueueEntryLoadStatus_Loaded || !curChunk->ipdHdr->isLoaded)
        {
            curChunk->materialCount = 0;
        }
        else
        {
            curChunk->materialCount = Ipd_HalfPageMaterialCountGet(curChunk->ipdHdr);
        }

        if (curChunk->distance0 > Q12(0.0f) && curChunk->distance1 > Q12(0.0f))
        {
            curChunk->outsideCount++;
        }
        else
        {
            curChunk->outsideCount = 0;
        }
    }
}

void Ipd_DistanceToEdgeCalc(s_IpdChunk* chunk, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x80043338
{
    chunk->distance0  = Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, chunk->cellX, chunk->cellZ, isExterior);
    chunk->distance1 = Ipd_PaddedDistanceToEdgeGet(posX1, posZ1, chunk->cellX, chunk->cellZ, isExterior);
}

void Ipd_ChunkMaterialsApply(s_Map* map) // 0x800433B8
{
    s_IpdChunk* curChunk;

#ifdef SH_PC_PORT
    q19_12 _matDist = (g_PcConfig.preloadChunks && g_Map.isExterior_588 && g_DebugCamEnabled && !g_DebugFogDisabled) ? Q12(35.0f) : Q12(0.0f);
#else
    #define _matDist Q12(0.0f)
#endif

    for (curChunk = &map->ipdActive_15C[0]; curChunk < &map->ipdActive_15C[map->ipdActiveSize_158]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            if (curChunk->ipdHdr->isLoaded &&
                curChunk->distance0 > _matDist && curChunk->distance1 > _matDist)
            {
                Lm_MaterialRefCountDec(curChunk->ipdHdr->lmHdr);
            }
        }
    }

    for (curChunk = &map->ipdActive_15C[0]; curChunk < &map->ipdActive_15C[map->ipdActiveSize_158]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= FsQueueEntryLoadStatus_Loaded)
        {
            if (curChunk->ipdHdr->isLoaded &&
                (curChunk->distance0 <= _matDist || curChunk->distance1 <= _matDist))
            {
                Ipd_MaterialsLoad(curChunk->ipdHdr, &map->ipdTextures_430.fullPage_0, &map->ipdTextures_430.halfPage_2C, map->texFileIdx_134);
                Lm_MaterialFlagsApply(curChunk->ipdHdr->lmHdr);
            }
        }
    }
}

s32 Map_IpdIdxGet(s32 cellX, s32 cellZ) // 0x80043554
{
    // @hack
    return ((s16*)&g_Map.ipdGridCenter_42C[cellZ])[cellX];
}

bool Map_IsIpdPresent(s_IpdChunk* chunks, s32 cellX, s32 cellZ) // 0x80043578
{
    s32 i;

    for (i = 0; i < g_Map.ipdActiveSize_158; i++)
    {
        if (chunks[i].queueIdx != NO_VALUE &&
            cellX == chunks[i].cellX && cellZ == chunks[i].cellZ)
        {
            return true;
        }
    }

    return false;
}

s_IpdChunk* Ipd_FreeChunkFind(s_IpdChunk* chunks, bool isExterior)
{
    s32         largestMatCount;
    q19_12      farthestDist;
    q19_12      dist;
    u32         largestOutsideCount;
    s32         matCount;
    s_IpdChunk* curChunk;
    s_IpdChunk* activeChunk;

    activeChunk         = NULL;
    largestOutsideCount = 0;
    largestMatCount     = 0;
    farthestDist        = Q12(0.0f);

    for (curChunk = chunks; curChunk < &chunks[g_Map.ipdActiveSize_158]; curChunk++)
    {
        if (!isExterior)
        {
            if (curChunk->queueIdx == NO_VALUE)
            {
                activeChunk = curChunk;
                break;
            }
            else
            {
                if (largestOutsideCount < curChunk->outsideCount)
                {
                    largestOutsideCount = curChunk->outsideCount;
                    activeChunk         = curChunk;
                }
            }
        }
        else
        {
            if (curChunk->queueIdx == NO_VALUE)
            {
#ifdef SH_PC_PORT
                /* On PC with 64 slots, immediately return empty slots
                   instead of competing with occupied chunks' materialCount.
                   PSX never had empty slots (4 slots always full). */
                activeChunk = curChunk;
                break;
#else
                matCount = 0;

                if (largestMatCount == 0)
                {
                    dist = INT_MAX;
                }
                else
                {
                    continue;
                }
#endif
            }
            else
            {
                matCount = curChunk->materialCount;

                dist = curChunk->distance0;
                if (dist == Q12(0.0f))
                {
                    continue;
                }
            }

#ifdef SH_PC_PORT
            /* Only reach here for occupied chunks (empty slots break above) */
#endif
            if (largestMatCount < matCount || (matCount == largestMatCount && farthestDist < dist))
            {
                farthestDist    = dist;
                activeChunk     = curChunk;
                largestMatCount = matCount;
            }
        }
    }

    return activeChunk;
}

s32 Ipd_LoadStart(s_IpdChunk* chunk, e_FsFile fileIdx, s32 cellX, s32 cellZ, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x800436D8
{
    if (fileIdx == NO_VALUE)
    {
        return fileIdx;
    }

    chunk->cellX    = cellX;
    chunk->cellZ    = cellZ;
    chunk->queueIdx = Fs_QueueStartRead(fileIdx, chunk->ipdHdr);

    Ipd_DistanceToEdgeCalc(chunk, posX0, posZ0, posX1, posZ1, isExterior);

#ifdef SH_PC_PORT
    SH_DBG("[IPD-LOAD] Ipd_LoadStart: cell=(%d,%d) fileIdx=%d queueIdx=%d chunkSlot=%d",
           cellX, cellZ, fileIdx, chunk->queueIdx, (s32)(chunk - g_Map.ipdActive_15C));
#endif

    return chunk->queueIdx;
}

bool Ipd_AreChunksLoaded(void) // 0x80043740
{
    s32         i;
    s_IpdChunk* curChunk;

    switch (LmHeader_LoadStateGet(&g_Map.globalLm_138))
    {
        case StaticModelLoadState_Invalid:
            break;

        case StaticModelLoadState_Unloaded:
            return false;

        case StaticModelLoadState_Corrupted:
            return false;
    }

    for (curChunk = g_Map.ipdActive_15C, i = 0;
         i < g_Map.ipdActiveSize_158;
         i++, curChunk++)
    {
        switch (IpdHeader_LoadStateGet(curChunk))
        {
            case StaticModelLoadState_Invalid:
            case StaticModelLoadState_Loaded:
                continue;
        }

        if (curChunk->distance0 <= Q12(0.0f) || curChunk->distance1 <= Q12(0.0f))
        {
            return false;
        }
    }

    return true;
}

bool func_80043830(void) // 0x80043830
{
    s32         loadState;
    s_IpdChunk* curChunk;

#ifdef SH_PC_PORT
    /* On PC, IPD loading is synchronous (Fs_QueueStartRead completes immediately).
       Never block the render loop waiting for chunks. */
    return false;
#endif

    for (curChunk = &g_Map.ipdActive_15C[0]; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
    {
        loadState = IpdHeader_LoadStateGet(curChunk);
        if (loadState == StaticModelLoadState_Invalid || loadState == StaticModelLoadState_Loaded ||
            (curChunk->distance0 > Q12(0.0f) && curChunk->distance1 > Q12(0.0f)))
        {
            continue;
        }

        if (!Ipd_CellPositionMatchCheck(curChunk, &g_Map))
        {
            continue;
        }

        if (Ipd_DistanceToEdgeGet(Q12_TO_Q8(g_Map.positionX_578), Q12_TO_Q8(g_Map.positionX_57C), curChunk->cellX, curChunk->cellZ) <= Q8(4.5f))
        {
            return true;
        }
    }

    return false;
}

bool func_8004393C(q19_12 posX, q19_12 posZ) // 0x8004393C
{
    s32 cellX;
    s32 cellZ;

    cellX = FLOOR_TO_STEP(Q12_TO_Q8(posX), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(Q12_TO_Q8(posZ), Q12_TO_Q8(CHUNK_CELL_SIZE));

    if (g_Map.isExterior_588)
    {
        return Ipd_DistanceToEdgeGet(Q12_TO_Q8(g_Map.positionX_578), Q12_TO_Q8(g_Map.positionX_57C), cellX, cellZ) <= Q8(4.5f);
    }

    if (cellX == g_Map.cellX_580 &&
        cellZ == g_Map.cellZ_584)
    {
        return true;
    }

    return false;
}

void Ipd_ChunkCheckDraw(GsOT* ot, s32 arg1) // 0x80043A24
{
    s32         queueState;
    s_IpdChunk* curChunk;

    queueState = Fs_QueueEntryLoadStatusGet(g_Map.globalLm_138.queueIdx);

    if (queueState == FsQueueEntryLoadStatus_Unloaded)
    {
        return;
    }

    if (!(queueState == FsQueueEntryLoadStatus_Invalid ||
          (queueState == FsQueueEntryLoadStatus_Loaded && g_Map.globalLm_138.lmHdr->isLoaded)))
    {
        return;
    }

    curChunk = &g_Map.ipdActive_15C[0];
#ifdef SH_PC_PORT
    {
        int drawCount = 0;
        int drawLimit = g_DebugCamEnabled ? 16 : PC_MAX_IPD_CHUNKS;
        int totalChunks = 0, loadedChunks = 0, cellMatchChunks = 0;
#endif
    for (; curChunk < &g_Map.ipdActive_15C[g_Map.ipdActiveSize_158]; curChunk++)
    {
#ifdef SH_PC_PORT
        totalChunks++;
        if (IpdHeader_LoadStateGet(curChunk) >= StaticModelLoadState_Loaded) loadedChunks++;
#endif
        if (IpdHeader_LoadStateGet(curChunk) >= StaticModelLoadState_Loaded && Ipd_CellPositionMatchCheck(curChunk, &g_Map))
        {
#ifdef SH_PC_PORT
            cellMatchChunks++;
#endif
            Gfx_IpdChunkDraw(curChunk->ipdHdr, g_Map.positionX_578, g_Map.positionX_57C, ot, arg1);
#ifdef SH_PC_PORT
            if (++drawCount >= drawLimit) break;
#endif
        }
    }
#ifdef SH_PC_PORT
    }
#endif
}

bool Ipd_CellPositionMatchCheck(s_IpdChunk* chunk, s_Map* map)
{
#ifdef SH_PC_PORT
    if (g_DebugCamEnabled || g_PcConfig.disableCulling) return true;
    /* Expand match in X and Z so adjacent geometry isn't clipped at the
     * screen edges. On PSX exact-cell match was fine because the 4:3
     * viewport stayed inside one cell width. On PC:
     *   * 16:9 + Hor+ extends X view ~1.33x
     *   * + pixel-aspect compensation extends X view another ~9.4%
     *     (commit 9275146d8 in PsyCross, fixes Harry-too-thick).
     * Combined, ~1.46x more horizontal extent than PSX. ±2 X cells
     * covers the worst case (e.g. cafe interior where the side walls
     * are ~1.5 cells away from Harry's cell). Z stays ±1 since the
     * vertical FOV is unchanged. */
    {
        s32 dx = (s32)chunk->cellX - map->cellX_580;
        s32 dz = (s32)chunk->cellZ - map->cellZ_584;
        if (dx >= -2 && dx <= 2 && dz >= -1 && dz <= 1) return true;
    }
#else
    if (map->cellX_580 == chunk->cellX &&
        map->cellZ_584 == chunk->cellZ)
    {
        return true;
    }
#endif

    return map->isExterior_588 != false;
}

bool IpdHeader_IsTextureLoaded(s_IpdHeader* ipdHdr) // 0x80043B70
{
    if (!ipdHdr->isLoaded)
    {
        return false;
    }

    return LmHeader_IsTextureLoaded(ipdHdr->lmHdr);
}

s_IpdCollisionData* IpdHeader_CollisionDataGet(s_IpdHeader* ipdHdr) // 0x80043BA4
{
    if (ipdHdr->isLoaded)
    {
        return &ipdHdr->collisionData;
    }

    return NULL;
}

void IpdHeader_FixOffsets(s_IpdHeader* ipdHdr, s_LmHeader** lmHdrs, s32 lmHdrCount, s_ActiveTextures* fullPageActiveTexs, s_ActiveTextures* halfPageActiveTexs, e_FsFile fileIdx) // 0x80043BC4
{
#ifdef SH_PC_PORT
    {
        s32 fi, slot = -1;

        for (fi = 0; fi < s_pcFixedIpdCount; fi++) {
            if (s_pcFixedIpd[fi].hdr == ipdHdr) { slot = fi; break; }
        }
        if (slot >= 0 && s_pcFixedIpd[slot].lm == ipdHdr->lmHdr) {
            return; /* already reformatted, lmHdr still our heap copy */
        }

        extern void IpdHeader_FixOffsets_PC(s_IpdHeader* ipdHdr);
        IpdHeader_FixOffsets_PC(ipdHdr);
        ipdHdr->isLoaded = true;
        /* LmHeader_FixOffsets now uses PC reformatter */
        LmHeader_FixOffsets(ipdHdr->lmHdr);
        {
            /* lmHdr lives in PSX RAM — subsequent chunk loads at overlapping
             * addresses overwrite the fixed-up modelHdrs/materials pointers.
             * Copy to heap so the struct survives other chunks loading. */
            s_LmHeader* heapLmHdr = (s_LmHeader*)malloc(sizeof(s_LmHeader));
            *heapLmHdr = *ipdHdr->lmHdr;
            ipdHdr->lmHdr = heapLmHdr;
        }
        if (slot < 0 && s_pcFixedIpdCount < 64) { slot = s_pcFixedIpdCount++; }
        if (slot >= 0) { s_pcFixedIpd[slot].hdr = ipdHdr; s_pcFixedIpd[slot].lm = ipdHdr->lmHdr; }
        {
            /* Find which chunk slot this belongs to for logging */
            s32 logSlot = -1, si;
            for (si = 0; si < g_Map.ipdActiveSize_158; si++) {
                if (g_Map.ipdActive_15C[si].ipdHdr == ipdHdr) { logSlot = si; break; }
            }
            SH_DBG("[IPD-FIXUP] IpdHeader_FixOffsets PC: slot=%d fileIdx=%d modelCount=%d lmHdr=%p",
                   logSlot, fileIdx, ipdHdr->lmHdr ? ipdHdr->lmHdr->modelCount : -1, (void*)ipdHdr->lmHdr);
        }
    }
#else
    if (ipdHdr->isLoaded)
    {
        return;
    }
    ipdHdr->isLoaded = true;

    IpdHeader_FixHeaderOffsets(ipdHdr);
    IpdCollData_FixOffsets(&ipdHdr->collisionData);
    LmHeader_FixOffsets(ipdHdr->lmHdr);
#endif
    func_8008E4EC(ipdHdr->lmHdr);
#ifdef SH_PC_PORT
    if (!(g_PcConfig.preloadChunks && g_Map.isExterior_588))
#endif
    {
        Ipd_MaterialsLoad(ipdHdr, fullPageActiveTexs, halfPageActiveTexs, fileIdx);
        Lm_MaterialFlagsApply(ipdHdr->lmHdr);
    }
    IpdHeader_ModelLinkObjectLists(ipdHdr, lmHdrs, lmHdrCount);
    IpdHeader_ModelBufferLinkObjectLists(ipdHdr, ipdHdr->modelInfo);
}

void Ipd_MaterialsLoad(s_IpdHeader* ipdHdr, s_ActiveTextures* fullPageActiveTexs, s_ActiveTextures* halfPageActiveTexs, e_FsFile fileIdx) // 0x80043C7C
{
    if (!ipdHdr->isLoaded)
    {
        return;
    }

    if (fullPageActiveTexs != NULL)
    {
        Lm_MaterialsLoadWithFilter(ipdHdr->lmHdr, fullPageActiveTexs, &LmFilter_IsFullPage, fileIdx, BlendMode_Additive);
    }

    if (halfPageActiveTexs != NULL)
    {
        Lm_MaterialsLoadWithFilter(ipdHdr->lmHdr, halfPageActiveTexs, &LmFilter_IsHalfPage, fileIdx, BlendMode_Additive);
    }
}

s32 Ipd_HalfPageMaterialCountGet(s_IpdHeader* ipdHdr) // 0x80043D00
{
    if (!ipdHdr->isLoaded)
    {
        return 0;
    }

    return Lm_MaterialCountGet(LmFilter_IsHalfPage, ipdHdr->lmHdr);
}

bool LmFilter_IsFullPage(s_Material* mat) // 0x80043D44
{
    return !LmFilter_IsHalfPage(mat);
}

/* Not sure what is the significance of textures that end with H.
 * I've looked at all of them and can't find any pattern.
 */
bool LmFilter_IsHalfPage(s_Material* mat) // 0x80043D64
{
    char* charCode;

    for (charCode = &mat->name_0.str[7]; charCode >= &mat->name_0.str[0]; charCode--)
    {
        if (*charCode == '\0')
        {
            continue;
        }

        return *charCode == 'H';
    }

    return false;
}

void IpdHeader_FixHeaderOffsets(s_IpdHeader* ipdHdr) // 0x80043DA4
{
    s_IpdModelBuffer* curModelBuf;

    ipdHdr->lmHdr           = (u8*)ipdHdr->lmHdr + (u32)ipdHdr;
    ipdHdr->modelInfo      = (u8*)ipdHdr->modelInfo + (u32)ipdHdr;
    ipdHdr->modelBuffers   = (u8*)ipdHdr->modelBuffers + (u32)ipdHdr;
    ipdHdr->modelOrderList = (u8*)ipdHdr->modelOrderList + (u32)ipdHdr;

    for (curModelBuf = &ipdHdr->modelBuffers[0];
         curModelBuf < &ipdHdr->modelBuffers[ipdHdr->modelBufferCount];
         curModelBuf++)
    {
        curModelBuf->field_C  = (u8*)curModelBuf->field_C + (u32)ipdHdr;
        curModelBuf->field_10 = (u8*)curModelBuf->field_10 + (u32)ipdHdr;
        curModelBuf->subcellPositions = (u8*)curModelBuf->subcellPositions + (u32)ipdHdr;
    }
}

void IpdHeader_ModelLinkObjectLists(s_IpdHeader* ipdHdr, s_LmHeader** lmHdrs, s32 lmHdrCount) // 0x80043E50
{
    s32             i;
    s32             j;
    s_IpdModelInfo* curModelInfo;

    for (i = 0; i < ipdHdr->modelCount; i++)
    {
        curModelInfo = &ipdHdr->modelInfo[i];

        if (!curModelInfo->isGlobalPlm)
        {
            curModelInfo->modelHdr = LmHeader_ModelHeaderSearch(&curModelInfo->modelName, ipdHdr->lmHdr);
        }
        else
        {
            for (j = 0; j < lmHdrCount; j++)
            {
                curModelInfo->modelHdr = LmHeader_ModelHeaderSearch(&curModelInfo->modelName, lmHdrs[j]);
                if (curModelInfo->modelHdr != NULL)
                {
                    break;
                }
            }
        }
#ifdef SH_PC_PORT
        SH_DBG("[MODEL-LINK] model[%d]: name='%.4s%.4s' isGlobal=%d -> modelHdr=%p (lmHdr=%p lmModels=%d)",
               i,
               (char*)&curModelInfo->modelName.str[0],
               (char*)&curModelInfo->modelName.str[4],
               curModelInfo->isGlobalPlm,
               (void*)curModelInfo->modelHdr,
               (void*)(curModelInfo->isGlobalPlm ? (lmHdrCount > 0 ? lmHdrs[0] : NULL) : ipdHdr->lmHdr),
               curModelInfo->isGlobalPlm ? (lmHdrCount > 0 ? lmHdrs[0]->modelCount : 0) : ipdHdr->lmHdr->modelCount);
#endif
    }
}

s_ModelHeader* LmHeader_ModelHeaderSearch(u_Filename* modelName, s_LmHeader* lmHdr) // 0x80043F2C
{
    s32            i;
    s_ModelHeader* modelHeader;

    modelHeader = lmHdr->modelHdrs;

    for (i = 0; i < lmHdr->modelCount; i++, modelHeader++)
    {
        if (!COMPARE_FILENAMES(modelName, &modelHeader->name_0))
        {
            return modelHeader;
        }
    }

    return NULL;
}

void IpdHeader_ModelBufferLinkObjectLists(s_IpdHeader* ipdHdr, s_IpdModelInfo* ipdModels) // 0x80043F88
{
    s_IpdModelBuffer*   curModelBuffer;
    s_IpdModelBuffer_C* unkData;

    for (curModelBuffer = ipdHdr->modelBuffers;
         curModelBuffer < &ipdHdr->modelBuffers[ipdHdr->modelBufferCount];
         curModelBuffer++)
    {
        for (unkData = &curModelBuffer->field_C[0];
             unkData < &curModelBuffer->field_C[curModelBuffer->field_0];
             unkData++)
        {
            // TODO: `unkData` originally stores model idx, replace that with pointer to the model's `modelHdr`.
            s32 modelIdx      = (s32)unkData->modelHdr;
            unkData->modelHdr = ipdModels[modelIdx].modelHdr;
        }
    }
}

void func_80044044(s_IpdHeader* ipd, s32 cellX, s32 cellZ) // 0x80044044
{
    s32 prevCellX;
    s32 prevCellZ;

    /* IPD reformatter now properly populates collisionData on PC */

    prevCellX = ipd->cellX;
    prevCellZ = ipd->cellZ;

    ipd->cellX                       = cellX;
    ipd->cellZ                       = cellZ;
    ipd->collisionData.positionX_0 += (cellX - prevCellX) * Q12_TO_Q8(CHUNK_CELL_SIZE);
    ipd->collisionData.positionZ_4 += (cellZ - prevCellZ) * Q12_TO_Q8(CHUNK_CELL_SIZE);
}

void Gfx_IpdChunkDraw(s_IpdHeader* ipdHdr, q19_12 posX, q19_12 posZ, GsOT* ot, s32 arg4) // 0x80044090
{
    #define CHUNK_SUBCELL_SIZE Q8(8.0f)

    s_ModelInfo         modelInfo;
    GsCOORDINATE2       modelCoord;
    MATRIX              viewMat;
    MATRIX              worldMat;
    s32                 geomX;
    s32                 geomZ;
    q23_8               cellBoundZ;
    q23_8               cellBoundX;
    s32                 subcellZ;
    s32                 subcellX;
    s32                 i;
    s_IpdModelBuffer*   ipdModelBuf;
    s_IpdModelBuffer_C* curBufC;
    u8*                 temp_fp;
    SVECTOR*            curUnk;

    // Convert position to geometry space.
    geomX = Q12_TO_Q8(posX);
    geomZ = Q12_TO_Q8(posZ);

    // Compute cell boundary position.
    cellBoundX = ipdHdr->cellX * Q12_TO_Q8(CHUNK_CELL_SIZE);
    cellBoundZ = ipdHdr->cellZ * Q12_TO_Q8(CHUNK_CELL_SIZE);

    // Compute subcells.
    subcellX = FLOOR_TO_STEP(geomX - cellBoundX, CHUNK_SUBCELL_SIZE);
    subcellZ = FLOOR_TO_STEP(geomZ - cellBoundZ, CHUNK_SUBCELL_SIZE);
    subcellX = MAX(subcellX, 0);
    subcellZ = MAX(subcellZ, 0);
    subcellX = MIN(subcellX, 4);
    subcellZ = MIN(subcellZ, 4);

    modelInfo.coord = &modelCoord;
    modelCoord.flg         = true;
    modelInfo.field_0 = 0;
    modelCoord.super       = NULL;

#ifdef SH_PC_PORT
    if (g_DebugCamEnabled || g_PcConfig.disableCulling) {
        /* Render ALL model buffers, skip subcell/spatial culling */
        s32 startI = 0, endI = ipdHdr->modelBufferCount;
        temp_fp = NULL;
        for (i = startI; i < endI; i++)
        {
            ipdModelBuf = &ipdHdr->modelBuffers[i];
            /* Skip frustum check in debug mode - draw everything */
            {
                for (curBufC = ipdModelBuf->field_C; curBufC < &ipdModelBuf->field_C[ipdModelBuf->field_0]; curBufC++)
                {
                    modelInfo.modelHdr = curBufC->modelHdr;
                    if (modelInfo.modelHdr != NULL)
                    {
                        modelCoord.workm       = curBufC->mat;
                        modelCoord.workm.t[0] += cellBoundX;
                        modelCoord.workm.t[2] += cellBoundZ;
                        Vw_CoordToWorldAndViewMatrices(&modelCoord, &worldMat, &viewMat);
                        func_80057090(&modelInfo, ot, arg4, &viewMat, &worldMat, 0);
                    }
                }

                for (curUnk = ipdModelBuf->field_10; curUnk < &ipdModelBuf->field_10[ipdModelBuf->field_1]; curUnk++)
                {
                    switch ((s8)curUnk->pad)
                    {
                        case 0:
                            Gfx_BillboardDraw(1, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4);
                            break;
                        case 1:
                            Gfx_BillboardDraw(2, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4);
                            break;
                    }
                }
            }
        }
    } else if (!g_Map.isExterior_588) {
        /* Interior maps: PSX subcell visibility rectangles were baked for
         * fixed-angle cameras. TPS camera orbits Harry and can fall outside
         * those rectangles, causing in-frustum model buffers to be dropped.
         * Skip subcell prefilter entirely; func_80057090 handles per-model
         * culling. Same approach as the debug-cam path above. */
        for (i = 0; i < ipdHdr->modelBufferCount; i++)
        {
            ipdModelBuf = &ipdHdr->modelBuffers[i];
            for (curBufC = ipdModelBuf->field_C; curBufC < &ipdModelBuf->field_C[ipdModelBuf->field_0]; curBufC++)
            {
                modelInfo.modelHdr = curBufC->modelHdr;
                if (modelInfo.modelHdr != NULL)
                {
                    modelCoord.workm       = curBufC->mat;
                    modelCoord.workm.t[0] += cellBoundX;
                    modelCoord.workm.t[2] += cellBoundZ;
                    Vw_CoordToWorldAndViewMatrices(&modelCoord, &worldMat, &viewMat);
                    func_80057090(&modelInfo, ot, arg4, &viewMat, &worldMat, 0);
                }
            }
            for (curUnk = ipdModelBuf->field_10; curUnk < &ipdModelBuf->field_10[ipdModelBuf->field_1]; curUnk++)
            {
                switch ((s8)curUnk->pad)
                {
                    case 0: Gfx_BillboardDraw(1, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4); break;
                    case 1: Gfx_BillboardDraw(2, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4); break;
                }
            }
        }
    } else
#endif
    {
    temp_fp = &ipdHdr->textureCount + (subcellZ * 10) + (subcellX * 2);
    for (i = temp_fp[0]; i < (temp_fp[1] + temp_fp[0]); i++)
    {
        ipdModelBuf = &ipdHdr->modelBuffers[ipdHdr->modelOrderList[i]];

        if (Gfx_ChunkSubcellVisibleCheck(ipdModelBuf, geomX - cellBoundX, geomZ - cellBoundZ, cellBoundX, cellBoundZ))
        {
            for (curBufC = ipdModelBuf->field_C; curBufC < &ipdModelBuf->field_C[ipdModelBuf->field_0]; curBufC++)
            {
                modelInfo.modelHdr = curBufC->modelHdr;
                if (modelInfo.modelHdr != NULL)
                {
                    // Set model matrix.
                    modelCoord.workm = curBufC->mat;

                    // Offset model on XZ plane.
                    modelCoord.workm.t[0] += cellBoundX;
                    modelCoord.workm.t[2] += cellBoundZ;

                    Vw_CoordToWorldAndViewMatrices(&modelCoord, &worldMat, &viewMat);
                    func_80057090(&modelInfo, ot, arg4, &viewMat, &worldMat, 0);
                }
            }

            for (curUnk = ipdModelBuf->field_10; curUnk < &ipdModelBuf->field_10[ipdModelBuf->field_1]; curUnk++)
            {
                switch ((s8)curUnk->pad) // TODO: Must be another field.
                {
                    case 0:
                        Gfx_BillboardDraw(1, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4);
                        break;

                    case 1:
                        Gfx_BillboardDraw(2, Q8_TO_Q12(curUnk->vx + cellBoundX), Q8_TO_Q12(curUnk->vy), Q8_TO_Q12(curUnk->vz + cellBoundZ), ot, arg4);
                        break;
                }
            }
        }
    }
#ifdef SH_PC_PORT
    } /* close else block */
#endif

    #undef CHUNK_SUBCELL_SIZE
}

bool Gfx_ChunkSubcellVisibleCheck(s_IpdModelBuffer* modelBuf, q7_8 subcellX, q7_8 subcellZ, q23_8 posX, q23_8 posZ) // 0x80044420
{
    GsCOORDINATE2 viewCoord;
    MATRIX        viewMat;
    SVECTOR*      curSubcellPos; // TODO: Subcell? Cell?

    // Run through subcell positions.
    for (curSubcellPos = modelBuf->subcellPositions;
         curSubcellPos < &modelBuf->subcellPositions[modelBuf->subcellCount];
         curSubcellPos++)
    {
        if (curSubcellPos->vx < subcellX          &&
            subcellX          < curSubcellPos->vy &&
            curSubcellPos->vz < subcellZ)
        {
            if (subcellZ < curSubcellPos->pad) // TODO: `pad` access indicates different struct.
            {
                viewCoord.flg   = true;
                viewCoord.super = NULL;
                viewCoord.workm = GsIDMATRIX;

                viewCoord.workm.t[0] = posX;
                viewCoord.workm.t[1] = Q8(0.0f);
                viewCoord.workm.t[2] = posZ;

                Vw_CoordToViewSpaceMatrix(&viewCoord, &viewMat);
                return Vw_AabbVisibleInFrustumCheck(&viewMat,
                                                    modelBuf->minX, Q8(-8.0f), modelBuf->minZ,
                                                    modelBuf->maxX, Q8(4.0f), modelBuf->maxZ,
                                                    Q8(25.0f), g_GameWork.gsScreenHeight);
            }
        }
    }

    return false;
}

