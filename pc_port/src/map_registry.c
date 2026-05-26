#include "map_registry.h"
#include "map_overlay_loader.h"
#include "pc_config.h"
#include "sh_log.h"
#include <string.h>
#include <stdio.h>

/* The global pointer that all game code uses via the macro:
 *   #define g_MapOverlayHeader (*g_pMapOverlayHeader)
 */
s_MapOverlayHeader* g_pMapOverlayHeader = NULL;

/* The fully-compiled map0_s00 header (renamed via -DSH_MAP_NAME=map0_s00). */
extern s_MapOverlayHeader g_MapOverlayHeader_map0_s00;

/* ========================================================================
 * Stub headers for maps that aren't fully compiled.
 * These provide correct metadata so geometry/chunks load properly,
 * but all function pointers are NULL (zeroed by default).
 * ======================================================================== */

/* No-op loading screen function (avoids NULL calls). */
static void MapStub_LoadScreenNoop(void) {}
static void (*g_StubLoadScreenFuncs[])(void) = { MapStub_LoadScreenNoop, NULL };

/* Default spawn point for stub maps (origin). */
static s_MapPoint2d g_StubMapPoint = { 0 };

/* Map name table - maps overlay IDs to directory names. */
static const char* const MAP_NAMES[] = {
    [MapIdx_MAP0_S00] = "map0_s00",
    [MapIdx_MAP0_S01] = "map0_s01",
    [MapIdx_MAP0_S02] = "map0_s02",
    [MapIdx_MAP1_S00] = "map1_s00",
    [MapIdx_MAP1_S01] = "map1_s01",
    [MapIdx_MAP1_S02] = "map1_s02",
    [MapIdx_MAP1_S03] = "map1_s03",
    [MapIdx_MAP1_S04] = "map1_s04",
    [MapIdx_MAP1_S05] = "map1_s05",
    [MapIdx_MAP1_S06] = "map1_s06",
    [MapIdx_MAP2_S00] = "map2_s00",
    [MapIdx_MAP2_S01] = "map2_s01",
    [MapIdx_MAP2_S02] = "map2_s02",
    [MapIdx_MAP2_S03] = "map2_s03",
    [MapIdx_MAP2_S04] = "map2_s04",
    [MapIdx_MAP3_S00] = "map3_s00",
    [MapIdx_MAP3_S01] = "map3_s01",
    [MapIdx_MAP3_S02] = "map3_s02",
    [MapIdx_MAP3_S03] = "map3_s03",
    [MapIdx_MAP3_S04] = "map3_s04",
    [MapIdx_MAP3_S05] = "map3_s05",
    [MapIdx_MAP3_S06] = "map3_s06",
    [MapIdx_MAP4_S00] = "map4_s00",
    [MapIdx_MAP4_S01] = "map4_s01",
    [MapIdx_MAP4_S02] = "map4_s02",
    [MapIdx_MAP4_S03] = "map4_s03",
    [MapIdx_MAP4_S04] = "map4_s04",
    [MapIdx_MAP4_S05] = "map4_s05",
    [MapIdx_MAP4_S06] = "map4_s06",
    [MapIdx_MAP5_S00] = "map5_s00",
    [MapIdx_MAP5_S01] = "map5_s01",
    [MapIdx_MAP5_S02] = "map5_s02",
    [MapIdx_MAP5_S03] = "map5_s03",
    [MapIdx_MAP6_S00] = "map6_s00",
    [MapIdx_MAP6_S01] = "map6_s01",
    [MapIdx_MAP6_S02] = "map6_s02",
    [MapIdx_MAP6_S03] = "map6_s03",
    [MapIdx_MAP6_S04] = "map6_s04",
    [MapIdx_MAP6_S05] = "map6_s05",
    [MapIdx_MAP7_S00] = "map7_s00",
    [MapIdx_MAP7_S01] = "map7_s01",
    [MapIdx_MAP7_S02] = "map7_s02",
    [MapIdx_MAP7_S03] = "map7_s03",
};

/* Stub map headers - one per overlay.
 * Metadata extracted from each map's original _header.c file.
 * Zero-initialized fields: all function pointers, spawn data, road data, etc.
 */
static s_MapOverlayHeader g_StubHeaders[] = {
    /* MapIdx_MAP0_S00 = 0 — fully compiled, not used as stub */
    [MapIdx_MAP0_S00] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 2, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP0_S01] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 3, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP0_S02] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 6, .ambientAudioIdx_15 = 11, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP1_S00] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 18, .ambientAudioIdx_15 = 5, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S01] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 18, .ambientAudioIdx_15 = 6, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S02] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 11, .ambientAudioIdx_15 = 7, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S03] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 11, .ambientAudioIdx_15 = 8, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S04] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 11, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S05] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 26, .ambientAudioIdx_15 = 9, .field_16 = 3, .field_17 = 0 },
    [MapIdx_MAP1_S06] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 8, .ambientAudioIdx_15 = 10, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP2_S00] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 6, .ambientAudioIdx_15 = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 27, .ambientAudioIdx_15 = 12, .field_16 = 1, .field_17 = 0 },
    [MapIdx_MAP2_S02] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 6, .ambientAudioIdx_15 = 13, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S03] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S04] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 38, .ambientAudioIdx_15 = 14, .field_16 = 1, .field_17 = 0 },

    [MapIdx_MAP3_S00] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 16, .ambientAudioIdx_15 = 15, .field_16 = 1, .field_17 = 1 },
    [MapIdx_MAP3_S01] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 16, .ambientAudioIdx_15 = 16, .field_16 = 1, .field_17 = 1 },
    [MapIdx_MAP3_S02] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 17, .field_16 = 1, .field_17 = 0 },
    [MapIdx_MAP3_S03] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 2, .ambientAudioIdx_15 = 18, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S04] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 2, .ambientAudioIdx_15 = 19, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S05] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 19, .ambientAudioIdx_15 = 20, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S06] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 21, .field_16 = 1, .field_17 = 1 },

    [MapIdx_MAP4_S00] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP4_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 24, .ambientAudioIdx_15 = 22, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S02] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 17, .ambientAudioIdx_15 = 23, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP4_S03] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 33, .ambientAudioIdx_15 = 24, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP4_S04] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 32, .ambientAudioIdx_15 = 25, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S05] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 13, .ambientAudioIdx_15 = 26, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S06] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 11, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP5_S00] = { .mapInfo = &MAP_INFOS[MapType_DR],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 28, .ambientAudioIdx_15 = 27, .field_16 = 2, .field_17 = 5 },
    [MapIdx_MAP5_S01] = { .mapInfo = &MAP_INFOS[MapType_RSR], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 7, .ambientAudioIdx_15 = 28, .field_16 = 2, .field_17 = 2 },
    [MapIdx_MAP5_S02] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 29, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP5_S03] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 7, .ambientAudioIdx_15 = 30, .field_16 = 2, .field_17 = 1 },

    [MapIdx_MAP6_S00] = { .mapInfo = &MAP_INFOS[MapType_RSU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 20, .ambientAudioIdx_15 = 31, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP6_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 32, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S02] = { .mapInfo = &MAP_INFOS[MapType_RSU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 21, .ambientAudioIdx_15 = 33, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S03] = { .mapInfo = &MAP_INFOS[MapType_DRU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 12, .ambientAudioIdx_15 = 34, .field_16 = 2, .field_17 = 5 },
    [MapIdx_MAP6_S04] = { .mapInfo = &MAP_INFOS[MapType_APU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 37, .ambientAudioIdx_15 = 35, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S05] = { .mapInfo = &MAP_INFOS[MapType_APU], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 3, .ambientAudioIdx_15 = 11, .field_16 = 2, .field_17 = 0 },

    [MapIdx_MAP7_S00] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 10, .ambientAudioIdx_15 = 36, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S01] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 37, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S02] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 1, .ambientAudioIdx_15 = 38, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S03] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs_18 = g_StubLoadScreenFuncs, .mapPointsOfInterest_1C = &g_StubMapPoint, .bgmIdx_14 = 35, .ambientAudioIdx_15 = 39, .field_16 = 3, .field_17 = 2 },
};

/* ========================================================================
 * Registry API
 * ======================================================================== */

void MapRegistry_Init(void)
{
    /* Default to map0_s00 (the fully compiled map). */
    g_pMapOverlayHeader = &g_MapOverlayHeader_map0_s00;

    /* If config specifies a different map, try to load it. */
    int id = MapRegistry_FindByName(g_PcConfig.mapName);
    if (id >= 0)
    {
        MapRegistry_Load((e_MapIdx)id);
    }
}

void MapRegistry_Load(e_MapIdx id)
{
    s_MapOverlayHeader* header;

    if (id < 0 || id > MapIdx_MAPX_S00)
    {
        fprintf(stderr, "[MapRegistry] Invalid overlay ID %d\n", id);
        return;
    }

    /* Try loading the map overlay DLL first. */
    header = MapOverlay_Load(id);
    if (header != NULL)
    {
        g_pMapOverlayHeader = header;
    }
    else if (id == MapIdx_MAP0_S00)
    {
        /* Fallback: map0_s00 is compiled into the main executable. */
        g_pMapOverlayHeader = &g_MapOverlayHeader_map0_s00;
    }
    else
    {
        /* Fallback: use stub header for maps without DLLs. */
        fprintf(stderr, "[MapRegistry] No DLL for %s, using stub header\n",
                MapRegistry_GetName(id));
        g_pMapOverlayHeader = &g_StubHeaders[id];
    }

    SH_DBG_ECHO("[MapRegistry] Active map: %s (overlay %d, mapType %d)",
        MapRegistry_GetName(id), id,
        (int)(g_pMapOverlayHeader->mapInfo - MAP_INFOS));
}

int MapRegistry_FindByName(const char* name)
{
    if (name == NULL) return -1;

    for (int i = 0; i <= MapIdx_MAP7_S03; i++)
    {
        if (MAP_NAMES[i] != NULL && strcmp(name, MAP_NAMES[i]) == 0)
        {
            return i;
        }
    }

    return -1;
}

const char* MapRegistry_GetName(e_MapIdx id)
{
    if (id >= 0 && id <= MapIdx_MAP7_S03 && MAP_NAMES[id] != NULL)
    {
        return MAP_NAMES[id];
    }
    return "unknown";
}
