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
 *   s_IpdModelInstance:   36 bytes (64-bit: 40)
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

#ifdef SH_XBOX_PORT
/* Xbox (64MB) heap-leak fix. IPD reformats on EVERY chunk stream (walking the
 * world) and the stock code frees NONE of: modelInfos, modelBuffers, each
 * buffer's modelInstances, or the clobber-relocated collision sub-arrays
 * (ipd_reformat.c even says "freed never"). On PC's gigabytes it is a slow
 * leak; on Xbox the continuous streaming exhausts the malloc heap -> the
 * "framerate/sound slows down the closer I get" report, then a NULL alloc
 * crash. Track each parse's allocations by buffer base and free the prior set
 * when the SAME buffer streams a new chunk (reused buffer => old chunk gone;
 * same proven contract as lm_reformat.c's LmTrack). */
#define IPD_TRACK_MAX 128
typedef struct {
    void*             raw;
    s_IpdModelInfo*   modelInfos;
    s_IpdModelBuffer* modelBuffers;
    int               modelBufferCount;
    void*             reloc[7];
    int               relocCount;
    int               used;
} IpdTrack;
static IpdTrack s_ipdTrack[IPD_TRACK_MAX];

/* Heap sub-arrays allocated by IpdColl_RelocateClobbered for the current parse;
 * captured here so IpdTrack_Record can hand them to the free registry. */
static void* s_ipdPendingReloc[7];
static int   s_ipdPendingRelocN;

static void IpdTrack_Free(IpdTrack* t)
{
    int i;
    if (t->modelBuffers) {
        for (i = 0; i < t->modelBufferCount; i++)
            free(t->modelBuffers[i].modelInstances);
        free(t->modelBuffers);
    }
    free(t->modelInfos);
    for (i = 0; i < t->relocCount; i++)
        free(t->reloc[i]);
    t->raw = NULL; t->modelInfos = NULL; t->modelBuffers = NULL;
    t->modelBufferCount = 0; t->relocCount = 0; t->used = 0;
}

static void IpdTrack_FreePrior(void* raw)
{
    int i;
    for (i = 0; i < IPD_TRACK_MAX; i++)
        if (s_ipdTrack[i].used && s_ipdTrack[i].raw == raw) {
            IpdTrack_Free(&s_ipdTrack[i]);
            return;
        }
}

static void IpdTrack_Record(void* raw, s_IpdModelInfo* modelInfos,
                            s_IpdModelBuffer* modelBuffers, int modelBufferCount)
{
    int i, slot = -1;
    for (i = 0; i < IPD_TRACK_MAX; i++) {
        if (s_ipdTrack[i].used && s_ipdTrack[i].raw == raw) { slot = i; break; }
        if (!s_ipdTrack[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return; /* table full (unreached: keyed by the small buffer pool) */
    s_ipdTrack[slot].raw = raw;
    s_ipdTrack[slot].modelInfos = modelInfos;
    s_ipdTrack[slot].modelBuffers = modelBuffers;
    s_ipdTrack[slot].modelBufferCount = modelBufferCount;
    for (i = 0; i < s_ipdPendingRelocN && i < 7; i++)
        s_ipdTrack[slot].reloc[i] = s_ipdPendingReloc[i];
    s_ipdTrack[slot].relocCount = s_ipdPendingRelocN;
    s_ipdTrack[slot].used = 1;
}
#endif

static void ParseIpdModelInfo(s_IpdModelInfo* dst, const u8* src)
{
    dst->isGlobalPlm = src[0];
    /* src[1..3] are padding in s_IpdModelInfo (no named field). */
    memcpy(&dst->name, &src[4], 8);
    /* modelHdr is initially NULL - will be resolved by ModelLinkObjectLists */
    dst->modelHdr = NULL;
}

static void ParseIpdModelBufferC(s_IpdModelInstance* dst, const u8* src)
{
    /* modelHdr initially holds a model index (small integer), not a pointer.
     * Ipd_HeaderModelBufferLinkObjectLists replaces it with actual pointer. */
    u32 modelIdx = rd32(&src[0]);
    dst->modelHdr = (s_ModelHeader*)(uintptr_t)modelIdx;

    /* PSX MATRIX is 32 bytes on disc: short m[3][3] (18B) + 2B pad + s32 t[3] (12B).
     * On LP64 (Linux/macOS) sizeof(MATRIX) = 48 (long t[] widens to 8B each plus
     * 6B alignment pad), so memcpy(sizeof(MATRIX)) reads 16B past the binary data
     * AND mis-places t[] — parse field-by-field instead. PSX offsets from src[4]:
     *   m[r][c] at (r*3+c)*2  (0..16);  t[0]=20, t[1]=24, t[2]=28 */
    {
        int r, c;
        const u8* msrc = &src[4];
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                dst->mat.m[r][c] = rds16(&msrc[(r * 3 + c) * 2]);
        dst->mat.t[0] = rds32(&msrc[20]);
        dst->mat.t[1] = rds32(&msrc[24]);
        dst->mat.t[2] = rds32(&msrc[28]);
    }
}

static void ParseIpdModelBuffer(s_IpdModelBuffer* dst, const u8* src, u8* base)
{
    dst->modelInstanceCount      = src[0];
    dst->field_1      = src[1];
    dst->subcellCount = src[2];
    dst->__pad_3      = (s8)src[3];
    dst->minX         = rds16(&src[4]);
    dst->maxX         = rds16(&src[6]);
    dst->minZ         = rds16(&src[8]);
    dst->maxZ         = rds16(&src[10]);

    u32 fieldC_off  = rd32(&src[12]);
    u32 field10_off = rd32(&src[16]);
    u32 field14_off = rd32(&src[20]);

    /* field_C: array of s_IpdModelInstance, field_0 entries */
    if (dst->modelInstanceCount > 0)
    {
        dst->modelInstances = (s_IpdModelInstance*)calloc(dst->modelInstanceCount, sizeof(s_IpdModelInstance));
#ifdef SH_XBOX_PORT
        if (!dst->modelInstances)
        {
            SH_DBG("  [IPD] modelInstances calloc FAILED (count=%d) — skipping (heap low)", dst->modelInstanceCount);
            dst->modelInstanceCount = 0;
        }
#endif
        for (int i = 0; i < dst->modelInstanceCount; i++)
        {
            ParseIpdModelBufferC(&dst->modelInstances[i],
                base + fieldC_off + i * PSX_IPD_MODEL_BUFFER_C_SIZE);
        }
    }
    else
    {
        dst->modelInstances = NULL;
    }

    /* field_10: SVECTOR array, field_1 entries - raw data in buffer, no reformatting needed */
    dst->field_10 = (SVECTOR*)(base + field10_off);

    /* subcellPositions: SVECTOR bounding box array, field_2 entries */
    dst->subcellPositions = (SVECTOR*)(base + field14_off);
}

static void ParseIpdCollisionData(s_IpdCollisionData* dst, const u8* collraw, u8* collbase)
{
    dst->positionX = rds32(&collraw[0x00]);
    dst->positionZ = rds32(&collraw[0x04]);

    /* Bitfield at offset 0x08 - read as u32 and manually set fields */
    u32 bf = rd32(&collraw[0x08]);
    dst->splitVertexCount  = bf & 0xFF;
    dst->surfaceCount  = (bf >> 8) & 0xFF;
    dst->subcellCount = (bf >> 16) & 0xFF;
    dst->field_8_24 = (bf >> 24) & 0xFF;

    /* Collision sub-pointers: offsets relative to collision data base */
    u32 ptrC_off  = rd32(&collraw[0x0C]);
    u32 ptr10_off = rd32(&collraw[0x10]);
    u32 ptr14_off = rd32(&collraw[0x14]);
    u32 ptr18_off = rd32(&collraw[0x18]);


    dst->splitVertices  = (SVECTOR3*)(collbase + ptrC_off);
    dst->surfaces = (s_IpdCollSurface*)(collbase + ptr10_off);
    dst->subcells = (s_IpdCollSubcell*)(collbase + ptr14_off);
    dst->ptr_18 = (s_IpdCollisionData_18*)(collbase + ptr18_off);

    dst->subcellSize = rds16(&collraw[0x1C]);
    dst->subcellCountX = collraw[0x1E];
    dst->subcellCountZ = collraw[0x1F];

    u32 ptr20_off = rd32(&collraw[0x20]);
    dst->subcellRanges = (s_IpdCollSubcellRange*)(collbase + ptr20_off);

    dst->field_24 = rd16(&collraw[0x24]);
    dst->field_26 = rd16(&collraw[0x26]);

    u32 ptr28_off = rd32(&collraw[0x28]);
    u32 ptr2C_off = rd32(&collraw[0x2C]);
    dst->ptr_28 = (u8*)(collbase + ptr28_off);
    dst->ptr_2C = (void*)(collbase + ptr2C_off);

    /* Dump first few ptr_10 entries to verify data alignment */
    {
        s_IpdCollSurface* p10 = dst->surfaces;
        if (dst->surfaceCount > 1)
            ;
    }

    dst->subcellCheckCount = collraw[0x30];
    dst->__pad[0] = collraw[0x31];
    dst->__pad[1] = collraw[0x32];
    dst->__pad[2] = collraw[0x33];

    memcpy(dst->subcellCheckIdx, &collraw[0x34], 256);
}

/* The 64-bit s_IpdHeader written in place over the file buffer is LARGER than
 * the PSX on-disk header (0x188 bytes), so writing it overwrites the first
 * sizeof(s_IpdHeader)-0x188 bytes of whatever file data follows — which is the
 * head of the collision sub-arrays (they start right after the PSX collision
 * struct, at +0x134 from the collision base = file offset 0x188). Zeroed
 * s_IpdCollSurface entries read as height-0/tilt-0 ground, and the wall check
 * (ground >= 0.5u above the character) then reports phantom walls at fixed
 * spots every time the chunk loads ("invisible walls", worst on stairs).
 *
 * Relocate every sub-array that begins inside the clobber window to the heap
 * BEFORE the header write. Array ends are bounded by the next array's file
 * offset (the arrays are laid out in ascending order), capped by lmHdrOff.
 * Small (typically one array, <2KB); freed never — matches the existing
 * modelInfos/modelBuffers per-reformat allocation pattern. */
static void IpdColl_RelocateClobbered(s_IpdCollisionData* dst, const u8* collraw, u8* collbase, u32 lmHdrOff)
{
    u32    offs[7];
    void** slots[7];
    u32    clobberEnd;
    u32    cap;
    int    i, j;

    offs[0] = rd32(&collraw[0x0C]); slots[0] = (void**)&dst->splitVertices;
    offs[1] = rd32(&collraw[0x10]); slots[1] = (void**)&dst->surfaces;
    offs[2] = rd32(&collraw[0x14]); slots[2] = (void**)&dst->subcells;
    offs[3] = rd32(&collraw[0x18]); slots[3] = (void**)&dst->ptr_18;
    offs[4] = rd32(&collraw[0x20]); slots[4] = (void**)&dst->subcellRanges;
    offs[5] = rd32(&collraw[0x28]); slots[5] = (void**)&dst->ptr_28;
    offs[6] = rd32(&collraw[0x2C]); slots[6] = (void**)&dst->ptr_2C;

    clobberEnd = (u32)sizeof(s_IpdHeader) - 0x54; /* relative to collbase */
    cap        = (lmHdrOff > 0x54) ? (lmHdrOff - 0x54) : 0;

    for (i = 0; i < 7; i++)
    {
        u32 end, size;
        u8* heap;

        /* Real sub-arrays start at/after the end of the PSX collision struct
         * (0x134 rel). Smaller values mean "absent" — leave those alone. */
        if (offs[i] < PSX_IPD_COLLISION_SIZE || offs[i] >= clobberEnd)
        {
            continue;
        }

        /* End bound = nearest larger sub-array offset, else the LM header. */
        end = cap;
        for (j = 0; j < 7; j++)
        {
            if (offs[j] > offs[i] && (end <= offs[i] || offs[j] < end))
            {
                end = offs[j];
            }
        }
        if (end <= offs[i])
        {
            end = offs[i] + 0x1000;
        }
        if (end - offs[i] > 0x4000)
        {
            end = offs[i] + 0x4000;
        }

        size = end - offs[i];
        heap = (u8*)malloc(size);
#ifdef SH_XBOX_PORT
        if (!heap)
        {
            /* Heap-critical: leave the pointer in the buffer (clobber-window bug
             * returns for this one array, but no crash) rather than memcpy NULL. */
            continue;
        }
        if (s_ipdPendingRelocN < 7)
            s_ipdPendingReloc[s_ipdPendingRelocN++] = heap;   /* register for later free */
#endif
        memcpy(heap, collbase + offs[i], size);
        *slots[i] = heap;

        {
            static int s_relocLogged = 0;
            if (s_relocLogged < 4)
            {
                SH_DBG("[IPD-RELOC] sub-array %d at +0x%X (%u bytes) was in the header clobber window [0x%X,0x%X) — relocated",
                       i, offs[i], size, (u32)PSX_IPD_COLLISION_SIZE, clobberEnd);
                s_relocLogged++;
            }
        }
    }
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
/* Returns true if the buffer was reformatted, false if it is not a valid IPD
 * (stale or still mid-load) so the caller can skip the rest of the fixup and
 * retry on a later frame instead of dereferencing a garbage lmHdr. */
bool IpdHeader_FixOffsets_PC(s_IpdHeader* ipdHdr)
{
    u8* raw = (u8*)ipdHdr;

    /* "Already reformatted" is now decided by the caller (IpdHeader_FixOffsets)
     * via a buffer-reuse-safe lmHdr check. The old raw[1] (isLoaded byte) test
     * was unreliable — that byte is undefined IPD file data and is 1 for some
     * IPDs, which skipped the fixup and left a garbage lmHdr (crash in
     * Lm_MaterialCountGet). Just validate the magic and reformat. */

    /* Validate magic */
    if (raw[0] != IPD_HEADER_MAGIC)
    {
        return false;
    }

#ifdef SH_XBOX_PORT
    /* Free this buffer's PRIOR chunk allocations before re-parsing (leak fix),
     * and reset the per-parse reloc capture that RelocateClobbered fills below. */
    IpdTrack_FreePrior(raw);
    s_ipdPendingRelocN = 0;
#endif

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

    /* Parse collision into a local and rescue clobber-window arrays NOW,
     * while the file bytes past 0x188 are still intact — the header
     * memset/writes below stomp [0x188, sizeof(s_IpdHeader)). */
    s_IpdCollisionData collParsed;
    memset(&collParsed, 0, sizeof(collParsed));
    ParseIpdCollisionData(&collParsed, collraw, collbase);
    IpdColl_RelocateClobbered(&collParsed, collraw, collbase, lmHdrOff);

    /* Parse model info array (PSX stride = 16 bytes) */
    s_IpdModelInfo* modelInfos = NULL;
    if (modelCount > 0)
    {
        modelInfos = (s_IpdModelInfo*)calloc(modelCount, sizeof(s_IpdModelInfo));
#ifdef SH_XBOX_PORT
        if (!modelInfos)
        {
            SH_DBG("  [IPD] modelInfo calloc FAILED (count=%d) — skipping (heap low)", modelCount);
            modelCount = 0;
        }
#endif
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
#ifdef SH_XBOX_PORT
        if (!modelBuffers)
        {
            SH_DBG("  [IPD] modelBuffers calloc FAILED (count=%d) — skipping (heap low)", modelBufferCount);
            modelBufferCount = 0;
        }
#endif
        for (int i = 0; i < modelBufferCount; i++)
        {
            ParseIpdModelBuffer(&modelBuffers[i],
                raw + modelBuffersOff + i * PSX_IPD_MODEL_BUFFER_SIZE, raw);
        }
    }

    /* Now overwrite the s_IpdHeader with 64-bit struct values.
     * This clobbers raw bytes but we've already parsed everything above. */
    memset(ipdHdr, 0, sizeof(s_IpdHeader));

    ipdHdr->magic            = magic;
    ipdHdr->isLoaded         = 0; /* Will be set to 1 by caller after all fixups */
    ipdHdr->cellX            = (s8)cellX;
    ipdHdr->cellZ            = (s8)cellZ;
    ipdHdr->lmHdr            = (s_LmHeader*)(raw + lmHdrOff);
    ipdHdr->modelCount       = modelCount;
    ipdHdr->modelBufferCount = modelBufferCount;
    ipdHdr->modelOrderCount  = modelOrderCount;
    ipdHdr->__pad_B[0]           = unkB;
    memcpy(&ipdHdr->__pad_B[1], unkC, 8);
    ipdHdr->modelInfo       = modelInfos;
    ipdHdr->modelBuffers    = modelBuffers;

#ifdef SH_XBOX_PORT
    /* Remember this parse's allocations (incl. the reloc sub-arrays captured in
     * s_ipdPendingReloc) so the next re-parse of this buffer frees them. */
    IpdTrack_Record(raw, modelInfos, modelBuffers, modelBufferCount);
#endif

    /* Restore subcell table: textureCount(1) + unk_1D[51] = 52 bytes total */
    ipdHdr->textureCount = subcellTable[0];
    memcpy(ipdHdr->unk_1D, &subcellTable[1], 51);

    ipdHdr->modelOrderList = raw + modelOrderOff;

    /* Sanity: the in-place header also clobbers [0x188, sizeof(s_IpdHeader))
     * for NON-collision sections. The collision arrays are rescued above; if
     * any model-side section ever starts that early too, surface it. */
    {
        static int s_modelClobLogged = 0;
        if (s_modelClobLogged < 4 &&
            ((modelInfoOff    && modelInfoOff    < sizeof(s_IpdHeader)) ||
             (modelBuffersOff && modelBuffersOff < sizeof(s_IpdHeader)) ||
             (modelOrderOff   && modelOrderOff   < sizeof(s_IpdHeader))))
        {
            SH_DBG("[IPD-RELOC] WARNING: model section in clobber window (info=0x%X bufs=0x%X order=0x%X hdr=0x%X)",
                   modelInfoOff, modelBuffersOff, modelOrderOff, (u32)sizeof(s_IpdHeader));
            s_modelClobLogged++;
        }
    }

    /* Install the collision data parsed (and clobber-rescued) above. */
    ipdHdr->collisionData = collParsed;

    /* Register as valid collision data for stale-pointer detection */
    {
        extern void PC_CollRegisterValid(s_IpdCollisionData* cd);
        PC_CollRegisterValid(&ipdHdr->collisionData);
    }

    return true;
}
