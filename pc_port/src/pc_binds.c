/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Console command binds. See pc_binds.h for why this is its own system rather
 * than part of the control-scheme binds.
 *
 * One entry per command string, with a key and a controller input that fire it
 * (either may be empty). The command text is stored verbatim and replayed
 * through Pc_ConsoleExec on the press edge, splitting on ';'. Storing the text
 * rather than a parsed form is what lets the config line be the same thing the
 * user typed, which is the point: a bind set is shareable by copy and paste.
 */
#include "game.h"
#include "sh_log.h"

#include "pc_binds.h"
#include "pc_bind_input.h"
#include "pc_config.h"
#include "pc_mod_registry.h" /* Pc_Console_Print */

#include <SDL_scancode.h>
#include <SDL_keyboard.h>
#include <SDL_stdinc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void Pc_ConsoleExec(const char* line);
void PcConfig_SaveBindLines(const char* const* lines, int count);
extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);
extern int PsyX_RawControllerBindHeld(int buttonOrAxis);

#define PAD_PREFIX "pad:"
#define NO_PAD     (-1)

typedef struct
{
    char cmds[PC_BIND_CMDS_CAP]; /* command string as typed, ';' separated */
    char key[PC_BIND_KEY_CAP];   /* key name as typed, e.g. "K" or "KEYPAD 1"; "" none */
    char pad[PC_BIND_KEY_CAP];   /* controller input name, e.g. "dpup"; "" none */
    int  scancode;
    int  padCode;
    int  pinned;   /* a row of its own on the controls screen, bound or not (addcontrol) */
    int  builtin;  /* one of the rows the game lists itself */
    int  prevKey, prevPad;
} s_PcBind;

static s_PcBind s_binds[PC_BIND_MAX];
static int      s_bindCount;

/* Rendered config lines, handed to the config writer: up to two per entry. */
static char s_lineBuf[PC_BIND_MAX * 2][PC_BIND_KEY_CAP + PC_BIND_CMDS_CAP + 16];

/* Commands that are part of playing rather than of debugging: these run from a
 * bind whether or not the debug controls are allowed, and are listed on the
 * controls screen from the start, unbound. */
static const struct
{
    const char* cmd;
    const char* label;
} s_builtins[] = {
    { "NEXTGUN", "Next Gun" },
    { "NEXTMELEE", "Next Melee Weapon" },
};
#define BUILTIN_COUNT ((int)(sizeof(s_builtins) / sizeof(s_builtins[0])))

static void bprintf(const char* fmt, ...)
{
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Pc_Console_Print(line);
}

static void Usage(void)
{
    Pc_Console_Print("bind <key> <command>[;<command>...]  bind console commands to a key");
    Pc_Console_Print("  bind k kill");
    Pc_Console_Print("  bind f give shotgun;give shotgunammo;spawn groaner");
    Pc_Console_Print("  bind pad:dpup nextgun      a controller input (a b x y dpup dpdown dpleft dpright");
    Pc_Console_Print("                             leftshoulder rightshoulder lefttrigger righttrigger ...)");
    Pc_Console_Print("bind list        show every bind");
    Pc_Console_Print("unbind <key>     clear one (unbind pad:dpup for a controller input)");
    Pc_Console_Print("unbindall        clear them all");
    Pc_Console_Print("addcontrol <command>     list a command on the controls screen, to bind there");
    Pc_Console_Print("removecontrol <command>  take it off again");
    Pc_Console_Print("Separate from the control binds: a key already used by the");
    Pc_Console_Print("game can carry one, and editing controls never touches these.");
}

/* A bind may not contain bind/unbind/unbindall. Without this a bind could
 * rewrite the bind table as a side effect of being pressed, which makes a
 * shared config able to silently rebind the keys around it. */
static int IsBindWord(const char* cmd)
{
    return SDL_strncasecmp(cmd, "BIND", 4) == 0 ||
           SDL_strncasecmp(cmd, "UNBIND", 6) == 0 ||
           SDL_strncasecmp(cmd, "ADDCONTROL", 10) == 0 ||
           SDL_strncasecmp(cmd, "REMOVECONTROL", 13) == 0;
}

static void Trim(char* s)
{
    char*  p = s;
    size_t n;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
    {
        s[--n] = '\0';
    }
}

/* Keys that would take the console or the game away from the user. Everything
 * else is allowed on purpose, including keys the game already uses. */
static const char* KeyRefusal(const char* name, SDL_Scancode sc)
{
    SDL_Scancode consoleSc = SDL_GetScancodeFromName(g_PcConfig.keyConsole[0]
                                                     ? g_PcConfig.keyConsole : "`");

    if (sc == SDL_SCANCODE_UNKNOWN)
        return "unknown key";
    if (consoleSc != SDL_SCANCODE_UNKNOWN && sc == consoleSc)
        return "that is the console key";
    if (sc == SDL_SCANCODE_GRAVE)
        return "that is the console key";
    if (sc == SDL_SCANCODE_ESCAPE)
        return "escape is reserved";
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER ||
        sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_TAB)
    {
        return "that key is part of the console";
    }
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ||
        sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT ||
        sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT ||
        sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI)
    {
        return "modifier keys cannot be bound";
    }

    (void)name;
    return NULL;
}

/* The rows the game lists itself exist from the start, unbound. */
static void Ensure(void)
{
    static int s_done = 0;
    int        b, i;

    if (s_done)
        return;
    s_done = 1;
    for (b = 0; b < BUILTIN_COUNT && s_bindCount < PC_BIND_MAX; b++)
    {
        for (i = 0; i < s_bindCount; i++)
        {
            if (SDL_strcasecmp(s_binds[i].cmds, s_builtins[b].cmd) == 0)
                break;
        }
        if (i == s_bindCount)
        {
            memset(&s_binds[i], 0, sizeof(s_binds[i]));
            snprintf(s_binds[i].cmds, sizeof(s_binds[i].cmds), "%s", s_builtins[b].cmd);
            s_binds[i].padCode = NO_PAD;
            s_bindCount++;
        }
        s_binds[i].builtin = 1;
    }
}

static int FindByCmds(const char* cmds)
{
    int i;
    for (i = 0; i < s_bindCount; i++)
    {
        if (SDL_strcasecmp(s_binds[i].cmds, cmds) == 0)
            return i;
    }
    return -1;
}

static int Kept(const s_PcBind* b)
{
    return b->builtin || b->pinned || b->key[0] != '\0' || b->pad[0] != '\0';
}

/* Entries left with nothing bound and no row of their own go. */
static void Prune(void)
{
    int i, n = 0;

    for (i = 0; i < s_bindCount; i++)
    {
        if (Kept(&s_binds[i]))
        {
            if (n != i)
                s_binds[n] = s_binds[i];
            n++;
        }
    }
    s_bindCount = n;
}

static int RenderLines(const char** lines)
{
    int i, n = 0;

    for (i = 0; i < s_bindCount; i++)
    {
        const s_PcBind* b = &s_binds[i];

        if (b->key[0] != '\0')
        {
            snprintf(s_lineBuf[n], sizeof(s_lineBuf[n]), "bind %s %s", b->key, b->cmds);
            lines[n] = s_lineBuf[n];
            n++;
        }
        if (b->pad[0] != '\0')
        {
            snprintf(s_lineBuf[n], sizeof(s_lineBuf[n]), "bind " PAD_PREFIX "%s %s", b->pad, b->cmds);
            lines[n] = s_lineBuf[n];
            n++;
        }
        /* A row added by hand that has nothing bound still has to come back. */
        if (b->pinned && !b->builtin && b->key[0] == '\0' && b->pad[0] == '\0')
        {
            snprintf(s_lineBuf[n], sizeof(s_lineBuf[n]), "bind none %s", b->cmds);
            lines[n] = s_lineBuf[n];
            n++;
        }
    }
    return n;
}

static void Save(void)
{
    const char* lines[PC_BIND_MAX * 2];

    PcConfig_SaveBindLines(lines, RenderLines(lines));
}

/* Cleans a command string in place. Returns 0 when it is empty or holds a
 * bind command. */
static int CleanCmds(char* body, int quiet)
{
    size_t bl;

    Trim(body);
    /* Tolerate a quoted command string. The syntax does not need quotes --
     * ';' separates commands -- but wrapping the list in them is the habit
     * from other consoles, and silently binding a command that begins with a
     * quote is worse than just accepting it. */
    bl = strlen(body);
    if (bl >= 2 && ((body[0] == '"' && body[bl - 1] == '"') ||
                    (body[0] == '\'' && body[bl - 1] == '\'')))
    {
        memmove(body, body + 1, bl - 2);
        body[bl - 2] = 0;
        Trim(body);
    }
    if (body[0] == '\0')
        return 0;

    {
        const char* p = body;
        while (*p != '\0')
        {
            char        one[PC_BIND_CMDS_CAP];
            const char* semi = strchr(p, ';');
            size_t      n    = (semi != NULL) ? (size_t)(semi - p) : strlen(p);

            if (n >= sizeof(one))
                n = sizeof(one) - 1;
            memcpy(one, p, n);
            one[n] = '\0';
            Trim(one);

            if (IsBindWord(one))
            {
                if (!quiet)
                    bprintf("bind: a bind cannot contain bind/unbind");
                return 0;
            }

            if (semi == NULL)
                break;
            p = semi + 1;
        }
    }
    return 1;
}

/* The entry for a command string, made if there is room. */
static int EntryFor(const char* cmds, int quiet)
{
    int slot = FindByCmds(cmds);

    if (slot >= 0)
        return slot;
    if (s_bindCount >= PC_BIND_MAX)
    {
        if (!quiet)
            bprintf("bind: full (%d commands)", PC_BIND_MAX);
        return -1;
    }
    slot = s_bindCount++;
    memset(&s_binds[slot], 0, sizeof(s_binds[slot]));
    snprintf(s_binds[slot].cmds, sizeof(s_binds[slot].cmds), "%s", cmds);
    s_binds[slot].padCode = NO_PAD;
    return slot;
}

/* Shared by the console command, the controls screen and the config loader.
 * keyName is a key label, "pad:<input>" for a controller input, or "none" for
 * a row with nothing bound. Returns 0 and prints nothing on failure when
 * `quiet` is set (config load), so a hand-edited file cannot spam the console
 * at boot. */
static int SetBind(const char* keyName, const char* cmds, int quiet)
{
    char key[PC_BIND_KEY_CAP];
    char body[PC_BIND_CMDS_CAP];
    int  slot, i;

    Ensure();
    if (keyName == NULL || keyName[0] == '\0' || cmds == NULL || cmds[0] == '\0')
        return 0;

    snprintf(key, sizeof(key), "%s", keyName);
    snprintf(body, sizeof(body), "%s", cmds);
    Trim(key);
    if (key[0] == '\0' || !CleanCmds(body, quiet))
        return 0;

    if (SDL_strcasecmp(key, "none") == 0)
    {
        slot = EntryFor(body, quiet);
        if (slot < 0)
            return 0;
        s_binds[slot].pinned = 1;
        return 1;
    }

    if (SDL_strncasecmp(key, PAD_PREFIX, strlen(PAD_PREFIX)) == 0)
    {
        const char* name = key + strlen(PAD_PREFIX);
        int         code = PsyX_LookupGameControllerMapping(name, NO_PAD);

        if (name[0] == '\0' || code == NO_PAD)
        {
            if (!quiet)
                bprintf("bind: unknown controller input (%s)", name);
            return 0;
        }
        /* One command per input: it leaves whatever had it. */
        for (i = 0; i < s_bindCount; i++)
        {
            if (s_binds[i].pad[0] != '\0' && s_binds[i].padCode == code)
                s_binds[i].pad[0] = '\0';
        }
        slot = EntryFor(body, quiet);
        if (slot < 0)
        {
            Prune();
            return 0;
        }
        snprintf(s_binds[slot].pad, sizeof(s_binds[slot].pad), "%s", name);
        s_binds[slot].padCode = code;
        /* An input held when it was bound must not fire on the next frame. */
        s_binds[slot].prevPad = 1;
        Prune();
        return 1;
    }

    {
        /* The key is named the way it is labelled, as the user typed it, so the
         * same bind line means the same key cap on any layout. */
        SDL_Scancode sc  = (SDL_Scancode)BindInput_ScancodeFromLabel(key);
        const char*  why = KeyRefusal(key, sc);

        if (why != NULL)
        {
            if (!quiet)
                bprintf("bind: %s (%s)", why, key);
            return 0;
        }
        for (i = 0; i < s_bindCount; i++)
        {
            if (s_binds[i].key[0] != '\0' && s_binds[i].scancode == (int)sc)
                s_binds[i].key[0] = '\0';
        }
        slot = EntryFor(body, quiet);
        if (slot < 0)
        {
            Prune();
            return 0;
        }
        snprintf(s_binds[slot].key, sizeof(s_binds[slot].key), "%s", key);
        s_binds[slot].scancode = (int)sc;
        s_binds[slot].prevKey  = 1;
        Prune();
    }
    return 1;
}

void PcBinds_CmdBind(const char* arg)
{
    char        key[PC_BIND_KEY_CAP];
    const char* cmds;
    size_t      n;

    Ensure();
    if (arg == NULL || arg[0] == '\0')
    {
        Usage();
        return;
    }

    if (SDL_strcasecmp(arg, "LIST") == 0)
    {
        const char* lines[PC_BIND_MAX * 2];
        int         count = RenderLines(lines), i;

        if (count == 0)
        {
            Pc_Console_Print("no binds set (type 'bind' for usage)");
            return;
        }
        for (i = 0; i < count; i++)
            Pc_Console_Print(lines[i]);
        {
            extern int g_PcAllowDebugControls;
            if (!g_PcAllowDebugControls)
                Pc_Console_Print("(only nextgun / nextmelee run: allow_debug_controls is off)");
        }
        return;
    }

    /* First word is the key, the rest is the command string. */
    cmds = strchr(arg, ' ');
    if (cmds == NULL)
    {
        bprintf("bind: no commands given for %s", arg);
        Usage();
        return;
    }

    n = (size_t)(cmds - arg);
    if (n >= sizeof(key))
        n = sizeof(key) - 1;
    memcpy(key, arg, n);
    key[n] = '\0';
    while (*cmds == ' ')
        cmds++;

    if (!SetBind(key, cmds, 0))
        return;

    Save();
    bprintf("bound %s to %s", key, cmds);
}

void PcBinds_CmdUnbind(const char* arg)
{
    int i, hit = -1, isPad;

    Ensure();
    if (arg == NULL || arg[0] == '\0')
    {
        Pc_Console_Print("unbind <key>   (unbindall clears every bind)");
        return;
    }

    isPad = SDL_strncasecmp(arg, PAD_PREFIX, strlen(PAD_PREFIX)) == 0;
    if (isPad)
    {
        int code = PsyX_LookupGameControllerMapping(arg + strlen(PAD_PREFIX), NO_PAD);

        for (i = 0; i < s_bindCount && code != NO_PAD; i++)
        {
            if (s_binds[i].pad[0] != '\0' && s_binds[i].padCode == code)
                hit = i;
        }
    }
    else
    {
        SDL_Scancode sc = (SDL_Scancode)BindInput_ScancodeFromLabel(arg);

        for (i = 0; i < s_bindCount && sc != SDL_SCANCODE_UNKNOWN; i++)
        {
            if (s_binds[i].key[0] != '\0' && s_binds[i].scancode == (int)sc)
                hit = i;
        }
    }
    if (hit < 0)
    {
        bprintf("unbind: nothing bound to %s", arg);
        return;
    }

    bprintf("unbound %s (was %s)", arg, s_binds[hit].cmds);
    if (isPad)
        s_binds[hit].pad[0] = '\0';
    else
        s_binds[hit].key[0] = '\0';
    Prune();
    Save();
}

void PcBinds_CmdUnbindAll(void)
{
    int i, n = 0;

    Ensure();
    for (i = 0; i < s_bindCount; i++)
    {
        n += (s_binds[i].key[0] != '\0') + (s_binds[i].pad[0] != '\0');
        s_binds[i].key[0] = s_binds[i].pad[0] = '\0';
    }
    Prune();
    Save();
    bprintf("cleared %d bind%s", n, (n == 1) ? "" : "s");
}

void PcBinds_CmdAddControl(const char* arg)
{
    Ensure();
    if (arg == NULL || arg[0] == '\0')
    {
        Pc_Console_Print("addcontrol <command>[;<command>...]  list it on the controls screen");
        Pc_Console_Print("  addcontrol give handgunammo    then bind a key or button to it there");
        return;
    }
    if (!SetBind("none", arg, 0))
        return;
    Save();
    bprintf("added to the controls screen: %s", arg);
}

void PcBinds_CmdRemoveControl(const char* arg)
{
    char body[PC_BIND_CMDS_CAP];
    int  slot;

    Ensure();
    snprintf(body, sizeof(body), "%s", arg ? arg : "");
    if (!CleanCmds(body, 1) || (slot = FindByCmds(body)) < 0)
    {
        Pc_Console_Print("removecontrol <command>  (as shown by 'bind list' or on the controls screen)");
        return;
    }
    if (s_binds[slot].builtin)
    {
        /* The game's own rows stay; this only clears what is bound to one. */
        s_binds[slot].key[0] = s_binds[slot].pad[0] = '\0';
        bprintf("%s is a built-in row: its binds were cleared", s_binds[slot].cmds);
    }
    else
    {
        s_binds[slot].pinned = 0;
        s_binds[slot].key[0] = s_binds[slot].pad[0] = '\0';
        bprintf("removed from the controls screen: %s", body);
        Prune();
    }
    Save();
}

void PcBinds_ParseConfigLine(const char* rest)
{
    char        key[PC_BIND_KEY_CAP];
    const char* cmds;
    size_t      n;

    if (rest == NULL)
        return;

    while (*rest == ' ' || *rest == '\t')
        rest++;

    cmds = strchr(rest, ' ');
    if (cmds == NULL)
        return;

    n = (size_t)(cmds - rest);
    if (n >= sizeof(key))
        n = sizeof(key) - 1;
    memcpy(key, rest, n);
    key[n] = '\0';
    while (*cmds == ' ')
        cmds++;

    if (!SetBind(key, cmds, 1))
        SH_DBG("[BIND] config line rejected: bind %s %s", key, cmds);
}

/* Every command in the string is one of the game's own rows. */
static int PlayCommands(const char* cmds)
{
    const char* p = cmds;

    while (*p != '\0')
    {
        char        one[PC_BIND_CMDS_CAP];
        const char* semi = strchr(p, ';');
        size_t      n    = (semi != NULL) ? (size_t)(semi - p) : strlen(p);
        int         b, ok = 0;

        if (n >= sizeof(one))
            n = sizeof(one) - 1;
        memcpy(one, p, n);
        one[n] = '\0';
        Trim(one);
        for (b = 0; b < BUILTIN_COUNT; b++)
        {
            if (SDL_strcasecmp(one, s_builtins[b].cmd) == 0)
                ok = 1;
        }
        if (one[0] != '\0' && !ok)
            return 0;
        if (semi == NULL)
            break;
        p = semi + 1;
    }
    return 1;
}

static void Run(const s_PcBind* b)
{
    char        all[PC_BIND_CMDS_CAP];
    const char* p = all;

    /* A copy: a command may change the table this came from. */
    snprintf(all, sizeof(all), "%s", b->cmds);
    while (*p != '\0')
    {
        char        one[PC_BIND_CMDS_CAP];
        const char* semi = strchr(p, ';');
        size_t      n    = (semi != NULL) ? (size_t)(semi - p) : strlen(p);

        if (n >= sizeof(one))
            n = sizeof(one) - 1;
        memcpy(one, p, n);
        one[n] = '\0';
        Trim(one);

        if (one[0] != '\0')
            Pc_ConsoleExec(one);

        if (semi == NULL)
            break;
        p = semi + 1;
    }
}

void PcBinds_Update(const unsigned char* keyState)
{
    extern int g_PcAllowDebugControls;
    int        i;

    Ensure();
    if (keyState == NULL || s_bindCount == 0)
        return;

    /* Config binds are parsed before SDL knows the keyboard layout, when a
     * label can only resolve to its US position; redo them now that it does. */
    {
        static int s_relabelled = 0;
        if (!s_relabelled)
        {
            s_relabelled = 1;
            for (i = 0; i < s_bindCount; i++)
            {
                int sc = (s_binds[i].key[0] != '\0') ? BindInput_ScancodeFromLabel(s_binds[i].key) : SDL_SCANCODE_UNKNOWN;
                if (sc != SDL_SCANCODE_UNKNOWN)
                    s_binds[i].scancode = sc;
            }
        }
    }

    for (i = 0; i < s_bindCount; i++)
    {
        s_PcBind* b      = &s_binds[i];
        int       keyNow = (b->key[0] != '\0' && b->scancode > 0) ? keyState[b->scancode] : 0;
        int       padNow = (b->pad[0] != '\0' && b->padCode != NO_PAD) ? (PsyX_RawControllerBindHeld(b->padCode) != 0) : 0;
        int       fire   = (keyNow && !b->prevKey) || (padNow && !b->prevPad);

        b->prevKey = keyNow;
        b->prevPad = padNow;

        /* Binds run console commands, so they live behind the same gate the
         * console does: otherwise a shared config would be a way to hand
         * someone cheats they never switched on. The game's own rows are play,
         * not cheats, and run regardless. */
        if (fire && (g_PcAllowDebugControls || PlayCommands(b->cmds)))
        {
            Run(b);
            /* The table may have moved under this index. */
            if (i >= s_bindCount)
                break;
        }
    }
}

int PcBinds_Count(void)
{
    const char* lines[PC_BIND_MAX * 2];

    Ensure();
    return RenderLines(lines);
}

const char* PcBinds_Line(int i)
{
    const char* lines[PC_BIND_MAX * 2];
    int         n;

    Ensure();
    n = RenderLines(lines);
    return (i >= 0 && i < n) ? lines[i] : "";
}

/* ---- the controls screen's view: one row per command -------------------- */

int PcBinds_RowCount(void)
{
    Ensure();
    return s_bindCount;
}

const char* PcBinds_RowLabel(int row)
{
    int b;

    Ensure();
    if (row < 0 || row >= s_bindCount)
        return "";
    for (b = 0; b < BUILTIN_COUNT; b++)
    {
        if (s_binds[row].builtin && SDL_strcasecmp(s_binds[row].cmds, s_builtins[b].cmd) == 0)
            return s_builtins[b].label;
    }
    return s_binds[row].cmds;
}

const char* PcBinds_RowValue(int row, int isPad)
{
    Ensure();
    if (row < 0 || row >= s_bindCount)
        return NULL;
    if (isPad)
        return s_binds[row].pad;
    /* The controls screen deals in key POSITIONS (SDL scancode names) and
     * labels them itself; a bind is kept by the label typed. */
    return (s_binds[row].key[0] != '\0') ? SDL_GetScancodeName((SDL_Scancode)s_binds[row].scancode) : "";
}

/* Sets or clears (value NULL, "" or "NONE") a row's key or controller input.
 * Returns the row's new index, which changes when a cleared row goes. */
int PcBinds_RowSet(int row, int isPad, const char* value)
{
    char cmds[PC_BIND_CMDS_CAP];
    char key[PC_BIND_KEY_CAP + 8];

    Ensure();
    if (row < 0 || row >= s_bindCount)
        return -1;
    snprintf(cmds, sizeof(cmds), "%s", s_binds[row].cmds);

    if (value == NULL || value[0] == '\0' || SDL_strcasecmp(value, "NONE") == 0)
    {
        if (isPad)
            s_binds[row].pad[0] = '\0';
        else
            s_binds[row].key[0] = '\0';
        /* Cleared from the screen it was shown on: it keeps its row there. */
        s_binds[row].pinned = 1;
    }
    else
    {
        if (isPad)
            snprintf(key, sizeof(key), PAD_PREFIX "%s", value);
        else
            BindInput_KeyLabel(value, key, sizeof(key));
        if (!SetBind(key, cmds, 1))
            return row;
    }
    Save();
    return FindByCmds(cmds);
}
