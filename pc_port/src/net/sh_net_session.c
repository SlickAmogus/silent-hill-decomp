/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_session.c - Steam lobby lifecycle and the peer-to-peer session.
 *
 * See sh_net_session.h for what a session is and why it is separate from the
 * master-server world.
 *
 * The shape is the same one the rest of the client already uses, because it
 * works: the game thread sets request flags under a lock, the worker services
 * them, and the worker publishes a copy the game thread reads without locking.
 * Steam is touched from the worker and nowhere else.
 */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_steam.h"
#include "sh_net_coop.h"
#include "pc_config.h"
#include "sh_log.h"

/* Round trip to each member, and how long before a silent member is no longer
 * called linked. A session is a handful of friends, so this can be leisurely. */
#define SESSION_PING_MS   2000
#define SESSION_LINK_MS   8000
#define SESSION_HELLO_MS  5000

/* Requests, and the published copy. Both under s_lock. */
static SDL_mutex* s_lock;

static struct
{
    int                wantHost;
    int                wantLeave;
    int                wantInvite;
    unsigned long long wantJoin;

    char               presenceArea[64];
    int                presenceMap;
    int                presenceDirty;
} s_req;

static struct
{
    int                active;
    int                isHost;
    unsigned long long lobbyId;
    int                memberCount;
    ShSessionMember    members[SHSESSION_MAX_MEMBERS];
    char               status[128];
} s_pub;

/* Worker-private. */
static ShSessionMember s_members[SHSESSION_MAX_MEMBERS];
static int             s_memberCount;
static unsigned int    s_lastPingMs;
static unsigned int    s_lastHelloMs;
static unsigned int    s_lastSeenMs[SHSESSION_MAX_MEMBERS];
static unsigned int    s_pingSentMs[SHSESSION_MAX_MEMBERS];
static int             s_enabled;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ShSession_Init(void)
{
    if (s_lock)
    {
        return;
    }
    s_lock = SDL_CreateMutex();
    if (!s_lock)
    {
        SH_DBG("[SESSION] SDL_CreateMutex failed: %s", SDL_GetError());
        return;
    }
    memset(&s_req, 0, sizeof(s_req));
    memset(&s_pub, 0, sizeof(s_pub));
    s_req.presenceMap = -1;

    if (!g_PcConfig.onlineSteam)
    {
        SDL_strlcpy(s_pub.status, "Steam: disabled", sizeof(s_pub.status));
        return;
    }

    s_enabled = ShSteam_Init((unsigned int)g_PcConfig.onlineSteamAppId);
    if (!s_enabled)
    {
        SDL_strlcpy(s_pub.status, "Steam: unavailable", sizeof(s_pub.status));
        return;
    }

    /* A lobby id on the command line means the player clicked Join on a
     * friend before the game existed; honour it as soon as we are up. */
    if (g_PcConfig.onlineSteamAutoHost)
    {
        SDL_LockMutex(s_lock);
        s_req.wantHost = 1;
        SDL_UnlockMutex(s_lock);
    }
}

void ShSession_Shutdown(void)
{
    if (s_enabled)
    {
        int i;
        /* Tell the others rather than letting them time out. */
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE];
            if (s_members[i].steamId == ShSteam_SelfId())
            {
                continue;
            }
            ShnPutHeader(buf, SHNET_MSG_S_BYE, 0, 0);
            ShSteam_Send(s_members[i].steamId, buf, SHNET_HDR_SIZE, 0);
        }
        ShSteam_Shutdown();
        s_enabled = 0;
    }
    if (s_lock)
    {
        SDL_DestroyMutex(s_lock);
        s_lock = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Requests from the game thread                                       */
/* ------------------------------------------------------------------ */

void ShSession_RequestHost(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantHost = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestJoin(unsigned long long lobbyId)
{
    if (!s_lock || !lobbyId) return;
    SDL_LockMutex(s_lock);
    s_req.wantJoin = lobbyId;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestLeave(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantLeave = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestInvite(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantInvite = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_PublishPresence(const char* area, int mapIdx)
{
    if (!s_lock || !area) return;
    SDL_LockMutex(s_lock);
    if (mapIdx != s_req.presenceMap || strcmp(area, s_req.presenceArea) != 0)
    {
        SDL_strlcpy(s_req.presenceArea, area, sizeof(s_req.presenceArea));
        s_req.presenceMap   = mapIdx;
        s_req.presenceDirty = 1;
    }
    SDL_UnlockMutex(s_lock);
}

/* ------------------------------------------------------------------ */
/* Published state                                                     */
/* ------------------------------------------------------------------ */

int ShSession_Active(void)
{
    return s_pub.active;
}

int ShSession_MemberCount(void)
{
    return s_pub.memberCount;
}

const ShSessionMember* ShSession_Member(int i)
{
    return (i >= 0 && i < s_pub.memberCount) ? &s_pub.members[i] : NULL;
}

unsigned long long ShSession_LobbyId(void)
{
    return s_pub.lobbyId;
}

int ShSession_IsHost(void)
{
    return s_pub.isHost;
}

void ShSession_StatusLine(char* out, int cap)
{
    if (!out || cap <= 0) return;
    SDL_strlcpy(out, s_pub.status, (size_t)cap);
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static int ShSession_IndexOf(unsigned long long id)
{
    int i;
    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i].steamId == id)
        {
            return i;
        }
    }
    return -1;
}

/* Rebuild the worker's member table from the lobby, preserving what we have
 * already learned about anyone still in it. Steam is the authority on WHO is
 * present; the ping and the map come from our own traffic. */
static void ShSession_SyncMembers(void)
{
    ShSessionMember fresh[SHSESSION_MAX_MEMBERS];
    unsigned int    freshSeen[SHSESSION_MAX_MEMBERS];
    unsigned int    freshSent[SHSESSION_MAX_MEMBERS];
    int             n = ShSteam_MemberCount();
    int             count = 0;
    int             i;

    if (n > SHSESSION_MAX_MEMBERS)
    {
        n = SHSESSION_MAX_MEMBERS;
    }
    memset(fresh, 0, sizeof(fresh));
    memset(freshSeen, 0, sizeof(freshSeen));
    memset(freshSent, 0, sizeof(freshSent));

    for (i = 0; i < n; i++)
    {
        unsigned long long id  = ShSteam_Member(i);
        int                old = ShSession_IndexOf(id);
        const char*        nm;

        if (!id)
        {
            continue;
        }
        if (old >= 0)
        {
            fresh[count]     = s_members[old];
            freshSeen[count] = s_lastSeenMs[old];
            freshSent[count] = s_pingSentMs[old];
        }
        else
        {
            fresh[count].steamId = id;
            fresh[count].pingMs  = -1;
            fresh[count].mapIdx  = -1;
            fresh[count].linked  = 0;
        }
        nm = ShSteam_NameOf(id);
        if (nm && nm[0])
        {
            SDL_strlcpy(fresh[count].name, nm, SHSESSION_NAME_MAX);
        }
        else if (!fresh[count].name[0])
        {
            SDL_snprintf(fresh[count].name, SHSESSION_NAME_MAX, "%llu",
                         (unsigned long long)id);
        }
        count++;
    }

    memcpy(s_members, fresh, sizeof(s_members));
    memcpy(s_lastSeenMs, freshSeen, sizeof(s_lastSeenMs));
    memcpy(s_pingSentMs, freshSent, sizeof(s_pingSentMs));
    s_memberCount = count;
}

static void ShSession_SendHello(unsigned long long to)
{
    shn_u8 buf[SHNET_HDR_SIZE + SHNET_NAME_MAX + 4];
    int    off = SHNET_HDR_SIZE;
    int    map;

    SDL_LockMutex(s_lock);
    map = s_req.presenceMap;
    SDL_UnlockMutex(s_lock);

    ShnPutStr(buf, &off, ShSteam_PersonaName(), SHNET_NAME_MAX);
    ShnPutU8(buf, &off, (shn_u8)((map < 0 || map > 255) ? 0xFF : map));
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_S_HELLO, (shn_u16)(off - SHNET_HDR_SIZE), 0);
    ShSteam_Send(to, buf, off, 1);
}

static void ShSession_Receive(unsigned int now)
{
    shn_u8             buf[SHNET_MTU];
    unsigned long long from = 0;
    int                budget = 32;

    while (budget-- > 0)
    {
        shn_u8  type;
        shn_u16 payLen;
        shn_u32 session;
        int     idx;
        int     n = ShSteam_Recv(&from, buf, (int)sizeof(buf));

        if (n <= 0)
        {
            break;
        }
        if (!ShnParseHeader(buf, n, &type, &payLen, &session))
        {
            continue;
        }
        /* Only from someone the lobby says is here. ShSteam_AcceptSession
         * already refuses non-members, so this is the second of two checks
         * rather than the only one. */
        idx = ShSession_IndexOf(from);
        if (idx < 0)
        {
            continue;
        }
        s_lastSeenMs[idx]    = now;
        s_members[idx].linked = 1;

        switch (type)
        {
        case SHNET_MSG_S_HELLO:
        {
            int  off = 0;
            char nm[SHNET_NAME_MAX];
            if (payLen < SHNET_NAME_MAX + 4)
            {
                break;
            }
            ShnGetStr(buf + SHNET_HDR_SIZE, &off, SHNET_NAME_MAX, nm, SHNET_NAME_MAX);
            if (nm[0])
            {
                SDL_strlcpy(s_members[idx].name, nm, SHSESSION_NAME_MAX);
            }
            {
                int m = (int)ShnGetU8(buf + SHNET_HDR_SIZE, &off);
                s_members[idx].mapIdx = (m == 0xFF) ? -1 : m;
            }
            SH_DBG("[SESSION] %s is in the session (map %d)",
                   s_members[idx].name, s_members[idx].mapIdx);
            break;
        }
        case SHNET_MSG_S_PING:
        {
            /* Echo the sender's clock straight back; they do the arithmetic,
             * so the two machines never have to agree on a time base. */
            shn_u8 out[SHNET_HDR_SIZE + 4];
            int    o  = SHNET_HDR_SIZE;
            int    ro = 0;
            shn_u32 stamp;
            if (payLen < 4)
            {
                break;
            }
            stamp = ShnGetU32(buf + SHNET_HDR_SIZE, &ro);
            ShnPutU32(out, &o, stamp);
            ShnPutHeader(out, SHNET_MSG_S_PONG, (shn_u16)(o - SHNET_HDR_SIZE), 0);
            ShSteam_Send(from, out, o, 0);
            break;
        }
        case SHNET_MSG_S_PONG:
        {
            int     ro = 0;
            shn_u32 stamp;
            if (payLen < 4)
            {
                break;
            }
            stamp = ShnGetU32(buf + SHNET_HDR_SIZE, &ro);
            {
                int rtt = (int)(now - stamp);
                if (rtt < 0)     rtt = 0;
                if (rtt > 9999)  rtt = 9999;
                s_members[idx].pingMs = rtt;
            }
            break;
        }
        case SHNET_MSG_S_BYE:
            SH_DBG("[SESSION] %s left the session", s_members[idx].name);
            s_members[idx].linked = 0;
            break;
        default:
            break;
        }
    }
}

static void ShSession_Publish(void)
{
    int i;

    SDL_LockMutex(s_lock);
    s_pub.active      = (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN);
    s_pub.isHost      = ShSteam_IsLobbyOwner();
    s_pub.lobbyId     = ShSteam_LobbyId();
    s_pub.memberCount = s_memberCount;
    for (i = 0; i < s_memberCount; i++)
    {
        s_pub.members[i] = s_members[i];
    }
    ShSteam_StatusLine(s_pub.status, (int)sizeof(s_pub.status));
    SDL_UnlockMutex(s_lock);
}

void ShSession_Tick(unsigned int nowMs)
{
    int                wantHost, wantLeave, wantInvite, presenceDirty;
    unsigned long long wantJoin;
    char               area[64];
    int                i;

    if (!s_lock || !s_enabled)
    {
        return;
    }

    ShSteam_RunCallbacks();

    SDL_LockMutex(s_lock);
    wantHost      = s_req.wantHost;
    wantLeave     = s_req.wantLeave;
    wantInvite    = s_req.wantInvite;
    wantJoin      = s_req.wantJoin;
    presenceDirty = s_req.presenceDirty;
    SDL_strlcpy(area, s_req.presenceArea, sizeof(area));
    s_req.wantHost      = 0;
    s_req.wantLeave     = 0;
    s_req.wantInvite    = 0;
    s_req.wantJoin      = 0;
    s_req.presenceDirty = 0;
    SDL_UnlockMutex(s_lock);

    /* An invite accepted in the overlay, or +connect_lobby, outranks anything
     * the player asked for in-game: they clicked Join on a friend. */
    {
        unsigned long long pending = ShSteam_TakePendingJoin();
        if (pending)
        {
            wantJoin = pending;
            wantHost = 0;
        }
    }

    if (wantLeave)
    {
        ShSteam_LeaveLobby();
        s_memberCount = 0;
    }
    if (wantJoin)
    {
        ShSteam_JoinLobby(wantJoin);
    }
    else if (wantHost)
    {
        ShSteam_CreateLobby(g_PcConfig.onlineSteamPublic ? SHSTEAM_LOBBY_PUBLIC
                                                         : SHSTEAM_LOBBY_FRIENDSONLY,
                            g_PcConfig.onlineSteamMaxPlayers);
    }
    if (wantInvite)
    {
        ShSteam_OpenInviteOverlay();
    }

    if (presenceDirty)
    {
        /* "status" is the line a friend sees under the game name. The other
         * keys are there so a future Steam store page can use a localised
         * presence string without another code change. */
        ShSteam_SetRichPresence("status", area[0] ? area : "Silent Hill");
        ShSteam_SetRichPresence("steam_display", "#Status");
        ShSteam_SetRichPresence("area", area);
    }

    if (ShSteam_LobbyState() != SHSTEAM_LOBBY_IN)
    {
        if (s_memberCount)
        {
            s_memberCount = 0;
        }
        ShSession_Publish();
        return;
    }

    ShSession_SyncMembers();

    /* The lobby advertises what it is, so a joiner can tell a Silent Hill
     * session from any other Spacewar lobby before it commits. */
    if (ShSteam_IsLobbyOwner())
    {
        ShSteam_SetLobbyData("game", "silenthill-online");
        ShSteam_SetLobbyData("proto", "1");
    }

    if (nowMs - s_lastHelloMs >= SESSION_HELLO_MS)
    {
        s_lastHelloMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            if (s_members[i].steamId != ShSteam_SelfId())
            {
                ShSession_SendHello(s_members[i].steamId);
            }
        }
    }

    if (nowMs - s_lastPingMs >= SESSION_PING_MS)
    {
        s_lastPingMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == ShSteam_SelfId())
            {
                continue;
            }
            ShnPutU32(buf, &off, nowMs);
            ShnPutHeader(buf, SHNET_MSG_S_PING, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            ShSteam_Send(s_members[i].steamId, buf, off, 0);
            s_pingSentMs[i] = nowMs;
        }
    }

    ShSession_Receive(nowMs);

    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i].steamId == ShSteam_SelfId())
        {
            s_members[i].linked = 1;
            s_members[i].pingMs = 0;
            continue;
        }
        if (s_members[i].linked && nowMs - s_lastSeenMs[i] > SESSION_LINK_MS)
        {
            s_members[i].linked = 0;
            s_members[i].pingMs = -1;
        }
    }

    /* THE CO-OP FLAG. It is deliberately NOT "two people are in a lobby":
     * standing in a lobby together is not standing in each other's world, and
     * blocking pause for it would be wrong. It wants the world handshake that
     * joining someone's game will perform, which does not exist yet -- so
     * nothing here turns it on, and `net coop 1` is how the suppression is
     * exercised until it does. When co-op lands, this is where it goes. */

    ShSession_Publish();
}
