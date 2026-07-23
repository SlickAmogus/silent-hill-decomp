/* Discord Rich Presence for the PC port. Shows the player's current Silent Hill
 * area (from g_SavegamePtr->mapIdx) on their Discord profile, with the project's
 * "cheryl" art asset as the large image.
 *
 * The Discord application id is data-driven: config.cfg `discord_app_id`, else
 * the compiled-in SH_DISCORD_DEFAULT_APP_ID. With neither set the feature stays
 * dormant (a fan port cannot ship on the RetroAchievements/official stores, so
 * there is no bundled account system — this is presence only; see
 * docs/RetroAchievements_Feasibility.md).
 *
 * The wire transport lives in pc_discord_ipc.c; that TU owns <windows.h>, whose
 * `byte` typedef clashes with the decomp's, so it cannot share this file. */

#include "game.h"
#include "bodyprog/map/map.h"
#include "pc_config.h"
#include "pc_discord.h"
#include "pc_discord_ipc.h"
#include "sh_log.h"
#include <string.h>
#include <stdio.h>

/* time()/getpid() are provided by the isolated IPC TU (pc_discord_ipc.c): their
 * <time.h>/<unistd.h> typedefs can clash with the decomp headers included here,
 * so this game-facing TU stays free of libc system headers. */
#define SH_DISCORD_PID() ShDiscordIpc_Pid()

/* The project's Discord application id (baked in; config.cfg discord_app_id can
 * still override, e.g. for a third-party build). Empty = feature dormant. */
#ifndef SH_DISCORD_DEFAULT_APP_ID
#define SH_DISCORD_DEFAULT_APP_ID "1529658536274034770"
#endif

/* Art-asset keys — must match the asset names uploaded in the Discord app's
 * Rich Presence -> Art Assets. Large = the journal (main image); small = the
 * Cheryl badge overlaid on the large image's corner. */
#define SH_DISCORD_LARGE_IMAGE "journal"
#define SH_DISCORD_SMALL_IMAGE "cheryl"

/* Discord IPC opcodes. */
#define DISCORD_OP_HANDSHAKE 0
#define DISCORD_OP_FRAME     1
#define DISCORD_OP_CLOSE     2
#define DISCORD_OP_PING      3
#define DISCORD_OP_PONG      4

/* SET_ACTIVITY is rate-limited to ~5/20s by Discord: keep changes >= 4s apart,
 * and re-assert the (unchanged) presence every 15s — the heartbeat also covers a
 * first activity that raced the post-handshake READY, and any transient hiccup. */
#define DISCORD_MIN_SEND_GAP   4
#define DISCORD_HEARTBEAT      15
#define DISCORD_RECONNECT_GAP  10

static int          s_enabled;
static char         s_appId[80];
static long long    s_startTime;
static long long    s_lastSend;
static long long    s_lastConnAttempt;
static int          s_forceSend;
static unsigned int s_nonce;
static char         s_lastDetails[64];
static char         s_lastState[32];

/* Read accumulator for framed IPC replies (PING/PONG keepalive). */
static unsigned char s_rx[8192];
static unsigned int  s_rxLen;

/* Player-facing area name by e_MapIdx (0..42). Unused/out-of-range fall back to
 * the game title. Every string MUST stay JSON-safe (no '"' or '\'); apostrophes
 * are fine. */
static const char* Discord_AreaName(int idx)
{
    switch (idx)
    {
    case MapIdx_MAP0_S00:
    case MapIdx_MAP0_S02: return "Old Silent Hill";
    case MapIdx_MAP0_S01: return "Cafe 5to2";
    case MapIdx_MAP1_S00:
    case MapIdx_MAP1_S01:
    case MapIdx_MAP1_S05:
    case MapIdx_MAP1_S06: return "Midwich Elementary School";
    case MapIdx_MAP1_S02:
    case MapIdx_MAP1_S03: return "Midwich School (Otherworld)";
    case MapIdx_MAP2_S00: return "Old Silent Hill - Streets";
    case MapIdx_MAP2_S01: return "Balkan Church";
    case MapIdx_MAP2_S02:
    case MapIdx_MAP2_S04: return "Central Silent Hill";
    case MapIdx_MAP3_S00:
    case MapIdx_MAP3_S01:
    case MapIdx_MAP3_S02:
    case MapIdx_MAP3_S06:
    case MapIdx_MAP4_S04: return "Alchemilla Hospital";
    case MapIdx_MAP3_S03:
    case MapIdx_MAP3_S04:
    case MapIdx_MAP3_S05: return "Alchemilla Hospital (Otherworld)";
    case MapIdx_MAP4_S01: return "Green Lion Antiques";
    case MapIdx_MAP4_S02:
    case MapIdx_MAP4_S05: return "Central Silent Hill (Otherworld)";
    case MapIdx_MAP4_S03: return "Central Square Shopping Center";
    case MapIdx_MAP5_S00:
    case MapIdx_MAP6_S03: return "The Sewers";
    case MapIdx_MAP5_S01: return "Resort Area";
    case MapIdx_MAP5_S02: return "Annie's Bar";
    case MapIdx_MAP5_S03: return "Norman's Motel";
    case MapIdx_MAP6_S00: return "Resort Area (Otherworld)";
    case MapIdx_MAP6_S01: return "Lakeside Pier";
    case MapIdx_MAP6_S02: return "The Lighthouse";
    case MapIdx_MAP6_S04: return "Lakeside Amusement Park";
    case MapIdx_MAP7_S00:
    case MapIdx_MAP7_S01:
    case MapIdx_MAP7_S02:
    case MapIdx_MAP7_S03: return "Nowhere";
    default:              return "Silent Hill";
    }
}

/* Second presence line: the action difficulty (savegame bitfield, signed 4-bit
 * e_GameDifficulty). Kept JSON-safe. */
static const char* Discord_DifficultyName(int diff)
{
    switch (diff)
    {
    case GameDifficulty_Easy: return "Easy";
    case GameDifficulty_Hard: return "Hard";
    default:                  return "Normal";
    }
}

/* Little-endian 8-byte frame header (opcode, length). */
static void Discord_PutHeader(unsigned char* h, unsigned int op, unsigned int len)
{
    h[0] = (unsigned char)op;  h[1] = (unsigned char)(op >> 8);
    h[2] = (unsigned char)(op >> 16); h[3] = (unsigned char)(op >> 24);
    h[4] = (unsigned char)len; h[5] = (unsigned char)(len >> 8);
    h[6] = (unsigned char)(len >> 16); h[7] = (unsigned char)(len >> 24);
}

static int Discord_SendFrame(unsigned int op, const char* payload)
{
    unsigned char hdr[8];
    unsigned int  plen = payload ? (unsigned int)strlen(payload) : 0;
    Discord_PutHeader(hdr, op, plen);
    if (!ShDiscordIpc_Write(hdr, 8))
        return 0;
    if (plen && !ShDiscordIpc_Write(payload, plen))
        return 0;
    return 1;
}

/* Drain incoming frames non-blocking; reply to PING with PONG so Discord keeps
 * the connection alive, and notice a server-side CLOSE. */
static void Discord_PumpReads(void)
{
    for (;;)
    {
        int got = 0;
        if (s_rxLen < sizeof(s_rx))
        {
            got = ShDiscordIpc_Read(s_rx + s_rxLen, (unsigned int)(sizeof(s_rx) - s_rxLen));
            if (got < 0) { s_rxLen = 0; return; }          /* closed */
            if (got > 0) s_rxLen += (unsigned int)got;
        }

        {
            int consumed = 0;
            while (s_rxLen >= 8)
            {
                unsigned int op  = (unsigned int)s_rx[0] | ((unsigned int)s_rx[1] << 8) |
                                   ((unsigned int)s_rx[2] << 16) | ((unsigned int)s_rx[3] << 24);
                unsigned int len = (unsigned int)s_rx[4] | ((unsigned int)s_rx[5] << 8) |
                                   ((unsigned int)s_rx[6] << 16) | ((unsigned int)s_rx[7] << 24);
                if (len > sizeof(s_rx) - 8) { s_rxLen = 0; return; } /* desync — drop */
                if (s_rxLen < 8 + len) break;                        /* need more */

                if (op == DISCORD_OP_PING)
                {
                    unsigned char hdr[8];
                    Discord_PutHeader(hdr, DISCORD_OP_PONG, len);
                    ShDiscordIpc_Write(hdr, 8);
                    if (len) ShDiscordIpc_Write(s_rx + 8, len);
                }
                else if (op == DISCORD_OP_CLOSE)
                {
                    ShDiscordIpc_Close();
                    s_rxLen = 0;
                    return;
                }
                /* DISCORD_OP_FRAME (READY / dispatch / errors) ignored. */

                memmove(s_rx, s_rx + 8 + len, s_rxLen - (8 + len));
                s_rxLen -= (8 + len);
                consumed = 1;
            }
            if (got <= 0 && !consumed)
                return;
        }
    }
}

static void Discord_SendActivity(const char* details, const char* state)
{
    char json[512];
    char stateField[64];
    stateField[0] = '\0';
    if (state && state[0])
        snprintf(stateField, sizeof(stateField), "\"state\":\"%s\",", state);
    snprintf(json, sizeof(json),
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{"
             "\"details\":\"%s\",%s"
             "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"Silent Hill\","
             "\"small_image\":\"%s\",\"small_text\":\"Cheryl\"},"
             "\"timestamps\":{\"start\":%lld}"
             "}},\"nonce\":\"%u\"}",
             SH_DISCORD_PID(), details, stateField,
             SH_DISCORD_LARGE_IMAGE, SH_DISCORD_SMALL_IMAGE,
             (long long)s_startTime, ++s_nonce);
    if (Discord_SendFrame(DISCORD_OP_FRAME, json))
    {
        strncpy(s_lastDetails, details, sizeof(s_lastDetails) - 1);
        s_lastDetails[sizeof(s_lastDetails) - 1] = '\0';
        strncpy(s_lastState, state ? state : "", sizeof(s_lastState) - 1);
        s_lastState[sizeof(s_lastState) - 1] = '\0';
    }
}

void Pc_Discord_Init(void)
{
    const char* id;
    s_enabled = 0;
    if (!g_PcConfig.discordRichPresence)
    {
        SH_DBG("[DISCORD] disabled by config");
        return;
    }
    id = g_PcConfig.discordAppId[0] ? g_PcConfig.discordAppId : SH_DISCORD_DEFAULT_APP_ID;
    if (id == NULL || id[0] == '\0')
    {
        SH_DBG("[DISCORD] no discord_app_id set — rich presence off");
        return;
    }
    strncpy(s_appId, id, sizeof(s_appId) - 1);
    s_appId[sizeof(s_appId) - 1] = '\0';
    s_startTime       = ShDiscordIpc_NowUnix();
    s_lastSend        = 0;
    s_lastConnAttempt = 0;
    s_forceSend       = 1;
    s_lastDetails[0]  = '\0';
    s_lastState[0]    = '\0';
    s_rxLen           = 0;
    s_enabled         = 1;
    SH_DBG("[DISCORD] rich presence on (app id %s)", s_appId);
}

void Pc_Discord_Update(void)
{
    long long now;
    char      details[64];
    char      state[32];

    if (!s_enabled)
        return;
    now = ShDiscordIpc_NowUnix();

    if (!ShDiscordIpc_IsOpen())
    {
        char hs[128];
        if (now - s_lastConnAttempt < DISCORD_RECONNECT_GAP)
            return;
        s_lastConnAttempt = now;
        s_rxLen = 0;
        if (!ShDiscordIpc_Connect())
            return; /* Discord not running — try again later */
        snprintf(hs, sizeof(hs), "{\"v\":1,\"client_id\":\"%s\"}", s_appId);
        if (!Discord_SendFrame(DISCORD_OP_HANDSHAKE, hs))
        {
            ShDiscordIpc_Close();
            return;
        }
        s_forceSend      = 1;
        s_lastDetails[0] = '\0';
        s_lastState[0]   = '\0';
        SH_DBG("[DISCORD] connected");
    }

    Discord_PumpReads();
    if (!ShDiscordIpc_IsOpen())
        return; /* PumpReads may have seen a CLOSE / error */

    state[0] = '\0';
    if (g_SavegamePtr != NULL && g_GameWork.gameState == GameState_InGame)
    {
        snprintf(details, sizeof(details), "%s", Discord_AreaName((int)g_SavegamePtr->mapIdx));
        snprintf(state, sizeof(state), "Difficulty: %s",
                 Discord_DifficultyName((int)g_SavegamePtr->gameDifficulty));
    }
    else
        snprintf(details, sizeof(details), "%s", "In the menus");

    if (s_forceSend || strcmp(details, s_lastDetails) != 0 || strcmp(state, s_lastState) != 0)
    {
        if (s_forceSend || (now - s_lastSend) >= DISCORD_MIN_SEND_GAP)
        {
            Discord_SendActivity(details, state);
            s_lastSend  = now;
            s_forceSend = 0;
        }
    }
    else if ((now - s_lastSend) >= DISCORD_HEARTBEAT)
    {
        Discord_SendActivity(details, state);
        s_lastSend = now;
    }
}

void Pc_Discord_Shutdown(void)
{
    if (!s_enabled)
        return;
    if (ShDiscordIpc_IsOpen())
    {
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":null},\"nonce\":\"%u\"}",
                 SH_DISCORD_PID(), ++s_nonce);
        Discord_SendFrame(DISCORD_OP_FRAME, json);
        ShDiscordIpc_Close();
    }
    s_enabled = 0;
    SH_DBG("[DISCORD] shut down");
}
