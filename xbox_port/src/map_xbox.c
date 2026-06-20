/*
 * map_xbox.c - Active map-overlay pointer for the Xbox port.
 *
 * PC's map_registry.c (which defines g_pMapOverlayHeader and switches it between
 * DLL-loaded maps) is excluded. The Xbox port static-links only map0_s00, so the
 * pointer is fixed to that map's header — emitted by map0_s00_header.c via
 * SH_MAP_OVERLAY_HEADER (the SH_PC_PORT pointer-indirection branch of map.h, which
 * is active because we define SH_PC_PORT).
 */
#include "common.h"
#include "game.h"
#include "bodyprog/map/map.h"

extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

s_MapOverlayHdr* g_pMapOverlayHeader = &g_MapOverlayHeader_map0_s00;
