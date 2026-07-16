#include "game.h"
#include "inline_no_dmpsx.h"
#ifdef SH_PC_PORT
#include <stdlib.h>
#include <SDL_timer.h>
#include "pc_config.h"
#include "bodyprog/gfx/world.h"
#include "sh_log.h"
#include "hires_override.h"
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

bool Chara_ModelLoadedCheck(e_CharaId charaId) // 0x80040B74
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

#define D_800BFBF0_STRIDE (sizeof(DR_TPAGE) * 2 + sizeof(POLY_G4) * 16 * 3 + sizeof(POLY_G3) * 16 + sizeof(POLY_F4) * 16)
PACKET D_800BFBF0[2][D_800BFBF0_STRIDE];

#ifdef SH_PC_PORT
s_IpdCollisionData* D_800C1010[256];
#else
s_IpdCollisionData* D_800C1010[4];
#endif

s_MapTerrain g_Map;

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
#ifdef SH_PC_PORT
    extern int g_PsyX_FlashlightActive;
#endif

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

#ifdef SH_PC_PORT
        if (!g_PsyX_FlashlightActive)
#endif
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

#ifdef SH_PC_PORT
    if (!g_PsyX_FlashlightActive)
#endif
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
        return ChunkLoadState_Invalid;
    }
    else if (!Fs_QueueIsEntryLoaded(queueIdx))
    {
        return ChunkLoadState_Unloaded;
    }

    return ChunkLoadState_Loaded;
}

u32 IpdHeader_LoadStateGet(s_Chunk* chunk) // 0x80041B1C
{
    s32 queueState;
    s32 queueStateCpy;

    queueState    = Fs_QueueEntryLoadStatusGet(chunk->queueIdx);
    queueStateCpy = queueState;

    if (queueStateCpy == ChunkLoadState_Unloaded)
    {
        return StaticModelLoadState_Unloaded;
    }
    else if (queueStateCpy == ChunkLoadState_Invalid ||
             queueState != ChunkLoadState_Loaded)
    {
        return StaticModelLoadState_Invalid;
    }
    else if (chunk->ipdHdr->isLoaded && Ipd_IsTextureLoaded(chunk->ipdHdr))
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

    if (queueStateCpy == ChunkLoadState_Unloaded)
    {
        return StaticModelLoadState_Unloaded;
    }
    else if (queueStateCpy == ChunkLoadState_Invalid ||
             queueState    != ChunkLoadState_Loaded)
    {
        return StaticModelLoadState_Invalid;
    }
    else if (globalLm->lmHdr->isLoaded && Lm_IsTextureLoaded(globalLm->lmHdr))
    {
        return StaticModelLoadState_Loaded;
    }

    return StaticModelLoadState_Corrupted;
}

void Map_Init(s_LmHeader* lmHdr, s_IpdHeader* ipdBuf, s32 ipdBufSize) // 0x80041C24
{
    bzero(&g_Map, sizeof(s_MapTerrain));
    Lm_Init(&g_Map.globalLm, lmHdr);

    g_Map.chunkBuffer     = ipdBuf;
    g_Map.chunkBufferSize = ipdBufSize;
    g_Map.activeChunkCount = 0;
    g_Map.isExterior    = true;

    Ipd_ActiveChunksQueueIdxClear(g_Map.activeChunks, 4);
    Ipd_TexturesInit();
    Map_CollisionDataInit();
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

void Ipd_ActiveChunksQueueIdxClear(s_Chunk* chunks, s32 chunkCount) // 0x80041D10
{
    s_Chunk* curChunk;

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

        Texture_Init(&g_Map.chunkTextures.fullPageTextures[i], 0, 0, y, 0, 0, x, j);
    }

    Textures_ActiveTex_CountReset(&g_Map.chunkTextures.fullPage);
    Textures_ActiveTex_PutTextures(&g_Map.chunkTextures.fullPage, g_Map.chunkTextures.fullPageTextures, 8);

    for (i = 0, y = 26, j = 0; i < 2; i++, x += 16)
    {
        Texture_Init(&g_Map.chunkTextures.halfPageTextures[i], 0, 0, y, (i & 0x1) * 32, 0, x, j);
        if (i & 0x1)
        {
            y++;
        }
    }

    Textures_ActiveTex_CountReset(&g_Map.chunkTextures.halfPage);
    Textures_ActiveTex_PutTextures(&g_Map.chunkTextures.halfPage, g_Map.chunkTextures.halfPageTextures, 2);

#ifdef SH_PC_PORT
    /* Expanded pool (resident_textures; key encoding in hires_override.h):
     * append VIRTUAL slots after the 10 vanilla page slots. Claim order in
     * Texture_Get is list order, so the physical slots fill first and
     * assignment stays vanilla-identical until vanilla capacity would have
     * been exceeded. Virtual slots never upload to VRAM (PostLoadTim skips
     * on the synthetic clutY and registers a per-slot GL texture instead);
     * the tpage byte only feeds the prim's page bits, which the GL override
     * path ignores — if a slot's registration ever fails, its prims render
     * broken (FT4 dropped / GT wrong palette), the same class of breakage as
     * vanilla's missing-TIM stale pages, and [POOLTEX] names the slot. */
    if (g_PcConfig.residentTextures)
    {
        s32 k;

        /* Virtual slot id -> synthetic clut coords (hires_override.h): the
         * id is split across the clut X bits (per-prim +64*row palette
         * deltas never touch them) and 16-row-spaced Y groups, so multi-row
         * chunk TIMs (school ships 6-12 palette rows) stay disambiguable. */
        for (k = 0; k < PC_TEXPOOL_FULL_EXTRA; k++)
        {
            Texture_Init(&g_Map.chunkTextures.fullPageTextures[8 + k], 0,
                         0, 8, 0, 0,
                         (s16)((k % 64) * 16),
                         (s16)(HIRES_POOL_CLUT_ROW_BASE + (k / 64) * HIRES_POOL_MAX_ROWS));
            g_Map.chunkTextures.fullPage.textures[g_Map.chunkTextures.fullPage.count++] =
                &g_Map.chunkTextures.fullPageTextures[8 + k];
        }
        for (k = 0; k < PC_TEXPOOL_HALF_EXTRA; k++)
        {
            s32 id = PC_TEXPOOL_FULL_EXTRA + k;
            Texture_Init(&g_Map.chunkTextures.halfPageTextures[2 + k], 0,
                         0, 26, 0, 0,
                         (s16)((id % 64) * 16),
                         (s16)(HIRES_POOL_CLUT_ROW_BASE + (id / 64) * HIRES_POOL_MAX_ROWS));
            /* The last CHUNK-range slot id (255) is reserved for the bullet-
             * decal texture (pc_decals.c): keep the s_Texture initialized
             * (RefClear walks the full array) but never offer it to the claim
             * list. Named constant — HIRES_POOL_SLOT_MAX now spans the chara
             * range too, so "SLOT_MAX - 1" would silently unreserve it. */
            if (id == HIRES_POOL_DECAL_SLOT)
            {
                continue;
            }
            g_Map.chunkTextures.halfPage.textures[g_Map.chunkTextures.halfPage.count++] =
                &g_Map.chunkTextures.halfPageTextures[2 + k];
        }
    }
    HiresOverride_PoolSlotsReset();
    /* The reset above also freed the decal slot's GL texture; drop the decal
     * FIFO and let pc_decals.c re-register lazily on its next draw. */
    { extern void Pc_DecalsReset(void); Pc_DecalsReset(); }
#endif
}

void Map_CollisionDataInit(void) // 0x80041E98
{
    bzero(&g_Map.collisionData, sizeof(s_IpdCollisionData));
    g_Map.collisionData.subcellSize = 512;
}

#ifdef SH_PC_PORT
static void Pc_ParkedCellRecord(s32 cellX, s32 cellZ); /* defined near Pc_WholeMapDrawActive */
#endif

void Map_PlaceIpdAtCell(s16 ipdFileIdx, s32 cellX, s32 cellZ) // 0x80041ED0
{
    s_Chunk*  curChunk;
    s_IpdHeader* ipdHdr;

#ifdef SH_PC_PORT
    /* Record hosted-interior host cells for the whole-town gate (see
     * Pc_WholeMapDrawActive). */
    Pc_ParkedCellRecord(cellX, cellZ);
#endif

    ((s16*)&g_Map.chunkGridCenter[cellZ])[cellX] = ipdFileIdx;

    for (curChunk = g_Map.activeChunks; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
        if (curChunk->cellX != cellX || curChunk->cellZ != cellZ)
        {
            continue;
        }

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
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
    Ipd_ActiveChunksClear(&g_Map, g_Map.activeChunkCount);
}

#ifdef SH_PC_PORT
/* Diagnostic: name every TIM upload that lands on a PINNED physical pool
 * slot. 2D event backgrounds / boss FX write straight into pool pages; with
 * resident texturing the stomped slot was never reloaded — the garbled
 * interior floors/walls class. Called from Fs_QueuePostLoadTim. */
void Pc_PoolStompProbe(int x, int y, int w, int h)
{
    static int s_stompLog = 0;
    s32 i;

    if (s_stompLog >= 64) return;

    for (i = 0; i < 10; i++)
    {
        s_Texture* t = (i < 8) ? &g_Map.chunkTextures.fullPageTextures[i]
                               : &g_Map.chunkTextures.halfPageTextures[i - 8];
        int sx = t->imageDesc.u + ((t->imageDesc.tPage[1] & 0xF) << 6);
        int sy = t->imageDesc.v + ((t->imageDesc.tPage[1] << 4) & 0x100);
        int sw = (i < 8) ? 64 : 32;

        if (t->refCount <= 0) continue;
        if (x >= sx + sw || x + w <= sx || y >= sy + 256 || y + h <= sy) continue;

        SH_DBG("[POOLSTOMP] upload (%d,%d %dx%d) hits PINNED pool slot %d '%.8s' (%d,%d)",
               x, y, w, h, (int)i, t->name.str, sx, sy);
        s_stompLog++;
    }
}
#endif

void Ipd_TexturesRefClear(void) // 0x8004201C
{
    s_Texture* curTex;

#ifdef SH_PC_PORT
    s32 fullBound = 8 + (g_PcConfig.residentTextures ? PC_TEXPOOL_FULL_EXTRA : 0);
    s32 halfBound = 2 + (g_PcConfig.residentTextures ? PC_TEXPOOL_HALF_EXTRA : 0);
#else
    #define fullBound 8
    #define halfBound 2
#endif

    // TODO: Will these match as for loops?
    curTex = &g_Map.chunkTextures.fullPageTextures[0];
    while (curTex < (&g_Map.chunkTextures.fullPageTextures[fullBound]))
    {
        if (curTex->refCount == 0)
        {
            Texture_RefClear(curTex);
        }

        curTex++;
    }

    curTex = &g_Map.chunkTextures.halfPageTextures[0];
    while (curTex < (&g_Map.chunkTextures.halfPageTextures[halfBound]))
    {
        if (curTex->refCount == 0)
        {
            Texture_RefClear(curTex);
        }

        curTex++;
    }
#ifndef SH_PC_PORT
    #undef fullBound
    #undef halfBound
#endif
}

void Map_WorldClearReset(void) // 0x800420C0
{
    Map_GlobalLmFree();
    Ipd_ActiveChunksClear(&g_Map, g_Map.activeChunkCount);
    Ipd_TexturesInit();
}

void Map_GlobalLmFree(void) // 0x800420FC
{
    s_GlobalLm* globalLm;

    globalLm = &g_Map.globalLm;

    if (Fs_QueueEntryLoadStatusGet(globalLm->queueIdx) >= ChunkLoadState_Loaded &&
        globalLm->lmHdr->isLoaded)
    {
        Lm_MaterialRefCountDec(g_Map.globalLm.lmHdr);
    }

    Lm_Init(&g_Map.globalLm, g_Map.globalLm.lmHdr);
}

s_Texture* Texture_InfoGet(char* texName) // 0x80042178
{
    s_Texture* tex;

    tex = Textures_ActiveTex_FindTexture(texName, &g_Map.chunkTextures.fullPage);
    if (tex != NULL)
    {
        return tex;
    }

    tex = Textures_ActiveTex_FindTexture(texName, &g_Map.chunkTextures.halfPage);
    if (tex != NULL)
    {
        return tex;
    }

    return NULL;
}

void Ipd_MapFileInfoSet(char* mapTag, e_FsFile plmIdx, s32 activeIpdCount, bool isExterior, e_FsFile ipdFileIdx, e_FsFile textureFileIdx) // 0x800421D8
{
#ifdef SH_PC_PORT
    {
        bool willRebuild = (g_Map.activeChunkCount != activeIpdCount || strcmp(mapTag, g_Map.mapTag) != 0);
        SH_DBG("[IPD-INIT] Ipd_MapFileInfoSet: mapTag='%s' activeIpdCount=%d isExterior=%d plmIdx=%d ipdFileIdx=%d textureFileIdx=%d curSize=%d willRebuildGrid=%d",
               mapTag, activeIpdCount, isExterior, plmIdx, ipdFileIdx, textureFileIdx, g_Map.activeChunkCount, willRebuild);
    }
#endif
    g_Map.isExterior = isExterior;
    g_Map.textureFileIdx = textureFileIdx;

    if (plmIdx != NO_VALUE)
    {
        if (plmIdx != g_Map.globalLm.fileIdx)
        {
            if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm.queueIdx) >= ChunkLoadState_Loaded &&
                g_Map.globalLm.lmHdr->isLoaded)
            {
                Lm_MaterialRefCountDec(g_Map.globalLm.lmHdr);
            }

            g_Map.globalLm.fileIdx  = plmIdx;
            g_Map.globalLm.queueIdx = NO_VALUE;
        }
    }

#ifdef SH_PC_PORT
    {
    /* Preload only applies to exterior maps — interior maps use PSX streaming. */
    bool _usePreload = g_PcConfig.preloadChunks && isExterior;
    /* On PC with preloading, only clear+rebuild when the map TAG changes
     * (actual map transition). Don't clear on same-map calls — our
     * ipdActiveSize (256) != PSX activeIpdCount (2-4) would always trigger. */
    if ((_usePreload && strcmp(mapTag, g_Map.mapTag) != 0) ||
        (!_usePreload && (g_Map.activeChunkCount != activeIpdCount || strcmp(mapTag, g_Map.mapTag) != 0)))
#else
    if (g_Map.activeChunkCount != activeIpdCount || strcmp(mapTag, g_Map.mapTag) != 0)
#endif
    {
        Ipd_ActiveChunksClear(&g_Map, activeIpdCount);

#ifdef SH_PC_PORT
        g_Map.activeChunkCount = _usePreload ? PC_MAX_IPD_CHUNKS : activeIpdCount;
#else
        g_Map.activeChunkCount = activeIpdCount;
#endif
        g_Map.ipdFileIdx    = ipdFileIdx;
        strcpy(g_Map.mapTag, mapTag);

        g_Map.mapTagSize = strlen(mapTag);
        Map_MakeIpdGrid(&g_Map, mapTag, ipdFileIdx);
    }
#ifdef SH_PC_PORT
    }
#endif
}

void Ipd_ActiveChunksClear(s_MapTerrain* map, s32 arg1) // 0x80042300
{
    s32          step;
    s32          i;
#ifdef SH_PC_PORT
    s32          sharedCount;
#endif
    s_Chunk*  curChunk;
    s_IpdHeader* ipdHdr0;
    s_IpdHeader* ipdHdr1;

#ifdef SH_PC_PORT
    {
        s32 activeCount = 0;
        for (i = 0; i < map->activeChunkCount; i++) {
            if (map->activeChunks[i].queueIdx != NO_VALUE) activeCount++;
        }
    }
#endif

    ipdHdr0 = map->chunkBuffer;
    step    = (map->chunkBufferSize / arg1) & ~0x3;

#ifdef SH_PC_PORT
    /* Slice the shared chunk buffer by the map's ORIGINAL active-chunk count,
       not the widescreen-bumped arg1. Ipd_PlayerChunkInit raises activeIpdCount
       to 4 so 4 chunks stay resident for the wider Hor+ view, but slicing the
       fixed 0x2C000 buffer into 4 yields 45KB slots. Interior chunks are
       authored for the map's original count (1 or 2 -> 180KB/90KB slots); a
       90KB interior chunk read into a 45KB slot overruns into the next slot and
       corrupts the adjacent chunk's header -> the invalid-magic reload thrash
       and exploded interior geometry. Residency-only slots past the original
       count get their own fixed 90KB buffers, big enough for any multi-slot
       chunk and a constant size so cross-map slot reuse never under-allocates. */
    {
        u32 mf = (g_WorldGfxWork.mapInfo != NULL) ? g_WorldGfxWork.mapInfo->flags : 0;
        if (mf & MapFlag_OneActiveChunk)       sharedCount = 1;
        else if (mf & MapFlag_TwoActiveChunks) sharedCount = 2;
        else                                   sharedCount = 4;
        step = (map->chunkBufferSize / sharedCount) & ~0x3;
    }

    /* Per-slot owned buffers for slots past the shared-slice count. A slot's
       role flips between maps (an exterior slices the shared buffer 4 ways,
       an interior 2 ways), so curChunk->ipdHdr alone can't tell "owns a
       calloc" from "points into the shared buffer". The old NULL-check kept
       stale shared-buffer pointers in slots >= sharedCount: entering an
       interior (2 slices) after an exterior (4 slices) left slots 2-3
       pointing at shared offsets 90K/135K, and loading into them overwrote
       slots 0-1's halves — corrupted chunk headers, nothing drawn (the
       "all gray/black world" reports; church/school first because the
       interior load window fills those slots immediately). The side table
       remembers each slot's owned buffer across role flips, fixing both the
       corruption and the leak of dropping callocs when a slot goes shared. */
    {
        static s_IpdHeader* s_pcSlotOwnedBuf[PC_MAX_IPD_CHUNKS];
        s32 indivSize = (map->chunkBufferSize / 2) & ~0x3;

        for (i = 0; i < PC_MAX_IPD_CHUNKS; i++)
        {
            curChunk = &map->activeChunks[i];

            if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
            {
                ipdHdr1 = curChunk->ipdHdr;
                if (ipdHdr1 != NULL && ipdHdr1->isLoaded)
                {
                    Lm_MaterialRefCountDec(ipdHdr1->lmHdr);
                }
            }

            curChunk->queueIdx      = NO_VALUE;
            curChunk->paddedDistanceToEdge1    = INT_MAX;
            curChunk->outsideCount = 0;

            if (i < sharedCount)
            {
                curChunk->ipdHdr = ipdHdr0;
                *(u8**)&ipdHdr0 += step;
            }
            else
            {
                if (s_pcSlotOwnedBuf[i] == NULL)
                {
                    s_pcSlotOwnedBuf[i] = (s_IpdHeader*)calloc(1, indivSize > 0 ? indivSize : 65536);
                }
                curChunk->ipdHdr = s_pcSlotOwnedBuf[i];
            }
        }
    }
#else
    for (i = 0; i < 4; i++, *(u8**)&ipdHdr0 += step)
    {
        curChunk = &map->activeChunks[i];

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
        {
            ipdHdr1 = curChunk->ipdHdr;
            if (ipdHdr1->isLoaded)
            {
                Lm_MaterialRefCountDec(ipdHdr1->lmHdr);
            }
        }

        curChunk->queueIdx      = NO_VALUE;
        curChunk->paddedDistanceToEdge1    = INT_MAX;
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

void Map_MakeIpdGrid(s_MapTerrain* map, char* mapTag, e_FsFile fileIdxStart) // 0x800423F4
{
    char            sp10[256];
    s32             x;
    s32             z;
    s32             i;
    char*           filenameSuffix;
    s_ChunkColumn*    col;

    map->chunkGridCenter = (s_ChunkColumn*)(&map->chunkGrid[8].idx[8]);

    for (z = -8; z < 11; z++)
    {
        for (x = -8; x < 8; x++)
        {
            ((s16*)&map->chunkGridCenter[z])[x] = NO_VALUE;
        }
    }

    // Run through all game files.
    for (i = fileIdxStart; i < FS_FILE_COUNT; i++)
    {
        if (g_FileTable[i].type == FileType_Ipd)
        {
            Fs_GetFileName(sp10, i);

            if (strncmp(sp10, map->mapTag, map->mapTagSize) == 0)
            {
                filenameSuffix = &sp10[map->mapTagSize];
                if (ConvertHexToS8(&x, filenameSuffix[0], filenameSuffix[1]) &&
                    ConvertHexToS8(&z, filenameSuffix[2], filenameSuffix[3]))
                {
                    col         = &map->chunkGridCenter[z];
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
                if (((s16*)&map->chunkGridCenter[gz])[gx] != NO_VALUE) gridCount++;
            }
        }
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
    s_Chunk*         ptr;
    s_IpdCollisionData* collData;
    s_IpdHeader*        ipdHdr;

    ptr          = g_Map.activeChunks;
    *collDataIdx = 0;

    while (ptr < &g_Map.activeChunks[g_Map.activeChunkCount])
    {
        if (Fs_QueueEntryLoadStatusGet(ptr->queueIdx) >= ChunkLoadState_Loaded)
        {
            ipdHdr = ptr->ipdHdr;
            if (ipdHdr->isLoaded)
            {
                collData = Ipd_HeaderCollisionDataGet(ipdHdr);
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
    s_Chunk*  curChunk;

    // Convert position to geometry space.
    geomX = Q12_TO_Q8(posX);
    geomZ = Q12_TO_Q8(posZ);

    // Compute cell coordinates.
    cellX = FLOOR_TO_STEP(geomX, Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(geomZ, Q12_TO_Q8(CHUNK_CELL_SIZE));

    // Run through active chunks.
    for (curChunk = g_Map.activeChunks; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
        // Check if chunk is loaded.
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded)
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
    if (((s16*)(&g_Map.chunkGridCenter[cellZ]))[cellX] != NO_VALUE)
    {
        return NULL;
    }
    else
    {
        return &g_Map.collisionData;
    }
}

s32 func_8004287C(s_WorldObjectModel* model, s_WorldObjectMetadata* metadata, q19_12 posX, q19_12 posZ) // 0x8004287C
{
#ifdef SH_PC_PORT
    /* The ±1-cell window below matches at most 3x3 = 9 chunks. PSX sized
     * these arrays for its 4 streaming-active chunks; with PC preloading
     * every chunk, up to 9 candidates exist and the old size-4 cap dropped
     * them in ENCOUNTER order (not nearest), so world objects vanished at
     * specific spots (street doghouse papers PPR*_NEA, school GOLD_HID).
     * Search all 9 possible candidates instead. */
    s_Chunk* chunks[9];
    q19_12      distsToEdges[9];
#else
    s_Chunk* chunks[4];
    q19_12      distsToEdges[4];
#endif
    q23_8       geomX;
    q23_8       geomZ;
    s32         cellX;
    s32         cellZ;
    q19_12      distToEdge;
    s32         i;
    s32         j;
    s32         k;
    s32         chunkIdx;
    s_Chunk* curChunk;
    s_GlobalLm* globalLm;

    globalLm = &g_Map.globalLm;

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
    if (Fs_QueueEntryLoadStatusGet(globalLm->queueIdx) >= ChunkLoadState_Loaded &&
        globalLm->lmHdr->isLoaded &&
        Lm_ModelFind(model, g_Map.globalLm.lmHdr, metadata))
    {
        return 2;
    }

    cellX = FLOOR_TO_STEP(geomX, Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(geomZ, Q12_TO_Q8(CHUNK_CELL_SIZE));

    for (curChunk = g_Map.activeChunks, chunkIdx = 0;
         curChunk < &g_Map.activeChunks[g_Map.activeChunkCount];
         curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded)
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

        if (!g_Map.isExterior)
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
                /* Belt-and-suspenders for the array bound (9 = max possible
                 * matches in the ±1 window; see declaration). The previous
                 * cap of 4 dropped candidates in encounter order and lost
                 * objects at specific spots. */
                if (chunkIdx >= (s32)ARRAY_SIZE(chunks)) {
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
            return (curChunk - g_Map.activeChunks) + 3;
        }
    }

    return 0;
}

bool IpdHeader_IsLoaded(s32 ipdIdx) // 0x80042C04
{
    return IpdHeader_LoadStateGet(&g_Map.activeChunks[ipdIdx]) >= StaticModelLoadState_Loaded;
}

void Ipd_ChunkInit(q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1) // 0x80042C3C
{
    s32         fullPageTexCount;
    s_Chunk* curChunk;

#ifdef SH_PC_PORT
    {
        static s32 chunkInitCallCount = 0;
        if (chunkInitCallCount < 20 || (chunkInitCallCount % 300) == 0) {
        }
        chunkInitCallCount++;
    }
#endif

    g_Map.positionX = posX1;
    g_Map.positionZ = posZ1;

    if (g_Map.globalLm.queueIdx == NO_VALUE)
    {
        g_Map.globalLm.queueIdx = Fs_QueueStartRead(g_Map.globalLm.fileIdx, g_Map.globalLm.lmHdr);
    }

#ifdef SH_PC_PORT
    if (g_PcConfig.preloadChunks && g_Map.isExterior)
    {
        /* PC: Load ALL IPD chunks for the entire map at once.
         * Scan the full grid (-8..+10 in Z, -8..+7 in X) and load every
         * IPD file that exists. This eliminates PSX CD streaming entirely. */
        static int _preloaded = 0;
        s32 px, pz, pFileIdx;
        s_Chunk* pChunk;

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
        if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm.queueIdx) >= ChunkLoadState_Loaded &&
            !g_Map.globalLm.lmHdr->isLoaded)
        {
            fullPageTexCount                         = g_Map.chunkTextures.fullPage.count;
            g_Map.chunkTextures.fullPage.count = 4;

            LmHeader_FixOffsets(g_Map.globalLm.lmHdr);
            Lm_MaterialsLoadWithFilter(g_Map.globalLm.lmHdr, &g_Map.chunkTextures.fullPage, NULL, g_Map.textureFileIdx, BlendMode_Additive);
            Lm_MaterialFlagsApply(g_Map.globalLm.lmHdr);

            g_Map.chunkTextures.fullPage.count = fullPageTexCount;
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
                    if (Map_IsIpdPresentCheck(g_Map.activeChunks, px, pz))
                        continue;

                    /* Find next unused slot */
                    while (nextSlot < PC_MAX_IPD_CHUNKS &&
                           Fs_QueueEntryLoadStatusGet(g_Map.activeChunks[nextSlot].queueIdx) >= ChunkLoadState_Loaded)
                    {
                        nextSlot++;
                    }

                    if (nextSlot >= PC_MAX_IPD_CHUNKS)
                    {
                        break;
                    }

                    pChunk = &g_Map.activeChunks[nextSlot];
                    pChunk->materialCount = 0;

                    /* Start loading */
                    Ipd_LoadStart(pChunk, pFileIdx, px, pz, posX0, posZ0, posX1, posZ1, g_Map.isExterior);

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
                    if (Fs_QueueEntryLoadStatusGet(pChunk->queueIdx) >= ChunkLoadState_Loaded)
                    {
                        IpdHeader_FixOffsets(pChunk->ipdHdr, &g_Map.globalLm.lmHdr, 1,
                                            &g_Map.chunkTextures.fullPage,
                                            &g_Map.chunkTextures.halfPage,
                                            g_Map.textureFileIdx);
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
            for (curChunk = g_Map.activeChunks; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
            {
                if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
                    loadedCount++;
            }
            _preloaded = 1;
        }

        /* Update cell position for rendering */
        {
            s32 cellX1 = FLOOR_TO_STEP(Q12_TO_Q8(posX1), Q12_TO_Q8(CHUNK_CELL_SIZE));
            s32 cellZ1 = FLOOR_TO_STEP(Q12_TO_Q8(posZ1), Q12_TO_Q8(CHUNK_CELL_SIZE));
            g_Map.cellX = cellX1;
            g_Map.cellZ = cellZ1;
        }

        /* Update distance samples for all chunks */
        Ipd_ActiveChunksSample(&g_Map, posX0, posZ0, posX1, posZ1, g_Map.isExterior);
        Ipd_ChunkMaterialsApply(&g_Map);
    }
    else
#endif
    {
        Map_ChunkLoad(&g_Map, posX0, posZ0, posX1, posZ1);
    }

    if (Fs_QueueEntryLoadStatusGet(g_Map.globalLm.queueIdx) >= ChunkLoadState_Loaded &&
        !g_Map.globalLm.lmHdr->isLoaded)
    {
        fullPageTexCount                         = g_Map.chunkTextures.fullPage.count;
        g_Map.chunkTextures.fullPage.count = 4;

        LmHeader_FixOffsets(g_Map.globalLm.lmHdr);
        Lm_MaterialsLoadWithFilter(g_Map.globalLm.lmHdr, &g_Map.chunkTextures.fullPage, NULL, g_Map.textureFileIdx, BlendMode_Additive);
        Lm_MaterialFlagsApply(g_Map.globalLm.lmHdr);

        g_Map.chunkTextures.fullPage.count = fullPageTexCount;
    }

    for (curChunk = g_Map.activeChunks; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
        {
            IpdHeader_FixOffsets(curChunk->ipdHdr, &g_Map.globalLm.lmHdr, 1, &g_Map.chunkTextures.fullPage, &g_Map.chunkTextures.halfPage, g_Map.textureFileIdx);
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

s32 Map_ChunkLoad(s_MapTerrain* map, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1) // 0x80042EBC
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
    s_Chunk*  chunk;
    s_IpdHeader* ipdHdr;

    queueIdx = NO_VALUE;

    cellX0 = FLOOR_TO_STEP(Q12_TO_Q8(posX0), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ0 = FLOOR_TO_STEP(Q12_TO_Q8(posZ0), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellX1 = FLOOR_TO_STEP(Q12_TO_Q8(posX1), Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ1 = FLOOR_TO_STEP(Q12_TO_Q8(posZ1), Q12_TO_Q8(CHUNK_CELL_SIZE));

    map->cellX = cellX1;
    map->cellZ = cellZ1;

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
        }
    }
#endif

    Ipd_ActiveChunksSample(map, posX0, posZ0, posX1, posZ1, map->isExterior);
    Ipd_ChunkMaterialsApply(map);

    {
#ifdef SH_PC_PORT
    /* Interiors must LOAD the same +-2 X / +-1 Z window that
     * Ipd_CellPositionMatchCheck DRAWS. Vanilla loaded only the center
     * cell because the exact-cell match never drew anything else; the PC
     * widened draw window without a widened load window showed stale
     * resident chunks (other rooms/floors) and left visible neighbors
     * with no geometry or collision until walked into. */
    s32 scanMin = g_DebugCamEnabled ? -4 : (map->isExterior ? -1 : -2);
    s32 scanMax = g_DebugCamEnabled ? 5 : (map->isExterior ? 1 : 2);
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
            if (g_DebugCamEnabled || map->isExterior || (z >= -1 && z <= 1))
#else
            if (map->isExterior || (x == 0 && z == 0))
#endif
            {
                projCellZ = cellZ0 + z;
                projCellX = cellX0 + x;

                chunkIdx = Map_IpdIdxGet(projCellX, projCellZ);
                if (chunkIdx != NO_VALUE &&
#ifdef SH_PC_PORT
                    /* Interior neighbor cells are by definition outside the
                     * player's cell (distance > 0); the window itself is the
                     * load gate. */
                    (g_DebugCamEnabled || !map->isExterior || Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, projCellX, projCellZ, map->isExterior) <= Q12(0.0f)) &&
#else
                    Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, projCellX, projCellZ, map->isExterior) <= Q12(0.0f) &&
#endif
                    !Map_IsIpdPresentCheck(map->activeChunks, projCellX, projCellZ))
                {
#ifdef SH_PC_PORT
                    if (loadsThisFrame >= maxLoadsPerFrame)
                    {
                        continue;
                    }
#endif
                    chunk = Ipd_FreeChunkFind(map->activeChunks, map->isExterior);

#ifdef SH_PC_PORT
                    if (chunk == NULL)
                    {
                        continue;
                    }
#endif

                    if (Fs_QueueEntryLoadStatusGet(chunk->queueIdx) >= ChunkLoadState_Loaded)
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

                    curQueueIdx = Ipd_LoadStart(chunk, chunkIdx, projCellX, projCellZ, posX0, posZ0, posX1, posZ1, map->isExterior);
                    if (curQueueIdx != NO_VALUE)
                    {
                        queueIdx = curQueueIdx;
#ifdef SH_PC_PORT
                        loadsThisFrame++;
                        /* The chunk we just loaded is in the window by
                         * definition, but its outsideCount/distances are
                         * stale from the pre-load sample. After a door
                         * teleport the whole window changes at once: every
                         * resident carried outsideCount >= 1, so a LATER
                         * load in this same loop could pick the chunk we
                         * JUST loaded as its eviction victim — including
                         * the player's own room (the "room is a black void
                         * until quicksave/load" reports). Zero them so
                         * same-frame eviction can never target it. */
                        chunk->outsideCount = 0;
                        chunk->paddedDistanceToEdge0 = Q12(0.0f);
                        chunk->paddedDistanceToEdge1 = Q12(0.0f);
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
/* Must cover every preloadable chunk: with preload_chunks=1 the preloader fixes
 * up to PC_MAX_IPD_CHUNKS chunks, and any chunk not registered here gets
 * re-reformatted every frame (reading its already-64-bit struct as PSX bytes =
 * garbage). A 64-slot table left chunks 65+ unregistered -> exterior maps with
 * >64 chunks (e.g. map0_s00, 129 chunks) lost most of their geometry. */
static struct { s_IpdHeader* hdr; s_LmHeader* lm; } s_pcFixedIpd[PC_MAX_IPD_CHUNKS];
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

void Ipd_ActiveChunksSample(s_MapTerrain* map, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x800431E4
{
    s_Chunk* curChunk;

    // Run through active chunks.
    for (curChunk = map->activeChunks; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
    {
        if (curChunk->queueIdx == NO_VALUE)
        {
            curChunk->paddedDistanceToEdge0  = INT_MAX;
            curChunk->paddedDistanceToEdge1 = INT_MAX;
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
            Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded &&
            !IpdHeader_PC_IsReformatted(curChunk->ipdHdr))
        {
            curChunk->ipdHdr->isLoaded = false;
        }
#endif

        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded || !curChunk->ipdHdr->isLoaded)
        {
            curChunk->materialCount = 0;
        }
        else
        {
            curChunk->materialCount = Ipd_HalfPageMaterialCountGet(curChunk->ipdHdr);
        }

        if (curChunk->paddedDistanceToEdge0 > Q12(0.0f) && curChunk->paddedDistanceToEdge1 > Q12(0.0f))
        {
            curChunk->outsideCount++;
        }
        else
        {
            curChunk->outsideCount = 0;
        }
    }
}

void Ipd_DistanceToEdgeCalc(s_Chunk* chunk, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x80043338
{
    chunk->paddedDistanceToEdge0  = Ipd_PaddedDistanceToEdgeGet(posX0, posZ0, chunk->cellX, chunk->cellZ, isExterior);
    chunk->paddedDistanceToEdge1 = Ipd_PaddedDistanceToEdgeGet(posX1, posZ1, chunk->cellX, chunk->cellZ, isExterior);
}

#ifdef SH_PC_PORT
/* Single gate for the experimental whole-town render mode: texture-all
 * (Ipd_ChunkMaterialsApply), draw-all model buffers (Ipd_ChunkDraw), and the
 * lifted per-poly far caps in bodyprog_80055028.c all key off this. Scenic
 * mode ONLY: it exists so the town is visible at once (on foot / flycam);
 * hosted interiors live in the SAME map's grid (Levin house = THR cells of
 * map2_s00), so "am I outdoors" cannot come from map identity or fog. */
extern s_WorldEnvWork g_WorldEnvWork;

/* Parked-cell registry: Map_PlaceIpdAtCell targets (THR05FD -> (-1,8) at THR
 * map load; THRF908 -> (-7,6) from an event). Their chunk content is swapped at
 * runtime, so exclude them from the outdoor draw set outright and recompute the
 * room table when a placement changes the grid. */
enum { PC_MAX_PARKED_CELLS = 16 };
static struct { s16 x, z; } s_pcParkedCells[PC_MAX_PARKED_CELLS];
static s32 s_pcParkedCount = 0;

/* Outdoor-room classification. Every grid cell with an IPD is classified by the
 * map's own authored position->room function (g_MapOverlayHdr.mapRoomIdxGet —
 * the same one Game_MapRoomIdxUpdate feeds mapRoomIdx from), sampled at the
 * cell center. A room spanning >= PC_WM_MIN_ROOM_CELLS cells is an outdoor
 * area (streets span dozens of cells, alleys several); a room on 1-2 cells is a
 * hosted interior island (Levin house). Both the activation gate (player's
 * CURRENT room must be outdoor-sized) and the far-draw / texture-all cell
 * filter key off this, so the whole town never renders or mass-textures from
 * inside a house, and interior islands never float in the flyover view. Pure
 * position math on the town maps -> safe to compute during load. */
enum { PC_WM_MIN_ROOM_CELLS = 3 };
static u16 s_pcRoomCellCount[256];
static u8  s_pcCellOutdoor[19][16]; /* [z + 8][x + 8], grid z -8..10, x -8..7 */
static s32 s_pcRoomTableValid = 0;

void Pc_ParkedCellsReset(void) /* called at map load, before any placement */
{
    s_pcParkedCount    = 0;
    s_pcRoomTableValid = 0;
}

static int Pc_CellIsParked(s32 cellX, s32 cellZ)
{
    s32 i;
    for (i = 0; i < s_pcParkedCount; i++)
    {
        if (s_pcParkedCells[i].x == cellX && s_pcParkedCells[i].z == cellZ)
            return 1;
    }
    return 0;
}

static void Pc_ParkedCellRecord(s32 cellX, s32 cellZ)
{
    if (Pc_CellIsParked(cellX, cellZ))
        return;
    if (s_pcParkedCount < PC_MAX_PARKED_CELLS)
    {
        s_pcParkedCells[s_pcParkedCount].x = (s16)cellX;
        s_pcParkedCells[s_pcParkedCount].z = (s16)cellZ;
        s_pcParkedCount++;
    }
    s_pcRoomTableValid = 0; /* grid content changed under the room table */
}

static void Pc_WholeMapRoomTableEnsure(void)
{
    /* 5 sample points per cell: center + 4 corners inset 14u. The authored
     * street bands (Map_RoomIdxGet primary grids) are ~24-32u wide and do NOT
     * always contain cell centers — on map2_s00 the east-west street rooms
     * miss every center, so center-only sampling classified street and
     * crossroads cells as indoor (gate flicker on E-W streets, holes at every
     * intersection, void under the player). Corner samples reach into the
     * adjoining bands. Hosted-interior rooms come from the per-cell fallback
     * grid (MAP_ROOM_IDXS), so all 5 samples agree inside a house cell and it
     * can never gain an outdoor sample. Room cell counts are DISTINCT-CELL
     * counts (a room seen by several samples of one cell counts once). */
    static const s32 SAMPLE_OFS[5][2] = {
        { 0, 0 },
        { -Q12(14.0f), -Q12(14.0f) }, { -Q12(14.0f), Q12(14.0f) },
        {  Q12(14.0f), -Q12(14.0f) }, {  Q12(14.0f), Q12(14.0f) },
    };
    u8  cellRooms[19][16][5];
    s32 x;
    s32 z;
    s32 s;
    s32 outdoorCells = 0;

    if (s_pcRoomTableValid)
        return;

    bzero(s_pcRoomCellCount, sizeof(s_pcRoomCellCount));
    bzero(s_pcCellOutdoor, sizeof(s_pcCellOutdoor));

    if (g_MapOverlayHdr.mapRoomIdxGet == NULL || g_Map.chunkGridCenter == NULL)
    {
        s_pcRoomTableValid = 1; /* no room data -> mode stays off on this map */
        return;
    }

    for (z = -8; z < 11; z++)
    {
        for (x = -8; x < 8; x++)
        {
            if (((s16*)&g_Map.chunkGridCenter[z])[x] == NO_VALUE)
                continue;

            for (s = 0; s < 5; s++)
            {
                s32 seen = 0;
                s32 k;
                u8  room = g_MapOverlayHdr.mapRoomIdxGet(
                    x * CHUNK_CELL_SIZE + (CHUNK_CELL_SIZE / 2) + SAMPLE_OFS[s][0],
                    z * CHUNK_CELL_SIZE + (CHUNK_CELL_SIZE / 2) + SAMPLE_OFS[s][1]);

                cellRooms[z + 8][x + 8][s] = room;
                for (k = 0; k < s; k++)
                {
                    if (cellRooms[z + 8][x + 8][k] == room)
                    {
                        seen = 1;
                        break;
                    }
                }
                if (!seen)
                    s_pcRoomCellCount[room]++;
            }
        }
    }

    for (z = -8; z < 11; z++)
    {
        for (x = -8; x < 8; x++)
        {
            if (((s16*)&g_Map.chunkGridCenter[z])[x] == NO_VALUE || Pc_CellIsParked(x, z))
                continue;

            for (s = 0; s < 5; s++)
            {
                if (s_pcRoomCellCount[cellRooms[z + 8][x + 8][s]] >= PC_WM_MIN_ROOM_CELLS)
                {
                    s_pcCellOutdoor[z + 8][x + 8] = 1;
                    outdoorCells++;
                    break;
                }
            }
        }
    }

    s_pcRoomTableValid = 1;

    if (g_PcConfig.wholeMapExteriors && g_Map.isExterior)
    {
        s32 r;
        SH_DBG("[WHOLEMAP] room table: %d outdoor cells; distinct-cell counts per room:", outdoorCells);
        for (r = 0; r < 256; r++)
        {
            if (s_pcRoomCellCount[r] != 0)
                SH_DBG("[WHOLEMAP]   room %d: %d cells%s", r, (int)s_pcRoomCellCount[r],
                       (s_pcRoomCellCount[r] >= PC_WM_MIN_ROOM_CELLS) ? " (outdoor)" : "");
        }
    }
}

static int Pc_WholeMapCellOutdoor(s32 cellX, s32 cellZ)
{
    if (cellX < -8 || cellX >= 8 || cellZ < -8 || cellZ >= 11)
        return 0;

    Pc_WholeMapRoomTableEnsure();
    return s_pcCellOutdoor[cellZ + 8][cellX + 8];
}

int Pc_WholeMapDrawActive(void)
{
    s32 pcx;
    s32 pcz;

    if (!(g_PcConfig.wholeMapExteriors && g_PcConfig.preloadChunks &&
          g_PcConfig.residentTextures && g_Map.isExterior))
    {
        return 0;
    }

    /* Gate on the player's CELL being outdoor, not the player's room: street
     * ROOMS can be small (an intersection room spans 1-2 cells; east-west
     * street bands miss cell centers), so a room-count test flickers the mode
     * off mid-street. The cell test uses the same classification as the draw/
     * texture filters, so an active gate implies the ground under the player
     * is textured and drawn. Player position (not g_Map.cellX/cellZ, which is
     * a camera-forward-projected sample that escapes the cell near edges). */
    pcx = FLOOR_TO_STEP(Q12_TO_Q8(g_SysWork.playerWork.player.position.vx), Q12_TO_Q8(CHUNK_CELL_SIZE));
    pcz = FLOOR_TO_STEP(Q12_TO_Q8(g_SysWork.playerWork.player.position.vz), Q12_TO_Q8(CHUNK_CELL_SIZE));

    return Pc_WholeMapCellOutdoor(pcx, pcz);
}
#endif

void Ipd_ChunkMaterialsApply(s_MapTerrain* map) // 0x800433B8
{
    s_Chunk* curChunk;

#ifdef SH_PC_PORT
    /* Interiors: texture the K nearest resident chunks, nearest-first.
     *
     * First attempt held materials for the whole 15-cell residency
     * window (_matDist = 2 cells) — but the texture-page pool is only
     * 8 full + 2 half pages, and IpdHeader_LoadStateGet only reports
     * Loaded when EVERY material has a resident texture. Pinning the
     * window starved the pool in dense interiors (school: 105 IPD
     * grid), leaving even the player's own cell in state Corrupted —
     * never drawn (the all-black school void) — and the load screen's
     * chunk wait timed out for the same reason. Distance-ordered
     * loading guarantees the player's cell always wins a page; the
     * nearest neighbors (the Hor+ screen-edge reveal, cafe side walls)
     * take what remains, and farther residents keep geometry+collision
     * but release their textures like vanilla's out-of-cell chunks. */
    /* Expanded pool (INTERIOR-class maps only): every loaded resident chunk
     * keeps its materials textured — the pool no longer starves, so the
     * keep-4 window, the steal loop, and the g_PcInteriorMatSync
     * flat/untexture shims below are unnecessary (they remain as the
     * resident_textures=0 fallback). Releases still happen on chunk unload
     * (Map_PlaceIpdAtCell / Ipd_ActiveChunksClear), exactly like vanilla.
     *
     * Do NOT extend this to exterior-class maps: their release-far/
     * reload-near loop below IS the draw-distance system (an untextured
     * chunk is skipped by the draw gate). Texturing all of a street map at
     * once draws the whole map through walls/fog (severe lag, distant
     * geometry visible) and mass-claims hundreds of slots simultaneously —
     * broke the Lenin St house (exterior-class map2_s00 hosts interior
     * cells). Whole-map exterior draw distance is its own future task. */
    /* whole_map_exteriors (EXPERIMENTAL, scenic mode): texture the OUTDOOR
     * cells so the whole town can render. Two hard lessons baked in here:
     *
     * - Outdoor cells only (Pc_WholeMapCellOutdoor). Hosted interiors live in
     *   the same grid (Levin house = THR cells of map2_s00); claiming them, or
     *   claiming ANYTHING while the player is inside one (the gate handles
     *   that), mass-loads the town through the house.
     * - Staggered, nearest-first. Claiming ~200 street cells in one frame with
     *   a hi-res texture pack composes + uploads GL textures for every CLUT
     *   row of every page at pack resolution — the 2026-07-12 system-crash
     *   (memory exhaustion during the Levin-house save load). A few first-time
     *   claims per frame, nearest first, spreads the cost and lets the pack
     *   byte budget (fsqueue_3.c) favor what is close to the player. */
    if ((!g_Map.isExterior && g_PcConfig.residentTextures) || Pc_WholeMapDrawActive())
    {
        enum { PC_WM_CLAIMS_PER_FRAME = 3 };
        s_Chunk* newClaims[PC_WM_CLAIMS_PER_FRAME];
        s32      newCount = 0;
        s32      wm = g_Map.isExterior; /* whole-map claim vs interior resident claim */
        s32      i;

        /* Visibility is NOT decided here: interiors draw exactly the
         * player's cell (Ipd_CellPositionMatchCheck), matching retail —
         * texturing everything must not widen what renders, or neighbor
         * room islands ghost into open areas (school-courtyard corridor). */
        for (curChunk = &map->activeChunks[0]; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
        {
            if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded ||
                curChunk->ipdHdr == NULL || !curChunk->ipdHdr->isLoaded)
            {
                continue;
            }

            if (wm)
            {
                /* Chunks inside the vanilla claim window (padded distance <= 0)
                 * always texture — exactly what the vanilla release/reload loop
                 * would do — so the ground around the player can never be
                 * starved by a misclassified cell. */
                if (curChunk->paddedDistanceToEdge0 > Q12(0.0f) &&
                    !Pc_WholeMapCellOutdoor(curChunk->cellX, curChunk->cellZ))
                    continue;

                if (IpdHeader_LoadStateGet(curChunk) < StaticModelLoadState_Loaded)
                {
                    /* First-time texture claim — stagger it. Keep the
                     * PC_WM_CLAIMS_PER_FRAME nearest candidates (insertion
                     * sort by padded edge distance, updated per frame by
                     * Ipd_ActiveChunksSample). */
                    s32 ins = newCount;
                    while (ins > 0 &&
                           newClaims[ins - 1]->paddedDistanceToEdge0 > curChunk->paddedDistanceToEdge0)
                    {
                        if (ins < PC_WM_CLAIMS_PER_FRAME)
                            newClaims[ins] = newClaims[ins - 1];
                        ins--;
                    }
                    if (ins < PC_WM_CLAIMS_PER_FRAME)
                    {
                        newClaims[ins] = curChunk;
                        if (newCount < PC_WM_CLAIMS_PER_FRAME)
                            newCount++;
                    }
                    continue;
                }
            }

            Ipd_MaterialsLoad(curChunk->ipdHdr, &map->chunkTextures.fullPage, &map->chunkTextures.halfPage, map->textureFileIdx);
            Lm_MaterialFlagsApply(curChunk->ipdHdr->lmHdr);
        }

        for (i = 0; i < newCount; i++)
        {
            Ipd_MaterialsLoad(newClaims[i]->ipdHdr, &map->chunkTextures.fullPage, &map->chunkTextures.halfPage, map->textureFileIdx);
            Lm_MaterialFlagsApply(newClaims[i]->ipdHdr->lmHdr);
        }

        return;
    }

    if (!g_Map.isExterior)
    {
        enum { PC_INTERIOR_TEXTURED_CHUNKS = 4 };
        s_Chunk* keep[PC_INTERIOR_TEXTURED_CHUNKS];
        s32      keepCount = 0;
        s32      ins;
        s32      j;
        q19_12   d;
        q19_12   dk;

        for (curChunk = &map->activeChunks[0]; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
        {
            if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded ||
                curChunk->ipdHdr == NULL || !curChunk->ipdHdr->isLoaded)
            {
                continue;
            }

            d = MIN(curChunk->paddedDistanceToEdge0, curChunk->paddedDistanceToEdge1);
            for (ins = 0; ins < keepCount; ins++)
            {
                dk = MIN(keep[ins]->paddedDistanceToEdge0, keep[ins]->paddedDistanceToEdge1);
                if (d < dk)
                {
                    break;
                }
            }

            if (ins < PC_INTERIOR_TEXTURED_CHUNKS)
            {
                for (j = (keepCount < PC_INTERIOR_TEXTURED_CHUNKS - 1) ? keepCount : (PC_INTERIOR_TEXTURED_CHUNKS - 1); j > ins; j--)
                {
                    keep[j] = keep[j - 1];
                }
                keep[ins] = curChunk;
                if (keepCount < PC_INTERIOR_TEXTURED_CHUNKS)
                {
                    keepCount++;
                }
            }
        }

        /* Release textures held by residents outside the keep set
         * (idempotent: RefCountDec NULLs material->texture). */
        for (curChunk = &map->activeChunks[0]; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
        {
            if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) < ChunkLoadState_Loaded ||
                curChunk->ipdHdr == NULL || !curChunk->ipdHdr->isLoaded)
            {
                continue;
            }

            for (ins = 0; ins < keepCount; ins++)
            {
                if (keep[ins] == curChunk)
                {
                    break;
                }
            }
            if (ins == keepCount)
            {
                Lm_MaterialRefCountDec(curChunk->ipdHdr->lmHdr);
            }
        }

        /* Load nearest-first so the pool can never be exhausted before
         * the player's own cell gets its pages.
         *
         * Stealing: keep members textured in EARLIER frames (when they
         * were the nearest) hold their pages with refCount > 0, so a
         * freshly entered room can find every slot pinned — the 2-slot
         * half-page pool especially. The room's chunk then never passes
         * Ipd_IsTextureLoaded, is never drawn, and the screen is black
         * except world objects, which resolve from the chunk's LM
         * without needing its pages (school turnstile-room report:
         * valves visible, room black). When a nearer chunk still has
         * NULL-texture materials after its load pass, release the
         * farthest keep members and retry. Steals only flow nearer ->
         * farther, so there is no oscillation: a farthest chunk that
         * lost its pages simply stays undrawn, exactly like vanilla's
         * out-of-cell chunks. (A NULL texture is the pool-exhausted
         * signal; a non-NULL texture with a still-pending TIM read is
         * fine and must not trigger a steal.) */
#ifdef SH_PC_PORT
        /* Untexture NULL-texture materials during this sync so a chunk that lost
         * a pool page (stolen for a nearer chunk) renders flat instead of
         * sampling the stale VRAM page another chunk now owns (the "rainbow"). */
        { extern int g_PcInteriorMatSync; g_PcInteriorMatSync = 1; }
#endif
        for (ins = 0; ins < keepCount; ins++)
        {
            s32 stealFrom;

            Ipd_MaterialsLoad(keep[ins]->ipdHdr, &map->chunkTextures.fullPage, &map->chunkTextures.halfPage, map->textureFileIdx);

            for (stealFrom = keepCount - 1; stealFrom > ins; stealFrom--)
            {
                s_Material* m;
                s_LmHeader* lm   = keep[ins]->ipdHdr->lmHdr;
                s32         miss = 0;

                for (m = &lm->materials[0]; m < &lm->materials[lm->materialCount]; m++)
                {
                    if (m->field_C == 0 && m->texture == NULL)
                    {
                        miss = 1;
                        break;
                    }
                }
                if (!miss)
                {
                    break;
                }

                Lm_MaterialRefCountDec(keep[stealFrom]->ipdHdr->lmHdr);
                Ipd_MaterialsLoad(keep[ins]->ipdHdr, &map->chunkTextures.fullPage, &map->chunkTextures.halfPage, map->textureFileIdx);
            }

            Lm_MaterialFlagsApply(keep[ins]->ipdHdr->lmHdr);
        }
#ifdef SH_PC_PORT
        { extern int g_PcInteriorMatSync; g_PcInteriorMatSync = 0; }
#endif

        return;
    }

    q19_12 _matDist = (g_PcConfig.preloadChunks && g_DebugCamEnabled && !g_DebugFogDisabled) ? Q12(35.0f) : Q12(0.0f);
#else
    #define _matDist Q12(0.0f)
#endif

    for (curChunk = &map->activeChunks[0]; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
        {
            if (curChunk->ipdHdr->isLoaded &&
                curChunk->paddedDistanceToEdge0 > _matDist && curChunk->paddedDistanceToEdge1 > _matDist)
            {
                Lm_MaterialRefCountDec(curChunk->ipdHdr->lmHdr);
            }
        }
    }

    for (curChunk = &map->activeChunks[0]; curChunk < &map->activeChunks[map->activeChunkCount]; curChunk++)
    {
        if (Fs_QueueEntryLoadStatusGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
        {
            if (curChunk->ipdHdr->isLoaded &&
                (curChunk->paddedDistanceToEdge0 <= _matDist || curChunk->paddedDistanceToEdge1 <= _matDist))
            {
                Ipd_MaterialsLoad(curChunk->ipdHdr, &map->chunkTextures.fullPage, &map->chunkTextures.halfPage, map->textureFileIdx);
                Lm_MaterialFlagsApply(curChunk->ipdHdr->lmHdr);
            }
        }
    }
}

s32 Map_IpdIdxGet(s32 cellX, s32 cellZ) // 0x80043554
{
#ifdef SH_PC_PORT
    /* The grid spans x -8..7, z -8..10 (Map_MakeIpdGrid). The interior
     * load window probes player cell +-2/+-1, which can step past the
     * grid edge; the unbounded read then returns garbage that looks like
     * a file index and a junk file gets loaded as an IPD. */
    if (cellX < -8 || cellX > 7 || cellZ < -8 || cellZ > 10)
    {
        return NO_VALUE;
    }
#endif
    // @hack
    return ((s16*)&g_Map.chunkGridCenter[cellZ])[cellX];
}

bool Map_IsIpdPresentCheck(const s_Chunk* chunks, s32 cellX, s32 cellZ) // 0x80043578
{
    s32 i;

    for (i = 0; i < g_Map.activeChunkCount; i++)
    {
        if (chunks[i].queueIdx != NO_VALUE &&
            cellX == chunks[i].cellX && cellZ == chunks[i].cellZ)
        {
            return true;
        }
    }

    return false;
}

s_Chunk* Ipd_FreeChunkFind(s_Chunk* chunks, bool isExterior)
{
    s32         largestMatCount;
    q19_12      farthestDist;
    q19_12      dist;
    u32         largestOutsideCount;
    s32         matCount;
    s_Chunk* curChunk;
    s_Chunk* activeChunk;

    activeChunk         = NULL;
    largestOutsideCount = 0;
    largestMatCount     = 0;
    farthestDist        = Q12(0.0f);

    for (curChunk = chunks; curChunk < &chunks[g_Map.activeChunkCount]; curChunk++)
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

                dist = curChunk->paddedDistanceToEdge0;
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

s32 Ipd_LoadStart(s_Chunk* chunk, e_FsFile fileIdx, s32 cellX, s32 cellZ, q19_12 posX0, q19_12 posZ0, q19_12 posX1, q19_12 posZ1, bool isExterior) // 0x800436D8
{
    if (fileIdx == NO_VALUE)
    {
        return fileIdx;
    }

    chunk->cellX    = cellX;
    chunk->cellZ    = cellZ;
    chunk->queueIdx = Fs_QueueStartRead(fileIdx, chunk->ipdHdr);

    Ipd_DistanceToEdgeCalc(chunk, posX0, posZ0, posX1, posZ1, isExterior);


    return chunk->queueIdx;
}

bool Ipd_AreChunksLoaded(void) // 0x80043740
{
    s32         i;
    s_Chunk* curChunk;

    switch (LmHeader_LoadStateGet(&g_Map.globalLm))
    {
        case StaticModelLoadState_Invalid:
            break;

        case StaticModelLoadState_Unloaded:
            return false;

        case StaticModelLoadState_Corrupted:
            return false;
    }

    for (curChunk = g_Map.activeChunks, i = 0;
         i < g_Map.activeChunkCount;
         i++, curChunk++)
    {
        switch (IpdHeader_LoadStateGet(curChunk))
        {
            case StaticModelLoadState_Invalid:
            case StaticModelLoadState_Loaded:
                continue;
        }

        if (curChunk->paddedDistanceToEdge0 <= Q12(0.0f) || curChunk->paddedDistanceToEdge1 <= Q12(0.0f))
        {
            return false;
        }
    }

    return true;
}

bool func_80043830(void) // 0x80043830
{
    s32         loadState;
    s_Chunk* curChunk;

#ifdef SH_PC_PORT
    /* On PC, IPD loading is synchronous (Fs_QueueStartRead completes immediately).
       Never block the render loop waiting for chunks. */
    return false;
#endif

    for (curChunk = &g_Map.activeChunks[0]; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
        loadState = IpdHeader_LoadStateGet(curChunk);
        if (loadState == StaticModelLoadState_Invalid || loadState == StaticModelLoadState_Loaded ||
            (curChunk->paddedDistanceToEdge0 > Q12(0.0f) && curChunk->paddedDistanceToEdge1 > Q12(0.0f)))
        {
            continue;
        }

        if (!Ipd_CellPositionMatchCheck(curChunk, &g_Map))
        {
            continue;
        }

        if (Ipd_DistanceToEdgeGet(Q12_TO_Q8(g_Map.positionX), Q12_TO_Q8(g_Map.positionZ), curChunk->cellX, curChunk->cellZ) <= Q8(4.5f))
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

    if (g_Map.isExterior)
    {
        return Ipd_DistanceToEdgeGet(Q12_TO_Q8(g_Map.positionX), Q12_TO_Q8(g_Map.positionZ), cellX, cellZ) <= Q8(4.5f);
    }

    if (cellX == g_Map.cellX &&
        cellZ == g_Map.cellZ)
    {
        return true;
    }

    return false;
}

#ifdef SH_PC_PORT
/* Cheap world-space per-chunk frustum reject for whole-town mode. Transforms the
 * 40u cell's center into view space via GsWSMATRIX (the world->view matrix, in Q8
 * world units — the same space Ipd_ChunkDraw uses for cell bounds) and rejects
 * cells fully behind the camera or outside a generous horizontal cone. It runs in
 * WORLD space, so it is immune to the GTE depth saturation the far-projection fix
 * works around — a far but on-screen chunk is correctly kept. Bounds the submit
 * set so the software vertex transform and the flat vertex buffer stay in budget.
 * GsWSMATRIX row 2 (m[2]/t[2]) is the forward/depth axis, matching RTPS's MAC3, so
 * view Z > 0 == in front. */
/* Horizontal cone half-tangent for the whole-map reject. Derived from the actual
 * projection so it can never be tighter than the visible frustum: 160/H is the
 * 4:3 horizontal half-tangent (H = ReadGeomScreen already folds in fps_fov), and
 * winAspect/(4:3) applies the Hor+ widescreen widening (tracks ultrawide). A 30%
 * margin guarantees nothing on screen is culled; the fix trades a little extra
 * submitted geometry (bounded by the widened vertex buffer) for zero over-cull. */
static float Pc_WholeMapConeSlope(void)
{
    float H = (float)ReadGeomScreen();
    float winA;
    float slope;

    if (H < 1.0f)
        H = 1.0f;
    winA = (g_PcConfig.windowHeight > 0)
         ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight
         : (4.0f / 3.0f);
    slope = (160.0f / H) * (winA / (4.0f / 3.0f)) * 1.3f;
    if (slope < 1.0f) /* never tighter than ~90deg full */
        slope = 1.0f;
    return slope;
}

/* View-space depth of a cell center (Q8 world units; GsWSMATRIX row 2 is the
 * camera forward axis, matching RTPS MAC3 — positive = in front). Also serves
 * as the painter's sort key for the whole-town submit order. */
static s32 Pc_WholeMapCellViewZ(s32 cellX, s32 cellZ)
{
    const s32 CELL = Q12_TO_Q8(CHUNK_CELL_SIZE);
    s32 cx = cellX * CELL + (CELL >> 1);
    s32 cz = cellZ * CELL + (CELL >> 1);
    return (s32)(((s64)GsWSMATRIX.m[2][0] * cx + (s64)GsWSMATRIX.m[2][2] * cz) >> 12) + GsWSMATRIX.t[2];
}

static int Pc_WholeMapChunkCulled(s32 cellX, s32 cellZ, float coneSlope)
{
    const s32 CELL = Q12_TO_Q8(CHUNK_CELL_SIZE); /* 40u in Q8 */
    s32 cx = cellX * CELL + (CELL >> 1);         /* cell center, Q8 world */
    s32 cz = cellZ * CELL + (CELL >> 1);
    /* cell center Y = 0 (ground); GsWSMATRIX.t carries the camera height. */
    s32 vx = (s32)(((s64)GsWSMATRIX.m[0][0] * cx + (s64)GsWSMATRIX.m[0][2] * cz) >> 12) + GsWSMATRIX.t[0];
    s32 vz = Pc_WholeMapCellViewZ(cellX, cellZ);
    float ax;

    if (vz < -CELL) /* whole cell behind the near plane */
        return 1;

    /* Horizontal cone (coneSlope * depth) plus a whole-cell margin. */
    ax = (float)(vx < 0 ? -vx : vx);
    if (ax > (float)vz * coneSlope + (float)CELL)
        return 1;

    return 0;
}
#endif

void Ipd_ChunkCheckDraw(GsOT* ot, s32 arg1) // 0x80043A24
{
    s32         queueState;
    s_Chunk* curChunk;

    queueState = Fs_QueueEntryLoadStatusGet(g_Map.globalLm.queueIdx);

    if (queueState == ChunkLoadState_Unloaded)
    {
        return;
    }

    if (!(queueState == ChunkLoadState_Invalid ||
          (queueState == ChunkLoadState_Loaded && g_Map.globalLm.lmHdr->isLoaded)))
    {
        return;
    }

    curChunk = &g_Map.activeChunks[0];
#ifdef SH_PC_PORT
    {
        extern int g_PsxWholeMapFar; /* PsyCross: gate the GTE far re-projection */
        extern int g_PsxWholeMapChunkSz; /* PsyCross: true depth for this chunk's saturated far polys */
        int drawCount = 0;
        /* The 16-chunk debug-cam cap predates the OT depth clamps; with the
         * whole-town mode active it was exactly what truncated the flycam view
         * to a block of houses. Keep the cap only for plain debug flights. */
        int drawLimit = (g_DebugCamEnabled && !Pc_WholeMapDrawActive()) ? 16 : PC_MAX_IPD_CHUNKS;
        int totalChunks = 0, loadedChunks = 0, cellMatchChunks = 0, culledChunks = 0;
        /* Whole-town mode: unclamp the GTE projection for far world vertices (see
         * PsyX_GTE.cpp). Scoped to this world-chunk loop so only the map geometry
         * takes the far path — characters/particles/HUD keep the exact PSX path. */
        int wmFar = Pc_WholeMapDrawActive();
        float wmConeSlope = wmFar ? Pc_WholeMapConeSlope() : 0.0f;
        s_Chunk* wmChunks[PC_MAX_IPD_CHUNKS];
        s32      wmViewZ[PC_MAX_IPD_CHUNKS];
        s32      wmCount = 0;
        g_PsxWholeMapFar = wmFar;
#endif
    for (; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
#ifdef SH_PC_PORT
        totalChunks++;
        if (IpdHeader_LoadStateGet(curChunk) >= StaticModelLoadState_Loaded) loadedChunks++;
#endif
        if (IpdHeader_LoadStateGet(curChunk) >= StaticModelLoadState_Loaded && Ipd_CellPositionMatchCheck(curChunk, &g_Map))
        {
#ifdef SH_PC_PORT
            cellMatchChunks++;
            /* Whole-town draw set = outdoor cells only (streets/alleys; hosted
             * interior islands and parked cells excluded — they'd float in the
             * flyover view and bleed through house rooms), bounded to what is
             * actually on screen by the frustum reject. Chunks inside the
             * vanilla claim window are exempt from both (vanilla draws every
             * loaded+textured chunk), so the local scene always matches
             * vanilla regardless of cell classification. */
            if (wmFar)
            {
                if (curChunk->paddedDistanceToEdge0 > Q12(0.0f) &&
                    (!Pc_WholeMapCellOutdoor(curChunk->cellX, curChunk->cellZ) ||
                     Pc_WholeMapChunkCulled(curChunk->cellX, curChunk->cellZ, wmConeSlope)))
                {
                    culledChunks++;
                    continue;
                }
                /* Collect for sorted submission instead of drawing inline. */
                wmChunks[wmCount] = curChunk;
                wmViewZ[wmCount]  = Pc_WholeMapCellViewZ(curChunk->cellX, curChunk->cellZ);
                wmCount++;
                continue;
            }
#endif
            Ipd_ChunkDraw(curChunk->ipdHdr, g_Map.positionX, g_Map.positionZ, ot, arg1);
#ifdef SH_PC_PORT
            if (++drawCount >= drawLimit) break;
#endif
        }
    }
#ifdef SH_PC_PORT
    /* Whole-town painter's submit order: everything past ~64u shares the LAST
     * OT bucket, and beyond 256u every poly also shares one GL depth value —
     * so DRAW order decides overlaps there. addPrim PREPENDS to a bucket
     * (traversal = reverse submission), so submitting chunks NEAR-first makes
     * the far bucket draw far->near = painter's order at chunk granularity.
     * This is what turns "random building sides by direction" into a stable
     * town silhouette. */
    if (wmFar && wmCount > 0)
    {
        /* The frame packet arena is 2MB (game_main.c PC_PKTBUF_SIZE) with
         * NOTHING stopping emission past its end — sized for ~25 near chunks,
         * while whole-town submits ~60+. Budget the town's share and stop at
         * the cap: the list is near-first, so what gets dropped is the far
         * tail (graceful fade, never the local scene), and characters/effects
         * later in the frame keep their headroom. */
        enum { PC_WM_PACKET_BUDGET = 1200 * 1024 };
        u8* wmPktBase = (u8*)GsOUT_PACKET_P;
        s32 i;
        s32 j;
        for (i = 1; i < wmCount; i++)
        {
            s_Chunk* c = wmChunks[i];
            s32      z = wmViewZ[i];
            for (j = i - 1; j >= 0 && wmViewZ[j] > z; j--)
            {
                wmChunks[j + 1] = wmChunks[j];
                wmViewZ[j + 1]  = wmViewZ[j];
            }
            wmChunks[j + 1] = c;
            wmViewZ[j + 1]  = z;
        }
        for (i = 0; i < wmCount; i++)
        {
            if ((u8*)GsOUT_PACKET_P - wmPktBase > PC_WM_PACKET_BUDGET)
            {
                culledChunks += wmCount - i;
                break;
            }
            /* Feed this chunk's true cell-center view depth so its saturated
             * (>256u) polys depth-sort by block instead of collapsing to one
             * plane (cells are 40u-confined, so per-chunk depth is exact enough
             * for block-vs-block ordering). SZ units == Q8 view units. */
            g_PsxWholeMapChunkSz = wmViewZ[i] > 0 ? wmViewZ[i] : 0;
            Ipd_ChunkDraw(wmChunks[i]->ipdHdr, g_Map.positionX, g_Map.positionZ, ot, arg1);
            if (++drawCount >= drawLimit) break;
        }
    }
    g_PsxWholeMapFar = 0; /* far re-projection is world-geometry only */
    g_PsxWholeMapChunkSz = 0;
#endif
#ifdef SH_PC_PORT
    /* TEMP [WHOLEMAP] probe (remove when the whole-town report closes): with
     * the mode active, confirm every chunk actually submits — separates a
     * residual chunk gate from the GTE far-projection limit. */
    if (g_Map.isExterior && g_PcConfig.wholeMapExteriors)
    {
        static u32 s_wmLogMs = 0;
        if ((SDL_GetTicks() - s_wmLogMs) > 2000)
        {
            s_wmLogMs = SDL_GetTicks();
            SH_DBG("[WHOLEMAP] active=%d preload=%d resident=%d fog=%d roomIdx=%d total=%d loaded=%d drawn=%d culled=%d",
                   Pc_WholeMapDrawActive(), g_PcConfig.preloadChunks, g_PcConfig.residentTextures,
                   (int)g_WorldEnvWork.isFogEnabled, (int)g_SavegamePtr->mapRoomIdx,
                   totalChunks, loadedChunks, drawCount, culledChunks);
        }
    }

    /* Once/sec while the world is void (black-void diagnosis): fires when
     * NOTHING draws OR when the player's own cell specifically isn't among
     * the drawn chunks (a "room missing, neighbor visible" void would
     * otherwise stay silent). Dumps which stage failed per slot. */
    {
        static u32 s_lastDrawLogMs = 0;
        int playerCellDrawn = 0;
        if (!g_Map.isExterior)
        {
            s_Chunk* pc;
            for (pc = &g_Map.activeChunks[0]; pc < &g_Map.activeChunks[g_Map.activeChunkCount]; pc++)
            {
                if (pc->queueIdx != NO_VALUE &&
                    pc->cellX == g_Map.cellX && pc->cellZ == g_Map.cellZ &&
                    IpdHeader_LoadStateGet(pc) >= StaticModelLoadState_Loaded)
                {
                    playerCellDrawn = 1;
                    break;
                }
            }
        }
        else
        {
            playerCellDrawn = (cellMatchChunks != 0);
        }
        if ((cellMatchChunks == 0 || !playerCellDrawn) && (SDL_GetTicks() - s_lastDrawLogMs) > 1000)
        {
            s_lastDrawLogMs = SDL_GetTicks();
            SH_DBG("[IPD-DRAW] VOID playerCellDrawn=%d total=%d loaded=%d cellMatch=%d mapCell=(%d,%d) tag=%s",
                   playerCellDrawn, totalChunks, loadedChunks, cellMatchChunks,
                   (int)g_Map.cellX, (int)g_Map.cellZ, g_Map.mapTag);
            {
                s_Chunk* c;
                for (c = &g_Map.activeChunks[0]; c < &g_Map.activeChunks[g_Map.activeChunkCount]; c++)
                {
                    if (c->queueIdx == NO_VALUE) continue;
                    SH_DBG("[IPD-DRAW]   slot=%d cell=(%d,%d) loadState=%d qState=%d isLoaded=%d",
                           (int)(c - g_Map.activeChunks), (int)c->cellX, (int)c->cellZ,
                           (int)IpdHeader_LoadStateGet(c),
                           (int)Fs_QueueEntryLoadStatusGet(c->queueIdx),
                           (c->ipdHdr != NULL) ? (int)c->ipdHdr->isLoaded : -1);
                }
            }
        }
    }
    }
#endif
}

bool Ipd_CellPositionMatchCheck(s_Chunk* chunk, s_MapTerrain* map)
{
#ifdef SH_PC_PORT
    if (g_DebugCamEnabled) return true;
    /* NOTE: disable_culling must NOT bypass the interior check below — it is
     * ROOM VISIBILITY, not culling, and disable_culling=1 is the SHIPPED
     * DEFAULT (its old first-line bypass here is why the courtyard ghost
     * survived the exact-cell fix on every default config). disableCulling
     * keeps its real meaning elsewhere: within the visible room every model
     * buffer still draws (Ipd_ChunkDraw draw-all), and exteriors pass below. */
    if (g_PcConfig.disableCulling && map->isExterior) return true;
    /* Interiors draw ONLY the player's cell, exactly like retail. Interior
     * maps are a packing of self-contained room islands — one room per 40u
     * cell, 16-28u of dead space between islands, zero cross-cell geometry
     * on the US disc (the lone cross-boundary vista, HP0002/HP0003, ships
     * duplicated geometry inside the viewing cell). So widescreen needs no
     * wider window: everything visible from a room lives in that room's
     * chunk. Any window beyond the exact cell draws OTHER rooms floating
     * unoccluded across the dead gaps (the school-courtyard corridor ghost;
     * previously band-aided per-arena via MapRegistry_IsExactCellArena and
     * a 4-nearest pcInDrawSet, both now subsumed). The old ±2/±1 window
     * masked mid-load voids from the 4-slot era, not a real retail gap —
     * interiors keep 16 resident slots and loads are synchronous now. */
    if (!map->isExterior)
    {
        return chunk->cellX == map->cellX && chunk->cellZ == map->cellZ;
    }
    return true;
#else
    if (map->cellX == chunk->cellX &&
        map->cellZ == chunk->cellZ)
    {
        return true;
    }
#endif

    return map->isExterior != false;
}

bool Ipd_IsTextureLoaded(s_IpdHeader* ipdHdr) // 0x80043B70
{
    if (!ipdHdr->isLoaded)
    {
        return false;
    }

    return Lm_IsTextureLoaded(ipdHdr->lmHdr);
}

s_IpdCollisionData* Ipd_HeaderCollisionDataGet(s_IpdHeader* ipdHdr) // 0x80043BA4
{
    if (ipdHdr->isLoaded)
    {
        return &ipdHdr->collisionData;
    }

    return NULL;
}

void IpdHeader_FixOffsets(s_IpdHeader* ipdHdr, s_LmHeader** lmHdrs, s32 lmHdrCount, s_ActiveChunkTextures* fullPageActiveTexs, s_ActiveChunkTextures* halfPageActiveTexs, e_FsFile fileIdx) // 0x80043BC4
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

        extern bool IpdHeader_FixOffsets_PC(s_IpdHeader* ipdHdr);
        if (!IpdHeader_FixOffsets_PC(ipdHdr)) {
            /* Buffer is not a valid IPD (stale/overlapping reuse, or still
             * mid-load while Fs already reports Loaded). Force isLoaded=false so
             * the renderer SKIPS this chunk instead of drawing the un-reformatted
             * garbage, and so it re-queues for reload. The comment used to claim
             * "isLoaded stays false" but never set it — after a map round-trip
             * (map1_s00 -> map1_s01 -> map1_s00) the buffer holds the other map's
             * chunk (magic != 0x14) while isLoaded is stale-true from the first
             * visit, so the garbage rendered as a black void engulfing the map. */
            ipdHdr->isLoaded = false;
            return;
        }
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
        if (slot < 0 && s_pcFixedIpdCount < PC_MAX_IPD_CHUNKS) { slot = s_pcFixedIpdCount++; }
        if (slot >= 0) { s_pcFixedIpd[slot].hdr = ipdHdr; s_pcFixedIpd[slot].lm = ipdHdr->lmHdr; }
        {
            /* Find which chunk slot this belongs to for logging */
            s32 logSlot = -1, si;
            for (si = 0; si < g_Map.activeChunkCount; si++) {
                if (g_Map.activeChunks[si].ipdHdr == ipdHdr) { logSlot = si; break; }
            }
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
    if (!(g_PcConfig.preloadChunks && g_Map.isExterior))
#endif
    {
        Ipd_MaterialsLoad(ipdHdr, fullPageActiveTexs, halfPageActiveTexs, fileIdx);
        Lm_MaterialFlagsApply(ipdHdr->lmHdr);
    }
    Ipd_HeaderModelLinkObjectLists(ipdHdr, lmHdrs, lmHdrCount);
    Ipd_HeaderModelBufferLinkObjectLists(ipdHdr, ipdHdr->modelInfo);
}

void Ipd_MaterialsLoad(s_IpdHeader* ipdHdr, s_ActiveChunkTextures* fullPageActiveTexs, s_ActiveChunkTextures* halfPageActiveTexs, e_FsFile fileIdx) // 0x80043C7C
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

    for (charCode = &mat->name.str[7]; charCode >= &mat->name.str[0]; charCode--)
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
        curModelBuf->modelInstances  = (u8*)curModelBuf->modelInstances + (u32)ipdHdr;
        curModelBuf->field_10 = (u8*)curModelBuf->field_10 + (u32)ipdHdr;
        curModelBuf->subcellPositions = (u8*)curModelBuf->subcellPositions + (u32)ipdHdr;
    }
}

void Ipd_HeaderModelLinkObjectLists(s_IpdHeader* ipdHdr, s_LmHeader** lmHdrs, s32 lmHdrCount) // 0x80043E50
{
    s32             i;
    s32             j;
    s_IpdModelInfo* curModelInfo;

    for (i = 0; i < ipdHdr->modelCount; i++)
    {
        curModelInfo = &ipdHdr->modelInfo[i];

        if (!curModelInfo->isGlobalPlm)
        {
            curModelInfo->modelHdr = LmHeader_ModelHeaderSearch(&curModelInfo->name, ipdHdr->lmHdr);
        }
        else
        {
            for (j = 0; j < lmHdrCount; j++)
            {
                curModelInfo->modelHdr = LmHeader_ModelHeaderSearch(&curModelInfo->name, lmHdrs[j]);
                if (curModelInfo->modelHdr != NULL)
                {
                    break;
                }
            }
        }
    }
}

s_ModelHeader* LmHeader_ModelHeaderSearch(u_Filename* name, s_LmHeader* lmHdr) // 0x80043F2C
{
    s32            i;
    s_ModelHeader* modelHeader;

    modelHeader = lmHdr->modelHdrs;

    for (i = 0; i < lmHdr->modelCount; i++, modelHeader++)
    {
        if (!COMPARE_FILENAMES(name, &modelHeader->name))
        {
            return modelHeader;
        }
    }

    return NULL;
}

void Ipd_HeaderModelBufferLinkObjectLists(s_IpdHeader* ipdHdr, s_IpdModelInfo* ipdModels) // 0x80043F88
{
    s_IpdModelBuffer*   curModelBuffer;
    s_IpdModelInstance* unkData;

    for (curModelBuffer = ipdHdr->modelBuffers;
         curModelBuffer < &ipdHdr->modelBuffers[ipdHdr->modelBufferCount];
         curModelBuffer++)
    {
        for (unkData = &curModelBuffer->modelInstances[0];
             unkData < &curModelBuffer->modelInstances[curModelBuffer->modelInstanceCount];
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
    ipd->collisionData.positionX += (cellX - prevCellX) * Q12_TO_Q8(CHUNK_CELL_SIZE);
    ipd->collisionData.positionZ += (cellZ - prevCellZ) * Q12_TO_Q8(CHUNK_CELL_SIZE);
}

void Ipd_ChunkDraw(s_IpdHeader* ipdHdr, q19_12 posX, q19_12 posZ, GsOT* ot, bool arg4) // 0x80044090
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
    s_IpdModelInstance* curBufC;
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
    if (g_DebugCamEnabled || g_PcConfig.disableCulling || Pc_WholeMapDrawActive()) {
        /* Render ALL model buffers, skip subcell/spatial culling. Whole-map
         * mode needs this too: the baked subcell PVS rectangles only cover
         * viewer offsets within ±3.2 cells of the chunk (s16 q7_8), so far
         * chunks would submit zero buffers no matter what is textured. */
        s32 startI = 0, endI = ipdHdr->modelBufferCount;
        temp_fp = NULL;
        for (i = startI; i < endI; i++)
        {
            ipdModelBuf = &ipdHdr->modelBuffers[i];
            /* Skip frustum check in debug mode - draw everything */
            {
                for (curBufC = ipdModelBuf->modelInstances; curBufC < &ipdModelBuf->modelInstances[ipdModelBuf->modelInstanceCount]; curBufC++)
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
    } else if (!g_Map.isExterior) {
        /* Interior maps: PSX subcell visibility rectangles were baked for
         * fixed-angle cameras. TPS camera orbits Harry and can fall outside
         * those rectangles, causing in-frustum model buffers to be dropped.
         * Skip subcell prefilter entirely; func_80057090 handles per-model
         * culling. Same approach as the debug-cam path above. */
        for (i = 0; i < ipdHdr->modelBufferCount; i++)
        {
            ipdModelBuf = &ipdHdr->modelBuffers[i];
            for (curBufC = ipdModelBuf->modelInstances; curBufC < &ipdModelBuf->modelInstances[ipdModelBuf->modelInstanceCount]; curBufC++)
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
            for (curBufC = ipdModelBuf->modelInstances; curBufC < &ipdModelBuf->modelInstances[ipdModelBuf->modelInstanceCount]; curBufC++)
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


// --- Ported from upstream (functions missing from our kept file) ---
#ifdef SH_PC_PORT
/* One entry per active chunk; PC preloads up to PC_MAX_IPD_CHUNKS (256), so a
   PSX-sized [4] overruns once >4 chunks are active (exterior maps). */
s_IpdCollisionData* g_Map_ActiveChunksCollisionData[256];
#else
s_IpdCollisionData* g_Map_ActiveChunksCollisionData[4];
#endif
s_IpdCollisionData* Ipd_CollisionDataGet(q19_12 posX, q19_12 posZ) // 0x800426E4
{
    s32          geomX;
    s32          geomZ;
    s32          cellX;
    s32          cellZ;
    s_IpdHeader* ipdHdr;
    s_Chunk*     curChunk;

    // Convert position to geometry space.
    geomX = Q12_TO_Q8(posX);
    geomZ = Q12_TO_Q8(posZ);

    // Compute cell coordinates.
    cellX = FLOOR_TO_STEP(geomX, Q12_TO_Q8(CHUNK_CELL_SIZE));
    cellZ = FLOOR_TO_STEP(geomZ, Q12_TO_Q8(CHUNK_CELL_SIZE));

    // Run through active chunks.
    for (curChunk = g_Map.activeChunks; curChunk < &g_Map.activeChunks[g_Map.activeChunkCount]; curChunk++)
    {
        // Check if chunk is loaded.
        if (Map_ChunkLoadStateGet(curChunk->queueIdx) < ChunkLoadState_Loaded)
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
    if (((s16*)(&g_Map.chunkGridCenter[cellZ]))[cellX] != NO_VALUE)
    {
#ifdef SH_PC_PORT
        /* [COLL-MISS] (#42 invisible walls): this NULL means "a chunk exists
         * for this cell but isn't serving collision" — downstream the miss
         * reads as ground 8 units down and movement refuses the step, i.e.
         * an invisible wall at the chunk border. Dump WHY each active slot
         * rejected the query (window membership, Fs load state, isLoaded) so
         * the failing gate is named. Rate-limited; the probes hit this many
         * times per frame while pushing against the block. */
        {
            static s32 s_lastMissLog = -1000;
            if (g_TickCount - s_lastMissLog > 30)
            {
                s_Chunk* c;
                s_lastMissLog = g_TickCount;
                SH_DBG("[COLL-MISS] cell(%d,%d) pos(%.1f,%.1f) activeCount=%d",
                       cellX, cellZ, (double)posX / 4096.0, (double)posZ / 4096.0,
                       (int)g_Map.activeChunkCount);
                for (c = g_Map.activeChunks; c < &g_Map.activeChunks[g_Map.activeChunkCount]; c++)
                {
                    SH_DBG("[COLL-MISS]   slot cell(%d,%d) qIdx=%d loadState=%d isLoaded=%d",
                           (int)c->cellX, (int)c->cellZ, (int)c->queueIdx,
                           (int)Map_ChunkLoadStateGet(c->queueIdx),
                           (c->ipdHdr != NULL) ? (int)c->ipdHdr->isLoaded : -1);
                }
            }
        }
#endif
        return NULL;
    }
    else
    {
        return &g_Map.collisionData;
    }
}

#ifdef SH_PC_PORT
/* Preload sets activeChunkCount to the WHOLE map (for rendering). Collision must
 * stay scoped to the player's local cell window (±1 — the exterior window the
 * chunk-select / Lm_ModelFind use, see ~L1107) like vanilla, which never had far
 * chunks active. Iterating far preloaded chunks lets their subcells fire phantom
 * wall-edges at the player = the preload-only invisible walls (#42). Console
 * `COLLSCOPE 0/1` (default 1 = scoped) for live A/B against a stuck spot. */
int g_PcChunkCollisionLocalScope = 1;
#endif

s_IpdCollisionData** Ipd_ActiveChunksCollisionDataGet(s32* collDataIdx) // 0x800425D8
{
    s_Chunk*            curChunk;
    s_IpdCollisionData* collData;
    s_IpdHeader*        ipdHdr;

    curChunk     = g_Map.activeChunks;
    *collDataIdx = 0;

    // Run through active chunks.
    while (curChunk < &g_Map.activeChunks[g_Map.activeChunkCount])
    {
#ifdef SH_PC_PORT
        if (g_PcChunkCollisionLocalScope && g_PcConfig.preloadChunks && g_Map.isExterior &&
            !(curChunk->cellX >= (g_Map.cellX - 1) && (g_Map.cellX + 1) >= curChunk->cellX &&
              curChunk->cellZ >= (g_Map.cellZ - 1) && (g_Map.cellZ + 1) >= curChunk->cellZ))
        {
            curChunk++;
            continue;
        }
#endif
        if (Map_ChunkLoadStateGet(curChunk->queueIdx) >= ChunkLoadState_Loaded)
        {
            ipdHdr = curChunk->ipdHdr;
            if (ipdHdr->isLoaded)
            {
                collData = Ipd_HeaderCollisionDataGet(ipdHdr);
                if (collData != NULL)
                {
                    g_Map_ActiveChunksCollisionData[(*collDataIdx)++] = collData;
                }
            }
        }

        curChunk++;
    }

    return &g_Map_ActiveChunksCollisionData[0];
}

// --- Ported from upstream ---
u32 Map_ChunkLoadStateGet(s32 queueIdx) // 0x80041ADC
{
    if (queueIdx == NO_VALUE)
    {
        return ChunkLoadState_Invalid;
    }
    else if (!Fs_QueueIsEntryLoaded(queueIdx))
    {
        return ChunkLoadState_Unloaded;
    }

    return ChunkLoadState_Loaded;
}
