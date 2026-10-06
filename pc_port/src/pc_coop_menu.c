/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_menu.c - the simple co-op front end (state + logic only).
 *
 * Rendering lives in sh_net_ui.c (Nu_DrawCoopMenu), so this menu shares the
 * clean panel look of the rest of the online UI and draws from the same
 * post-capture hook on the title screen and in game alike. This file only holds
 * the page/selection state, services input, and exposes accessors the renderer
 * reads. Host / Join drive the Steam session layer (sh_net_session.h).
 */

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_coop_menu.h"
#include "pc_coop_save.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h" /* hover/click hit-test in Update, like the other windows */
#include "sh_net_session.h"
#include "sh_net_steam.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>

typedef enum
{
    COOP_PAGE_ROOT = 0,     /* main-menu: Host / Join / Back */
    COOP_PAGE_HOST_SETUP,
    COOP_PAGE_HOST,
    COOP_PAGE_JOIN,
    COOP_PAGE_ROOMS,        /* server room browser */
    COOP_PAGE_INGAME,       /* in-game M menu: Resume / prefs / players / leave */
    COOP_PAGE_PLAYERS       /* member list (in-game) */
} CoopPage;

static int      s_open;
static int      s_inGame;   /* 1 = opened in-game (M menu), 0 = main-menu popup */
static CoopPage s_page;
static int      s_sel;
static char     s_msg[64];  /* transient feedback shown on the in-game status line */

/* The row under the pointer, handed over by the renderer each frame; -1 none. */
static int      s_hover = -1;

/* 1 while the CURRENT game was launched as multiplayer (host or join via the
 * Multiplayer menu). This -- not a config flag -- is what turns on the in-game
 * co-op features (the M menu, no pausing). A normal New Game / Continue leaves
 * it 0, so single-player is identical to the stock port. title.c sets it at the
 * boot and clears it at the menu. */
int g_PcCoopGame;

/* Pending host settings, seeded from config when the setup page is entered and
 * written back to config just before the lobby is opened (the worker reads them
 * there -- see ShSession_Tick's wantHost handler). */
static int s_setMaxPlayers = 4;
static int s_setPublic     = 0;
static int s_setFps        = 60;
/* Where to host: 0 = this PC (relay runs in-process), 1 = my configured server,
 * 2 = Steam. This PC is the default -- the lowest-setup way to host. */
#define COOP_HOST_THISPC 0
#define COOP_HOST_SERVER 1
#define COOP_HOST_STEAM  2
static int s_hostMode = COOP_HOST_THISPC;

/* The co-op relay server: the configured online server, or loopback. */
static const char* Coop_ServerIp(void)
{
    const char* ip = g_PcConfig.onlineServer;
    return (ip && ip[0]) ? ip : "127.0.0.1";
}
static int Coop_ServerPort(void)
{
    return g_PcConfig.onlinePort ? g_PcConfig.onlinePort : 27888;
}

/* ------------------------------------------------------------------ */
/* Text entry (Join by IP / room code)                                 */
/* ------------------------------------------------------------------ */
/* A single editable field, driven straight off the SDL keyboard state the
 * overlay already reads each frame -- the same approach the chat composer uses
 * (sh_net_chat.c), so no SDL text-input events are needed. While a field is
 * active Pc_CoopMenu_Update ignores navigation so typed keys never move the
 * selection, and the overlay feeds us the keyboard until Enter or Esc. */
#define COOP_EDIT_NONE 0
#define COOP_EDIT_IP   1   /* host[:port] */
#define COOP_EDIT_CODE 2   /* 4-digit room code on the configured server */

static int           s_editKind;
static char          s_editBuf[80];
static int           s_editLen;
static unsigned char s_editPrev[SDL_NUM_SCANCODES];
static int           s_editHaveKeys;

int Pc_CoopMenu_Editing(void)
{
    return s_editKind != COOP_EDIT_NONE;
}

static void Coop_BeginEdit(int kind)
{
    s_editKind     = kind;
    s_editBuf[0]   = '\0';
    s_editLen      = 0;
    s_editHaveKeys = 0; /* swallow the key that opened the field */
}

static char Coop_Glyph(int sc, int shift)
{
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (char)('1' + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        char base = (char)('a' + (sc - SDL_SCANCODE_A));
        return shift ? (char)(base - 32) : base;
    }
    switch (sc)
    {
    case SDL_SCANCODE_0:      return '0';
    case SDL_SCANCODE_PERIOD: return '.';
    case SDL_SCANCODE_SEMICOLON: return shift ? ':' : ';'; /* ':' for host:port */
    case SDL_SCANCODE_MINUS:  return '-';
    default:                  return '\0';
    }
}

static void Coop_CommitEdit(void)
{
    if (s_editKind == COOP_EDIT_IP && s_editLen > 0)
    {
        /* host[:port]; default port when none is typed. */
        char host[80];
        int  port = Coop_ServerPort();
        char* colon;
        SDL_strlcpy(host, s_editBuf, sizeof(host));
        colon = strrchr(host, ':');
        if (colon)
        {
            *colon = '\0';
            if (colon[1]) port = atoi(colon + 1);
        }
        if (host[0])
        {
            ShSession_RequestJoinServer(host, port, 0); /* 0 = the host's room */
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
    }
    else if (s_editKind == COOP_EDIT_CODE && s_editLen > 0)
    {
        int code = atoi(s_editBuf);
        if (code > 0)
        {
            ShSession_RequestJoinServer(Coop_ServerIp(), Coop_ServerPort(), code);
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
    }
    s_editKind = COOP_EDIT_NONE;
}

void Pc_CoopMenu_FeedKeys(const unsigned char* ks)
{
    int sc;
    int shift;

    if (s_editKind == COOP_EDIT_NONE || !ks)
    {
        return;
    }
    if (!s_editHaveKeys) /* seed from live so the opening key is not typed */
    {
        memcpy(s_editPrev, ks, SDL_NUM_SCANCODES);
        s_editHaveKeys = 1;
        return;
    }

    if (ks[SDL_SCANCODE_ESCAPE] && !s_editPrev[SDL_SCANCODE_ESCAPE])
    {
        s_editKind = COOP_EDIT_NONE;
        goto done;
    }
    if ((ks[SDL_SCANCODE_RETURN]    && !s_editPrev[SDL_SCANCODE_RETURN]) ||
        (ks[SDL_SCANCODE_KP_ENTER]  && !s_editPrev[SDL_SCANCODE_KP_ENTER]))
    {
        Coop_CommitEdit();
        goto done;
    }
    if (ks[SDL_SCANCODE_BACKSPACE] && !s_editPrev[SDL_SCANCODE_BACKSPACE] && s_editLen > 0)
    {
        s_editBuf[--s_editLen] = '\0';
    }

    shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    for (sc = 0; sc < SDL_NUM_SCANCODES; sc++)
    {
        if (ks[sc] && !s_editPrev[sc])
        {
            char c = Coop_Glyph(sc, shift);
            /* The code field is digits only. */
            if (s_editKind == COOP_EDIT_CODE && (c < '0' || c > '9')) c = '\0';
            if (c && s_editLen < (int)sizeof(s_editBuf) - 1)
            {
                s_editBuf[s_editLen++] = c;
                s_editBuf[s_editLen]   = '\0';
            }
        }
    }

done:
    memcpy(s_editPrev, ks, SDL_NUM_SCANCODES);
}

/* Host-setup save picker: the co-op saves on disk and which one to start from
 * (-1 = a fresh New Game). */
static CoopSaveEntry s_saves[8];
static int           s_saveCount;
static int           s_loadIdx = -1;

/* Start-game request handed to title.c (GameState_MainMenu_Update), which does
 * the actual boot. 0 = none, 1 = new game, 2 = load s_startSave. */
static int  s_startReq;
static char s_startSave[COOP_SAVE_NAME_MAX];

void Pc_CoopMenu_Open(void)
{
    s_open   = 1;
    s_inGame = 0;
    s_page   = COOP_PAGE_ROOT;
    s_sel    = 0;
    SH_DBG("[COOP] menu opened");
}

void Pc_CoopMenu_OpenInGame(void)
{
    s_open   = 1;
    s_inGame = 1;
    s_page   = COOP_PAGE_INGAME;
    s_sel    = 0;
    s_msg[0] = '\0';
    SH_DBG("[COOP] in-game M menu opened");
}

int Pc_CoopMenu_InGame(void)
{
    return s_open && s_inGame;
}

void Pc_CoopMenu_Close(void)
{
    s_open = 0;
}

int Pc_CoopMenu_IsOpen(void)
{
    return s_open;
}

static void Coop_EnterHostSetup(void)
{
    s_setMaxPlayers = g_PcConfig.onlineSteamMaxPlayers;
    if (s_setMaxPlayers < 2) s_setMaxPlayers = 2;
    if (s_setMaxPlayers > 4) s_setMaxPlayers = 4;
    s_setPublic   = g_PcConfig.onlineSteamPublic ? 1 : 0;
    s_setFps      = (g_PcConfig.fpsCap >= 60) ? 60 : 30;
    s_saveCount   = Pc_CoopSave_List(s_saves, (int)(sizeof(s_saves) / sizeof(s_saves[0])));
    s_loadIdx     = -1; /* default: start a fresh game */
    s_page        = COOP_PAGE_HOST_SETUP;
    s_sel         = 0;
}

/* Queue the game boot for title.c. Reads the host-setup load choice. */
static void Coop_RequestStart(void)
{
    if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
    {
        s_startReq = 2;
        snprintf(s_startSave, sizeof(s_startSave), "%s", s_saves[s_loadIdx].name);
    }
    else
    {
        s_startReq      = 1;
        s_startSave[0]  = '\0';
    }
    Pc_CoopMenu_Close();
}

int Pc_CoopMenu_TakeStartRequest(char* outName, int cap)
{
    int req = s_startReq;
    s_startReq = 0;
    if (outName && cap > 0)
    {
        snprintf(outName, cap, "%s", s_startSave);
    }
    return req;
}

static void Coop_StartHosting(void)
{
    /* The worker reads these out of config when it creates the lobby. */
    g_PcConfig.onlineSteamMaxPlayers = s_setMaxPlayers;
    g_PcConfig.onlineSteamPublic     = s_setPublic;
    g_PcConfig.fpsCap                = s_setFps;
    SH_DBG("[COOP] host mode %d: %d players, %s, fps %d",
           s_hostMode, s_setMaxPlayers, s_setPublic ? "public" : "private", s_setFps);
    if (s_hostMode == COOP_HOST_STEAM)
    {
        ShSession_RequestHost();
    }
    else if (s_hostMode == COOP_HOST_SERVER)
    {
        ShSession_RequestHostServer(Coop_ServerIp(), Coop_ServerPort(), s_setMaxPlayers);
    }
    else /* this PC: run the relay in-process */
    {
        ShSession_RequestHostListen(Coop_ServerPort(), s_setMaxPlayers);
    }
    s_page = COOP_PAGE_HOST;
    s_sel  = 0;
}

/* Rows on the current page; the last row is always Back. */
int Pc_CoopMenu_RowCount(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:       return 3; /* Host, Join, Back */
    case COOP_PAGE_HOST_SETUP: return 7; /* Host on, Players, Visibility, FPS, Load, Open, Back */
    case COOP_PAGE_HOST:       return 4; /* Invite, Start Game, Leave, Back */
    case COOP_PAGE_JOIN:       return 5; /* Browse, Join by IP, Enter code, Steam, Back */
    case COOP_PAGE_ROOMS:      return ShSession_RoomCount() + 2; /* rooms + Refresh + Back */
    case COOP_PAGE_INGAME:     return 6; /* Resume, Save, Save&Exit, Nameplates, Players, Leave */
    case COOP_PAGE_PLAYERS:    return ShSession_MemberCount() + 1; /* members + Back */
    default:                   return 1;
    }
}

int Pc_CoopMenu_Selected(void)
{
    return s_sel;
}

const char* Pc_CoopMenu_Title(void)
{
    switch (s_page)
    {
    case COOP_PAGE_HOST_SETUP: return "HOST GAME";
    case COOP_PAGE_HOST:       return "LOBBY";
    case COOP_PAGE_JOIN:       return "JOIN GAME";
    case COOP_PAGE_ROOMS:      return "SERVER ROOMS";
    case COOP_PAGE_INGAME:     return "MULTIPLAYER";
    case COOP_PAGE_PLAYERS:    return "PLAYERS";
    default:                   return "MULTIPLAYER";
    }
}

void Pc_CoopMenu_RowText(int i, char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (i == 0) snprintf(out, cap, "Host Game");
        else if (i == 1) snprintf(out, cap, "Join Game");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST_SETUP:
        if (i == 0)
        {
            const char* where = (s_hostMode == COOP_HOST_STEAM)  ? "Steam"
                              : (s_hostMode == COOP_HOST_SERVER)  ? "My Server"
                                                                  : "This PC";
            snprintf(out, cap, "Host on:  %s", where);
        }
        else if (i == 1) snprintf(out, cap, "Max Players:  %d", s_setMaxPlayers);
        else if (i == 2) snprintf(out, cap, "Visibility:  %s", s_setPublic ? "Public" : "Private");
        else if (i == 3) snprintf(out, cap, "FPS Lock:  %d", s_setFps);
        else if (i == 4)
        {
            if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
                snprintf(out, cap, "Load:  %s", s_saves[s_loadIdx].name);
            else
                snprintf(out, cap, "Load:  New Game");
        }
        else if (i == 5) snprintf(out, cap, s_hostMode == COOP_HOST_STEAM ? "Open Lobby" : "Open Room");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST:
        if (i == 0) snprintf(out, cap, "Invite Friend");
        else if (i == 1) snprintf(out, cap, "Start Game");
        else if (i == 2) snprintf(out, cap, "Close Lobby");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_INGAME:
        if (i == 0) snprintf(out, cap, "Resume");
        else if (i == 1) snprintf(out, cap, "Save");
        else if (i == 2) snprintf(out, cap, "Save & Exit");
        else if (i == 3) snprintf(out, cap, "Nameplates:  %s",
                                  g_PcConfig.onlineNameplates ? "On" : "Off");
        else if (i == 4) snprintf(out, cap, "Players");
        else snprintf(out, cap, "Leave to Title");
        break;

    case COOP_PAGE_PLAYERS:
    {
        int mc = ShSession_MemberCount();
        if (i < mc)
        {
            const ShSessionMember* m = ShSession_Member(i);
            if (m && m->pingMs >= 0)
                snprintf(out, cap, "%s  (%dms)", m->name, m->pingMs);
            else if (m)
                snprintf(out, cap, "%s", m->name);
        }
        else
        {
            snprintf(out, cap, "Back");
        }
        break;
    }

    case COOP_PAGE_JOIN:
        if (i == 0)      snprintf(out, cap, "Browse Server Rooms");
        else if (i == 1) snprintf(out, cap, "Join by IP...");
        else if (i == 2) snprintf(out, cap, "Enter Room Code...");
        else if (i == 3) snprintf(out, cap, "Join a Friend (Steam)");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_ROOMS:
    {
        int rc = ShSession_RoomCount();
        if (i < rc)
        {
            int  code = 0, players = 0, max = 0;
            char host[SHSESSION_NAME_MAX];
            host[0] = '\0';
            ShSession_RoomGet(i, &code, &players, &max, host, (int)sizeof(host));
            snprintf(out, cap, "Room %d  (%d/%d)  %s", code, players, max, host);
        }
        else if (i == rc) snprintf(out, cap, "Refresh");
        else              snprintf(out, cap, "Back");
        break;
    }

    default:
        snprintf(out, cap, "Back");
        break;
    }
}

void Pc_CoopMenu_StatusText(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (s_editKind == COOP_EDIT_IP)
    {
        snprintf(out, cap, "Server IP (host or host:port):  %s_   [Enter join, Esc cancel]", s_editBuf);
        return;
    }
    if (s_editKind == COOP_EDIT_CODE)
    {
        snprintf(out, cap, "Room code:  %s_   [Enter join, Esc cancel]", s_editBuf);
        return;
    }
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (!ShSteam_Available())
        {
            snprintf(out, cap, "Steam not running - start Steam and relaunch.");
        }
        break;

    case COOP_PAGE_HOST:
        ShSession_StatusLine(out, cap);
        break;

    case COOP_PAGE_JOIN:
        if (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN)
        {
            /* Already in a friend's lobby: tell them whether it is still
             * gathering or already a game in progress (you drop straight in). */
            const char* st = ShSteam_GetLobbyData("state");
            if (st && strcmp(st, "ingame") == 0)
            {
                snprintf(out, cap, "Game in progress - dropping you in...");
            }
            else
            {
                snprintf(out, cap, "In the lobby - waiting for the host to start.");
            }
        }
        else
        {
            snprintf(out, cap, "Accept a Steam invite from a friend to join.");
        }
        break;

    case COOP_PAGE_ROOMS:
        if (ShSession_RoomCount() > 0) { ShSession_StatusLine(out, cap); }
        else                           { snprintf(out, cap, "No open rooms - pick Refresh, or Host one."); }
        break;

    case COOP_PAGE_INGAME:
        if (s_msg[0]) { snprintf(out, cap, "%s", s_msg); }
        else          { ShSession_StatusLine(out, cap); }
        break;

    case COOP_PAGE_HOST_SETUP:
        if (s_hostMode == COOP_HOST_THISPC)
            snprintf(out, cap, "This PC hosts. Friends join with your IP (forward UDP %d).", Coop_ServerPort());
        else if (s_hostMode == COOP_HOST_SERVER)
            snprintf(out, cap, "A room on your server (%s). Friends browse or enter the code.", Coop_ServerIp());
        else
            snprintf(out, cap, "Opens a Steam lobby; invite friends from the overlay.");
        break;

    case COOP_PAGE_PLAYERS:
    default:
        break;
    }
}

/* Write the co-op save (named after the player), refused during a boss fight.
 * andExit also disconnects and returns to the title. Feedback goes to s_msg. */
static void Coop_DoSave(int andExit)
{
    const char* nm = (g_PcConfig.onlineName[0]) ? g_PcConfig.onlineName : "coop";

    if (Pc_Save_BossActive())
    {
        snprintf(s_msg, sizeof(s_msg), "Can't save during a boss fight");
        return;
    }
    if (!Pc_CoopSave_Write(nm))
    {
        snprintf(s_msg, sizeof(s_msg), "Save failed");
        return;
    }
    snprintf(s_msg, sizeof(s_msg), "Saved as \"%s\"", nm);
    if (andExit)
    {
        ShSession_RequestLeave();
        g_SysWork.sysFlags |= SysFlag_DoWarmReset;
        Pc_CoopMenu_Close();
    }
}

static void Coop_Confirm(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (s_sel == 0)      { Coop_EnterHostSetup(); }
        else if (s_sel == 1) { s_page = COOP_PAGE_JOIN; s_sel = 0; }
        else                 { Pc_CoopMenu_Close(); }
        break;

    case COOP_PAGE_HOST_SETUP:
        /* Setting rows cycle on confirm; no left/right needed. */
        if (s_sel == 0)      { s_hostMode = (s_hostMode + 1) % 3; }
        else if (s_sel == 1) { s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1; }
        else if (s_sel == 2) { s_setPublic = !s_setPublic; }
        else if (s_sel == 3) { s_setFps = (s_setFps == 60) ? 30 : 60; }
        else if (s_sel == 4) { s_loadIdx = (s_loadIdx + 1 >= s_saveCount) ? -1 : s_loadIdx + 1; }
        else if (s_sel == 5) { Coop_StartHosting(); } /* Open Room / Lobby */
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_HOST:
        if (s_sel == 0)      { ShSession_RequestInvite(); }
        else if (s_sel == 1) { Coop_RequestStart(); }                       /* Start Game */
        else if (s_sel == 2) { ShSession_RequestLeave(); s_page = COOP_PAGE_ROOT; s_sel = 0; }
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_INGAME:
        if (s_sel == 0)      { Pc_CoopMenu_Close(); }                       /* Resume */
        else if (s_sel == 1) { Coop_DoSave(0); }                            /* Save */
        else if (s_sel == 2) { Coop_DoSave(1); }                            /* Save & Exit */
        else if (s_sel == 3) { g_PcConfig.onlineNameplates = !g_PcConfig.onlineNameplates; }
        else if (s_sel == 4) { s_page = COOP_PAGE_PLAYERS; s_sel = 0; }     /* Players */
        else
        {
            /* Leave to Title without saving. */
            ShSession_RequestLeave();
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            Pc_CoopMenu_Close();
        }
        break;

    case COOP_PAGE_PLAYERS:
        /* Rows above Back are members; host-kick lands with the session kick
         * API. Back returns to the in-game root (the Players row). */
        s_page = COOP_PAGE_INGAME;
        s_sel  = 4;
        break;

    case COOP_PAGE_JOIN:
        if (s_sel == 0) /* browse the server's open rooms */
        {
            ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort());
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
        else if (s_sel == 1) { Coop_BeginEdit(COOP_EDIT_IP); }   /* type host[:port] */
        else if (s_sel == 2) { Coop_BeginEdit(COOP_EDIT_CODE); } /* type a room code */
        else if (s_sel == 3)
        {
            /* Steam: the friend's invite is accepted from Steam's overlay, not
             * here; this row is the reminder. The pending-join flow boots them. */
        }
        else { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_ROOMS:
    {
        int rc = ShSession_RoomCount();
        if (s_sel < rc)
        {
            int code = 0;
            ShSession_RoomGet(s_sel, &code, NULL, NULL, NULL, 0);
            if (code > 0)
                ShSession_RequestJoinServer(Coop_ServerIp(), Coop_ServerPort(), code);
            /* The guest boots when the host starts (title.c takes the world). */
        }
        else if (s_sel == rc) /* Refresh */
        {
            ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort());
        }
        else { s_page = COOP_PAGE_JOIN; s_sel = 0; }
        break;
    }

    default:
        s_page = COOP_PAGE_ROOT;
        s_sel  = 0;
        break;
    }
}

void Pc_CoopMenu_SetHover(int row)
{
    s_hover = row;
    /* The highlight follows the pointer: a row under it is the selected row. */
    if (s_open && row >= 0 && row < Pc_CoopMenu_RowCount())
    {
        s_sel = row;
    }
}

/* Mouse hover drives the selection; a click selects then confirms. The renderer
 * (sh_net_ui.c) owns the row geometry, so it maps the pointer to a row and calls
 * these. */
void Pc_CoopMenu_SetSelected(int i)
{
    if (s_open && i >= 0 && i < Pc_CoopMenu_RowCount())
    {
        s_sel = i;
    }
}

void Pc_CoopMenu_Confirm(void)
{
    if (s_open)
    {
        Coop_Confirm();
    }
}

void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm)
{
    int rows;
    if (!s_open)
    {
        return;
    }
    /* While a text field is active the keyboard belongs to it (the overlay feeds
     * Pc_CoopMenu_FeedKeys); ignore navigation so typing never moves the row. */
    if (s_editKind != COOP_EDIT_NONE)
    {
        return;
    }
    rows = Pc_CoopMenu_RowCount();

    /* Left-click confirms the row the renderer said is under the pointer. The
     * press edge is computed from our OWN previous-held state, so it fires
     * exactly once per click even if this runs more than once per rendered frame
     * (fixed-timestep catch-up) -- a single press firing several confirms was
     * cycling settings past themselves and jumping pages. */
    {
        static int s_prevHeld = 0;
        int        held = Pc_MouseCursor_LeftHeld();
        int        edge = held && !s_prevHeld;
        s_prevHeld = held;
        if (edge && s_hover >= 0 && s_hover < rows)
        {
            s_sel = s_hover;
            Coop_Confirm();
            return;
        }
    }

    if (up)
    {
        s_sel = (s_sel + rows - 1) % rows;
    }
    if (down)
    {
        s_sel = (s_sel + 1) % rows;
    }
    if (confirm)
    {
        Coop_Confirm();
        return;
    }
    if (cancel)
    {
        if (s_page == COOP_PAGE_PLAYERS)
        {
            s_page = COOP_PAGE_INGAME; /* back out of the member list */
            s_sel  = 4;
        }
        else if (s_page == COOP_PAGE_ROOT || s_page == COOP_PAGE_INGAME)
        {
            Pc_CoopMenu_Close(); /* top level: close / resume */
        }
        else
        {
            s_page = s_inGame ? COOP_PAGE_INGAME : COOP_PAGE_ROOT;
            s_sel  = 0;
        }
    }
}
