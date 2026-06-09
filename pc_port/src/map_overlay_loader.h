/*
 * map_overlay_loader.h — Dynamic map overlay loading for PC port
 *
 * On PSX, map overlays are separate executables loaded at the same
 * memory address. On PC, each map is compiled as a shared library
 * (.dll on Windows, .so on Linux) exporting g_MapOverlayHeader_<name>.
 *
 * This allows:
 *  - All 43 maps to coexist without symbol conflicts
 *  - Hot-reloading for mod development
 *  - Custom map mods distributed as single DLL files
 */
#ifndef MAP_OVERLAY_LOADER_H
#define MAP_OVERLAY_LOADER_H

#include "game.h"
#include "bodyprog/bodyprog.h"

/* Load a map overlay DLL by overlay ID.
 * Returns pointer to the map's s_MapOverlayHdr, or NULL on failure.
 * The previous overlay is unloaded automatically. */
s_MapOverlayHdr* MapOverlay_Load(e_MapIdx id);

/* Unload the currently loaded map overlay DLL. */
void MapOverlay_Unload(void);

/* Get the name of the currently loaded overlay DLL, or NULL. */
const char* MapOverlay_GetLoadedName(void);

#endif /* MAP_OVERLAY_LOADER_H */
