/*
 * dms_reformat.c - Reformat PSX 32-bit DMS binary data to 64-bit PC layout
 *
 * On PSX, s_DmsHeader and s_DmsEntry have 4-byte pointers. When loaded from
 * disc on a 64-bit PC, the binary data uses the 32-bit layout but the C
 * structs have 8-byte pointers + alignment padding.
 *
 * This module parses the raw PSX binary data and writes properly formatted
 * 64-bit struct fields, with offsets converted to real pointers.
 *
 * PSX struct sizes (all pointers are 4 bytes):
 *   s_DmsHeader:  44 bytes (28 header + 16 inline camera entry)
 *   s_DmsEntry:   16 bytes (64-bit: 24 bytes)
 *   s_DmsSegment: 4 bytes (no pointers, layout-identical)
 *   s_DmsKeyframeCamera:    16 bytes (no pointers, layout-identical)
 *   s_DmsKeyframeCharacter: 12 bytes (no pointers, layout-identical)
 *   SVECTOR3:                6 bytes (no pointers, layout-identical)
 *
 * PSX binary layout at buffer start:
 *   0x00 [1]  isLoaded
 *   0x01 [1]  characterCount
 *   0x02 [1]  intervalCount
 *   0x03 [1]  field_3
 *   0x04 [4]  field_4
 *   0x08 [4]  intervalOffset    (relative to buffer start)
 *   0x0C [12] origin          (VECTOR3: 3 x s32)
 *   0x18 [4]  charactersOffset  (relative to buffer start)
 *   0x1C [16] camera entry      (s_DmsEntry, PSX layout)
 *   --- total header region: 0x2C (44 bytes) ---
 *
 * PSX s_DmsEntry layout (16 bytes):
 *   0x00 [2]  keyframeCount
 *   0x02 [1]  holdRangeCount
 *   0x03 [1]  field_3
 *   0x04 [4]  name[4]
 *   0x08 [4]  svectorOffset    (relative to buffer start)
 *   0x0C [4]  keyframeOffset   (relative to buffer start)
 */

#include "game.h"
#include "sh_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/dms.h"

#define PSX_SIZEOF_DMS_HEADER 28   /* 0x1C — just the header fields, no camera */
#define PSX_SIZEOF_DMS_ENTRY  16

/*
 * Heap-backed DMS header.  The game stores DMS data in FS_BUFFER_16 which can
 * get overwritten by other file loads (chunk streaming, etc.) while the
 * cutscene is still running.  We keep a full heap copy of the parsed header
 * and all pointed-to data so nothing depends on the FS buffer after parsing.
 */
s_DmsHeader* g_DmsHeapHeader = NULL;

static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline s16 rd16s(const u8* p) { return (s16)(p[0] | (p[1] << 8)); }

static void ParseDmsEntry(s_DmsEntry* dst, const u8* src, u8* base)
{
    dst->keyframeCount = rd16s(&src[0]);
    dst->holdRangeCount  = src[2];
    dst->field_3         = src[3];
    memcpy(dst->name, &src[4], 4);

    u32 svecOff = rd32(&src[8]);
    u32 kfOff   = rd32(&src[12]);

    /* SVECTORs and keyframes — copy to heap so they survive buffer overwrites */
    s32 svecBytes = dst->holdRangeCount * sizeof(SVECTOR3);
    dst->holdRanges = (SVECTOR3*)malloc(svecBytes);
    memcpy(dst->holdRanges, base + svecOff, svecBytes);

    /* Keyframe size depends on whether this is a camera or character entry.
     * We don't know yet, so store raw pointer temporarily — the caller
     * (Dms_HeaderFixOffsets_PC) will heap-copy keyframes after parsing. */
    dst->keyframes.character = (s_DmsKeyframeCharacter*)(base + kfOff);
}

/**
 * PC replacement for DmsHeader_FixOffsets.
 *
 * Reads the raw PSX 32-bit binary data at dmsHdr's address, parses it into
 * properly formatted 64-bit structs (heap-allocated for character entries),
 * then overwrites the s_DmsHeader with the correct 64-bit values.
 *
 * Intervals are also heap-copied since they may overlap with the expanded
 * 64-bit header region (PSX header = 44 bytes, 64-bit header = 64 bytes).
 */
void Dms_HeaderFixOffsets_PC(s_DmsHeader* dmsHdr)
{
    u8* raw = (u8*)dmsHdr;

    SH_DBG("[DMS] FixOffsets ENTER: raw[0]=%d raw[1]=%d raw[2]=%d raw[3]=%d ptr=%p",
           raw[0], raw[1], raw[2], raw[3], (void*)dmsHdr);
    SH_DBG("[DMS]   raw bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
           raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
           raw[8], raw[9], raw[10], raw[11]);

    /* Check if already reformatted (isLoaded is at byte 0 in both layouts) */
    if (raw[0] == 1)
    {
        SH_DBG("[DMS] FixOffsets: already loaded, skipping");
        return;
    }

    /* Read all PSX header fields from raw bytes before we overwrite anything */
    u8  characterCount = raw[1];
    u8  intervalCount  = raw[2];
    u8  field_3        = raw[3];
    u32 field_4        = rd32(&raw[4]);
    u32 intervalOff    = rd32(&raw[8]);
    s32 originVx       = (s32)rd32(&raw[12]);
    s32 originVy       = (s32)rd32(&raw[16]);
    s32 originVz       = (s32)rd32(&raw[20]);
    u32 charactersOff  = rd32(&raw[24]);

    SH_DBG("[DMS] FixOffsets: chars=%d intervals=%d origin=(%d,%d,%d) intOff=0x%x charOff=0x%x",
           characterCount, intervalCount, originVx, originVy, originVz, intervalOff, charactersOff);

    /* Parse camera entry (immediately after PSX header at offset 0x1C) */
    s_DmsEntry camera;
    memset(&camera, 0, sizeof(camera));
    ParseDmsEntry(&camera, &raw[PSX_SIZEOF_DMS_HEADER], raw);

    SH_DBG("[DMS]   camera: keyframes=%d svecs=%d name=%.4s",
           camera.keyframeCount, camera.holdRangeCount, camera.name);

    /* Parse character entries from PSX layout (16 bytes each) into heap */
    s_DmsEntry* characters = NULL;
    if (characterCount > 0)
    {
        characters = (s_DmsEntry*)calloc(characterCount, sizeof(s_DmsEntry));
        for (int i = 0; i < characterCount; i++)
        {
            ParseDmsEntry(&characters[i],
                          raw + charactersOff + i * PSX_SIZEOF_DMS_ENTRY,
                          raw);
            SH_DBG("[DMS]   character[%d]: name=%.4s keyframes=%d svecs=%d",
                   i, characters[i].name, characters[i].keyframeCount,
                   characters[i].holdRangeCount);
        }
    }

    /*
     * Copy intervals to heap. They may overlap with the expanded 64-bit
     * header region (PSX header = 44 bytes, 64-bit = 64 bytes, so bytes
     * 44-63 get clobbered by the memset below).
     */
    s_DmsSegment* intervals = NULL;
    if (intervalCount > 0)
    {
        intervals = (s_DmsSegment*)malloc(intervalCount * sizeof(s_DmsSegment));
        memcpy(intervals, raw + intervalOff, intervalCount * sizeof(s_DmsSegment));
    }

    /* Heap-copy camera keyframes */
    {
        s32 camKfBytes = camera.keyframeCount * sizeof(s_DmsKeyframeCamera);
        s_DmsKeyframeCamera* camKfHeap = (s_DmsKeyframeCamera*)malloc(camKfBytes);
        memcpy(camKfHeap, camera.keyframes.camera, camKfBytes);
        camera.keyframes.camera = camKfHeap;
    }

    /* Heap-copy character keyframes */
    for (int i = 0; i < characterCount; i++)
    {
        s32 charKfBytes = characters[i].keyframeCount * sizeof(s_DmsKeyframeCharacter);
        s_DmsKeyframeCharacter* charKfHeap = (s_DmsKeyframeCharacter*)malloc(charKfBytes);
        memcpy(charKfHeap, characters[i].keyframes.character, charKfBytes);
        characters[i].keyframes.character = charKfHeap;
    }

    /*
     * Build the header on the heap so it survives FS buffer overwrites.
     * Also write it into the FS buffer for backwards compatibility,
     * but DMS functions will use g_DmsHeapHeader via the redirect in dms.c.
     */
    if (g_DmsHeapHeader) free(g_DmsHeapHeader);
    g_DmsHeapHeader = (s_DmsHeader*)calloc(1, sizeof(s_DmsHeader));

    g_DmsHeapHeader->isLoaded       = 1;
    g_DmsHeapHeader->characterCount = characterCount;
    g_DmsHeapHeader->segmentCount  = intervalCount;
    g_DmsHeapHeader->field_3          = field_3;
    g_DmsHeapHeader->field_4          = field_4;
    g_DmsHeapHeader->segments    = intervals;
    g_DmsHeapHeader->origin.vx      = originVx;
    g_DmsHeapHeader->origin.vy      = originVy;
    g_DmsHeapHeader->origin.vz      = originVz;
    g_DmsHeapHeader->characters    = characters;
    g_DmsHeapHeader->camera        = camera;

    /* Also write into the buffer so code that reads it directly still works */
    memset(dmsHdr, 0, sizeof(s_DmsHeader));
    *dmsHdr = *g_DmsHeapHeader;

    SH_DBG("[DMS] FixOffsets: done");
}
