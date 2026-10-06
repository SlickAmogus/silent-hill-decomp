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
#include "sh_net_coopudp.h"
#include "sh_net_coop.h"
#include "pc_config.h"
#include "sh_log.h"

/* Which transport this co-op session is riding. Steam P2P by default; switched to
 * the relay server when the player hosts/joins through a server. */
#define COOP_BACKEND_STEAM 0
#define COOP_BACKEND_UDP   1
static int s_backend = COOP_BACKEND_STEAM;

/* Transport primitives, dispatched to the active backend. The session logic
 * below is written against these, so it is identical for Steam and the server. */
static unsigned long long Tp_SelfId(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_SelfId() : ShSteam_SelfId(); }
static int Tp_MemberCount(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_MemberCount() : ShSteam_MemberCount(); }
static unsigned long long Tp_Member(int i)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Member(i) : ShSteam_Member(i); }
static const char* Tp_NameOf(unsigned long long id)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_NameOf(id) : ShSteam_NameOf(id); }
static int Tp_IsHost(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_IsHost() : ShSteam_IsLobbyOwner(); }
static int Tp_Active(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Active() : (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN); }
static unsigned long long Tp_LobbyId(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_RoomId() : ShSteam_LobbyId(); }
static void Tp_Send(unsigned long long to, const shn_u8* buf, int len, int reliable)
{ if (s_backend == COOP_BACKEND_UDP) CoopUdp_Send(to, buf, len); else ShSteam_Send(to, buf, len, reliable); }
static int Tp_Recv(unsigned long long* from, shn_u8* buf, int cap)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Recv(from, buf, cap) : ShSteam_Recv(from, buf, cap); }

/* Round trip to each member, and how long before a silent member is no longer
 * called linked. A session is a handful of friends, so this can be leisurely. */
#define SESSION_PING_MS   2000
#define SESSION_LINK_MS   8000
#define SESSION_HELLO_MS  5000
#define SESSION_POS_MS    100  /* co-op presence broadcast rate (~10 Hz) */

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

    /* Local player pose for the co-op presence broadcast. poseValid is cleared
     * when not in a map so no pose goes out from the menus. */
    int                poseValid;
    int                poseMap, poseChara, poseFlags;
    int                poseX, poseY, poseZ;
    short              poseRotY;
    unsigned short     poseAnim, poseFrame;

    int                wantWorldMap; /* host: send S_WORLD(map) to guests; -1 none */

    int                hostGuestDebug; /* host: allow joined players debug controls (0 default) */

    /* Co-op over a relay server (vs Steam). Set by the server host/join/list
     * requests; the Tick arms the UDP backend from them. */
    char               coopHost[80];
    int                coopPort;
    int                coopMax;
    int                coopJoinCode;
    int                wantHostServer;
    int                wantJoinServer;
    int                wantListServer;
} s_req;

static struct
{
    int                active;
    int                isHost;
    unsigned long long lobbyId;
    int                memberCount;
    ShSessionMember    members[SHSESSION_MAX_MEMBERS];
    char               status[128];
    int                worldReq;    /* guest: a map the host said to boot into, -1 none */
    int                guestDebug;  /* host granted joined players debug controls */
} s_pub;

/* Worker-private. */
static ShSessionMember s_members[SHSESSION_MAX_MEMBERS];
static int             s_memberCount;
static unsigned int    s_lastPingMs;
static unsigned int    s_lastHelloMs;
static unsigned int    s_lastSeenMs[SHSESSION_MAX_MEMBERS];
static unsigned int    s_pingSentMs[SHSESSION_MAX_MEMBERS];
static unsigned int    s_lastPosMs;   /* last co-op pose broadcast */
static int             s_worldReqIn = -1; /* a map the host told us to boot into */
static int             s_enabled;

/* Co-op shared-item queues, both under s_lock. Out: local pickups waiting for the
 * worker to broadcast. In: pickups received from members, waiting for the game
 * thread to grant. A handful of pickups a room, so a small ring is plenty. */
#define SESSION_ITEM_Q 32
typedef struct { shn_u8 id; shn_u16 n; } ShSessionItem;
static ShSessionItem s_itemOut[SESSION_ITEM_Q];
static int           s_itemOutCount;
static ShSessionItem s_itemIn[SESSION_ITEM_Q];
static int           s_itemInCount;

/* Host: the map guests should be booted into, kept persistent (not one-shot) so a
 * player who joins AFTER the host started still gets sent in -- join-in-progress.
 * s_worldSent[i] is "member i has been sent the current world"; a map change
 * clears it so everyone is re-sent, and a fresh joiner's stays 0 so only they
 * are sent. -1 = host is not in a game yet (lobby only). */
static int           s_worldMapCur = -1;
static int           s_worldSent[SHSESSION_MAX_MEMBERS];

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
    s_req.presenceMap  = -1;
    s_req.wantWorldMap = -1;
    s_pub.worldReq     = -1;

    /* The server (UDP) co-op transport does not need Steam, so it always comes
     * up. Steam is a second transport on top: ShSteam_Init fails gracefully
     * (returns 0) with no steam_api64.dll or no Steam running, and then only the
     * "join a friend" path is unavailable -- server rooms still work. */
    CoopUdp_Init();

    s_enabled = ShSteam_Init((unsigned int)g_PcConfig.onlineSteamAppId);
    if (!s_enabled)
    {
        SDL_strlcpy(s_pub.status, "Steam unavailable (server co-op still works)",
                    sizeof(s_pub.status));
    }
    /* A lobby id on the command line means the player clicked Join on a
     * friend before the game existed; honour it as soon as we are up. */
    else if (g_PcConfig.onlineSteamAutoHost)
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
    CoopUdp_Shutdown();
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

void ShSession_RequestHostServer(const char* host, int port, int maxPlayers)
{
    if (!s_lock || !host || !host[0]) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopMax        = maxPlayers;
    s_req.wantHostServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestJoinServer(const char* host, int port, int code)
{
    if (!s_lock || !host || !host[0] || code <= 0) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopJoinCode   = code;
    s_req.wantJoinServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestListServer(const char* host, int port)
{
    if (!s_lock || !host || !host[0]) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.wantListServer = 1;
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

void ShSession_PublishLocalPos(int mapIdx, int charaId, int flags,
                               int x, int y, int z, short rotY,
                               unsigned short anim, unsigned short frame)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    if (mapIdx < 0)
    {
        s_req.poseValid = 0;
    }
    else
    {
        s_req.poseValid = 1;
        s_req.poseMap   = mapIdx;
        s_req.poseChara = charaId;
        s_req.poseFlags = flags;
        s_req.poseX     = x;
        s_req.poseY     = y;
        s_req.poseZ     = z;
        s_req.poseRotY  = rotY;
        s_req.poseAnim  = anim;
        s_req.poseFrame = frame;
    }
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestWorld(int mapIdx)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantWorldMap = mapIdx;
    SDL_UnlockMutex(s_lock);
}

int ShSession_TakeWorldRequest(void)
{
    int map = -1;
    if (!s_lock) return -1;
    SDL_LockMutex(s_lock);
    map            = s_pub.worldReq;
    s_pub.worldReq = -1;
    SDL_UnlockMutex(s_lock);
    return map;
}

void ShSession_QueueItem(int itemId, int count)
{
    if (!s_lock || itemId < 0 || itemId > 255 || count <= 0) return;
    SDL_LockMutex(s_lock);
    if (s_itemOutCount < SESSION_ITEM_Q)
    {
        s_itemOut[s_itemOutCount].id = (shn_u8)itemId;
        s_itemOut[s_itemOutCount].n  = (shn_u16)(count > 65535 ? 65535 : count);
        s_itemOutCount++;
    }
    SDL_UnlockMutex(s_lock);
}

int ShSession_TakeItem(int* itemId, int* count)
{
    int got = 0;
    if (!s_lock) return 0;
    SDL_LockMutex(s_lock);
    if (s_itemInCount > 0)
    {
        if (itemId) *itemId = s_itemIn[0].id;
        if (count)  *count  = s_itemIn[0].n;
        s_itemInCount--;
        memmove(s_itemIn, s_itemIn + 1, (size_t)s_itemInCount * sizeof(s_itemIn[0]));
        got = 1;
    }
    SDL_UnlockMutex(s_lock);
    return got;
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

void ShSession_SetGuestDebug(int on)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.hostGuestDebug = on ? 1 : 0;
    SDL_UnlockMutex(s_lock);
}

int ShSession_GuestDebugGranted(void)
{
    return s_pub.guestDebug;
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
    int             freshWorld[SHSESSION_MAX_MEMBERS];
    int             n = Tp_MemberCount();
    int             count = 0;
    int             i;

    if (n > SHSESSION_MAX_MEMBERS)
    {
        n = SHSESSION_MAX_MEMBERS;
    }
    memset(fresh, 0, sizeof(fresh));
    memset(freshSeen, 0, sizeof(freshSeen));
    memset(freshSent, 0, sizeof(freshSent));
    memset(freshWorld, 0, sizeof(freshWorld)); /* a new member has not been sent the world */

    for (i = 0; i < n; i++)
    {
        unsigned long long id  = Tp_Member(i);
        int                old = ShSession_IndexOf(id);
        const char*        nm;

        if (!id)
        {
            continue;
        }
        if (old >= 0)
        {
            fresh[count]      = s_members[old];
            freshSeen[count]  = s_lastSeenMs[old];
            freshSent[count]  = s_pingSentMs[old];
            freshWorld[count] = s_worldSent[old];
        }
        else
        {
            fresh[count].steamId = id;
            fresh[count].pingMs  = -1;
            fresh[count].mapIdx  = -1;
            fresh[count].linked  = 0;
        }
        nm = Tp_NameOf(id);
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
    memcpy(s_worldSent, freshWorld, sizeof(s_worldSent));
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
        int     n = Tp_Recv(&from, buf, (int)sizeof(buf));

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
        case SHNET_MSG_S_POS:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           m;
            if (payLen < 22)
            {
                break;
            }
            m                      = (int)ShnGetU8(p, &o);
            s_members[idx].mapIdx  = (m == 0xFF) ? -1 : m;
            s_members[idx].charaId = (int)ShnGetU8(p, &o);
            s_members[idx].flags   = (int)ShnGetU8(p, &o);
            (void)ShnGetU8(p, &o); /* pad */
            s_members[idx].x       = (int)ShnGetS32(p, &o);
            s_members[idx].y       = (int)ShnGetS32(p, &o);
            s_members[idx].z       = (int)ShnGetS32(p, &o);
            s_members[idx].rotY    = (short)ShnGetS16(p, &o);
            s_members[idx].anim    = (unsigned short)ShnGetU16(p, &o);
            s_members[idx].frame   = (unsigned short)ShnGetU16(p, &o);
            s_members[idx].poseMs  = now;
            break;
        }
        case SHNET_MSG_S_WORLD:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           m;
            if (payLen < 1)
            {
                break;
            }
            m = (int)ShnGetU8(p, &o);
            s_worldReqIn = (m == 0xFF) ? -1 : m;
            SH_DBG("[SESSION] host says: boot into map %d", s_worldReqIn);
            break;
        }
        case SHNET_MSG_S_ITEM:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           id, n;
            if (payLen < 4)
            {
                break;
            }
            id = (int)ShnGetU8(p, &o);
            (void)ShnGetU8(p, &o); /* pad */
            n  = (int)ShnGetU16(p, &o);
            SDL_LockMutex(s_lock);
            if (s_itemInCount < SESSION_ITEM_Q && n > 0)
            {
                s_itemIn[s_itemInCount].id = (shn_u8)id;
                s_itemIn[s_itemInCount].n  = (shn_u16)n;
                s_itemInCount++;
            }
            SDL_UnlockMutex(s_lock);
            SH_DBG("[SESSION] %s picked up item %d x%d", s_members[idx].name, id, n);
            break;
        }
        default:
            break;
        }
    }
}

static void ShSession_Publish(void)
{
    int i;

    SDL_LockMutex(s_lock);
    s_pub.active      = Tp_Active();
    s_pub.isHost      = Tp_IsHost();
    s_pub.lobbyId     = Tp_LobbyId();
    s_pub.memberCount = s_memberCount;
    for (i = 0; i < s_memberCount; i++)
    {
        s_pub.members[i] = s_members[i];
    }
    if (s_backend == COOP_BACKEND_UDP) CoopUdp_StatusLine(s_pub.status, (int)sizeof(s_pub.status));
    else                               ShSteam_StatusLine(s_pub.status, (int)sizeof(s_pub.status));
    /* Hand a received world-boot request to the game thread. Only overwrite when
     * there is a new one, so a request already waiting to be taken is not lost. */
    if (s_worldReqIn >= 0)
    {
        s_pub.worldReq = s_worldReqIn;
        s_worldReqIn   = -1;
    }
    /* The host's debug-grant, read off the Steam lobby data (host reads back its
     * own value, a guest reads the host's). Steam path only for now; 0 otherwise. */
    if (s_pub.active && s_backend == COOP_BACKEND_STEAM)
    {
        const char* d  = ShSteam_GetLobbyData("dbg");
        s_pub.guestDebug = (d && d[0] == '1') ? 1 : 0;
    }
    else
    {
        s_pub.guestDebug = 0;
    }
    SDL_UnlockMutex(s_lock);
}

void ShSession_Tick(unsigned int nowMs)
{
    int                wantHost, wantLeave, wantInvite, presenceDirty;
    unsigned long long wantJoin;
    char               area[64];
    int                i;

    int                poseValid, poseMap, poseChara, poseFlags;
    int                poseX, poseY, poseZ, wantWorldMap;
    short              poseRotY;
    unsigned short     poseAnim, poseFrame;

    ShSessionItem      itemOut[SESSION_ITEM_Q];
    int                itemOutN = 0;
    int                q;
    int                hostGuestDebug;

    char               coopHost[80];
    int                coopPort, coopMax, coopJoinCode;
    int                wantHostServer, wantJoinServer, wantListServer;

    if (!s_lock)
    {
        return;
    }

    /* Pump both transports: Steam for its overlay/invites (only if it came up),
     * the UDP relay always (it no-ops until a server is armed). */
    if (s_enabled) ShSteam_RunCallbacks();
    CoopUdp_Tick(nowMs);

    SDL_LockMutex(s_lock);
    wantHost      = s_req.wantHost;
    wantLeave     = s_req.wantLeave;
    wantInvite    = s_req.wantInvite;
    wantJoin      = s_req.wantJoin;
    presenceDirty = s_req.presenceDirty;
    SDL_strlcpy(area, s_req.presenceArea, sizeof(area));
    poseValid = s_req.poseValid;
    poseMap   = s_req.poseMap;  poseChara = s_req.poseChara; poseFlags = s_req.poseFlags;
    poseX     = s_req.poseX;    poseY     = s_req.poseY;     poseZ     = s_req.poseZ;
    poseRotY  = s_req.poseRotY; poseAnim  = s_req.poseAnim;  poseFrame = s_req.poseFrame;
    wantWorldMap        = s_req.wantWorldMap;
    hostGuestDebug      = s_req.hostGuestDebug; /* persistent: snapshot, do not reset */
    SDL_strlcpy(coopHost, s_req.coopHost, sizeof(coopHost));
    coopPort        = s_req.coopPort;
    coopMax         = s_req.coopMax;
    coopJoinCode    = s_req.coopJoinCode;
    wantHostServer  = s_req.wantHostServer;
    wantJoinServer  = s_req.wantJoinServer;
    wantListServer  = s_req.wantListServer;
    itemOutN            = s_itemOutCount;
    if (itemOutN > 0)
    {
        memcpy(itemOut, s_itemOut, (size_t)itemOutN * sizeof(itemOut[0]));
        s_itemOutCount = 0;
    }
    s_req.wantHost      = 0;
    s_req.wantLeave     = 0;
    s_req.wantInvite    = 0;
    s_req.wantJoin      = 0;
    s_req.presenceDirty = 0;
    s_req.wantWorldMap  = -1;
    s_req.wantHostServer = 0;
    s_req.wantJoinServer = 0;
    s_req.wantListServer = 0;
    SDL_UnlockMutex(s_lock);

    /* A Steam invite accepted in the overlay outranks an in-game Steam host,
     * but not an explicit server host/join the player just chose. */
    if (s_enabled && !wantHostServer && !wantJoinServer)
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
        if (s_backend == COOP_BACKEND_UDP) CoopUdp_Leave();
        else if (s_enabled)                ShSteam_LeaveLobby();
        s_memberCount = 0;
    }

    /* Host/join routes the session onto a transport. A server choice switches to
     * the UDP backend (leaving any Steam lobby first); a Steam choice switches
     * back (leaving any server room first). The living world is untouched. */
    if (wantJoinServer)
    {
        if (s_enabled) ShSteam_LeaveLobby();
        s_backend = COOP_BACKEND_UDP;
        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
        CoopUdp_JoinRoom((unsigned short)coopJoinCode);
    }
    else if (wantHostServer)
    {
        if (s_enabled) ShSteam_LeaveLobby();
        s_backend = COOP_BACKEND_UDP;
        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
        CoopUdp_CreateRoom(coopMax);
    }
    else if (wantJoin && s_enabled)
    {
        CoopUdp_Disconnect();
        s_backend = COOP_BACKEND_STEAM;
        ShSteam_JoinLobby(wantJoin);
    }
    else if (wantHost && s_enabled)
    {
        CoopUdp_Disconnect();
        s_backend = COOP_BACKEND_STEAM;
        ShSteam_CreateLobby(g_PcConfig.onlineSteamPublic ? SHSTEAM_LOBBY_PUBLIC
                                                         : SHSTEAM_LOBBY_FRIENDSONLY,
                            g_PcConfig.onlineSteamMaxPlayers);
    }

    /* A room-list query does not change the active transport. */
    if (wantListServer)
    {
        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
        CoopUdp_RequestRoomList();
    }

    if (wantInvite && s_backend == COOP_BACKEND_STEAM && s_enabled)
    {
        ShSteam_OpenInviteOverlay();
    }

    if (presenceDirty && s_backend == COOP_BACKEND_STEAM && s_enabled)
    {
        /* "status" is the line a friend sees under the game name. */
        ShSteam_SetRichPresence("status", area[0] ? area : "Silent Hill");
        ShSteam_SetRichPresence("steam_display", "#Status");
        ShSteam_SetRichPresence("area", area);
    }

    if (!Tp_Active())
    {
        if (s_memberCount)
        {
            s_memberCount = 0;
        }
        s_worldMapCur = -1;
        memset(s_worldSent, 0, sizeof(s_worldSent));
        ShSession_Publish();
        return;
    }

    ShSession_SyncMembers();

    /* Steam only: advertise the lobby (so a joiner can tell a Silent Hill session
     * from any other Spacewar lobby, its state, and the debug grant). The server
     * backend gets all of this from the room roster instead. */
    if (s_backend == COOP_BACKEND_STEAM && ShSteam_IsLobbyOwner())
    {
        ShSteam_SetLobbyData("game", "silenthill-online");
        ShSteam_SetLobbyData("proto", "1");
        ShSteam_SetLobbyData("state", s_worldMapCur >= 0 ? "ingame" : "lobby");
        ShSteam_SetLobbyData("dbg", hostGuestDebug ? "1" : "0");
    }

    /* Steam only: S_HELLO carries each peer's name/map. On the server backend the
     * roster already supplies names, so no hello is needed. */
    if (s_backend == COOP_BACKEND_STEAM && nowMs - s_lastHelloMs >= SESSION_HELLO_MS)
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
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU32(buf, &off, nowMs);
            ShnPutHeader(buf, SHNET_MSG_S_PING, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 0);
            s_pingSentMs[i] = nowMs;
        }
    }

    /* Host -> guests: boot into the co-op map (reliable). The map is remembered
     * (s_worldMapCur) and a fresh map change clears every member's sent-flag, so
     * this one loop handles both the start broadcast AND a player who joins
     * mid-game: anyone who has not yet been sent the current world gets it now. */
    if (wantWorldMap >= 0 && wantWorldMap != s_worldMapCur)
    {
        s_worldMapCur = wantWorldMap;
        memset(s_worldSent, 0, sizeof(s_worldSent));
        SH_DBG("[SESSION] co-op world is now map %d", s_worldMapCur);
    }
    if (Tp_IsHost() && s_worldMapCur >= 0)
    {
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId() || s_worldSent[i])
            {
                continue;
            }
            ShnPutU8(buf, &off, (shn_u8)((s_worldMapCur > 255) ? 0xFF : s_worldMapCur));
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, 0);
            ShnPutHeader(buf, SHNET_MSG_S_WORLD, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 1);
            s_worldSent[i] = 1;
            SH_DBG("[SESSION] sent world (map %d) to %s", s_worldMapCur, s_members[i].name);
        }
    }

    /* Co-op presence: broadcast our pose, unreliable, throttled. */
    if (poseValid && nowMs - s_lastPosMs >= SESSION_POS_MS)
    {
        s_lastPosMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 24];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU8(buf, &off, (shn_u8)((poseMap > 255) ? 0xFF : poseMap));
            ShnPutU8(buf, &off, (shn_u8)poseChara);
            ShnPutU8(buf, &off, (shn_u8)poseFlags);
            ShnPutU8(buf, &off, 0);
            ShnPutS32(buf, &off, poseX);
            ShnPutS32(buf, &off, poseY);
            ShnPutS32(buf, &off, poseZ);
            ShnPutS16(buf, &off, poseRotY);
            ShnPutU16(buf, &off, poseAnim);
            ShnPutU16(buf, &off, poseFrame);
            ShnPutHeader(buf, SHNET_MSG_S_POS, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 0);
        }
    }

    /* Co-op shared items: a local pickup goes to everyone, reliable (a dropped
     * grant means a missing key). */
    for (q = 0; q < itemOutN; q++)
    {
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU8(buf, &off, itemOut[q].id);
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, itemOut[q].n);
            ShnPutHeader(buf, SHNET_MSG_S_ITEM, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 1);
        }
        SH_DBG("[SESSION] shared pickup item %d x%d to session", itemOut[q].id, itemOut[q].n);
    }

    ShSession_Receive(nowMs);

    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i].steamId == Tp_SelfId())
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
