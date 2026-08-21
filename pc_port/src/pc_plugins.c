#include "pc_plugins.h"
#include "dll_loader.h"
#include "dll_security.h"
#include "sh_log.h"
#include "pc_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLUGINS 32

typedef struct
{
    char                        name[128];
    DllHandle                   handle;
    SH_Plugin_InitFunc          initFunc;
    SH_Plugin_ShutdownFunc      shutdownFunc;
    SH_Plugin_NewGameFunc       newGameFunc;
    SH_Plugin_MapLoadFunc       mapLoadFunc;
    SH_Plugin_UpdateFunc        updateFunc;
    SH_Plugin_RenderFunc        renderFunc;
    SH_Plugin_GetNameFunc       getNameFunc;
    SH_Plugin_GetApiVersionFunc getApiVersionFunc;
} PluginEntry;

static PluginEntry s_plugins[MAX_PLUGINS];
static int         s_pluginCount = 0;
static int         s_initialized = 0;

void Pc_Plugins_Init(void)
{
    if (s_initialized) return;
    s_initialized = 1;
    s_pluginCount = 0;

    /* Opt-in: config `enable_plugins` defaults OFF. A DLL runs arbitrary code
     * on load; the user enables the surface deliberately, it never enables
     * itself because a file appeared in plugins/. */
    if (!g_PcConfig.enablePlugins)
    {
        return;
    }

    SH_LOG("[PLUGINS] Scanning for plugin DLLs in plugins/ ...");

    char pluginPaths[MAX_PLUGINS][260];
    int foundCount = DllLoader_ListPlugins(pluginPaths, MAX_PLUGINS);

    for (int i = 0; i < foundCount; i++)
    {
        const char* path = pluginPaths[i];

        /* Pre-execution static binary security audit */
        char auditReason[256] = {0};
        DllSecurityResult secResult = DllSecurity_AuditPlugin(path, 1, auditReason, sizeof(auditReason));
        if (secResult != DLL_SECURITY_OK)
        {
            SH_LOG("[PLUGINS] rejected '%s': %s", path, auditReason);
            continue;
        }
        SH_LOG("[PLUGINS] static check passed for '%s' (%s)", path, auditReason);

        DllHandle handle = DllLoader_Open(path);
        if (handle)
        {
            PluginEntry* p = &s_plugins[s_pluginCount];
            strncpy(p->name, path, sizeof(p->name) - 1);
            p->handle            = handle;
            p->initFunc          = (SH_Plugin_InitFunc)DllLoader_GetSymbol(handle, "SH_Plugin_Init");
            p->shutdownFunc      = (SH_Plugin_ShutdownFunc)DllLoader_GetSymbol(handle, "SH_Plugin_Shutdown");
            p->newGameFunc       = (SH_Plugin_NewGameFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnNewGame");
            p->mapLoadFunc       = (SH_Plugin_MapLoadFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnMapLoad");
            p->updateFunc        = (SH_Plugin_UpdateFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnUpdate");
            p->renderFunc        = (SH_Plugin_RenderFunc)DllLoader_GetSymbol(handle, "SH_Plugin_OnRender");
            p->getNameFunc       = (SH_Plugin_GetNameFunc)DllLoader_GetSymbol(handle, "SH_Plugin_GetName");
            p->getApiVersionFunc = (SH_Plugin_GetApiVersionFunc)DllLoader_GetSymbol(handle, "SH_Plugin_GetApiVersion");

            const char* displayName = p->getNameFunc ? p->getNameFunc() : path;
            SH_LOG("[PLUGINS] Loaded plugin: %s", displayName);

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
        if (s_plugins[i].shutdownFunc)
        {
            s_plugins[i].shutdownFunc();
        }
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

void Pc_Plugins_OnRender(void)
{
    for (int i = 0; i < s_pluginCount; i++)
    {
        if (s_plugins[i].renderFunc)
        {
            s_plugins[i].renderFunc();
        }
    }
}
