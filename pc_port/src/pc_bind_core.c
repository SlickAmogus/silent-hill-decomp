/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_bind_core.c - bind table logic, no renderer and no SDL. See pc_bind_core.h.
 */

#include <stdio.h>
#include <string.h>

#include "pc_bind_core.h"
#include "pc_config.h"
#include "sh_log.h"

extern void Pc_ControlStyle_ReapplyBinds(void);
extern void Pc_QuickOptions_InvalidateRows(void);

/* Labels and order follow the launcher. The PSX-button rows resolve mouse
 * names in Pc_ApplyKeyOrMouse; the port's own actions resolve keyboard names
 * only (Swap Shoulder also takes a mouse button), so the mouse is offered only
 * where the game can use it. */
static const BcRow s_rows[] = {
    { "Move Up",        { "key_up",            "key_up_2",       NULL,                NULL },             BCF_STICK | BCF_MOUSE | BCF_WHEEL },
    { "Move Down",      { "key_down",          "key_down_2",     NULL,                NULL },             BCF_STICK | BCF_MOUSE | BCF_WHEEL },
    { "Turn Left",      { "key_left",          "key_left_2",     NULL,                NULL },             BCF_STICK | BCF_MOUSE | BCF_WHEEL },
    { "Turn Right",     { "key_right",         "key_right_2",    NULL,                NULL },             BCF_STICK | BCF_MOUSE | BCF_WHEEL },
    { "Action / Shoot", { "key_cross",         "key_cross_2",    "pad_cross",         "pad_cross_2" },    BCF_MOUSE | BCF_WHEEL },
    { "Flashlight",     { "key_circle",        "key_circle_2",   "pad_circle",        "pad_circle_2" },   BCF_MOUSE | BCF_WHEEL },
    { "Map",            { "key_triangle",      "key_triangle_2", "pad_triangle",      "pad_triangle_2" }, BCF_MOUSE | BCF_WHEEL },
    { "Run",            { "key_square",        "key_square_2",   "pad_square",        "pad_square_2" },   BCF_MOUSE | BCF_WHEEL },
    { "Sidestep Left",  { "key_l1",            "key_l1_2",       "pad_l1",            "pad_l1_2" },       BCF_MOUSE | BCF_WHEEL },
    { "Sidestep Right", { "key_r1",            "key_r1_2",       "pad_r1",            "pad_r1_2" },       BCF_MOUSE | BCF_WHEEL },
    { "View",           { "key_l2",            "key_l2_2",       "pad_l2",            "pad_l2_2" },       BCF_MOUSE | BCF_WHEEL },
    { "Aim",            { "key_r2",            "key_r2_2",       "pad_r2",            "pad_r2_2" },       BCF_MOUSE | BCF_WHEEL },
    { "Pause",          { "key_start",         "key_start_2",    "pad_start",         "pad_start_2" },    BCF_MOUSE | BCF_WHEEL },
    { "Inventory",      { "key_select",        "key_select_2",   "pad_select",        "pad_select_2" },   BCF_MOUSE | BCF_WHEEL },
    { "Reload",         { "key_reload",        "key_reload_2",   "pad_reload",        NULL },             0 },
    { "Cycle Weapons",  { "key_cycle_weapons", NULL,             "pad_cycle_weapons", NULL },             0 },
    { "Quick Heal",     { "key_quick_heal",    NULL,             "pad_quick_heal",    NULL },             0 },
    { "Quick Turn",     { "key_quick_turn",    NULL,             "pad_quick_turn",    NULL },             0 },
    { "Rear Look",      { "key_rear_look",     NULL,             "pad_rear_look",     NULL },             BCF_ALTCAM },
    { "Change Camera",  { "key_change_cam",    NULL,             "pad_change_cam",    NULL },             0 },
    { "Swap Shoulder",  { "key_swap_shoulder", NULL,             "pad_swap_shoulder", NULL },             BCF_GLOBAL | BCF_MOUSE },
    { "Quick Options",  { "key_quick_options", NULL,             "pad_quick_options", NULL },             BCF_GLOBAL },
    { "Quick Save",     { "key_quicksave",     NULL,             NULL,                NULL },             BCF_GLOBAL },
    { "Quick Load",     { "key_quickload",     NULL,             NULL,                NULL },             BCF_GLOBAL },
};

#define BC_ROWS ((int)(sizeof(s_rows) / sizeof(s_rows[0])))

static int s_scheme; /* 0 = classic, 1 = alternate cameras */

static int bc_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
    {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

int BindCore_RowCount(void)
{
    return BC_ROWS;
}

const BcRow* BindCore_Row(int row)
{
    return (row >= 0 && row < BC_ROWS) ? &s_rows[row] : NULL;
}

void BindCore_SetScheme(int scheme)
{
    s_scheme = scheme ? 1 : 0;
}

int BindCore_Scheme(void)
{
    return s_scheme;
}

int BindCore_Unbound(const char* v)
{
    return v == NULL || v[0] == '\0' || bc_ieq(v, "NONE");
}

const char* BindCore_Value(int row, int col)
{
    if (row < 0 || row >= BC_ROWS || col < 0 || col >= BC_COLS || s_rows[row].key[col] == NULL)
        return NULL;
    return PcConfig_BindField(s_rows[row].key[col], s_scheme, NULL);
}

int BindCore_RowEnabled(int row)
{
    if (row >= BC_ROWS)
        return 1;
    return !((s_rows[row].flags & BCF_ALTCAM) && s_scheme == 0);
}

int BindCore_CellUsable(int row, int col)
{
    return row >= 0 && row < BC_ROWS && col >= 0 && col < BC_COLS && s_rows[row].key[col] != NULL &&
           BindCore_RowEnabled(row);
}

static void bc_binds_changed(void)
{
    g_PcBindsGen++;
    Pc_ControlStyle_ReapplyBinds();
    /* The quick options footer names the Quick Options key. */
    Pc_QuickOptions_InvalidateRows();
}

static void bc_cfg_key(const char* key, int perScheme, char* out, size_t outSize)
{
    if (perScheme && s_scheme != 0)
        snprintf(out, outSize, "%s_altcam", key);
    else
        snprintf(out, outSize, "%s", key);
}

void BindCore_Set(int row, int col, const char* value)
{
    const char* key       = s_rows[row].key[col];
    int         perScheme = 0;
    char*       field     = PcConfig_BindField(key, s_scheme, &perScheme);
    char        cfgKey[64];

    if (field == NULL)
        return;
    snprintf(field, 24, "%s", value);
    bc_cfg_key(key, perScheme, cfgKey, sizeof(cfgKey));
    PcConfig_SaveKeyValue(cfgKey, field);
    SH_DBG("[BINDS] %s = %s", cfgKey, field);
    bc_binds_changed();
}

/* This scheme's binds back to the built-in values; the shared (global) rows
 * are left alone because they belong to both schemes. One file write. */
void BindCore_ResetScheme(void)
{
    static char keyBuf[BC_ROWS * BC_COLS][40];
    const char* keys[BC_ROWS * BC_COLS];
    const char* vals[BC_ROWS * BC_COLS];
    int         n = 0, r, c;

    for (r = 0; r < BC_ROWS; r++)
    {
        if (s_rows[r].flags & BCF_GLOBAL)
            continue;
        for (c = 0; c < BC_COLS; c++)
        {
            const char* key = s_rows[r].key[c];
            const char* def;
            char*       field;
            int         perScheme = 0;

            if (key == NULL)
                continue;
            field = PcConfig_BindField(key, s_scheme, &perScheme);
            if (field == NULL)
                continue;
            def = PcConfig_BindDefault(key, s_scheme);
            snprintf(field, 24, "%s", BindCore_Unbound(def) ? "NONE" : def);
            bc_cfg_key(key, perScheme, keyBuf[n], sizeof(keyBuf[n]));
            keys[n] = keyBuf[n];
            vals[n] = field;
            n++;
        }
    }
    PcConfig_SaveKeyValues(keys, vals, n);
    SH_DBG("[BINDS] %s scheme reset to defaults (%d binds)", s_scheme ? "altcam" : "classic", n);
    bc_binds_changed();
}

/* The same input on two actions of this scheme, same device. Allowed, as the
 * launcher allows it, but shown so it is never a surprise. */
int BindCore_IsDuplicate(int row, int col)
{
    const char* v = BindCore_Value(row, col);
    int         pad = (col >= BC_PAD1);
    int         r, c;

    if (BindCore_Unbound(v))
        return 0;
    for (r = 0; r < BC_ROWS; r++)
    {
        if (!BindCore_RowEnabled(r))
            continue;
        for (c = pad ? BC_PAD1 : BC_KB1; c <= (pad ? BC_PAD2 : BC_KB2); c++)
        {
            const char* o;
            if (r == row && c == col)
                continue;
            o = BindCore_Value(r, c);
            if (!BindCore_Unbound(o) && bc_ieq(o, v))
                return 1;
        }
    }
    return 0;
}

void BindCore_FormatRebindToast(int row, int col, char* out, size_t n)
{
    const BcRow* r = BindCore_Row(row);

    snprintf(out, n, "Changing %s input for %s", (col == BC_KB2 || col == BC_PAD2) ? "Secondary" : "Primary",
             r ? r->label : "");
}

void BindCore_DisplayKey(const char* v, char* out, size_t n)
{
    static const struct { const char* id; const char* name; } mouse[] = {
        { "Mouse1", "L-Mouse" },      { "Mouse2", "R-Mouse" }, { "Mouse3", "M-Mouse" },
        { "MouseWheelUp", "Wheel Up" }, { "MouseWheelDown", "Wheel Down" },
    };
    static const struct { const char* prefix; const char* repl; } prefixes[] = {
        { "Left ", "L-" }, { "Right ", "R-" }, { "Keypad ", "KP " },
    };
    size_t i;

    if (v == NULL)
        v = "";
    for (i = 0; i < sizeof(mouse) / sizeof(mouse[0]); i++)
    {
        if (bc_ieq(v, mouse[i].id))
        {
            snprintf(out, n, "%s", mouse[i].name);
            return;
        }
    }
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        size_t len = strlen(prefixes[i].prefix);

        if (strncmp(v, prefixes[i].prefix, len) == 0 && v[len] != '\0')
        {
            snprintf(out, n, "%s%s", prefixes[i].repl, v + len);
            return;
        }
    }
    snprintf(out, n, "%s", v);
}
