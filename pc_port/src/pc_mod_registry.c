/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_mod_registry.c — extension points for mod map DLLs.
 *
 * A map DLL is edited game code with full engine access, so exposing a
 * registration surface adds no capability it lacks (it already runs every
 * frame). What it buys is a clean way for a mod to add console commands and
 * read its own config keys without patching the exe's hardcoded chains.
 *
 * Lifetime: a map DLL is unloaded on map change, so any handler it registered
 * would dangle. Commands are generation-tagged and cleared at the overlay
 * unload choke point (Pc_ModConsole_ClearTransient). chara_global.dll loads
 * once at boot and is never unloaded, so a command it registers with
 * persistent=1 survives the whole session.
 */
#include "pc_mod_registry.h"
#include "sh_log.h"
#include <string.h>

#define MOD_CMD_MAX 64

typedef struct
{
    char             name[32];
    char             help[80];
    Pc_ModCommandFn  fn;
    int              persistent; /* 1 = survives map unload (boot-lifetime DLLs) */
    int              active;
} ModCommand;

static ModCommand s_cmds[MOD_CMD_MAX];
static int        s_cmdCount = 0;

int Pc_ModConsole_Register(const char* name, Pc_ModCommandFn fn, const char* help, int persistent)
{
    int i;

    if (name == NULL || name[0] == '\0' || fn == NULL) return 0;

    /* Never let a mod shadow (or duplicate) an existing registration; built-in
     * commands are checked separately by the dispatcher, which tries them
     * first regardless. */
    for (i = 0; i < s_cmdCount; i++)
    {
        if (s_cmds[i].active && Pc_StrCaseEq(s_cmds[i].name, name))
        {
            SH_DBG("[MODCMD] '%s' already registered — ignored", name);
            return 0;
        }
    }
    if (s_cmdCount >= MOD_CMD_MAX)
    {
        SH_DBG("[MODCMD] registry full (%d) — '%s' ignored", MOD_CMD_MAX, name);
        return 0;
    }

    {
        ModCommand* c = &s_cmds[s_cmdCount++];
        memset(c, 0, sizeof(*c));
        strncpy(c->name, name, sizeof(c->name) - 1);
        if (help) strncpy(c->help, help, sizeof(c->help) - 1);
        c->fn         = fn;
        c->persistent = persistent ? 1 : 0;
        c->active     = 1;
    }
    SH_DBG("[MODCMD] registered '%s'%s", name, persistent ? " (persistent)" : "");
    return 1;
}

int Pc_ModConsole_Dispatch(const char* cmd, const char* arg)
{
    int i;
    for (i = 0; i < s_cmdCount; i++)
    {
        if (s_cmds[i].active && Pc_StrCaseEq(s_cmds[i].name, cmd))
        {
            s_cmds[i].fn(arg ? arg : "");
            return 1;
        }
    }
    return 0;
}

void Pc_ModConsole_ClearTransient(void)
{
    int i, w = 0;
    for (i = 0; i < s_cmdCount; i++)
    {
        if (s_cmds[i].active && s_cmds[i].persistent)
        {
            if (w != i) s_cmds[w] = s_cmds[i];
            w++;
        }
    }
    if (w != s_cmdCount)
    {
        SH_DBG("[MODCMD] cleared %d transient command(s) on map unload", s_cmdCount - w);
        s_cmdCount = w;
    }
}

int Pc_ModConsole_List(const char** outName, const char** outHelp, int idx)
{
    if (idx < 0 || idx >= s_cmdCount || !s_cmds[idx].active) return 0;
    if (outName) *outName = s_cmds[idx].name;
    if (outHelp) *outHelp = s_cmds[idx].help;
    return 1;
}

/* --- config side list: keys the exe's own parser did not recognize ------- */

#define MOD_CFG_MAX 128

typedef struct { char key[48]; char value[80]; } ModCfg;
static ModCfg s_cfg[MOD_CFG_MAX];
static int    s_cfgCount = 0;

void Pc_ModConfig_Store(const char* key, const char* value)
{
    int i;
    if (key == NULL || key[0] == '\0' || value == NULL) return;

    for (i = 0; i < s_cfgCount; i++)
    {
        if (Pc_StrCaseEq(s_cfg[i].key, key))
        {
            strncpy(s_cfg[i].value, value, sizeof(s_cfg[i].value) - 1);
            s_cfg[i].value[sizeof(s_cfg[i].value) - 1] = '\0';
            return;
        }
    }
    if (s_cfgCount >= MOD_CFG_MAX) return;
    strncpy(s_cfg[s_cfgCount].key, key, sizeof(s_cfg[s_cfgCount].key) - 1);
    strncpy(s_cfg[s_cfgCount].value, value, sizeof(s_cfg[s_cfgCount].value) - 1);
    s_cfgCount++;
}

const char* Pc_ModConfig_Value(const char* key)
{
    int i;
    if (key == NULL) return NULL;
    for (i = 0; i < s_cfgCount; i++)
    {
        if (Pc_StrCaseEq(s_cfg[i].key, key)) return s_cfg[i].value;
    }
    return NULL;
}

int Pc_StrCaseEq(const char* a, const char* b)
{
    if (a == NULL || b == NULL) return 0;
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
