/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Randomizer settings store + gamedata/randomizer.cfg reader/writer. Owns the
 * single source of truth (g_RandoConfig) and the descriptor table that the
 * in-game panel (pc_rando_settings.c) and the Lua layer both enumerate. */

#include "pc_rando_config.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RANDO_CFG_PATH "gamedata/randomizer.cfg"

static const s_RandoConfig RANDO_DEFAULTS = {
    .spawnDensity    = 100,
    .monsterMax      = 30,
    .areasToBoss     = 10,
    .entryLockSec    = 10,
    .enemyHealthPct  = 100,
    .weaponDamagePct = 100,
    .extraAmmo       = 30,
};

s_RandoConfig g_RandoConfig = {
    .spawnDensity    = 100,
    .monsterMax      = 30,
    .areasToBoss     = 10,
    .entryLockSec    = 10,
    .enemyHealthPct  = 100,
    .weaponDamagePct = 100,
    .extraAmmo       = 30,
};

void Pc_RandoConfig_ResetDefaults(void)
{
    g_RandoConfig = RANDO_DEFAULTS;
}

/* Order here is the panel's row order. The count/density knobs top out at the
 * engine's limits (32 concurrent NPCs; 100% = every good spot); the scaling
 * knobs open up to 100x so "harder" is the natural direction to push. */
static const s_RandoSetting SETTINGS[] = {
    { "spawn_density",  "Spawn density",      &g_RandoConfig.spawnDensity,     0,   100,  5, "%"  },
    { "monster_max",    "Monster cap",        &g_RandoConfig.monsterMax,       1,    32,  1, ""   },
    { "enemy_health",   "Enemy health",       &g_RandoConfig.enemyHealthPct,  10, 10000, 10, "%"  },
    { "weapon_damage",  "Weapon damage",      &g_RandoConfig.weaponDamagePct, 10, 10000, 10, "%"  },
    { "extra_ammo",     "Bonus handgun ammo", &g_RandoConfig.extraAmmo,         0,   999, 10, ""   },
    { "areas_to_boss",  "Areas to boss",      &g_RandoConfig.areasToBoss,       1,    99,  1, ""   },
    { "entry_lock_sec", "Entry door lock",    &g_RandoConfig.entryLockSec,      0,   120,  5, " s" },
};
#define N_SETTINGS ((int)(sizeof(SETTINGS) / sizeof(SETTINGS[0])))

int Pc_RandoConfig_Count(void)
{
    return N_SETTINGS;
}

const s_RandoSetting* Pc_RandoConfig_At(int i)
{
    return (i >= 0 && i < N_SETTINGS) ? &SETTINGS[i] : NULL;
}

const s_RandoSetting* Pc_RandoConfig_ByKey(const char* key)
{
    int i;
    if (key == NULL)
        return NULL;
    for (i = 0; i < N_SETTINGS; i++)
        if (strcmp(SETTINGS[i].key, key) == 0)
            return &SETTINGS[i];
    return NULL;
}

int Pc_RandoConfig_Clamp(const s_RandoSetting* s, int v)
{
    if (s == NULL)
        return v;
    if (v < s->min)
        v = s->min;
    if (v > s->max)
        v = s->max;
    return v;
}

int Pc_RandoConfig_Adjust(int i, int dir)
{
    const s_RandoSetting* s = Pc_RandoConfig_At(i);
    if (s == NULL)
        return 0;
    *s->value = Pc_RandoConfig_Clamp(s, *s->value + dir * s->step);
    return *s->value;
}

static char* trim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

void Pc_RandoConfig_Load(void)
{
    FILE* f = fopen(RANDO_CFG_PATH, "rb");
    char  line[128];

    if (f == NULL)
        return; /* defaults stand; no file exists until a setting is changed */

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char* eq;
        char* key;
        const s_RandoSetting* s;

        if (line[0] == '#' || line[0] == ';')
            continue;
        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        key = trim(line);
        s   = Pc_RandoConfig_ByKey(key);
        if (s != NULL)
            *s->value = Pc_RandoConfig_Clamp(s, atoi(trim(eq + 1)));
    }
    fclose(f);
    SH_LOG("[RANDO] settings loaded from %s", RANDO_CFG_PATH);
}

void Pc_RandoConfig_Save(void)
{
    FILE* f = fopen(RANDO_CFG_PATH, "wb");
    int   i;

    if (f == NULL)
    {
        SH_LOG("[RANDO] could not write %s", RANDO_CFG_PATH);
        return;
    }
    fprintf(f, "# Silent Hill randomizer settings. Edited in-game (Map button during a\n");
    fprintf(f, "# run) or by hand. Delete this file to restore every default.\n");
    for (i = 0; i < N_SETTINGS; i++)
        fprintf(f, "%s = %d\n", SETTINGS[i].key, *SETTINGS[i].value);
    fclose(f);
    SH_LOG("[RANDO] settings saved to %s", RANDO_CFG_PATH);
}
