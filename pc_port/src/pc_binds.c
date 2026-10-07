/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Console key binds. See pc_binds.h for why this is its own system rather than
 * part of the control-scheme binds.
 *
 * A bind stores the command string verbatim and replays it through
 * Pc_ConsoleExec on the key's press edge, splitting on ';'. Storing the text
 * rather than a parsed form is what lets the config line be the same thing the
 * user typed, which is the point: a bind set is shareable by copy and paste.
 */
#include "game.h"
#include "sh_log.h"

#include "pc_binds.h"
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

typedef struct
{
    char key[PC_BIND_KEY_CAP];   /* key name as typed, e.g. "K" or "KEYPAD 1" */
    char cmds[PC_BIND_CMDS_CAP]; /* command string as typed, ';' separated */
    int  scancode;
    int  prevDown;
} s_PcBind;

static s_PcBind s_binds[PC_BIND_MAX];
static int      s_bindCount;

/* Rendered "bind <key> <cmds>" lines, handed to the config writer. */
static char s_lineBuf[PC_BIND_MAX][PC_BIND_KEY_CAP + PC_BIND_CMDS_CAP + 8];

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
    Pc_Console_Print("bind list        show every bind");
    Pc_Console_Print("unbind <key>     clear one");
    Pc_Console_Print("unbindall        clear them all");
    Pc_Console_Print("Separate from the control binds: a key already used by the");
    Pc_Console_Print("game can carry one, and editing controls never touches these.");
}

/* A bind may not contain bind/unbind/unbindall. Without this a bind could
 * rewrite the bind table as a side effect of being pressed, which makes a
 * shared config able to silently rebind the keys around it. */
static int IsBindWord(const char* cmd)
{
    return SDL_strncasecmp(cmd, "BIND", 4) == 0 ||
           SDL_strncasecmp(cmd, "UNBIND", 6) == 0;
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

static int FindByScancode(int sc)
{
    int i;
    for (i = 0; i < s_bindCount; i++)
    {
        if (s_binds[i].scancode == sc)
            return i;
    }
    return -1;
}

static void Save(void)
{
    const char* lines[PC_BIND_MAX];
    int         i;

    for (i = 0; i < s_bindCount; i++)
    {
        snprintf(s_lineBuf[i], sizeof(s_lineBuf[i]), "bind %s %s",
                 s_binds[i].key, s_binds[i].cmds);
        lines[i] = s_lineBuf[i];
    }

    PcConfig_SaveBindLines(lines, s_bindCount);
}

/* Shared by the console command and the config loader. Returns 0 and prints
 * nothing on failure when `quiet` is set (config load), so a hand-edited file
 * cannot spam the console at boot. */
static int SetBind(const char* keyName, const char* cmds, int quiet)
{
    char         key[PC_BIND_KEY_CAP];
    char         body[PC_BIND_CMDS_CAP];
    SDL_Scancode sc;
    const char*  why;
    int          slot;

    if (keyName == NULL || keyName[0] == '\0' || cmds == NULL || cmds[0] == '\0')
        return 0;

    snprintf(key, sizeof(key), "%s", keyName);
    snprintf(body, sizeof(body), "%s", cmds);
    Trim(key);
    Trim(body);

    if (key[0] == '\0' || body[0] == '\0')
        return 0;

    /* Tolerate a quoted command string. The syntax does not need quotes --
     * ';' separates commands -- but wrapping the list in them is the habit
     * from other consoles, and silently binding a command that begins with a
     * quote is worse than just accepting it. */
    {
        size_t bl = strlen(body);
        if (bl >= 2 && ((body[0] == '"' && body[bl - 1] == '"') ||
                        (body[0] == '\'' && body[bl - 1] == '\'')))
        {
            memmove(body, body + 1, bl - 2);
            body[bl - 2] = 0;
            Trim(body);
            if (body[0] == 0)
                return 0;
        }
    }

    sc  = SDL_GetScancodeFromName(key);
    why = KeyRefusal(key, sc);
    if (why != NULL)
    {
        if (!quiet)
            bprintf("bind: %s (%s)", why, key);
        return 0;
    }

    /* Reject the whole bind if any command in it is a bind command. */
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

    slot = FindByScancode((int)sc);
    if (slot < 0)
    {
        if (s_bindCount >= PC_BIND_MAX)
        {
            if (!quiet)
                bprintf("bind: full (%d binds)", PC_BIND_MAX);
            return 0;
        }
        slot = s_bindCount++;
        s_binds[slot].prevDown = 0;
    }

    snprintf(s_binds[slot].key, sizeof(s_binds[slot].key), "%s", key);
    snprintf(s_binds[slot].cmds, sizeof(s_binds[slot].cmds), "%s", body);
    s_binds[slot].scancode = (int)sc;

    /* A key held down when it was bound must not fire on the next frame. */
    s_binds[slot].prevDown = 1;

    return 1;
}

void PcBinds_CmdBind(const char* arg)
{
    char        key[PC_BIND_KEY_CAP];
    const char* cmds;
    size_t      n;

    if (arg == NULL || arg[0] == '\0')
    {
        Usage();
        return;
    }

    if (SDL_strcasecmp(arg, "LIST") == 0)
    {
        int i;
        if (s_bindCount == 0)
        {
            Pc_Console_Print("no binds set (type 'bind' for usage)");
            return;
        }
        for (i = 0; i < s_bindCount; i++)
            bprintf("bind %s %s", s_binds[i].key, s_binds[i].cmds);
        {
            extern int g_PcAllowDebugControls;
            if (!g_PcAllowDebugControls)
                Pc_Console_Print("(binds are idle: allow_debug_controls is off)");
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
    SDL_Scancode sc;
    int          slot;

    if (arg == NULL || arg[0] == '\0')
    {
        Pc_Console_Print("unbind <key>   (unbindall clears every bind)");
        return;
    }

    sc   = SDL_GetScancodeFromName(arg);
    slot = (sc != SDL_SCANCODE_UNKNOWN) ? FindByScancode((int)sc) : -1;
    if (slot < 0)
    {
        bprintf("unbind: nothing bound to %s", arg);
        return;
    }

    bprintf("unbound %s (was %s)", s_binds[slot].key, s_binds[slot].cmds);

    for (; slot < s_bindCount - 1; slot++)
        s_binds[slot] = s_binds[slot + 1];
    s_bindCount--;

    Save();
}

void PcBinds_CmdUnbindAll(void)
{
    int n = s_bindCount;

    s_bindCount = 0;
    Save();
    bprintf("cleared %d bind%s", n, (n == 1) ? "" : "s");
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

void PcBinds_Update(const unsigned char* keyState)
{
    extern int g_PcAllowDebugControls;
    int        i;

    if (keyState == NULL || s_bindCount == 0)
        return;

    /* Binds run console commands, so they live behind the same gate the console
     * does. Otherwise a shared config would be a way to hand someone cheats
     * they never switched on. */
    if (!g_PcAllowDebugControls)
        return;

    for (i = 0; i < s_bindCount; i++)
    {
        s_PcBind* b   = &s_binds[i];
        int       now = (b->scancode > 0) ? keyState[b->scancode] : 0;

        if (now && !b->prevDown)
        {
            const char* p = b->cmds;

            /* Run the whole string. Each command is re-read from the bind, so a
             * command that clears binds cannot leave this loop walking freed
             * text -- bind/unbind are refused inside a bind for that reason. */
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

        b->prevDown = now;
    }
}

int PcBinds_Count(void)
{
    return s_bindCount;
}

const char* PcBinds_Line(int i)
{
    if (i < 0 || i >= s_bindCount)
        return "";

    snprintf(s_lineBuf[i], sizeof(s_lineBuf[i]), "bind %s %s",
             s_binds[i].key, s_binds[i].cmds);
    return s_lineBuf[i];
}
