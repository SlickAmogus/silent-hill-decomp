/*
 * map_overlay_loader.c — Dynamic map overlay loading via DLLs
 *
 * Uses dll_loader.h for OS calls so we never mix <windows.h> with
 * PSX decomp headers (they define conflicting types).
 */
#include "map_overlay_loader.h"
#include "map_registry.h"
#include "dll_loader.h"
#include "sh_log.h"
#include <string.h>

#ifdef _WIN32
#define DLL_EXT ".dll"
#else
#define DLL_EXT ".so"
#endif

/* Currently loaded overlay */
static DllHandle s_currentDll = NULL;
static char s_currentName[64] = { 0 };

/* The compiled-in map0_s00 (always available as fallback) */
extern s_MapOverlayHeader g_MapOverlayHeader_map0_s00;

s_MapOverlayHeader* MapOverlay_Load(e_MapIdx id)
{
    char dllPath[256];
    char symbolName[64];
    const char* mapName;
    s_MapOverlayHeader* header;

    mapName = MapRegistry_GetName(id);
    if (mapName == NULL || strcmp(mapName, "unknown") == 0)
    {
        SH_DBG("[MapOverlay] Unknown overlay ID %d", id);
        return NULL;
    }

    /* map0_s00 is compiled into the main executable — no DLL needed */
    if (id == MapOverlayId_MAP0_S00)
    {
        MapOverlay_Unload();
        snprintf(s_currentName, sizeof(s_currentName), "%s", mapName);
        SH_DBG("[MapOverlay] Using built-in %s", mapName);
        return &g_MapOverlayHeader_map0_s00;
    }

    /* Build DLL path: maps/<mapname>.dll */
    snprintf(dllPath, sizeof(dllPath), "maps/%s%s", mapName, DLL_EXT);

    /* Build symbol name: g_MapOverlayHeader_<mapname> */
    snprintf(symbolName, sizeof(symbolName), "g_MapOverlayHeader_%s", mapName);

    /* Unload previous overlay */
    MapOverlay_Unload();

    /* Load the DLL */
    s_currentDll = DllLoader_Open(dllPath);
    if (!s_currentDll)
    {
        SH_DBG("[MapOverlay] Failed to load %s (%s)", dllPath, DllLoader_GetError());
        return NULL;
    }

    /* Find the header symbol */
    header = (s_MapOverlayHeader*)DllLoader_GetSymbol(s_currentDll, symbolName);
    if (!header)
    {
        SH_DBG("[MapOverlay] Symbol '%s' not found in %s (%s)",
                symbolName, dllPath, DllLoader_GetError());
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
        return NULL;
    }

    /* Sanitize raw PSX addresses in the header. Many map headers have
     * un-decompiled function pointers stored as raw 0x800XXXXX values.
     * These were valid on PSX but are garbage on PC-64bit. NULL them out. */
    {
        uintptr_t* fields = (uintptr_t*)header;
        size_t count = sizeof(s_MapOverlayHeader) / sizeof(uintptr_t);
        size_t nulled = 0;
        for (size_t i = 0; i < count; i++)
        {
            uintptr_t val = fields[i];
            /* Detect PSX address: fits in 32 bits and starts with 0x80 */
            if (val != 0 && val <= 0xFFFFFFFF && (val & 0xFF000000) == 0x80000000)
            {
                SH_DBG("[MapOverlay]   Nulling PSX addr 0x%08X at offset 0x%zX",
                        (unsigned)val, i * sizeof(uintptr_t));
                fields[i] = 0;
                nulled++;
            }
        }
        if (nulled > 0)
            SH_DBG("[MapOverlay]   Nulled %zu raw PSX addresses", nulled);
    }

    snprintf(s_currentName, sizeof(s_currentName), "%s", mapName);
    SH_DBG("[MapOverlay] Loaded %s from %s", symbolName, dllPath);
    return header;
}

void MapOverlay_Unload(void)
{
    if (s_currentDll)
    {
        SH_DBG("[MapOverlay] Unloading %s", s_currentName);
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
    }
    s_currentName[0] = '\0';
}

const char* MapOverlay_GetLoadedName(void)
{
    return s_currentName[0] ? s_currentName : NULL;
}
