#ifndef _BODYPROG_MAP_TERRAIN_H
#define _BODYPROG_MAP_TERRAIN_H

#include "bodyprog/formats/ipd.h"
#include "bodyprog/formats/lm.h"

/** @brief Map terrain chunk IPD file load states.
 *
 * See `Map_ChunkLoadStateGet`.
 */
typedef enum _ChunkLoadState
{
    ChunkLoadState_Invalid  = 0, /** Entry index is `NO_VALUE`. */
    ChunkLoadState_Unloaded = 1, /** Not currently loaded. */
    ChunkLoadState_Loaded   = 2  /** Currently loaded. */
} e_ChunkLoadState;

/** @brief Map terrain chunk. */
typedef struct _MapChunk
{
    /* 0x0  */ s_IpdHeader* ipdHdr;
    /* 0x4  */ s32          queueIdx;
    /* 0x8  */ s16          cellX;
    /* 0xA  */ s16          cellZ;
    /* 0xC  */ q19_12       paddedDistanceToEdge0;
    /* 0x10 */ q19_12       paddedDistanceToEdge1;
    /* 0x14 */ u8           materialCount;
    /* 0x15 */ s8           __pad_15[3];
    /* 0x18 */ s32          outsideCount;
#ifdef SH_PC_PORT
    /* Interior draw gate under resident_textures: set for the 4 nearest
     * loaded chunks each scan (the visibility rule vanilla enforced through
     * distance-limited TEXTURING — with every chunk textured, drawing needs
     * its own gate or neighbor rooms ghost into open areas like the school
     * courtyard). */
    u8 pcInDrawSet;
#endif
} s_Chunk;
STATIC_ASSERT_SIZEOF(s_Chunk, 28);

/** @brief Map terrain chunk column. TODO: Or row? */
typedef struct _MapChunkColumn
{
    /* 0x0 */ s16 idx[16];
} s_ChunkColumn;
STATIC_ASSERT_SIZEOF(s_ChunkColumn, 32);

#ifdef SH_PC_PORT
/* Expanded chunk-texture pool (resident_textures config, default on): past
 * the 10 vanilla VRAM page slots, materials claim VIRTUAL slots backed by
 * persistent per-slot GL textures instead of VRAM pages (key encoding and
 * rationale: pc_port/include/hires_override.h). Physical slots keep
 * byte-exact vanilla behavior and are claimed first. */
#define PC_TEXPOOL_FULL_EXTRA 192
#define PC_TEXPOOL_HALF_EXTRA 64
#define PC_TEXPOOL_LIST_MAX   (8 + PC_TEXPOOL_FULL_EXTRA)
#else
#define PC_TEXPOOL_FULL_EXTRA 0
#define PC_TEXPOOL_HALF_EXTRA 0
#define PC_TEXPOOL_LIST_MAX   10
#endif

typedef struct _ActiveChunkTextures
{
    /* 0x0 */ s32        count;
    /* 0x4 */ s_Texture* textures[PC_TEXPOOL_LIST_MAX];
} s_ActiveChunkTextures;

/** @brief Texture data associated with map terrain chunks. */
typedef struct _ChunkTextures
{
    /* 0x0   */ s_ActiveChunkTextures fullPage;
    /* 0x2C  */ s_ActiveChunkTextures halfPage;
    /* 0x58  */ s_Texture             fullPageTextures[8 + PC_TEXPOOL_FULL_EXTRA];
    /* 0x118 */ s_Texture             halfPageTextures[2 + PC_TEXPOOL_HALF_EXTRA];
} s_ChunkTextures;
STATIC_ASSERT_SIZEOF(s_ChunkTextures, 328);

/** @brief Map terrain layout. */
typedef struct _MapTerrain
{
    /* 0x0   */ s_IpdCollisionData collisionData; // Default chunk collision data?
    /* 0x134 */ s32                textureFileIdx;
    /* 0x138 */ s_GlobalLm         globalLm;
    /* 0x144 */ char               mapTag[4];
    /* 0x148 */ s32                mapTagSize;
    /* 0x14C */ s32                ipdFileIdx;
    /* 0x150 */ s_IpdHeader*       chunkBuffer;
    /* 0x154 */ s32                chunkBufferSize;
    /* 0x158 */ s32                activeChunkCount;
#ifdef SH_PC_PORT
    /* 0x15C */ s_Chunk            activeChunks[256]; // PC preloads up to PC_MAX_IPD_CHUNKS (256) chunks; PSX streams 4. MUST match PC_MAX_IPD_CHUNKS in bodyprog_80040B74.c.
#else
    /* 0x15C */ s_Chunk            activeChunks[4];
#endif
    /* 0x1CC */ s_ChunkColumn      chunkGrid[19];
    /* 0x42C */ s_ChunkColumn*     chunkGridCenter;
    /* 0x430 */ s_ChunkTextures    chunkTextures;
    /* 0x578 */ q19_12             positionX;
    /* 0x57C */ q19_12             positionZ;
    /* 0x580 */ s32                cellX;
    /* 0x584 */ s32                cellZ;
    /* 0x588 */ bool               isExterior;
} s_MapTerrain;
STATIC_ASSERT_SIZEOF(s_MapTerrain, 1420);

#endif
