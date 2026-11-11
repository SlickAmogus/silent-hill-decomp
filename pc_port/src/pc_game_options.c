/* The original game's options block (g_GameWork.config) is written only as
 * part of a memory-card save and read back from the newest save at boot, so a
 * setting changed without saving afterwards was gone on the next launch. Each
 * player-facing field is mirrored into config.cfg here; a key present in the
 * file wins over the save's copy, and a key that was never written leaves the
 * save's value alone. Unlock flags and the tip history stay save-only. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "sh_log.h"
#include "pc_config.h"
#include "pc_game_options.h"

#define GOPT_UNSET (-9999)

typedef struct
{
    const char* key;
    size_t      offset;
    int         isSigned;
    int         min;
    int         max;
} s_GameOpt;

#define GOPT(key, field, sgn, mn, mx) { key, offsetof(s_OptionsConfig, field), sgn, mn, mx }

static const s_GameOpt s_opts[] = {
    GOPT("opt_bgm_volume",      volumeBgm,        0, 0, OPT_SOUND_VOLUME_MAX),
    GOPT("opt_se_volume",       volumeSe,         0, 0, OPT_SOUND_VOLUME_MAX),
    GOPT("opt_vibration",       vibrationEnabled, 0, 0, OPT_VIBRATION_ENABLED),
    GOPT("opt_brightness_level", brightness,      0, 0, 7),
    GOPT("opt_screen_pos_x",    screenPositionX,  1, -11, 11),
    GOPT("opt_screen_pos_y",    screenPositionY,  1, -8, 8),
    GOPT("opt_auto_load",       autoLoad,         1, 0, 1),
    GOPT("opt_weapon_control",  extraWeaponCtrl,  0, 0, 1),
    GOPT("opt_blood_color",     extraBloodColor,  0, 0, 11),
    GOPT("opt_view_control",    extraViewCtrl,    1, 0, 1),
    GOPT("opt_retreat_turn",    extraRetreatTurn, 1, 0, 1),
    GOPT("opt_walk_run_control", extraWalkRunCtrl, 1, 0, 1),
    GOPT("opt_auto_aiming",     extraAutoAiming,  1, 0, 1),
    GOPT("opt_view_mode",       extraViewMode,    1, 0, 1),
    GOPT("opt_bullet_adjust",   extraBulletAdjust, 1, 0, 5),
};

#define GOPT_COUNT ((int)(sizeof(s_opts) / sizeof(s_opts[0])))

static int s_cfg[GOPT_COUNT];
static int s_shadow[GOPT_COUNT];
static int s_init;

static void GOpt_Init(void)
{
    int i;

    if (s_init)
        return;
    s_init = 1;
    for (i = 0; i < GOPT_COUNT; i++)
    {
        s_cfg[i]    = GOPT_UNSET;
        s_shadow[i] = GOPT_UNSET;
    }
}

static int GOpt_Get(const s_GameOpt* o)
{
    const u8* p = (const u8*)&g_GameWork.config + o->offset;

    return o->isSigned ? (int)*(const s8*)p : (int)*p;
}

static void GOpt_Set(const s_GameOpt* o, int v)
{
    *((u8*)&g_GameWork.config + o->offset) = (u8)v;
}

/* Both rows only exist in the Extra menu once a clear has unlocked them, and
 * the multiplier is capped by how many clears the save holds, so a value in
 * the file must not hand out what the save has not earned. */
static int GOpt_Allowed(const s_GameOpt* o, int v)
{
    int flags = g_GameWork.config.extraOptionsEnabled;

    if (o->offset == offsetof(s_OptionsConfig, extraBloodColor))
        return v == 0 || v == 2 || v == 5 || v == 11;

    if (o->offset == offsetof(s_OptionsConfig, extraViewMode))
        return v == 0 || flags != 0;

    if (o->offset == offsetof(s_OptionsConfig, extraBulletAdjust))
    {
        int i, unlocked = 1;

        for (i = 0; i < 5; i++)
        {
            if (flags & (1 << i))
                unlocked++;
        }
        return v < unlocked;
    }

    return 1;
}

int Pc_GameOptions_ConfigKey(const char* key, const char* value)
{
    int i;

    GOpt_Init();
    if (key == NULL || value == NULL || strncmp(key, "opt_", 4) != 0)
        return 0;

    for (i = 0; i < GOPT_COUNT; i++)
    {
        if (strcmp(key, s_opts[i].key) == 0)
        {
            int v = atoi(value);

            if (v < s_opts[i].min) v = s_opts[i].min;
            if (v > s_opts[i].max) v = s_opts[i].max;
            s_cfg[i] = v;
            return 1;
        }
    }
    return 0;
}

void Pc_GameOptions_Apply(void)
{
    int i;

    GOpt_Init();
    for (i = 0; i < GOPT_COUNT; i++)
    {
        const s_GameOpt* o = &s_opts[i];

        if (s_cfg[i] != GOPT_UNSET && GOpt_Allowed(o, s_cfg[i]) && GOpt_Get(o) != s_cfg[i])
        {
            SH_DBG("[GOPT] %s: %d -> %d (config.cfg)", o->key, GOpt_Get(o), s_cfg[i]);
            GOpt_Set(o, s_cfg[i]);
        }
        s_shadow[i] = GOpt_Get(o);
    }

    {
        extern unsigned char g_PcTrustedBloodColor;
        g_PcTrustedBloodColor = g_GameWork.config.extraBloodColor;
    }
}

void Pc_GameOptions_Persist(void)
{
    static char vals[GOPT_COUNT][12];
    const char* keys[GOPT_COUNT];
    const char* values[GOPT_COUNT];
    int         n = 0;
    int         i;

    GOpt_Init();
    for (i = 0; i < GOPT_COUNT; i++)
    {
        int v = GOpt_Get(&s_opts[i]);

        if (v == s_shadow[i])
            continue;

        s_shadow[i] = v;
        s_cfg[i]    = v;
        snprintf(vals[n], sizeof(vals[n]), "%d", v);
        keys[n]   = s_opts[i].key;
        values[n] = vals[n];
        n++;
    }

    if (n > 0)
    {
        PcConfig_SaveKeyValues(keys, values, n);
        SH_DBG("[GOPT] saved %d original-game option(s) to config (first: %s = %s)", n, keys[0], values[0]);
    }
}
