/*
 * map_xbox.c - Active map-overlay pointer + real map registry for the Xbox
 * port, with ALL map overlays statically linked.
 *
 * PC's map_registry.c / map_overlay_loader.c (DLL-based) are excluded from the
 * Xbox build. Instead every map is statically linked into the xbe
 * (Makefile.nxdk "Additional statically-linked map overlays"): map0_s00
 * unprefixed (as always), every other map compiled like a PC map DLL and then
 * symbol-prefixed with llvm-objcopy (<name>_<sym>) so the 500+ cross-map
 * duplicate symbols (shared player/particle/chara code #included per map)
 * cannot collide. Each map's header symbol g_MapOverlayHeader_<name> is
 * therefore reachable as <name>_g_MapOverlayHeader_<name>.
 *
 * MapRegistry_Load(id) replicates the PC loader's switch semantics:
 *   1. look the header up in the static table (PC: DllLoader_GetSymbol),
 *   2. null raw 0x800XXXXX PSX addresses left in un-decompiled header slots
 *      (PC: the MapOverlay_Load sanitize pass — they are garbage pointers on
 *      x86 exactly as on x64),
 *   3. switch g_pMapOverlayHeader + remember the current map index (PC:
 *      g_CurrentMapIdx, feeds MapRegistry_IsExactCellArena).
 *
 * MapXbox_OverlayIsLinked() still gates the SH_XBOX_PORT transition guards in
 * events_main.c / game_sys_states.c / game_load.c for any map that is NOT in
 * the table (only mapx_s00, which has no sources in the decomp).
 */
#include <string.h>

#include "common.h"
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "sh_log.h"

/* Shared cutscene helper used by many map events. On PSX it lived in each
 * overlay; the native ports hoist ONE copy into the exe (main_pc.c does the
 * same include) and the maps' references resolve against it. */
#include "maps/shared/SysWork_StateStepIncrementAfterTime.h"

extern void SH_DebugLogFlush(void); /* sh_log_xbox.c (not declared in sh_log.h) */

/* map0_s00 is linked unprefixed (Makefile.nxdk MAP_SRCS). */
extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

/* All other maps are symbol-prefixed by the build (see Makefile.nxdk):
 * g_MapOverlayHeader_<name> -> <name>_g_MapOverlayHeader_<name>. */
extern s_MapOverlayHdr map0_s01_g_MapOverlayHeader_map0_s01;
extern s_MapOverlayHdr map0_s02_g_MapOverlayHeader_map0_s02;
extern s_MapOverlayHdr map1_s00_g_MapOverlayHeader_map1_s00;
extern s_MapOverlayHdr map1_s01_g_MapOverlayHeader_map1_s01;
extern s_MapOverlayHdr map1_s02_g_MapOverlayHeader_map1_s02;
extern s_MapOverlayHdr map1_s03_g_MapOverlayHeader_map1_s03;
extern s_MapOverlayHdr map1_s04_g_MapOverlayHeader_map1_s04;
extern s_MapOverlayHdr map1_s05_g_MapOverlayHeader_map1_s05;
extern s_MapOverlayHdr map1_s06_g_MapOverlayHeader_map1_s06;
extern s_MapOverlayHdr map2_s00_g_MapOverlayHeader_map2_s00;
extern s_MapOverlayHdr map2_s01_g_MapOverlayHeader_map2_s01;
extern s_MapOverlayHdr map2_s02_g_MapOverlayHeader_map2_s02;
extern s_MapOverlayHdr map2_s03_g_MapOverlayHeader_map2_s03;
extern s_MapOverlayHdr map2_s04_g_MapOverlayHeader_map2_s04;
extern s_MapOverlayHdr map3_s00_g_MapOverlayHeader_map3_s00;
extern s_MapOverlayHdr map3_s01_g_MapOverlayHeader_map3_s01;
extern s_MapOverlayHdr map3_s02_g_MapOverlayHeader_map3_s02;
extern s_MapOverlayHdr map3_s03_g_MapOverlayHeader_map3_s03;
extern s_MapOverlayHdr map3_s04_g_MapOverlayHeader_map3_s04;
extern s_MapOverlayHdr map3_s05_g_MapOverlayHeader_map3_s05;
extern s_MapOverlayHdr map3_s06_g_MapOverlayHeader_map3_s06;
extern s_MapOverlayHdr map4_s00_g_MapOverlayHeader_map4_s00;
extern s_MapOverlayHdr map4_s01_g_MapOverlayHeader_map4_s01;
extern s_MapOverlayHdr map4_s02_g_MapOverlayHeader_map4_s02;
extern s_MapOverlayHdr map4_s03_g_MapOverlayHeader_map4_s03;
extern s_MapOverlayHdr map4_s04_g_MapOverlayHeader_map4_s04;
extern s_MapOverlayHdr map4_s05_g_MapOverlayHeader_map4_s05;
extern s_MapOverlayHdr map4_s06_g_MapOverlayHeader_map4_s06;
extern s_MapOverlayHdr map5_s00_g_MapOverlayHeader_map5_s00;
extern s_MapOverlayHdr map5_s01_g_MapOverlayHeader_map5_s01;
extern s_MapOverlayHdr map5_s02_g_MapOverlayHeader_map5_s02;
extern s_MapOverlayHdr map5_s03_g_MapOverlayHeader_map5_s03;
extern s_MapOverlayHdr map6_s00_g_MapOverlayHeader_map6_s00;
extern s_MapOverlayHdr map6_s01_g_MapOverlayHeader_map6_s01;
extern s_MapOverlayHdr map6_s02_g_MapOverlayHeader_map6_s02;
extern s_MapOverlayHdr map6_s03_g_MapOverlayHeader_map6_s03;
extern s_MapOverlayHdr map6_s04_g_MapOverlayHeader_map6_s04;
extern s_MapOverlayHdr map6_s05_g_MapOverlayHeader_map6_s05;
extern s_MapOverlayHdr map7_s00_g_MapOverlayHeader_map7_s00;
extern s_MapOverlayHdr map7_s01_g_MapOverlayHeader_map7_s01;
extern s_MapOverlayHdr map7_s02_g_MapOverlayHeader_map7_s02;
extern s_MapOverlayHdr map7_s03_g_MapOverlayHeader_map7_s03;

s_MapOverlayHdr* g_pMapOverlayHeader = &g_MapOverlayHeader_map0_s00;

/* Same order as e_MapIdx / PC map_registry.c. */
#define MAP_XBOX_COUNT 44
static const char* const MAP_XBOX_NAMES[MAP_XBOX_COUNT] = {
    "map0_s00", "map0_s01", "map0_s02",
    "map1_s00", "map1_s01", "map1_s02", "map1_s03", "map1_s04", "map1_s05", "map1_s06",
    "map2_s00", "map2_s01", "map2_s02", "map2_s03", "map2_s04",
    "map3_s00", "map3_s01", "map3_s02", "map3_s03", "map3_s04", "map3_s05", "map3_s06",
    "map4_s00", "map4_s01", "map4_s02", "map4_s03", "map4_s04", "map4_s05", "map4_s06",
    "map5_s00", "map5_s01", "map5_s02", "map5_s03",
    "map6_s00", "map6_s01", "map6_s02", "map6_s03", "map6_s04", "map6_s05",
    "map7_s00", "map7_s01", "map7_s02", "map7_s03",
    "mapx_s00",
};

/* mapx_s00 (idx 43) has no sources in the decomp -> NULL, guard refuses it. */
static s_MapOverlayHdr* const MAP_XBOX_HEADERS[MAP_XBOX_COUNT] = {
    &g_MapOverlayHeader_map0_s00,
    &map0_s01_g_MapOverlayHeader_map0_s01,
    &map0_s02_g_MapOverlayHeader_map0_s02,
    &map1_s00_g_MapOverlayHeader_map1_s00,
    &map1_s01_g_MapOverlayHeader_map1_s01,
    &map1_s02_g_MapOverlayHeader_map1_s02,
    &map1_s03_g_MapOverlayHeader_map1_s03,
    &map1_s04_g_MapOverlayHeader_map1_s04,
    &map1_s05_g_MapOverlayHeader_map1_s05,
    &map1_s06_g_MapOverlayHeader_map1_s06,
    &map2_s00_g_MapOverlayHeader_map2_s00,
    &map2_s01_g_MapOverlayHeader_map2_s01,
    &map2_s02_g_MapOverlayHeader_map2_s02,
    &map2_s03_g_MapOverlayHeader_map2_s03,
    &map2_s04_g_MapOverlayHeader_map2_s04,
    &map3_s00_g_MapOverlayHeader_map3_s00,
    &map3_s01_g_MapOverlayHeader_map3_s01,
    &map3_s02_g_MapOverlayHeader_map3_s02,
    &map3_s03_g_MapOverlayHeader_map3_s03,
    &map3_s04_g_MapOverlayHeader_map3_s04,
    &map3_s05_g_MapOverlayHeader_map3_s05,
    &map3_s06_g_MapOverlayHeader_map3_s06,
    &map4_s00_g_MapOverlayHeader_map4_s00,
    &map4_s01_g_MapOverlayHeader_map4_s01,
    &map4_s02_g_MapOverlayHeader_map4_s02,
    &map4_s03_g_MapOverlayHeader_map4_s03,
    &map4_s04_g_MapOverlayHeader_map4_s04,
    &map4_s05_g_MapOverlayHeader_map4_s05,
    &map4_s06_g_MapOverlayHeader_map4_s06,
    &map5_s00_g_MapOverlayHeader_map5_s00,
    &map5_s01_g_MapOverlayHeader_map5_s01,
    &map5_s02_g_MapOverlayHeader_map5_s02,
    &map5_s03_g_MapOverlayHeader_map5_s03,
    &map6_s00_g_MapOverlayHeader_map6_s00,
    &map6_s01_g_MapOverlayHeader_map6_s01,
    &map6_s02_g_MapOverlayHeader_map6_s02,
    &map6_s03_g_MapOverlayHeader_map6_s03,
    &map6_s04_g_MapOverlayHeader_map6_s04,
    &map6_s05_g_MapOverlayHeader_map6_s05,
    &map7_s00_g_MapOverlayHeader_map7_s00,
    &map7_s01_g_MapOverlayHeader_map7_s01,
    &map7_s02_g_MapOverlayHeader_map7_s02,
    &map7_s03_g_MapOverlayHeader_map7_s03,
    NULL, /* mapx_s00 */
};

/* PC map_registry.c's g_CurrentMapIdx equivalent. */
static int s_currentMapIdx = 0; /* MapIdx_MAP0_S00 */

/* ---- Xbox guard helpers (called from SH_XBOX_PORT blocks in shared code) -- */

int MapXbox_OverlayIsLinked(int mapIdx)
{
    return mapIdx >= 0 && mapIdx < MAP_XBOX_COUNT && MAP_XBOX_HEADERS[mapIdx] != NULL;
}

/* Rate-limited logging for refused overlay transitions. The blocking event is
 * typically TriggerType_None and re-fires EVERY frame, so log occurrence #1
 * and then every 300th (~10s at 30fps), flushing so the evidence survives any
 * later crash. */
void MapXbox_LogUnlinkedOverlay(int mapIdx, const char* where)
{
    static int      s_lastIdx = -1;
    static unsigned s_count;

    if (mapIdx != s_lastIdx)
    {
        s_lastIdx = mapIdx;
        s_count   = 0;
    }

    if ((s_count++ % 300) == 0)
    {
        SH_DBG("[MAP-GUARD] overlay %d (%s) NOT LINKED — transition refused at %s (x%u)",
               mapIdx,
               (mapIdx >= 0 && mapIdx < MAP_XBOX_COUNT) ? MAP_XBOX_NAMES[mapIdx] : "??",
               where, s_count);
        SH_DebugLogFlush();
    }
}

/* ---- MapRegistry_* API (mirrors pc_port/src/map_registry.c) --------------- */

int MapRegistry_Count(void)
{
    return MAP_XBOX_COUNT;
}

int MapRegistry_FindByName(const char* name)
{
    int i;

    if (name == NULL)
        return -1;

    for (i = 0; i < MAP_XBOX_COUNT; i++)
    {
        if (strcmp(name, MAP_XBOX_NAMES[i]) == 0)
            return i;
    }

    return -1;
}

const char* MapRegistry_GetName(int id)
{
    if (id >= 0 && id < MAP_XBOX_COUNT)
        return MAP_XBOX_NAMES[id];

    return "unknown";
}

const char* MapRegistry_GetDescription(int id)
{
    (void)id;
    return "";
}

/* Single-cell interior boss arenas (PC map_registry.c): draw exact-cell for
 * map1_s05 (school otherworld boss) and map7_s03 (final boss arena). */
int MapRegistry_IsExactCellArena(void)
{
    return s_currentMapIdx == 8 ||  /* MapIdx_MAP1_S05 */
           s_currentMapIdx == 42;   /* MapIdx_MAP7_S03 */
}

/* PC MapOverlay_Load's sanitize pass: many map headers carry un-decompiled
 * function pointers as raw 0x800XXXXX PSX addresses — garbage on x86. NULL
 * them so callers take their NULL-check paths (same behavior as the PC DLL
 * loader). Idempotent: after the first pass they are 0. */
static void MapXbox_SanitizeHeader(int id, s_MapOverlayHdr* header)
{
    u32*     fields = (u32*)header;
    unsigned count  = sizeof(s_MapOverlayHdr) / sizeof(u32);
    unsigned nulled = 0;
    unsigned i;

    for (i = 0; i < count; i++)
    {
        if ((fields[i] & 0xFF000000) == 0x80000000)
        {
            fields[i] = 0;
            nulled++;
        }
    }

    if (nulled > 0)
    {
        SH_DBG("[MAP-LOAD] %s: nulled %u raw PSX addrs in overlay header",
               MapRegistry_GetName(id), nulled);
    }
}

/* Switch the active overlay to a statically-linked map. Every overlay request
 * (GameBoot_MapLoad / game_main.c map transitions) lands here. */
void MapRegistry_Load(int id)
{
    s_MapOverlayHdr* header;

    if (!MapXbox_OverlayIsLinked(id))
    {
        SH_DBG("[MAP-LOAD] MapRegistry_Load(%d)=%s linked=0 — REFUSED (header stays %s)",
               id, MapRegistry_GetName(id), MapRegistry_GetName(s_currentMapIdx));
        SH_DebugLogFlush();
        return;
    }

    header = MAP_XBOX_HEADERS[id];

    /* map0_s00 is the PC exe's built-in map and is not sanitized there. */
    if (id != 0)
        MapXbox_SanitizeHeader(id, header);

    s_currentMapIdx     = id;
    g_pMapOverlayHeader = header;

    SH_DBG("[MAP-LOAD] MapRegistry_Load(%d)=%s linked=1 header=%08x",
           id, MapRegistry_GetName(id), (unsigned)header);
    {
        extern void Xbox_MemReport(const char*);
        Xbox_MemReport("after map-load");   /* leak watch across map switches */
    }
    SH_DebugLogFlush();
}
