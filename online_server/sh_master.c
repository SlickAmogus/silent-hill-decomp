/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_master.c - Silent Hill Online master server.
 *
 * One file, one UDP socket, one thread. It holds the roster of connected
 * players, fans each map's positions out to the players standing in it, and
 * keeps the marker store that outlives everybody's session.
 *
 * WHY IT IS THIS SMALL. The protocol is stateless per datagram and has no
 * ordering requirements (sh_net_proto.h explains why), so the server needs no
 * connection tracking beyond "when did I last hear from this address", no
 * retransmit buffers and no per-client threads. A select() loop over one
 * socket serves a few hundred players on a home connection, and a home
 * connection is the design target: this is meant to be run by one person for
 * their friends, not operated as a service.
 *
 * BUILD
 *   cmake -S online_server -B online_server/build && cmake --build online_server/build
 * or, without cmake:
 *   Windows:  gcc -O2 -I../pc_port/include sh_master.c -o sh_master.exe -lws2_32
 *   Linux:    cc  -O2 -I../pc_port/include sh_master.c -o sh_master
 *
 * RUN
 *   sh_master -n "Fog" -m "Welcome to Silent Hill"
 *   sh_master -p 27888 -w hunter2 --max 64
 *
 * The listening port must be reachable: forward UDP 27888 to this machine, or
 * keep every player on the same LAN.
 */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET    ShSock;
typedef int       ShSockLen;
#define SH_INVALID_SOCK INVALID_SOCKET
#define SH_CLOSESOCK    closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
typedef int       ShSock;
typedef socklen_t ShSockLen;
#define SH_INVALID_SOCK (-1)
#define SH_CLOSESOCK    close
#endif

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_net_proto.h"

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define SRV_MAX_CLIENTS   256
#define SRV_MAX_MEMOS     20000
#define SRV_MEMOS_PER_MAP 400
#define SRV_MEMOS_PER_USER 40
#define SRV_MAP_COUNT     64   /* e_MapIdx tops out at 42; 64 covers it */

#define SRV_TICK_MS       100  /* snapshot rate; matches the client's STATE rate */
#define SRV_TIMEOUT_MS    15000
#define SRV_SAVE_GAP_MS   30000
/* A client that floods gets its extra datagrams dropped rather than getting
 * disconnected: a burst is far more likely to be a bad network than an
 * attacker, and dropping is free. */
#define SRV_RATE_PER_SEC  120

/* Two death markers within this distance and this long apart are one death.
 * Q19.12 metres: 1.5m, 30s. */
#define SRV_DEATH_DEDUPE_DIST (((1 << 12) * 3) / 2)
#define SRV_DEATH_DEDUPE_MS   30000

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int                used;
    shn_u32            playerId;
    shn_u32            session;
    struct sockaddr_in addr;

    char               name[SHNET_NAME_MAX];
    int                mapIdx;
    int                charaId;
    int                flags;
    shn_s32            x, y, z;
    shn_s16            rotY;
    shn_s16            health;
    shn_u16            anim, frame;

    unsigned int       lastRxMs;
    unsigned int       joinMs;
    unsigned int       lastSnapMs;
    int                pingMs;

    /* Rate limiter: a packet budget refilled once a second. */
    unsigned int       rateWindowMs;
    int                rateCount;

    unsigned int       lastDeathMs;
    shn_s32            lastDeathX, lastDeathZ;
    int                memoCount;
    int                loggedSnap;
} Client;

typedef struct
{
    shn_u32 memoId;
    shn_u32 ownerId;
    char    ownerName[SHNET_NAME_MAX];
    int     kind;
    int     mapIdx;
    shn_s32 x, y, z;
    shn_s16 rotY;
    shn_s16 rating;
    shn_u16 phraseA, wordA, phraseB, wordB;
    long    placedUnix;
} Memo;

static Client        g_clients[SRV_MAX_CLIENTS];
static Memo*         g_memos;
static int           g_memoCount;
static shn_u32       g_nextMemoId = 1;
static shn_u32       g_nextPlayerId = 1;
static shn_u32       g_sessionSeed;

static ShSock        g_sock = SH_INVALID_SOCK;
static unsigned short g_port = SHNET_DEFAULT_PORT;
static char          g_serverName[SHNET_SERVER_MAX] = "Silent Hill";
static char          g_motd[SHNET_MOTD_MAX]         = "";
static shn_u32       g_passHash;
static int           g_maxClients = 64;
static int           g_featureFlags = SHNET_SF_GHOSTS | SHNET_SF_MEMOS |
                                      SHNET_SF_DEATHS | SHNET_SF_EVENTS;
static char          g_dbPath[512] = "memos.db";
static int           g_dbDirty;
static unsigned int  g_lastSaveMs;
static volatile int  g_running = 1;
static int           g_verbose;

/* ------------------------------------------------------------------ */
/* Platform odds and ends                                              */
/* ------------------------------------------------------------------ */

static unsigned int SrvMillis(void)
{
#if defined(_WIN32)
    return (unsigned int)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

static void SrvOnSignal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void SrvLog(const char* fmt, ...)
{
    va_list ap;
    time_t    t  = time(NULL);
    struct tm* lt = localtime(&t);
    char      stamp[32];

    strftime(stamp, sizeof(stamp), "%H:%M:%S", lt);
    printf("[%s] ", stamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Client table                                                        */
/* ------------------------------------------------------------------ */

static int SrvAddrEqual(const struct sockaddr_in* a, const struct sockaddr_in* b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void SrvAddrStr(const struct sockaddr_in* a, char* out, int cap)
{
    unsigned int ip = ntohl(a->sin_addr.s_addr);
    snprintf(out, (size_t)cap, "%u.%u.%u.%u:%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
             (unsigned)ntohs(a->sin_port));
}

static Client* SrvFindByAddr(const struct sockaddr_in* a)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && SrvAddrEqual(&g_clients[i].addr, a))
        {
            return &g_clients[i];
        }
    }
    return NULL;
}

static Client* SrvFindBySession(shn_u32 session)
{
    int i;
    if (session == 0)
    {
        return NULL;
    }
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && g_clients[i].session == session)
        {
            return &g_clients[i];
        }
    }
    return NULL;
}

/* Session ids authorise the reconnect path, which rebinds a live session to a
 * new address. A sequential id would make that path a takeover primitive for
 * anyone who can count, so ids come out of a seeded xorshift instead. Not
 * cryptographic; enough that guessing one is not a matter of trying id+1. */
static shn_u32 SrvNewSession(void)
{
    shn_u32 v;
    do
    {
        g_sessionSeed ^= g_sessionSeed << 13;
        g_sessionSeed ^= g_sessionSeed >> 17;
        g_sessionSeed ^= g_sessionSeed << 5;
        v = g_sessionSeed;
    } while (v == 0 || SrvFindBySession(v) != NULL);
    return v;
}

static int SrvClientCount(void)
{
    int i, n = 0;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used) n++;
    }
    return n;
}

static void SrvSanitize(char* s, int cap)
{
    int i;
    int keep = 0;
    for (i = 0; i < cap - 1 && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126)
        {
            s[i] = '?';
        }
        if (s[i] != ' ')
        {
            keep = 1;
        }
    }
    s[cap - 1] = '\0';
    if (!keep)
    {
        snprintf(s, (size_t)cap, "Wanderer");
    }
}

/* ------------------------------------------------------------------ */
/* Send helpers                                                        */
/* ------------------------------------------------------------------ */

static void SrvSend(const struct sockaddr_in* to, const shn_u8* buf, int len)
{
    sendto(g_sock, (const char*)buf, (size_t)len, 0,
           (const struct sockaddr*)to, (ShSockLen)sizeof(*to));
}

static void SrvSendTo(Client* c, shn_u8* buf, int payloadLen, int type)
{
    ShnPutHeader(buf, (shn_u8)type, (shn_u16)payloadLen, c->session);
    SrvSend(&c->addr, buf, SHNET_HDR_SIZE + payloadLen);
}

static void SrvSendEvent(Client* c, int kind, shn_u32 who, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8 + SHNET_EVENT_MAX];
    int    off = SHNET_HDR_SIZE;

    if (!(g_featureFlags & SHNET_SF_EVENTS))
    {
        return;
    }
    ShnPutU8(buf, &off, (shn_u8)kind);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, who);
    ShnPutStr(buf, &off, text, SHNET_EVENT_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_EVENT);
}

static void SrvBroadcastEvent(int kind, shn_u32 who, const char* text, Client* except)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && &g_clients[i] != except)
        {
            SrvSendEvent(&g_clients[i], kind, who, text);
        }
    }
}

static void SrvSendChat(Client* c, int scope, shn_u32 fromId, const char* name, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8 + SHNET_NAME_MAX + SHNET_CHAT_MAX];
    int    off = SHNET_HDR_SIZE;

    ShnPutU8(buf, &off, (shn_u8)scope);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, fromId);
    ShnPutStr(buf, &off, name, SHNET_NAME_MAX);
    ShnPutStr(buf, &off, text, SHNET_CHAT_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_CHAT_MSG);
}

static void SrvHandleChatSay(Client* c, const shn_u8* p, int len)
{
    int  off = 0;
    int  scope;
    char text[SHNET_CHAT_MAX];
    int  i;

    if (len < 4 + SHNET_CHAT_MAX)
    {
        return;
    }
    scope = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    (void)ShnGetU16(p, &off);
    ShnGetStr(p, &off, SHNET_CHAT_MAX, text, sizeof(text));
    SrvSanitize(text, SHNET_CHAT_MAX);
    if (text[0] == '\0')
    {
        return;
    }
    if (scope != SHNET_CHAT_GLOBAL && scope != SHNET_CHAT_GAME)
    {
        scope = SHNET_CHAT_GLOBAL;
    }

    SrvLog("  chat[%s] %s: %s", scope == SHNET_CHAT_GAME ? "game" : "all", c->name, text);

    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* o = &g_clients[i];
        if (!o->used)
        {
            continue;
        }
        /* Game chat only reaches players standing in the same map. A client
         * with no map yet (still on a menu) is not in anyone's game. */
        if (scope == SHNET_CHAT_GAME && (o->mapIdx < 0 || o->mapIdx != c->mapIdx))
        {
            continue;
        }
        SrvSendChat(o, scope, c->playerId, c->name, text);
    }
}

static void SrvSendReject(const struct sockaddr_in* to, int reason, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 4 + SHNET_REJECT_MAX];
    int    off = SHNET_HDR_SIZE;

    ShnPutU8(buf, &off, (shn_u8)reason);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutStr(buf, &off, text, SHNET_REJECT_MAX);
    ShnPutHeader(buf, SHNET_MSG_REJECT, (shn_u16)(off - SHNET_HDR_SIZE), 0);
    SrvSend(to, buf, off);
}

static void SrvSendWelcome(Client* c)
{
    shn_u8 buf[SHNET_HDR_SIZE + 16 + SHNET_SERVER_MAX + SHNET_MOTD_MAX];
    int    off = SHNET_HDR_SIZE;

    ShnPutU32(buf, &off, c->session);
    ShnPutU32(buf, &off, c->playerId);
    ShnPutU16(buf, &off, SHNET_PROTO_VER);
    ShnPutU16(buf, &off, SRV_TICK_MS);
    ShnPutU16(buf, &off, SHNET_SNAP_MAX);
    ShnPutU16(buf, &off, (shn_u16)g_featureFlags);
    ShnPutStr(buf, &off, g_serverName, SHNET_SERVER_MAX);
    ShnPutStr(buf, &off, g_motd, SHNET_MOTD_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_WELCOME);
}

/* ------------------------------------------------------------------ */
/* Marker store                                                        */
/* ------------------------------------------------------------------ */

static int SrvMemosOnMap(int mapIdx)
{
    int i, n = 0;
    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].mapIdx == mapIdx) n++;
    }
    return n;
}

/* Drop the lowest-rated, oldest marker on a full map so a busy area keeps
 * turning over instead of freezing at whatever was placed first. */
static void SrvEvictOnMap(int mapIdx)
{
    int i, worst = -1;
    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].mapIdx != mapIdx)
        {
            continue;
        }
        if (worst < 0 ||
            g_memos[i].rating < g_memos[worst].rating ||
            (g_memos[i].rating == g_memos[worst].rating &&
             g_memos[i].placedUnix < g_memos[worst].placedUnix))
        {
            worst = i;
        }
    }
    if (worst >= 0)
    {
        g_memos[worst] = g_memos[--g_memoCount];
        g_dbDirty = 1;
    }
}

static void SrvSaveDb(void)
{
    FILE* f;
    int   i;

    if (!g_dbPath[0])
    {
        return;
    }
    f = fopen(g_dbPath, "w");
    if (!f)
    {
        SrvLog("could not write %s", g_dbPath);
        return;
    }
    /* Plain text on purpose: an admin has to be able to delete one bad marker
     * with a text editor without a tool for it. */
    fprintf(f, "# sh_master marker store v1\n");
    fprintf(f, "# id owner kind map x y z rot rating phraseA wordA phraseB wordB unix name\n");
    for (i = 0; i < g_memoCount; i++)
    {
        const Memo* m = &g_memos[i];
        fprintf(f, "%u %u %d %d %d %d %d %d %d %u %u %u %u %ld %s\n",
                m->memoId, m->ownerId, m->kind, m->mapIdx,
                (int)m->x, (int)m->y, (int)m->z, (int)m->rotY, (int)m->rating,
                (unsigned)m->phraseA, (unsigned)m->wordA,
                (unsigned)m->phraseB, (unsigned)m->wordB,
                m->placedUnix, m->ownerName);
    }
    fclose(f);
    g_dbDirty = 0;
}

static void SrvLoadDb(void)
{
    FILE* f = fopen(g_dbPath, "r");
    char  line[512];

    if (!f)
    {
        SrvLog("no marker store at %s (starting empty)", g_dbPath);
        return;
    }
    while (fgets(line, sizeof(line), f))
    {
        Memo m;
        unsigned int id, owner, pa, wa, pb, wb;
        int kind, map, x, y, z, rot, rating;
        long unixT;
        char nm[64];

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }
        nm[0] = '\0';
        if (sscanf(line, "%u %u %d %d %d %d %d %d %d %u %u %u %u %ld %63[^\n]",
                   &id, &owner, &kind, &map, &x, &y, &z, &rot, &rating,
                   &pa, &wa, &pb, &wb, &unixT, nm) < 14)
        {
            continue;
        }
        if (g_memoCount >= SRV_MAX_MEMOS)
        {
            break;
        }
        memset(&m, 0, sizeof(m));
        m.memoId  = id;
        m.ownerId = owner;
        m.kind    = kind;
        m.mapIdx  = map;
        m.x       = x;
        m.y       = y;
        m.z       = z;
        m.rotY    = (shn_s16)rot;
        m.rating  = (shn_s16)rating;
        m.phraseA = (shn_u16)pa;
        m.wordA   = (shn_u16)wa;
        m.phraseB = (shn_u16)pb;
        m.wordB   = (shn_u16)wb;
        m.placedUnix = unixT;
        snprintf(m.ownerName, sizeof(m.ownerName), "%s", nm[0] ? nm : "?");
        SrvSanitize(m.ownerName, SHNET_NAME_MAX);

        g_memos[g_memoCount++] = m;
        if (id >= g_nextMemoId)
        {
            g_nextMemoId = id + 1;
        }
    }
    fclose(f);
    SrvLog("loaded %d markers from %s (next id %u)", g_memoCount, g_dbPath, g_nextMemoId);
}

/* ------------------------------------------------------------------ */
/* Message handlers                                                    */
/* ------------------------------------------------------------------ */

static void SrvHandleHello(const struct sockaddr_in* from, const shn_u8* p, int len)
{
    int      off = 0;
    shn_u16  ver;
    int      charaId;
    shn_u32  reconnect;
    char     name[SHNET_NAME_MAX];
    char     build[SHNET_BUILD_MAX];
    shn_u32  pass;
    Client*  c;
    char     addr[64];
    char     text[SHNET_EVENT_MAX];

    if (len < 2 + 1 + 1 + 4 + SHNET_NAME_MAX + SHNET_BUILD_MAX + 4)
    {
        return;
    }
    ver       = ShnGetU16(p, &off);
    (void)ShnGetU8(p, &off);
    charaId   = (int)ShnGetU8(p, &off);
    reconnect = ShnGetU32(p, &off);
    ShnGetStr(p, &off, SHNET_NAME_MAX, name, SHNET_NAME_MAX);
    ShnGetStr(p, &off, SHNET_BUILD_MAX, build, SHNET_BUILD_MAX);
    pass = ShnGetU32(p, &off);

    SrvAddrStr(from, addr, (int)sizeof(addr));

    if (ver != SHNET_PROTO_VER)
    {
        SrvSendReject(from, SHNET_REJ_VERSION, "Wrong protocol version");
        SrvLog("rejected %s: protocol v%u", addr, (unsigned)ver);
        return;
    }
    if (g_passHash && pass != g_passHash)
    {
        SrvSendReject(from, SHNET_REJ_PASSWORD, "Password required or incorrect");
        SrvLog("rejected %s: bad password", addr);
        return;
    }

    SrvSanitize(name, SHNET_NAME_MAX);

    /* An identical HELLO from an address already in the table is a retry the
     * WELCOME did not reach: answer again with the SAME session, or the client
     * would get a new identity every second of packet loss. */
    c = SrvFindByAddr(from);
    if (!c && reconnect)
    {
        c = SrvFindBySession(reconnect);
    }

    if (!c)
    {
        int i;
        if (SrvClientCount() >= g_maxClients)
        {
            SrvSendReject(from, SHNET_REJ_FULL, "Server is full");
            return;
        }
        for (i = 0; i < SRV_MAX_CLIENTS; i++)
        {
            if (!g_clients[i].used)
            {
                c = &g_clients[i];
                break;
            }
        }
        if (!c)
        {
            SrvSendReject(from, SHNET_REJ_FULL, "Server is full");
            return;
        }
        memset(c, 0, sizeof(*c));
        c->used     = 1;
        c->playerId = g_nextPlayerId++;
        c->session  = SrvNewSession();
        c->joinMs   = SrvMillis();
        c->mapIdx   = -1;

        snprintf(text, sizeof(text), "%s entered the fog", name);
        SrvBroadcastEvent(SHNET_EV_JOIN, c->playerId, text, c);
        SrvLog("+ %s joined from %s (id %u, build %s)", name, addr, c->playerId, build);
    }

    c->addr    = *from;
    c->charaId = charaId;
    c->lastRxMs = SrvMillis();
    memcpy(c->name, name, SHNET_NAME_MAX);

    SrvSendWelcome(c);
    if (g_motd[0])
    {
        SrvSendEvent(c, SHNET_EV_SERVER, 0, g_motd);
    }
}

static void SrvHandleState(Client* c, const shn_u8* p, int len)
{
    int off = 0;
    if (len < 28)
    {
        return;
    }
    (void)ShnGetU32(p, &off); /* sequence; ordering does not matter for ghosts */
    c->mapIdx  = (int)ShnGetU8(p, &off);
    c->charaId = (int)ShnGetU8(p, &off);
    c->flags   = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    c->x      = ShnGetS32(p, &off);
    c->y      = ShnGetS32(p, &off);
    c->z      = ShnGetS32(p, &off);
    c->rotY   = ShnGetS16(p, &off);
    c->health = ShnGetS16(p, &off);
    c->anim   = ShnGetU16(p, &off);
    c->frame  = ShnGetU16(p, &off);

    if (c->mapIdx < 0 || c->mapIdx >= SRV_MAP_COUNT)
    {
        c->mapIdx = -1;
    }
}

static void SrvHandlePing(Client* c, const shn_u8* p, int len)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8];
    int    off = SHNET_HDR_SIZE;
    int    ro  = 0;
    shn_u32 clientMs;

    if (len < 4)
    {
        return;
    }
    clientMs = ShnGetU32(p, &ro);
    ShnPutU32(buf, &off, clientMs);
    ShnPutU32(buf, &off, SrvMillis());
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_PONG);
}

static void SrvHandleRosterReq(Client* c)
{
    shn_u8 buf[SHNET_MTU];
    int    i;
    int    sent  = 0;
    int    total = SrvClientCount();
    unsigned int now = SrvMillis();

    i = 0;
    while (sent < total)
    {
        int off   = SHNET_HDR_SIZE;
        int count = 0;
        int hdrAt;

        hdrAt = off;
        ShnPutU16(buf, &off, (shn_u16)total);
        ShnPutU16(buf, &off, 0); /* count, back-patched */
        ShnPutU16(buf, &off, (shn_u16)sent);
        ShnPutU16(buf, &off, 0);

        for (; i < SRV_MAX_CLIENTS && count < SHNET_ROSTER_MAX; i++)
        {
            Client* o = &g_clients[i];
            if (!o->used)
            {
                continue;
            }
            ShnPutU32(buf, &off, o->playerId);
            ShnPutU8(buf, &off, (shn_u8)(o->mapIdx < 0 ? 0xFF : o->mapIdx));
            ShnPutU8(buf, &off, (shn_u8)o->charaId);
            ShnPutU8(buf, &off, (shn_u8)o->flags);
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, (shn_u16)(o->pingMs > 65535 ? 65535 : o->pingMs));
            ShnPutU16(buf, &off, (shn_u16)((now - o->joinMs) / 60000u));
            ShnPutStr(buf, &off, o->name, SHNET_NAME_MAX);
            count++;
        }

        {
            int patch = hdrAt + 2;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_ROSTER);

        sent += count;
        if (count == 0)
        {
            break;
        }
    }
}

static void SrvHandleMemoQuery(Client* c, const shn_u8* p, int len)
{
    shn_u8 buf[SHNET_MTU];
    int    off = 0;
    int    mapIdx;
    int    kinds;
    int    i;
    int    emitted = 0;

    if (!(g_featureFlags & SHNET_SF_MEMOS))
    {
        return;
    }
    if (len < 8)
    {
        return;
    }
    mapIdx = (int)ShnGetU8(p, &off);
    kinds  = (int)ShnGetU8(p, &off);

    if (mapIdx < 0 || mapIdx >= SRV_MAP_COUNT)
    {
        return;
    }

    i = 0;
    do
    {
        int o     = SHNET_HDR_SIZE;
        int count = 0;
        int hdrAt = o;

        ShnPutU8(buf, &o, (shn_u8)mapIdx);
        ShnPutU8(buf, &o, 0);
        ShnPutU16(buf, &o, 0); /* count, back-patched */
        ShnPutU32(buf, &o, 0);

        for (; i < g_memoCount && count < SHNET_MEMO_MAX; i++)
        {
            const Memo* m = &g_memos[i];
            if (m->mapIdx != mapIdx)
            {
                continue;
            }
            if (!(kinds & (1 << m->kind)) && kinds != 0xFF)
            {
                continue;
            }
            ShnPutU32(buf, &o, m->memoId);
            ShnPutU32(buf, &o, m->ownerId);
            ShnPutU8(buf, &o, (shn_u8)m->kind);
            ShnPutU8(buf, &o, (shn_u8)m->mapIdx);
            ShnPutS16(buf, &o, m->rating);
            ShnPutU16(buf, &o, m->phraseA);
            ShnPutU16(buf, &o, m->wordA);
            ShnPutU16(buf, &o, m->phraseB);
            ShnPutU16(buf, &o, m->wordB);
            ShnPutS32(buf, &o, m->x);
            ShnPutS32(buf, &o, m->y);
            ShnPutS32(buf, &o, m->z);
            ShnPutS16(buf, &o, m->rotY);
            count++;
        }

        {
            int patch = hdrAt + 2;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        SrvSendTo(c, buf, o - SHNET_HDR_SIZE, SHNET_MSG_MEMO_LIST);
        emitted += count;

        /* An empty list still goes out once: the client is retrying until it
         * hears something, and "this map has no markers" is an answer. */
        if (count == 0)
        {
            break;
        }
    } while (i < g_memoCount);

    if (g_verbose)
    {
        SrvLog("  %s queried map %d -> %d markers", c->name, mapIdx, emitted);
    }
}

static void SrvHandleMemoPlace(Client* c, const shn_u8* p, int len)
{
    int     off = 0;
    Memo    m;
    shn_u8  ack[SHNET_HDR_SIZE + 8];
    int     ao = SHNET_HDR_SIZE;
    int     mine = 0;
    int     i;

    if (!(g_featureFlags & SHNET_SF_MEMOS))
    {
        return;
    }
    if (len < 28)
    {
        return;
    }

    memset(&m, 0, sizeof(m));
    m.kind    = (int)ShnGetU8(p, &off);
    m.mapIdx  = (int)ShnGetU8(p, &off);
    m.phraseA = ShnGetU16(p, &off);
    m.wordA   = ShnGetU16(p, &off);
    m.phraseB = ShnGetU16(p, &off);
    m.wordB   = ShnGetU16(p, &off);
    (void)ShnGetU16(p, &off);
    m.x       = ShnGetS32(p, &off);
    m.y       = ShnGetS32(p, &off);
    m.z       = ShnGetS32(p, &off);
    m.rotY    = ShnGetS16(p, &off);

    if (m.mapIdx < 0 || m.mapIdx >= SRV_MAP_COUNT)
    {
        return;
    }
    if (m.kind < 0 || m.kind > SHNET_MARK_SAVE)
    {
        return;
    }
    if (m.kind == SHNET_MARK_DEATH && !(g_featureFlags & SHNET_SF_DEATHS))
    {
        return;
    }

    /* The client latches its death report, but a reconnect or a replayed
     * datagram can still produce a second one; collapse those here so a single
     * death never leaves a cluster of markers. */
    if (m.kind == SHNET_MARK_DEATH)
    {
        unsigned int now = SrvMillis();
        shn_s32      dx  = m.x - c->lastDeathX;
        shn_s32      dz  = m.z - c->lastDeathZ;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        if (c->lastDeathMs && (now - c->lastDeathMs) < SRV_DEATH_DEDUPE_MS &&
            dx < (SRV_DEATH_DEDUPE_DIST) && dz < (SRV_DEATH_DEDUPE_DIST))
        {
            return;
        }
        c->lastDeathMs = now;
        c->lastDeathX  = m.x;
        c->lastDeathZ  = m.z;
    }

    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].ownerId == c->playerId) mine++;
    }
    if (mine >= SRV_MEMOS_PER_USER)
    {
        /* Retire this player's oldest rather than refusing: a session that hit
         * its cap should still be able to leave a message. */
        int oldest = -1;
        for (i = 0; i < g_memoCount; i++)
        {
            if (g_memos[i].ownerId != c->playerId)
            {
                continue;
            }
            if (oldest < 0 || g_memos[i].placedUnix < g_memos[oldest].placedUnix)
            {
                oldest = i;
            }
        }
        if (oldest >= 0)
        {
            g_memos[oldest] = g_memos[--g_memoCount];
        }
    }

    while (SrvMemosOnMap(m.mapIdx) >= SRV_MEMOS_PER_MAP)
    {
        SrvEvictOnMap(m.mapIdx);
    }
    if (g_memoCount >= SRV_MAX_MEMOS)
    {
        SrvEvictOnMap(m.mapIdx);
    }
    if (g_memoCount >= SRV_MAX_MEMOS)
    {
        return;
    }

    m.memoId     = g_nextMemoId++;
    m.ownerId    = c->playerId;
    m.rating     = 0;
    m.placedUnix = (long)time(NULL);
    memcpy(m.ownerName, c->name, SHNET_NAME_MAX);

    g_memos[g_memoCount++] = m;
    g_dbDirty = 1;

    ShnPutU32(ack, &ao, m.memoId);
    ShnPutU32(ack, &ao, (shn_u32)m.mapIdx);
    SrvSendTo(c, ack, ao - SHNET_HDR_SIZE, SHNET_MSG_MEMO_ACK);

    SrvLog("  %s placed %s on map %d (id %u)", c->name,
           m.kind == SHNET_MARK_DEATH ? "a death marker" :
           m.kind == SHNET_MARK_SAVE  ? "a save mark" : "a message",
           m.mapIdx, m.memoId);

    if (m.kind == SHNET_MARK_DEATH)
    {
        char text[SHNET_EVENT_MAX];
        snprintf(text, sizeof(text), "%s died", c->name);
        SrvBroadcastEvent(SHNET_EV_DEATH, c->playerId, text, c);
    }
}

static void SrvHandleMemoRate(Client* c, const shn_u8* p, int len)
{
    int     off = 0;
    shn_u32 id;
    int     delta;
    int     i;

    if (len < 8)
    {
        return;
    }
    id    = ShnGetU32(p, &off);
    delta = (int)(signed char)ShnGetU8(p, &off);

    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].memoId != id)
        {
            continue;
        }
        if (g_memos[i].ownerId == c->playerId)
        {
            return; /* no self-rating */
        }
        g_memos[i].rating = (shn_s16)(g_memos[i].rating + (delta >= 0 ? 1 : -1));
        g_dbDirty = 1;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Snapshots                                                           */
/* ------------------------------------------------------------------ */

/* Position fan-out is filtered by MAP on the server rather than by distance:
 * the server has no collision or geometry, so it cannot know what is visible,
 * and the client's own online_ghost_range already handles the near case. Map
 * filtering is what actually matters — it keeps a 60-player server's traffic
 * proportional to the people you could plausibly meet. */
static void SrvSendSnapshots(unsigned int now)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* c = &g_clients[i];
        shn_u8  buf[SHNET_MTU];
        int     off   = SHNET_HDR_SIZE;
        int     count = 0;
        int     hdrAt;
        int     j;

        if (!c->used || c->mapIdx < 0)
        {
            continue;
        }
        if (now - c->lastSnapMs < SRV_TICK_MS)
        {
            continue;
        }
        c->lastSnapMs = now;

        if (!(g_featureFlags & SHNET_SF_GHOSTS))
        {
            continue;
        }

        hdrAt = off;
        ShnPutU32(buf, &off, now);
        ShnPutU16(buf, &off, 0); /* count, back-patched */
        ShnPutU16(buf, &off, 0);

        for (j = 0; j < SRV_MAX_CLIENTS && count < SHNET_SNAP_MAX; j++)
        {
            Client* o = &g_clients[j];
            if (!o->used || o == c || o->mapIdx != c->mapIdx)
            {
                continue;
            }
            ShnPutU32(buf, &off, o->playerId);
            ShnPutU8(buf, &off, (shn_u8)o->charaId);
            ShnPutU8(buf, &off, (shn_u8)o->flags);
            ShnPutS32(buf, &off, o->x);
            ShnPutS32(buf, &off, o->y);
            ShnPutS32(buf, &off, o->z);
            ShnPutS16(buf, &off, o->rotY);
            ShnPutU16(buf, &off, o->anim);
            ShnPutU16(buf, &off, o->frame);
            count++;
        }

        /* Nothing to say: skip the datagram entirely. The client's stale
         * timer removes ghosts on its own, so silence is a valid message and
         * an empty map costs zero bandwidth. */
        if (count == 0)
        {
            continue;
        }

        {
            int patch = hdrAt + 4;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        if (g_verbose && !c->loggedSnap)
        {
            c->loggedSnap = 1;
            SrvLog("  -> first snapshot to %s: %d peer(s) on map %d, %d bytes",
                   c->name, count, c->mapIdx, SHNET_HDR_SIZE + off - SHNET_HDR_SIZE);
        }
        SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_SNAPSHOT);
    }
}

static void SrvExpire(unsigned int now)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* c = &g_clients[i];
        char    text[SHNET_EVENT_MAX];

        if (!c->used || (now - c->lastRxMs) < SRV_TIMEOUT_MS)
        {
            continue;
        }
        snprintf(text, sizeof(text), "%s faded away", c->name);
        SrvLog("- %s timed out (id %u)", c->name, c->playerId);
        c->used = 0;
        SrvBroadcastEvent(SHNET_EV_LEAVE, c->playerId, text, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void SrvDispatch(const struct sockaddr_in* from, const shn_u8* buf, int n)
{
    shn_u8       type;
    shn_u16      payLen;
    shn_u32      session;
    const shn_u8* pay;
    Client*      c;
    unsigned int now = SrvMillis();

    if (!ShnParseHeader(buf, n, &type, &payLen, &session))
    {
        return;
    }
    pay = buf + SHNET_HDR_SIZE;

    if (type == SHNET_MSG_HELLO)
    {
        SrvHandleHello(from, pay, (int)payLen);
        return;
    }

    /* Every other message needs a live session, and it has to have come from
     * the address that session was established on: a session id alone would
     * let anyone who saw one impersonate that player. */
    c = SrvFindBySession(session);
    if (!c || !SrvAddrEqual(&c->addr, from))
    {
        return;
    }

    if (now - c->rateWindowMs >= 1000u)
    {
        c->rateWindowMs = now;
        c->rateCount    = 0;
    }
    if (++c->rateCount > SRV_RATE_PER_SEC)
    {
        return;
    }

    c->lastRxMs = now;

    switch (type)
    {
    case SHNET_MSG_STATE:      SrvHandleState(c, pay, (int)payLen);      break;
    case SHNET_MSG_PING:       SrvHandlePing(c, pay, (int)payLen);       break;
    case SHNET_MSG_ROSTER_REQ: SrvHandleRosterReq(c);                    break;
    case SHNET_MSG_MEMO_QUERY: SrvHandleMemoQuery(c, pay, (int)payLen);  break;
    case SHNET_MSG_MEMO_PLACE: SrvHandleMemoPlace(c, pay, (int)payLen);  break;
    case SHNET_MSG_MEMO_RATE:  SrvHandleMemoRate(c, pay, (int)payLen);   break;
    case SHNET_MSG_CHAT_SAY:   SrvHandleChatSay(c, pay, (int)payLen);    break;
    case SHNET_MSG_BYE:
    {
        char text[SHNET_EVENT_MAX];
        snprintf(text, sizeof(text), "%s left", c->name);
        SrvLog("- %s left (id %u)", c->name, c->playerId);
        c->used = 0;
        SrvBroadcastEvent(SHNET_EV_LEAVE, c->playerId, text, NULL);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void SrvUsage(const char* argv0)
{
    printf("Silent Hill Online master server (protocol v%d)\n\n", SHNET_PROTO_VER);
    printf("  %s [options]\n\n", argv0);
    printf("  -p PORT      UDP port to listen on (default %d)\n", SHNET_DEFAULT_PORT);
    printf("  -n NAME      server name shown to players\n");
    printf("  -m TEXT      message of the day\n");
    printf("  -w WORD      require this password\n");
    printf("  -d FILE      marker store path (default memos.db)\n");
    printf("  --max N      maximum simultaneous players (default 64)\n");
    printf("  --no-ghosts  do not fan out positions\n");
    printf("  --no-memos   do not accept or serve messages\n");
    printf("  --no-deaths  do not accept or serve death markers\n");
    printf("  -v           log every query\n");
    printf("  -h           this text\n");
}

int main(int argc, char** argv)
{
    struct sockaddr_in bindAddr;
    unsigned int       lastExpire = 0;
    int                i;

    for (i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc)
        {
            g_port = (unsigned short)atoi(argv[++i]);
        }
        else if (!strcmp(a, "-n") && i + 1 < argc)
        {
            snprintf(g_serverName, sizeof(g_serverName), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-m") && i + 1 < argc)
        {
            snprintf(g_motd, sizeof(g_motd), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-w") && i + 1 < argc)
        {
            g_passHash = ShnHashPassword(argv[++i]);
        }
        else if (!strcmp(a, "-d") && i + 1 < argc)
        {
            snprintf(g_dbPath, sizeof(g_dbPath), "%s", argv[++i]);
        }
        else if (!strcmp(a, "--max") && i + 1 < argc)
        {
            g_maxClients = atoi(argv[++i]);
            if (g_maxClients < 1)              g_maxClients = 1;
            if (g_maxClients > SRV_MAX_CLIENTS) g_maxClients = SRV_MAX_CLIENTS;
        }
        else if (!strcmp(a, "--no-ghosts")) g_featureFlags &= ~SHNET_SF_GHOSTS;
        else if (!strcmp(a, "--no-memos"))  g_featureFlags &= ~SHNET_SF_MEMOS;
        else if (!strcmp(a, "--no-deaths")) g_featureFlags &= ~SHNET_SF_DEATHS;
        else if (!strcmp(a, "-v"))          g_verbose = 1;
        else
        {
            SrvUsage(argv[0]);
            return strcmp(a, "-h") ? 1 : 0;
        }
    }

    SrvSanitize(g_serverName, SHNET_SERVER_MAX);

    g_sessionSeed = (shn_u32)time(NULL) ^ ((shn_u32)(size_t)&argc * 2654435761u) ^ 0x9E3779B9u;
    if (g_sessionSeed == 0)
    {
        g_sessionSeed = 0x1234567u;
    }

    g_memos = (Memo*)calloc(SRV_MAX_MEMOS, sizeof(Memo));
    if (!g_memos)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

#if defined(_WIN32)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#endif

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == SH_INVALID_SOCK)
    {
        fprintf(stderr, "socket() failed\n");
        return 1;
    }

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(g_port);
    if (bind(g_sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) != 0)
    {
        fprintf(stderr, "could not bind UDP port %u (already in use?)\n", (unsigned)g_port);
        return 1;
    }

    /* Non-blocking: the receive loop below drains until the socket is empty,
     * and on a blocking socket that last read parks the whole server between
     * datagrams, which stops the snapshot fan-out running at all. */
#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
    }
#else
    {
        int fl = fcntl(g_sock, F_GETFL, 0);
        fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);
    }
#endif

#if defined(_WIN32)
    {
        /* An ICMP port-unreachable from a client that just quit otherwise
         * latches recvfrom into a permanent WSAECONNRESET. */
        BOOL  b   = FALSE;
        DWORD ret = 0;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        WSAIoctl(g_sock, SIO_UDP_CONNRESET, &b, sizeof(b), NULL, 0, &ret, NULL, NULL);
    }
#endif

    signal(SIGINT, SrvOnSignal);
#ifdef SIGTERM
    signal(SIGTERM, SrvOnSignal);
#endif

    SrvLoadDb();
    g_lastSaveMs = SrvMillis();

    SrvLog("\"%s\" listening on UDP %u  (max %d players%s%s%s%s)",
           g_serverName, (unsigned)g_port, g_maxClients,
           g_passHash ? ", password" : "",
           (g_featureFlags & SHNET_SF_GHOSTS) ? "" : ", no ghosts",
           (g_featureFlags & SHNET_SF_MEMOS)  ? "" : ", no memos",
           (g_featureFlags & SHNET_SF_DEATHS) ? "" : ", no deaths");

    while (g_running)
    {
        fd_set          rd;
        struct timeval  tv;
        unsigned int    now;

        FD_ZERO(&rd);
        FD_SET(g_sock, &rd);
        tv.tv_sec  = 0;
        tv.tv_usec = 20000;

        if (select((int)g_sock + 1, &rd, NULL, NULL, &tv) > 0 && FD_ISSET(g_sock, &rd))
        {
            for (;;)
            {
                shn_u8             buf[SHNET_MTU];
                struct sockaddr_in from;
                ShSockLen          flen = (ShSockLen)sizeof(from);
                int                n;

                n = (int)recvfrom(g_sock, (char*)buf, sizeof(buf), 0,
                                  (struct sockaddr*)&from, &flen);
                if (n <= 0)
                {
                    break;
                }
                SrvDispatch(&from, buf, n);
            }
        }

        now = SrvMillis();
        SrvSendSnapshots(now);

        if (now - lastExpire >= 1000u)
        {
            lastExpire = now;
            SrvExpire(now);
        }
        if (g_dbDirty && (now - g_lastSaveMs) >= SRV_SAVE_GAP_MS)
        {
            g_lastSaveMs = now;
            SrvSaveDb();
        }
    }

    SrvLog("shutting down");
    SrvSaveDb();
    SH_CLOSESOCK(g_sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    free(g_memos);
    return 0;
}
