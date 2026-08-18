/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_RANDO_CONFIG_H
#define PC_RANDO_CONFIG_H

/* Randomizer tunables — their OWN store and file (gamedata/randomizer.cfg), kept
 * out of config.cfg so it stays clean. Defaults reproduce the original hardcoded
 * behaviour and are inert unless a run is live. The in-game settings panel and
 * the Lua scripting layer both drive these through the descriptor table below,
 * so a new tunable is added in exactly one place. */
typedef struct
{
    int spawnDensity;    /* % of good candidate spots to populate (100 = all) */
    int monsterMax;      /* per-area monster cap (engine hard limit is 32 NPCs) */
    int areasToBoss;     /* areas entered before the run ends at the boss */
    int entryLockSec;    /* seconds the door you came in through stays shut */
    int enemyHealthPct;  /* enemy HP scale %  (>100 = tougher) */
    int weaponDamagePct; /* player weapon-damage scale %  (>100 = stronger) */
    int extraAmmo;       /* bonus handgun rounds granted at run start */
} s_RandoConfig;

extern s_RandoConfig g_RandoConfig;

/* One tunable — enumerable by the panel, addressable by name from Lua. `value`
 * points straight at the matching g_RandoConfig field. */
typedef struct
{
    const char* key;    /* stable id: file key AND Lua name */
    const char* label;  /* panel display text */
    int*        value;  /* -> field in g_RandoConfig */
    int         min;
    int         max;
    int         step;   /* panel left/right increment */
    const char* suffix; /* "%", " s", "" — panel display only */
} s_RandoSetting;

int                   Pc_RandoConfig_Count(void);
const s_RandoSetting* Pc_RandoConfig_At(int i);
const s_RandoSetting* Pc_RandoConfig_ByKey(const char* key);

int  Pc_RandoConfig_Clamp(const s_RandoSetting* s, int v);
/* Nudge setting i by dir*step, clamped. Returns the new value. */
int  Pc_RandoConfig_Adjust(int i, int dir);
/* Restore every tunable to its shipped default (the panel's "Reset defaults"). */
void Pc_RandoConfig_ResetDefaults(void);

/* gamedata/randomizer.cfg. Load sets defaults then overlays the file if present;
 * Save (re)writes it — called only when a value actually changes, so an untouched
 * install never creates the file. */
void Pc_RandoConfig_Load(void);
void Pc_RandoConfig_Save(void);

#endif /* PC_RANDO_CONFIG_H */
