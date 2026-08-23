#include "pc_plugins.h"
#include "pc_config.h"
#include "dll_loader.h"
#include "dll_security.h"
#include "sh_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLUGINS 32

typedef struct
{
    char      name[128];
    DllHandle handle;
    SH_Plugin_InitFunc          initFunc;
    SH_Plugin_NewGameFunc       newGameFunc;
    SH_Plugin_MapLoadFunc       mapLoadFunc;
    SH_Plugin_UpdateFunc        updateFunc;
    SH_Plugin_PlayerDamageFunc  playerDamageFunc;
    SH_Plugin_HideHealthFunc    hideHealthFunc;
    SH_Plugin_WeatherFunc       weatherFunc;
    SH_Plugin_ScreenFadeFunc    screenFadeFunc;
    SH_Plugin_NpcSpawnFunc      npcSpawnFunc;
    SH_Plugin_RadioVolumeFunc     radioVolumeFunc;
    SH_Plugin_RadioAttributesFunc radioAttributesFunc;
    SH_Plugin_LiveInventoryFunc   liveInventoryFunc;
} PluginEntry;

static PluginEntry s_plugins[MAX_PLUGINS];
static int         s_pluginCount = 0;
static int         s_initialized = 0;

void Pc_Plugins_Init(void)
{
    if (s_initialized) return;
    s_initialized = 1;
    s_pluginCount = 0;

    SH_LOG("[PLUGINS] Scanning for plugin DLLs in plugins/ ...");

    char pluginPaths[MAX_PLUGINS][260];
    int foundCount = DllLoader_ListPlugins(pluginPaths, MAX_PLUGINS);

    for (int i = 0; i < foundCount; i++)
    {
        const char* path = pluginPaths[i];

        /* Pre-execution security audit: inspect PE header and imports before LoadLibrary */
        char auditReason[256] = {0};
        DllSecurityResult secResult = DllSecurity_AuditPlugin(path, auditReason, sizeof(auditReason));
        if (secResult != DLL_SECURITY_OK)
        {
            SH_LOG("[SECURITY] ❌ REJECTED plugin '%s': %s", path, auditReason);
            continue;
        }
        SH_LOG("[SECURITY] ✔ VERIFIED plugin '%s': %s", path, auditReason);

        DllHandle handle = DllLoader_Open(path);
        if (handle)
        {
            PluginEntry* p = &s_plugins[s_pluginCount];
            strncpy(p->name, path, sizeof(p->name) - 1);
            p->handle            = handle;
            p->initFunc          = (SH_Plugin_InitFunc)DllLoader_GetSymbol(handle, "SH_Plugin_Init");
            p->newGameFunc       = (SH_Plugin_NewGameFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnNewGame");
            p->mapLoadFunc       = (SH_Plugin_MapLoadFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnMapLoad");
            p->updateFunc        = (SH_Plugin_UpdateFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnUpdate");
            p->playerDamageFunc  = (SH_Plugin_PlayerDamageFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnPlayerDamage");
            p->hideHealthFunc    = (SH_Plugin_HideHealthFunc)DllLoader_GetSymbol(handle, "SH_Plugin_ShouldHideHealth");
            p->weatherFunc       = (SH_Plugin_WeatherFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OverrideWeather");
            p->screenFadeFunc    = (SH_Plugin_ScreenFadeFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnScreenFadeDraw");
            p->npcSpawnFunc      = (SH_Plugin_NpcSpawnFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OverrideNpcSpawn");
            p->radioVolumeFunc   = (SH_Plugin_RadioVolumeFunc)DllLoader_GetSymbol(handle, "SH_Plugin_ModifyRadioVolume");
            p->radioAttributesFunc = (SH_Plugin_RadioAttributesFunc)DllLoader_GetSymbol(handle, "SH_Plugin_ModifyRadioAttributes");
            p->liveInventoryFunc = (SH_Plugin_LiveInventoryFunc)DllLoader_GetSymbol(handle, "SH_Plugin_IsLiveInventory");

            SH_LOG("[PLUGINS] Loaded plugin: %s", path);
            if (p->initFunc)
            {
                p->initFunc();
            }
            s_pluginCount++;
        }
        else
        {
            SH_LOG("[PLUGINS] Failed to load plugin %s (%s)", path, DllLoader_GetError());
        }
    }

    SH_LOG("[PLUGINS] Active plugins: %d", s_pluginCount);
}

void Pc_Plugins_Shutdown(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].handle)
        {
            DllLoader_Close(s_plugins[i].handle);
            s_plugins[i].handle = NULL;
        }
    }
    s_pluginCount = 0;
    s_initialized = 0;
}

void Pc_Plugins_OnNewGame(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].newGameFunc)
        {
            s_plugins[i].newGameFunc();
        }
    }
}

void Pc_Plugins_OnMapLoad(s32 mapIdx)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].mapLoadFunc)
        {
            s_plugins[i].mapLoadFunc(mapIdx);
        }
    }
}

void Pc_Plugins_OnUpdate(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].updateFunc)
        {
            s_plugins[i].updateFunc();
        }
    }
}

void Pc_Plugins_OnPlayerDamage(s32* damage)
{
    if (!damage) return;
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].playerDamageFunc)
        {
            s_plugins[i].playerDamageFunc(damage);
        }
    }
}

int Pc_Plugins_ShouldHideHealth(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].hideHealthFunc && s_plugins[i].hideHealthFunc())
        {
            return 1;
        }
    }
    return 0;
}

int Pc_Plugins_OverrideWeather(s32* ambient, s32* rain)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].weatherFunc && s_plugins[i].weatherFunc(ambient, rain))
        {
            return 1;
        }
    }
    return 0;
}

void Pc_Plugins_OnScreenFadeDraw(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].screenFadeFunc)
        {
            s_plugins[i].screenFadeFunc();
        }
    }
}

int Pc_Plugins_OverrideNpcSpawn(e_CharaId* charaId)
{
    if (!charaId) return 0;
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].npcSpawnFunc && s_plugins[i].npcSpawnFunc(charaId))
        {
            return 1;
        }
    }
    return 0;
}

void Pc_Plugins_ModifyRadioVolume(s32* volume)
{
    if (!volume) return;
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].radioVolumeFunc)
        {
            s_plugins[i].radioVolumeFunc(volume);
        }
    }
}

void Pc_Plugins_ModifyRadioAttributes(s32* volume, s32* pitch)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].radioAttributesFunc)
        {
            s_plugins[i].radioAttributesFunc(volume, pitch);
        }
        else if (s_plugins[i].radioVolumeFunc && volume)
        {
            s_plugins[i].radioVolumeFunc(volume);
        }
    }
}

int Pc_Plugins_IsLiveInventoryEnabled(void)
{
    if (!g_PcConfig.liveInventory)
        return 0;

    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].liveInventoryFunc && s_plugins[i].liveInventoryFunc())
        {
            return 1;
        }
    }
    return 0;
}

int Pc_Plugins_HasNightmarePlugin(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].liveInventoryFunc)
        {
            return 1;
        }
    }
    return 0;
}
