/*
 * map_xbox.c - Active map-overlay pointer + real (read-only) map registry for
 * the Xbox port.
 *
 * PC's map_registry.c (which defines g_pMapOverlayHeader and switches it between
 * DLL-loaded maps) is excluded. The Xbox port static-links only map0_s00, so the
 * pointer is fixed to that map's header — emitted by map0_s00_header.c via
 * SH_MAP_OVERLAY_HEADER (the SH_PC_PORT pointer-indirection branch of map.h, which
 * is active because we define SH_PC_PORT).
 *
 * The MapRegistry_* API used to be blind link stubs (Count()=1, FindByName()=0,
 * Load()=no-op). That made every overlay request "succeed" while silently
 * keeping map0_s00's header/events/collision active — the direct cause of the
 * end-of-intro infinite transition loop (map0_s00's SysState_LoadOverlay event
 * for MAP0_S01 re-fires every frame because the events table never swaps).
 * Now the registry answers truthfully (names/count/find), MapRegistry_Load
 * logs every request, and MapXbox_OverlayIsLinked() lets the SH_XBOX_PORT
 * guards in events_main.c / game_sys_states.c refuse transitions to maps that
 * are not in the xbe instead of corrupting game state.
 */
#include <string.h>

#include "common.h"
#include "game.h"
#include "bodyprog/map/map.h"
#include "sh_log.h"

extern void SH_DebugLogFlush(void); /* sh_log_xbox.c (not declared in sh_log.h) */

extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

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

/* ---- Xbox guard helpers (called from SH_XBOX_PORT blocks in shared code) -- */

/* Only map0_s00 is statically linked into the xbe (Makefile.nxdk MAP_SRCS).
 * Keep in sync when more maps get linked. */
int MapXbox_OverlayIsLinked(int mapIdx)
{
    return mapIdx == 0; /* MapIdx_MAP0_S00 */
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
        SH_DBG("[MAP-GUARD] overlay %d (%s) NOT LINKED (only map0_s00 in xbe) — transition refused at %s (x%u)",
               mapIdx,
               (mapIdx >= 0 && mapIdx < MAP_XBOX_COUNT) ? MAP_XBOX_NAMES[mapIdx] : "??",
               where, s_count);
        SH_DebugLogFlush();
    }
}

/* ---- MapRegistry_* API (real signatures; replaces the old blind stubs) ---- */

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

int MapRegistry_IsExactCellArena(void)
{
    /* Correct while only map0_s00 (not an arena map) can be active. */
    return 0;
}

/* PROBE: every overlay switch request lands here (GameBoot_MapLoad calls it
 * under SH_PC_PORT). We cannot switch the header (only map0_s00 is linked),
 * but we now SAY so loudly instead of silently pretending success. */
void MapRegistry_Load(int id)
{
    SH_DBG("[MAP-LOAD] MapRegistry_Load(%d)=%s linked=%d (header stays map0_s00)",
           id, MapRegistry_GetName(id), MapXbox_OverlayIsLinked(id));
    SH_DebugLogFlush();
}
