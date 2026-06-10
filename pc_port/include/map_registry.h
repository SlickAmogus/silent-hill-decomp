#ifndef MAP_REGISTRY_H
#define MAP_REGISTRY_H

#include "bodyprog/bodyprog.h"
#include "game.h"

/* Map registry for PC port.
 *
 * On PSX, map overlays are loaded from disc one at a time into a fixed
 * memory region. On PC, we compile map0_s00 fully and provide stub
 * headers for all other maps. The stub headers contain correct metadata
 * (map type, BGM, ambient, weather) so the geometry/chunk system works,
 * but event/AI function pointers are NULL.
 *
 * To add full support for a map, compile its source files with
 * -DSH_MAP_NAME=mapN_sXX and add it to the registry.
 */

/* Initialize the registry and set the default map. */
void MapRegistry_Init(void);

/* Load a map by overlay ID. Sets g_pMapOverlayHeader. */
void MapRegistry_Load(e_MapIdx id);

/* Look up overlay ID from a map name string (e.g. "map0_s00").
 * Returns -1 if not found. */
int MapRegistry_FindByName(const char* name);

/* Get the name string for an overlay ID. */
const char* MapRegistry_GetName(e_MapIdx id);

/* Get the human-readable description for an overlay ID (matches config.cfg). */
const char* MapRegistry_GetDescription(e_MapIdx id);

/* Total number of maps in the registry (for cycling). */
int MapRegistry_Count(void);

#endif /* MAP_REGISTRY_H */
