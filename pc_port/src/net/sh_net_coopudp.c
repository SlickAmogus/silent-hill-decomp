/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_coopudp.c - co-op over the relay server. See sh_net_coopudp.h.
 *
 * Worker-thread only (pumped from ShSession_Tick). One UDP socket, the living
 * world's HELLO/WELCOME handshake, then ROOM_CREATE / ROOM_JOIN and COOP_RELAY.
 */

#include <string.h>
#include <stdio.h>

#include <SDL.h>

#include "sh_net_proto.h"
#include "sh_net_platform.h"
#include "sh_net_coopudp.h"
#include "pc_config.h"
#include "sh_log.h"

#define COOPUDP_MAX_MEMBERS 8
#define COOPUDP_RX_RING     64
#define COOPUDP_MAX_ROOMS   40

#define COOPUDP_HELLO_MS    1000u
#define COOPUDP_PING_MS     2000u
#define COOPUDP_TIMEOUT_MS  10000u

typedef struct { shn_u32 id; char name[SHNET_NAME_MAX]; } CoopMember;
typedef struct { shn_u32 from; int len; shn_u8 buf[SHNET_MAX_PAYLOAD]; } CoopRx;
typedef struct { shn_u16 code; shn_u8 players, max; char host[SHNET_NAME_MAX]; } CoopRoom;

static int          s_inited;
static ShNetSock*   s_sock;
static ShNetAddr    s_server;
static int          s_haveServer;
static int          s_connected;
static shn_u32      s_session;
static shn_u32      s_selfId;
static char         s_name[SHNET_NAME_MAX];

static unsigned int s_lastHelloMs;
static unsigned int s_lastPingMs;
static unsigned int s_lastRxMs;

/* Deferred until the WELCOME lands: 0 none, else a create (max in s_pendCreate)
 * or a join (code in s_pendJoin). */
static int          s_pendCreate;
static shn_u16      s_pendJoin;
static int          s_pendJoinSet; /* separate flag: code 0 is a valid "any room" join */
static int          s_pendList;

static shn_u16      s_room;
static shn_u32      s_hostId;
static CoopMember   s_members[COOPUDP_MAX_MEMBERS];
static int          s_memberCount;

static CoopRx       s_rx[COOPUDP_RX_RING];
static int          s_rxHead, s_rxTail;

static CoopRoom     s_rooms[COOPUDP_MAX_ROOMS];
static int          s_roomCount;

static char         s_status[128];

/* ------------------------------------------------------------------ */

static void CoopUdp_SetStatus(const char* s)
{
    SDL_strlcpy(s_status, s ? s : "", sizeof(s_status));
}

static void CoopUdp_SendRaw(const shn_u8* buf, int len)
{
    if (s_sock && s_haveServer)
    {
        ShNetPlat_Send(s_sock, &s_server, buf, len);
    }
}

static void CoopUdp_SendHello(void)
{
    shn_u8 buf[SHNET_HDR_SIZE + 2 + 1 + 1 + 4 + SHNET_NAME_MAX + SHNET_BUILD_MAX + 4];
    int    off = SHNET_HDR_SIZE;
    ShnPutU16(buf, &off, SHNET_PROTO_VER);
    ShnPutU8(buf, &off, 0);
    ShnPutU8(buf, &off, 0);              /* charaId; unused for co-op */
    ShnPutU32(buf, &off, s_session);     /* reconnect (0 while fresh) */
    ShnPutStr(buf, &off, s_name, SHNET_NAME_MAX);
    ShnPutStr(buf, &off, "shpc-online-1", SHNET_BUILD_MAX);
    ShnPutU32(buf, &off, 0);             /* no password on the co-op relay */
    ShnPutHeader(buf, SHNET_MSG_HELLO, (shn_u16)(off - SHNET_HDR_SIZE), s_session);
    CoopUdp_SendRaw(buf, off);
}

static void CoopUdp_SendSimple(int type)
{
    shn_u8 buf[SHNET_HDR_SIZE];
    ShnPutHeader(buf, (shn_u8)type, 0, s_session);
    CoopUdp_SendRaw(buf, SHNET_HDR_SIZE);
}

static void CoopUdp_DoCreate(int maxPlayers)
{
    shn_u8 buf[SHNET_HDR_SIZE + 1];
    int    off = SHNET_HDR_SIZE;
    ShnPutU8(buf, &off, (shn_u8)(maxPlayers < 2 ? 2 : (maxPlayers > COOPUDP_MAX_MEMBERS ? COOPUDP_MAX_MEMBERS : maxPlayers)));
    ShnPutHeader(buf, SHNET_MSG_ROOM_CREATE, (shn_u16)(off - SHNET_HDR_SIZE), s_session);
    CoopUdp_SendRaw(buf, off);
}

static void CoopUdp_DoJoin(shn_u16 code)
{
    shn_u8 buf[SHNET_HDR_SIZE + 2];
    int    off = SHNET_HDR_SIZE;
    ShnPutU16(buf, &off, code);
    ShnPutHeader(buf, SHNET_MSG_ROOM_JOIN, (shn_u16)(off - SHNET_HDR_SIZE), s_session);
    CoopUdp_SendRaw(buf, off);
}

static void CoopUdp_FlushPending(void)
{
    if (s_pendCreate) { CoopUdp_DoCreate(s_pendCreate); s_pendCreate = 0; }
    if (s_pendJoinSet) { CoopUdp_DoJoin(s_pendJoin);     s_pendJoin = 0; s_pendJoinSet = 0; }
    if (s_pendList)   { CoopUdp_SendSimple(SHNET_MSG_ROOM_LIST_REQ); s_pendList = 0; }
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

static void CoopUdp_OnWelcome(const shn_u8* p, int len)
{
    int off = 0;
    if (len < 8) return;
    s_session = ShnGetU32(p, &off);
    s_selfId  = ShnGetU32(p, &off);
    s_connected = 1;
    CoopUdp_SetStatus("Connected");
    SH_DBG("[COOPUDP] connected (id %u)", s_selfId);
    CoopUdp_FlushPending();
}

static void CoopUdp_ReadRoster(const shn_u8* p, int len, int joined)
{
    int off = 0, i, n;
    if (joined) { if (len < 4) return; (void)ShnGetU32(p, &off); } /* yourId (== selfId) */
    if (off + 2 + 4 + 1 > len) return;
    s_room   = ShnGetU16(p, &off);
    s_hostId = ShnGetU32(p, &off);
    n        = (int)ShnGetU8(p, &off);
    if (n > COOPUDP_MAX_MEMBERS) n = COOPUDP_MAX_MEMBERS;
    s_memberCount = 0;
    for (i = 0; i < n; i++)
    {
        shn_u32 id;
        char    name[SHNET_NAME_MAX];
        if (off + 4 + 1 > len) break;
        id = ShnGetU32(p, &off);
        ShnGetStr(p, &off, SHNET_NAME_MAX, name, SHNET_NAME_MAX);
        (void)ShnGetU8(p, &off); /* isHost flag (derivable from s_hostId) */
        s_members[s_memberCount].id = id;
        SDL_strlcpy(s_members[s_memberCount].name, name, SHNET_NAME_MAX);
        s_memberCount++;
    }
    if (s_hostId == 0)
    {
        /* Host left: the room ended. */
        s_room = 0; s_memberCount = 0;
        CoopUdp_SetStatus("Host left");
        SH_DBG("[COOPUDP] room ended (host left)");
    }
}

static void CoopUdp_OnReject(const shn_u8* p, int len)
{
    int  off = 0;
    char text[SHNET_REJECT_MAX];
    if (len < 1) return;
    (void)ShnGetU8(p, &off);
    ShnGetStr(p, &off, SHNET_REJECT_MAX, text, SHNET_REJECT_MAX);
    s_room = 0; s_memberCount = 0;
    CoopUdp_SetStatus(text[0] ? text : "Room refused");
    SH_DBG("[COOPUDP] room refused: %s", text);
}

static void CoopUdp_OnRoomList(const shn_u8* p, int len)
{
    int off = 0, i, n;
    if (len < 1) return;
    n = (int)ShnGetU8(p, &off);
    if (n > COOPUDP_MAX_ROOMS) n = COOPUDP_MAX_ROOMS;
    s_roomCount = 0;
    for (i = 0; i < n; i++)
    {
        if (off + 2 + 1 + 1 > len) break;
        s_rooms[s_roomCount].code    = ShnGetU16(p, &off);
        s_rooms[s_roomCount].players = ShnGetU8(p, &off);
        s_rooms[s_roomCount].max     = ShnGetU8(p, &off);
        ShnGetStr(p, &off, SHNET_NAME_MAX, s_rooms[s_roomCount].host, SHNET_NAME_MAX);
        s_roomCount++;
    }
}

static void CoopUdp_OnRelay(const shn_u8* p, int len)
{
    int     off = 0, innerLen, next;
    shn_u32 from;
    if (len < 4) return;
    from     = ShnGetU32(p, &off);
    innerLen = len - off;
    if (innerLen <= 0 || innerLen > SHNET_MAX_PAYLOAD) return;
    next = (s_rxHead + 1) % COOPUDP_RX_RING;
    if (next == s_rxTail) /* full: drop oldest */
    {
        s_rxTail = (s_rxTail + 1) % COOPUDP_RX_RING;
    }
    s_rx[s_rxHead].from = from;
    s_rx[s_rxHead].len  = innerLen;
    memcpy(s_rx[s_rxHead].buf, p + off, (size_t)innerLen);
    s_rxHead = next;
}

static void CoopUdp_Drain(unsigned int now)
{
    shn_u8    buf[SHNET_MTU];
    ShNetAddr from;
    int       budget = 64;

    while (budget-- > 0)
    {
        shn_u8  type;
        shn_u16 payLen;
        shn_u32 session;
        const shn_u8* pay;
        int n = ShNetPlat_Recv(s_sock, &from, buf, (int)sizeof(buf));
        if (n <= 0) break;
        if (!ShnParseHeader(buf, n, &type, &payLen, &session)) continue;
        pay = buf + SHNET_HDR_SIZE;
        s_lastRxMs = now;
        switch (type)
        {
        case SHNET_MSG_WELCOME:     CoopUdp_OnWelcome(pay, (int)payLen);     break;
        case SHNET_MSG_REJECT:      CoopUdp_SetStatus("Server refused the connection"); break;
        case SHNET_MSG_ROOM_JOINED: CoopUdp_ReadRoster(pay, (int)payLen, 1); break;
        case SHNET_MSG_ROOM_ROSTER: CoopUdp_ReadRoster(pay, (int)payLen, 0); break;
        case SHNET_MSG_ROOM_REJECT: CoopUdp_OnReject(pay, (int)payLen);      break;
        case SHNET_MSG_ROOM_LIST:   CoopUdp_OnRoomList(pay, (int)payLen);    break;
        case SHNET_MSG_COOP_RELAY:  CoopUdp_OnRelay(pay, (int)payLen);       break;
        case SHNET_MSG_PONG:        break;
        default:                    break; /* ignore living-world traffic */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

void CoopUdp_Init(void)
{
    if (s_inited) return;
    if (!ShNetPlat_Init()) { SH_DBG("[COOPUDP] platform init failed"); return; }
    s_sock = ShNetPlat_Open();
    if (!s_sock) { SH_DBG("[COOPUDP] socket open failed"); return; }
    s_inited = 1;
}

void CoopUdp_Shutdown(void)
{
    if (!s_inited) return;
    if (s_room && s_connected) CoopUdp_SendSimple(SHNET_MSG_ROOM_LEAVE);
    if (s_sock) { ShNetPlat_Close(s_sock); s_sock = NULL; }
    s_inited = 0;
}

int CoopUdp_SetServer(const char* host, unsigned short port)
{
    ShNetAddr a;
    if (!s_inited || !host || !host[0]) return 0;
    if (!ShNetPlat_Resolve(host, port, &a)) { CoopUdp_SetStatus("Cannot resolve server"); return 0; }
    /* Changing servers resets everything. */
    s_server      = a;
    s_haveServer  = 1;
    s_connected   = 0;
    s_session     = 0;
    s_selfId      = 0;
    s_room        = 0;
    s_memberCount = 0;
    s_rxHead = s_rxTail = 0;
    s_lastHelloMs = 0;
    {
        const char* nm = g_PcConfig.onlineName;
        SDL_strlcpy(s_name, (nm && nm[0]) ? nm : "Player", sizeof(s_name));
    }
    CoopUdp_SetStatus("Connecting...");
    return 1;
}

void CoopUdp_CreateRoom(int maxPlayers)
{
    if (!s_haveServer) return;
    if (s_connected) CoopUdp_DoCreate(maxPlayers);
    else             s_pendCreate = maxPlayers;
}

void CoopUdp_JoinRoom(unsigned short code)
{
    if (!s_haveServer) return;
    if (s_connected) CoopUdp_DoJoin(code);
    else             { s_pendJoin = code; s_pendJoinSet = 1; }
}

void CoopUdp_RequestRoomList(void)
{
    if (!s_haveServer) return;
    if (s_connected) CoopUdp_SendSimple(SHNET_MSG_ROOM_LIST_REQ);
    else             s_pendList = 1;
}

void CoopUdp_Leave(void)
{
    if (s_connected && s_room) CoopUdp_SendSimple(SHNET_MSG_ROOM_LEAVE);
    s_room = 0; s_memberCount = 0;
    s_pendCreate = 0; s_pendJoin = 0; s_pendJoinSet = 0;
}

void CoopUdp_Disconnect(void)
{
    CoopUdp_Leave();
    s_haveServer = 0;
    s_connected  = 0;
    s_session    = 0;
    s_selfId     = 0;
    CoopUdp_SetStatus("");
}

void CoopUdp_Tick(unsigned int nowMs)
{
    if (!s_inited || !s_haveServer) return;

    if (!s_connected)
    {
        if (nowMs - s_lastHelloMs >= COOPUDP_HELLO_MS)
        {
            s_lastHelloMs = nowMs;
            CoopUdp_SendHello();
        }
    }
    else
    {
        if (nowMs - s_lastPingMs >= COOPUDP_PING_MS)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            s_lastPingMs = nowMs;
            ShnPutU32(buf, &off, nowMs);
            ShnPutHeader(buf, SHNET_MSG_PING, (shn_u16)(off - SHNET_HDR_SIZE), s_session);
            CoopUdp_SendRaw(buf, off);
        }
        if (s_lastRxMs && nowMs - s_lastRxMs > COOPUDP_TIMEOUT_MS)
        {
            /* Lost the server: fall back to reconnecting. */
            s_connected = 0;
            s_room = 0; s_memberCount = 0;
            CoopUdp_SetStatus("Lost the server, reconnecting...");
        }
    }

    CoopUdp_Drain(nowMs);
}

int                CoopUdp_Connected(void) { return s_connected; }
int                CoopUdp_Active(void)    { return s_connected && s_room != 0; }
int                CoopUdp_IsHost(void)    { return s_room != 0 && s_hostId == s_selfId; }
unsigned long long CoopUdp_SelfId(void)    { return (unsigned long long)s_selfId; }
unsigned long long CoopUdp_RoomId(void)    { return (unsigned long long)s_room; }
int                CoopUdp_MemberCount(void) { return s_memberCount; }

unsigned long long CoopUdp_Member(int i)
{
    return (i >= 0 && i < s_memberCount) ? (unsigned long long)s_members[i].id : 0;
}

const char* CoopUdp_NameOf(unsigned long long id)
{
    int i;
    for (i = 0; i < s_memberCount; i++)
        if ((unsigned long long)s_members[i].id == id) return s_members[i].name;
    return "";
}

void CoopUdp_StatusLine(char* out, int cap)
{
    if (!out || cap <= 0) return;
    if (s_room)
    {
        snprintf(out, cap, "Room %u  -  %d player%s%s", (unsigned)s_room,
                 s_memberCount, s_memberCount == 1 ? "" : "s",
                 CoopUdp_IsHost() ? "  (hosting)" : "");
    }
    else
    {
        SDL_strlcpy(out, s_status, (size_t)cap);
    }
}

int CoopUdp_Send(unsigned long long to, const unsigned char* buf, int len)
{
    shn_u8 out[SHNET_MTU];
    int    off = SHNET_HDR_SIZE;
    if (!s_connected || !s_room || len <= 0) return 0;
    if (len > SHNET_MAX_PAYLOAD - 4) return 0;
    ShnPutU32(out, &off, (shn_u32)to);           /* target member, 0 = whole room */
    memcpy(out + off, buf, (size_t)len); off += len;
    ShnPutHeader(out, SHNET_MSG_COOP_RELAY, (shn_u16)(off - SHNET_HDR_SIZE), s_session);
    CoopUdp_SendRaw(out, off);
    return len;
}

int CoopUdp_Recv(unsigned long long* from, unsigned char* buf, int cap)
{
    int len;
    if (s_rxTail == s_rxHead) return 0;
    len = s_rx[s_rxTail].len;
    if (len > cap) len = cap;
    if (from) *from = (unsigned long long)s_rx[s_rxTail].from;
    memcpy(buf, s_rx[s_rxTail].buf, (size_t)len);
    s_rxTail = (s_rxTail + 1) % COOPUDP_RX_RING;
    return len;
}

int CoopUdp_RoomListCount(void) { return s_roomCount; }

int CoopUdp_RoomListGet(int i, unsigned short* code, int* players, int* max,
                        char* host, int hostCap)
{
    if (i < 0 || i >= s_roomCount) return 0;
    if (code)    *code    = s_rooms[i].code;
    if (players) *players = s_rooms[i].players;
    if (max)     *max     = s_rooms[i].max;
    if (host && hostCap > 0) SDL_strlcpy(host, s_rooms[i].host, (size_t)hostCap);
    return 1;
}
