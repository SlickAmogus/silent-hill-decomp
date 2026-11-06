/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include <stdlib.h>
#ifdef SH_STATIC_MAPS
#include "map_static_registry.h"
#else
#include <SDL.h>
#endif

#ifdef _WIN32
#define DLL_EXT ".dll"
#elif defined(__APPLE__)
#define DLL_EXT ".dylib"
#else
#define DLL_EXT ".so"
#endif

/* Currently loaded overlay */
static DllHandle s_currentDll = NULL;
static char s_currentName[64] = { 0 };

/* The compiled-in map0_s00 (always available as fallback) */
extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

#ifndef SH_STATIC_MAPS
/* A map module that is on disk but will not load leaves MapRegistry_Load on
 * its stub header, which has no code behind it: the game then segfaults a few
 * frames into the new map with the real reason buried in the log (Linux report
 * 2026-11-03: EMFILE from the Discord socket leak, crash entering the cafe).
 * Say why and stop. A module that is simply absent still takes the stub path,
 * which builds without SH_BUILD_MAP_DLLS rely on. */
static void MapOverlay_FatalLoadFailure(const char* dllPath, const char* why)
{
    char msg[768];

    SH_LOG("[MapOverlay] FATAL: %s could not be loaded: %s", dllPath, why);
    if (g_ShDebugLog)
        fflush(g_ShDebugLog);
    snprintf(msg, sizeof(msg),
             "The map file\n  %s\ncould not be loaded:\n  %s\n\n"
             "The game cannot continue into this area. If the error says\n"
             "\"Too many open files\", update to the latest build. Otherwise\n"
             "the file may be damaged: reinstall or re-extract the game.",
             dllPath, why ? why : "(unknown error)");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Silent Hill - map failed to load", msg, NULL);
    exit(1);
}
#endif

s_MapOverlayHdr* MapOverlay_Load(e_MapIdx id)
{
    char dllPath[256];
    char symbolName[64];
    const char* mapName;
    s_MapOverlayHdr* header;

    mapName = MapRegistry_GetName(id);
    if (mapName == NULL || strcmp(mapName, "unknown") == 0)
    {
        SH_DBG("[MapOverlay] Unknown overlay ID %d", id);
        return NULL;
    }

    /* map0_s00 is compiled into the main executable — no DLL needed */
    if (id == MapIdx_MAP0_S00)
    {
        MapOverlay_Unload();
        snprintf(s_currentName, sizeof(s_currentName), "%s", mapName);
        SH_DBG("[MapOverlay] Using built-in %s", mapName);
        return &g_MapOverlayHeader_map0_s00;
    }

    /* Build symbol name: g_MapOverlayHeader_<mapname> */
    snprintf(symbolName, sizeof(symbolName), "g_MapOverlayHeader_%s", mapName);

    /* Drop the previous overlay. Harmless in the static build (no handle is
     * ever open); it still clears the current-name bookkeeping. */
    MapOverlay_Unload();

#ifdef SH_STATIC_MAPS
    /* Every overlay is linked in, its defined symbols renamed <map>_<sym> so
     * the 43 copies of the shared code do not collide. The generated registry
     * maps a name back to its header; there is no module to open or close. */
    snprintf(dllPath, sizeof(dllPath), "<static>");
    header = MapStatic_Find(mapName);
    if (!header)
    {
        SH_DBG("[MapOverlay] No static overlay linked for '%s'", mapName);
        return NULL;
    }
#else
    /* Build DLL path: maps/<mapname>.dll */
    snprintf(dllPath, sizeof(dllPath), "maps/%s%s", mapName, DLL_EXT);

    /* Load the DLL */
    s_currentDll = DllLoader_Open(dllPath);
    if (!s_currentDll)
    {
        char why[512];

        snprintf(why, sizeof(why), "%s", DllLoader_GetError());
        SH_DBG("[MapOverlay] Failed to load %s (%s)", dllPath, why);
        if (DllLoader_FileExists(dllPath))
            MapOverlay_FatalLoadFailure(dllPath, why);
        return NULL;
    }

    header = (s_MapOverlayHdr*)DllLoader_GetSymbol(s_currentDll, symbolName);
    if (!header)
    {
        char why[512];

        snprintf(why, sizeof(why), "symbol '%s' not found (%s)",
                 symbolName, DllLoader_GetError());
        SH_DBG("[MapOverlay] %s in %s", why, dllPath);
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
        MapOverlay_FatalLoadFailure(dllPath, why);
    }
#endif

    /* Sanitize raw PSX addresses in the header. Many map headers have
     * un-decompiled function pointers stored as raw 0x800XXXXX values.
     * These were valid on PSX but are garbage on PC-64bit. NULL them out. */
    {
        uintptr_t* fields = (uintptr_t*)header;
        size_t count = sizeof(s_MapOverlayHdr) / sizeof(uintptr_t);
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
    /* Overlay code is gone; drop console commands it registered so the
     * dispatcher can never call a freed handler (persistent ones kept). */
    { extern void Pc_ModConsole_ClearTransient(void); Pc_ModConsole_ClearTransient(); }
    s_currentName[0] = '\0';
}

const char* MapOverlay_GetLoadedName(void)
{
    return s_currentName[0] ? s_currentName : NULL;
}
