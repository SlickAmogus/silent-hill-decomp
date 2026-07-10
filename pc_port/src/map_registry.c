#include "map_registry.h"
#include "map_overlay_loader.h"
#include "pc_config.h"
#include "sh_log.h"
#include <string.h>
#include <stdio.h>

/* The global pointer that all game code uses via the macro:
 *   #define g_MapOverlayHdr (*g_pMapOverlayHeader)
 */
s_MapOverlayHdr* g_pMapOverlayHeader = NULL;

/* Currently loaded overlay area (set in MapRegistry_Load). Used by the chunk
 * renderer to special-case specific areas. */
e_MapIdx g_CurrentMapIdx = MapIdx_MAP0_S00;

/* The fully-compiled map0_s00 header (renamed via -DSH_MAP_NAME=map0_s00). */
extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

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

/* Human-readable map descriptions (kept in sync with config.cfg's comment list).
 * Used by the in-game map-cycle debug keys. */
static const char* const MAP_DESCRIPTIONS[] = {
    [MapIdx_MAP0_S00] = "Old Silent Hill - streets (start area)",
    [MapIdx_MAP0_S01] = "Old Silent Hill - streets (Cybil encounter)",
    [MapIdx_MAP0_S02] = "Old Silent Hill - streets (alley)",
    [MapIdx_MAP1_S00] = "Midwich Elementary - exterior",
    [MapIdx_MAP1_S01] = "Midwich Elementary - 1F",
    [MapIdx_MAP1_S02] = "Midwich Elementary - 2F",
    [MapIdx_MAP1_S03] = "Midwich Elementary - 3F",
    [MapIdx_MAP1_S04] = "Midwich Elementary - courtyard",
    [MapIdx_MAP1_S05] = "Midwich Elementary - otherworld",
    [MapIdx_MAP1_S06] = "Midwich Elementary - exterior (after)",
    [MapIdx_MAP2_S00] = "Old Silent Hill - streets (after school)",
    [MapIdx_MAP2_S01] = "Old Silent Hill - sewers",
    [MapIdx_MAP2_S02] = "Old Silent Hill - streets (shopping district)",
    [MapIdx_MAP2_S03] = "Old Silent Hill - streets (residential)",
    [MapIdx_MAP2_S04] = "Old Silent Hill - Balkan Church",
    [MapIdx_MAP3_S00] = "Alchemilla Hospital - 1F",
    [MapIdx_MAP3_S01] = "Alchemilla Hospital - 2F/3F",
    [MapIdx_MAP3_S02] = "Alchemilla Hospital - otherworld 1F",
    [MapIdx_MAP3_S03] = "Alchemilla Hospital - otherworld 2F",
    [MapIdx_MAP3_S04] = "Alchemilla Hospital - otherworld 3F",
    [MapIdx_MAP3_S05] = "Alchemilla Hospital - otherworld basement",
    [MapIdx_MAP3_S06] = "Alchemilla Hospital - 1F (Lisa)",
    [MapIdx_MAP4_S00] = "Central Silent Hill - streets",
    [MapIdx_MAP4_S01] = "Central Silent Hill - antique shop",
    [MapIdx_MAP4_S02] = "Central Silent Hill - otherworld streets",
    [MapIdx_MAP4_S03] = "Central Silent Hill - otherworld (sewers)",
    [MapIdx_MAP4_S04] = "Central Silent Hill - otherworld (apartment)",
    [MapIdx_MAP4_S05] = "Central Silent Hill - otherworld",
    [MapIdx_MAP4_S06] = "Central Silent Hill - streets (return)",
    [MapIdx_MAP5_S00] = "Nowhere - otherworld streets",
    [MapIdx_MAP5_S01] = "Resort area - streets",
    [MapIdx_MAP5_S02] = "Resort area - Annie's Bar",
    [MapIdx_MAP5_S03] = "Resort area - lighthouse",
    [MapIdx_MAP6_S00] = "Resort area - otherworld streets",
    [MapIdx_MAP6_S01] = "Resort area - resort building",
    [MapIdx_MAP6_S02] = "Resort area - otherworld",
    [MapIdx_MAP6_S03] = "Nowhere - void",
    [MapIdx_MAP6_S04] = "Nowhere - otherworld (Alessa)",
    [MapIdx_MAP6_S05] = "Nowhere - otherworld",
    [MapIdx_MAP7_S00] = "Nowhere - final",
    [MapIdx_MAP7_S01] = "Nowhere - final (2)",
    [MapIdx_MAP7_S02] = "Nowhere - final (3)",
    [MapIdx_MAP7_S03] = "Nowhere - ending",
};

/* Stub map headers - one per overlay.
 * Metadata extracted from each map's original _header.c file.
 * Zero-initialized fields: all function pointers, spawn data, road data, etc.
 */
static s_MapOverlayHdr g_StubHeaders[] = {
    /* MapIdx_MAP0_S00 = 0 — fully compiled, not used as stub */
    [MapIdx_MAP0_S00] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 2, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP0_S01] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 3, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP0_S02] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 6, .ambientAudioIdx = 11, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP1_S00] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 18, .ambientAudioIdx = 5, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S01] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 18, .ambientAudioIdx = 6, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S02] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 11, .ambientAudioIdx = 7, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S03] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 11, .ambientAudioIdx = 8, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S04] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 11, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP1_S05] = { .mapInfo = &MAP_INFOS[MapType_SU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 26, .ambientAudioIdx = 9, .field_16 = 3, .field_17 = 0 },
    [MapIdx_MAP1_S06] = { .mapInfo = &MAP_INFOS[MapType_SC],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 8, .ambientAudioIdx = 10, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP2_S00] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 6, .ambientAudioIdx = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 27, .ambientAudioIdx = 12, .field_16 = 1, .field_17 = 0 },
    [MapIdx_MAP2_S02] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 6, .ambientAudioIdx = 13, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S03] = { .mapInfo = &MAP_INFOS[MapType_THR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP2_S04] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 38, .ambientAudioIdx = 14, .field_16 = 1, .field_17 = 0 },

    [MapIdx_MAP3_S00] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 16, .ambientAudioIdx = 15, .field_16 = 1, .field_17 = 1 },
    [MapIdx_MAP3_S01] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 16, .ambientAudioIdx = 16, .field_16 = 1, .field_17 = 1 },
    [MapIdx_MAP3_S02] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 17, .field_16 = 1, .field_17 = 0 },
    [MapIdx_MAP3_S03] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 2, .ambientAudioIdx = 18, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S04] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 2, .ambientAudioIdx = 19, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S05] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 19, .ambientAudioIdx = 20, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP3_S06] = { .mapInfo = &MAP_INFOS[MapType_HP],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 21, .field_16 = 1, .field_17 = 1 },

    [MapIdx_MAP4_S00] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 11, .field_16 = 1, .field_17 = 2 },
    [MapIdx_MAP4_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 24, .ambientAudioIdx = 22, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S02] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 17, .ambientAudioIdx = 23, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP4_S03] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 33, .ambientAudioIdx = 24, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP4_S04] = { .mapInfo = &MAP_INFOS[MapType_HU],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 32, .ambientAudioIdx = 25, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S05] = { .mapInfo = &MAP_INFOS[MapType_SPU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 13, .ambientAudioIdx = 26, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP4_S06] = { .mapInfo = &MAP_INFOS[MapType_SPR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 11, .field_16 = 1, .field_17 = 2 },

    [MapIdx_MAP5_S00] = { .mapInfo = &MAP_INFOS[MapType_DR],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 28, .ambientAudioIdx = 27, .field_16 = 2, .field_17 = 5 },
    [MapIdx_MAP5_S01] = { .mapInfo = &MAP_INFOS[MapType_RSR], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 7, .ambientAudioIdx = 28, .field_16 = 2, .field_17 = 2 },
    [MapIdx_MAP5_S02] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 29, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP5_S03] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 7, .ambientAudioIdx = 30, .field_16 = 2, .field_17 = 1 },

    [MapIdx_MAP6_S00] = { .mapInfo = &MAP_INFOS[MapType_RSU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 20, .ambientAudioIdx = 31, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP6_S01] = { .mapInfo = &MAP_INFOS[MapType_ER],  .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 32, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S02] = { .mapInfo = &MAP_INFOS[MapType_RSU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 21, .ambientAudioIdx = 33, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S03] = { .mapInfo = &MAP_INFOS[MapType_DRU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 12, .ambientAudioIdx = 34, .field_16 = 2, .field_17 = 5 },
    [MapIdx_MAP6_S04] = { .mapInfo = &MAP_INFOS[MapType_APU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 37, .ambientAudioIdx = 35, .field_16 = 2, .field_17 = 0 },
    [MapIdx_MAP6_S05] = { .mapInfo = &MAP_INFOS[MapType_APU], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 3, .ambientAudioIdx = 11, .field_16 = 2, .field_17 = 0 },

    [MapIdx_MAP7_S00] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 10, .ambientAudioIdx = 36, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S01] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 37, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S02] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 1, .ambientAudioIdx = 38, .field_16 = 2, .field_17 = 6 },
    [MapIdx_MAP7_S03] = { .mapInfo = &MAP_INFOS[MapType_ER2], .loadingScreenFuncs = g_StubLoadScreenFuncs, .mapPoints = &g_StubMapPoint, .bgmIdx = 35, .ambientAudioIdx = 39, .field_16 = 3, .field_17 = 2 },
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
    s_MapOverlayHdr* header;

    if (id < 0 || id > MapIdx_MAPX_S00)
    {
        fprintf(stderr, "[MapRegistry] Invalid overlay ID %d\n", id);
        return;
    }

    g_CurrentMapIdx = id;

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

    /* SH_LOG (not SH_DBG_ECHO): a per-map-load diagnostic belongs in the debug
     * log/console, not the top-left system-message toast that shows while the
     * console is hidden. */
    SH_LOG("[MapRegistry] Active map: %s (overlay %d, mapType %d)",
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

const char* MapRegistry_GetDescription(e_MapIdx id)
{
    if (id >= 0 && id <= MapIdx_MAP7_S03 && MAP_DESCRIPTIONS[id] != NULL)
    {
        return MAP_DESCRIPTIONS[id];
    }
    return "";
}

int MapRegistry_Count(void)
{
    return (int)(sizeof(MAP_NAMES) / sizeof(MAP_NAMES[0]));
}
