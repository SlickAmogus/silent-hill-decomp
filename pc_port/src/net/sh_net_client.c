/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_client.c - Silent Hill Online protocol client.
 *
 * Owns the socket, the connection state machine and every byte of the wire
 * protocol. Includes NO game header: everything here is plain C plus SDL's
 * thread primitives, which keeps the piece that can be reasoned about (and
 * broken) independently of the engine.
 *
 * Two threads, one lock:
 *
 *   WORKER  runs the loop below at ~200Hz. It is the only thread that ever
 *           touches the socket. Everything it learns lands in s_sh under
 *           s_lock.
 *   GAME    calls ShNet_PublishLocal + ShNet_PumpToGameThread once a frame
 *           (both from ShNet_GameTick). PumpToGameThread copies s_sh into
 *           s_gt, and every accessor the renderer and UI use reads s_gt with
 *           no lock — so the world-draw path never blocks on the network.
 *
 * The worker never allocates and never blocks except in getaddrinfo, which is
 * exactly why it is not on the game thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "sh_net.h"
#include "sh_net_platform.h"
#include "sh_net_internal.h"
#include "sh_net_session.h"
#include "pc_config.h"
#include "sh_log.h"

/* How often this player's position goes out. 10Hz is the rate the ghost
 * interpolator is tuned for and costs ~2.8 kbit/s up. */
#define SHNET_STATE_MS      100
#define SHNET_PING_MS       2000
#define SHNET_HELLO_MS      1000
#define SHNET_RETRY_MS      1500
/* Retries for the two requests that expect an answer. A server started with
 * --no-memos answers a MEMO_QUERY with silence, so an unbounded retry would
 * send one every SHNET_RETRY_MS for the rest of the session. Giving up is
 * safe: both are re-armed by a map change or by the player asking. */
#define SHNET_REQ_TRIES     5
#define SHNET_TIMEOUT_MS    8000
#define SHNET_HELLO_MAX     30    /* ~30s of retries before reporting failure */

/* A ghost older than this is dropped: the other player quit, changed map, or
 * their connection died. Deliberately longer than SHNET_TIMEOUT_MS so a brief
 * packet-loss burst does not make everyone flicker out. */
#define SHNET_GHOST_STALE_MS 3000

/* Outbound commands the game thread queues for the worker. Kept as a tiny
 * ring rather than a malloc'd list so the game thread's enqueue is a memcpy
 * under the lock and can never fail in a way that matters. */
typedef struct
{
    int            kind; /* SHNET_MSG_* */
    int            a, b, c, d;
    unsigned int   u0;
    unsigned short w0, w1, w2, w3;
    short          s0;
} ShNetCmd;

#define SHNET_CMD_RING 32

static struct
{
    int          status;
    char         statusText[96];
    char         serverName[SHNET_SERVER_MAX];
    char         motd[SHNET_MOTD_MAX];
    unsigned int selfId;
    int          pingMs;
    int          serverFlags;
    int          tickMs;

    ShNetGhost   ghosts[SHNET_MAX_GHOSTS];
    int          ghostCount;

    ShNetMemo    memos[SHNET_MAX_MEMOS];
    int          memoCount;
    int          memoMap;      /* which map the list above belongs to */

    ShNetPeer    peers[SHNET_MAX_PEERS];
    int          peerCount;
    int          peerTotal;

    char         events[SHNET_MAX_EVENTS][SHNET_EVENT_MAX];
    int          eventHead;
    int          eventTail;

    int          localValid;
    int          localMap;
    int          localChara;
    int          localFlags;
    int          localX, localY, localZ;
    short        localRotY;
    short        localHealth;
    unsigned short localAnim, localFrame;

    ShNetCmd     cmds[SHNET_CMD_RING];
    int          cmdHead, cmdTail;

    int          wantRoster;
    int          wantMemos;
    int          wantReconnect;
} s_sh;

/* Game-thread-private mirror. Never touched by the worker. */
static struct
{
    int          status;
    char         statusText[96];
    char         serverName[SHNET_SERVER_MAX];
    char         motd[SHNET_MOTD_MAX];
    unsigned int selfId;
    int          pingMs;
    int          serverFlags;

    ShNetGhost   ghosts[SHNET_MAX_GHOSTS];
    int          ghostCount;
    ShNetMemo    memos[SHNET_MAX_MEMOS];
    int          memoCount;
    ShNetPeer    peers[SHNET_MAX_PEERS];
    int          peerCount;
    int          peerTotal;
} s_gt;

static SDL_mutex*  s_lock;
static SDL_Thread* s_thread;
static volatile int s_quit;
static int          s_enabled;   /* the worker is running at all */
static int          s_masterOn;  /* the master-server half is wanted */

static char           s_host[128];
static unsigned short s_port;
static char           s_name[SHNET_NAME_MAX];
static unsigned int   s_passHash;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void ShNet_SetStatus(int st, const char* text)
{
    s_sh.status = st;
    if (text)
    {
        SDL_strlcpy(s_sh.statusText, text, sizeof(s_sh.statusText));
    }
}

static void ShNet_PushEvent(const char* line)
{
    int next = (s_sh.eventHead + 1) % SHNET_MAX_EVENTS;
    if (next == s_sh.eventTail)
    {
        /* Full: drop the oldest. An event feed that blocks would be worse
         * than one that loses a join message. */
        s_sh.eventTail = (s_sh.eventTail + 1) % SHNET_MAX_EVENTS;
    }
    SDL_strlcpy(s_sh.events[s_sh.eventHead], line, SHNET_EVENT_MAX);
    s_sh.eventHead = next;
}

/* Strip anything that would make a name unreadable in the game's font or
 * useful to someone trying to inject control characters into a log line. */
static void ShNet_SanitizeName(char* s)
{
    int i;
    int keep = 0;
    for (i = 0; s[i]; i++)
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
    if (!keep)
    {
        SDL_strlcpy(s, "Wanderer", SHNET_NAME_MAX);
    }
}

const char* ShNet_LocalName(void)
{
    return s_name;
}

/* ------------------------------------------------------------------ */
/* Worker: send                                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
    ShNetSock*   sock;
    ShNetAddr    server;
    unsigned int session;
    unsigned int lastStateMs;
    unsigned int lastPingMs;
    unsigned int lastHelloMs;
    unsigned int lastRxMs;
    unsigned int lastRosterMs;
    unsigned int lastMemoMs;
    unsigned int pingSentMs;
    int          helloTries;
    int          rosterPending;
    int          memoPending;
    int          memoPendingMap;
    int          rosterTries;
    int          memoTries;
} ShNetWorker;

static void ShNetW_Send(ShNetWorker* w, const unsigned char* buf, int len)
{
    ShNetPlat_Send(w->sock, &w->server, buf, len);
}

static void ShNetW_SendHello(ShNetWorker* w, int charaId)
{
    unsigned char buf[SHNET_HDR_SIZE + 128];
    int           off = SHNET_HDR_SIZE;

    ShnPutU16(buf, &off, SHNET_PROTO_VER);
    ShnPutU8(buf, &off, 0);
    ShnPutU8(buf, &off, (shn_u8)charaId);
    ShnPutU32(buf, &off, w->session);
    ShnPutStr(buf, &off, s_name, SHNET_NAME_MAX);
    ShnPutStr(buf, &off, "shpc-online-1", SHNET_BUILD_MAX);
    ShnPutU32(buf, &off, s_passHash);

    ShnPutHeader(buf, SHNET_MSG_HELLO, (shn_u16)(off - SHNET_HDR_SIZE), w->session);
    ShNetW_Send(w, buf, off);
}

static void ShNetW_SendState(ShNetWorker* w)
{
    unsigned char buf[SHNET_HDR_SIZE + 64];
    int           off = SHNET_HDR_SIZE;
    static unsigned int seq;

    ShnPutU32(buf, &off, ++seq);
    ShnPutU8(buf, &off, (shn_u8)s_sh.localMap);
    ShnPutU8(buf, &off, (shn_u8)s_sh.localChara);
    ShnPutU8(buf, &off, (shn_u8)s_sh.localFlags);
    ShnPutU8(buf, &off, 0);
    ShnPutS32(buf, &off, s_sh.localX);
    ShnPutS32(buf, &off, s_sh.localY);
    ShnPutS32(buf, &off, s_sh.localZ);
    ShnPutS16(buf, &off, s_sh.localRotY);
    ShnPutS16(buf, &off, s_sh.localHealth);
    ShnPutU16(buf, &off, s_sh.localAnim);
    ShnPutU16(buf, &off, s_sh.localFrame);

    ShnPutHeader(buf, SHNET_MSG_STATE, (shn_u16)(off - SHNET_HDR_SIZE), w->session);
    ShNetW_Send(w, buf, off);
}

static void ShNetW_SendPing(ShNetWorker* w, unsigned int now)
{
    unsigned char buf[SHNET_HDR_SIZE + 8];
    int           off = SHNET_HDR_SIZE;
    w->pingSentMs = now;
    ShnPutU32(buf, &off, now);
    ShnPutHeader(buf, SHNET_MSG_PING, (shn_u16)(off - SHNET_HDR_SIZE), w->session);
    ShNetW_Send(w, buf, off);
}

static void ShNetW_SendSimple(ShNetWorker* w, int type)
{
    unsigned char buf[SHNET_HDR_SIZE];
    ShnPutHeader(buf, (shn_u8)type, 0, w->session);
    ShNetW_Send(w, buf, SHNET_HDR_SIZE);
}

static void ShNetW_SendMemoQuery(ShNetWorker* w, int mapIdx)
{
    unsigned char buf[SHNET_HDR_SIZE + 8];
    int           off = SHNET_HDR_SIZE;
    ShnPutU8(buf, &off, (shn_u8)mapIdx);
    ShnPutU8(buf, &off, 0xFF); /* every kind */
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_MEMO_QUERY, (shn_u16)(off - SHNET_HDR_SIZE), w->session);
    ShNetW_Send(w, buf, off);
}

static void ShNetW_SendCmd(ShNetWorker* w, const ShNetCmd* c)
{
    unsigned char buf[SHNET_HDR_SIZE + 64];
    int           off = SHNET_HDR_SIZE;

    switch (c->kind)
    {
    case SHNET_MSG_MEMO_PLACE:
        ShnPutU8(buf, &off, (shn_u8)c->a);          /* kind */
        ShnPutU8(buf, &off, (shn_u8)c->b);          /* mapIdx */
        ShnPutU16(buf, &off, c->w0);                /* phraseA */
        ShnPutU16(buf, &off, c->w1);                /* wordA */
        ShnPutU16(buf, &off, c->w2);                /* phraseB */
        ShnPutU16(buf, &off, c->w3);                /* wordB */
        ShnPutU16(buf, &off, 0);
        ShnPutS32(buf, &off, c->c);
        ShnPutS32(buf, &off, c->d);
        ShnPutS32(buf, &off, (shn_s32)c->u0);
        ShnPutS16(buf, &off, c->s0);
        ShnPutU16(buf, &off, 0);
        break;

    case SHNET_MSG_MEMO_RATE:
        ShnPutU32(buf, &off, c->u0);
        ShnPutU8(buf, &off, (shn_u8)(signed char)c->a);
        ShnPutU8(buf, &off, 0);
        ShnPutU16(buf, &off, 0);
        break;

    default:
        return;
    }

    ShnPutHeader(buf, (shn_u8)c->kind, (shn_u16)(off - SHNET_HDR_SIZE), w->session);
    ShNetW_Send(w, buf, off);
}

/* ------------------------------------------------------------------ */
/* Worker: receive                                                     */
/* ------------------------------------------------------------------ */

/* Fold a snapshot entry into the ghost table, preserving the previous sample
 * so the renderer has two points to walk between. */
static void ShNetW_GhostUpdate(unsigned int playerId, int charaId, int flags,
                               int x, int y, int z, short rotY,
                               unsigned short anim, unsigned short frame,
                               unsigned int now)
{
    int i;
    ShNetGhost* g = NULL;

    for (i = 0; i < s_sh.ghostCount; i++)
    {
        if (s_sh.ghosts[i].playerId == playerId)
        {
            g = &s_sh.ghosts[i];
            break;
        }
    }

    if (!g)
    {
        if (s_sh.ghostCount >= SHNET_MAX_GHOSTS)
        {
            return;
        }
        g = &s_sh.ghosts[s_sh.ghostCount++];
        memset(g, 0, sizeof(*g));
        g->playerId = playerId;
        SH_DBG("[NET] player %u appeared on this map at (%d.%03d, %d.%03d) - %d ghost(s) here",
               playerId, x >> 12, ((x & 0xFFF) * 1000) >> 12,
               z >> 12, ((z & 0xFFF) * 1000) >> 12, s_sh.ghostCount);
        /* First sight: both samples are the same point, so the ghost appears
         * standing still rather than sliding in from the origin. */
        g->prevX    = x;
        g->prevY    = y;
        g->prevZ    = z;
        g->prevRotY = rotY;
        g->prevMs   = now;
        SDL_strlcpy(g->name, "?", SHNET_NAME_MAX);
    }
    else
    {
        g->prevX    = g->curX;
        g->prevY    = g->curY;
        g->prevZ    = g->curZ;
        g->prevRotY = g->curRotY;
        g->prevMs   = g->curMs;
    }

    g->charaId   = charaId;
    g->flags     = flags;
    g->curX      = x;
    g->curY      = y;
    g->curZ      = z;
    g->curRotY   = rotY;
    g->curMs     = now;
    g->animIdx   = anim;
    g->animFrame = frame;
}

static void ShNetW_GhostExpire(unsigned int now)
{
    int i = 0;
    while (i < s_sh.ghostCount)
    {
        if (now - s_sh.ghosts[i].curMs > SHNET_GHOST_STALE_MS)
        {
            SH_DBG("[NET] player %u left this map (%d ghost(s) here)",
                   s_sh.ghosts[i].playerId, s_sh.ghostCount - 1);
            s_sh.ghosts[i] = s_sh.ghosts[s_sh.ghostCount - 1];
            s_sh.ghostCount--;
        }
        else
        {
            i++;
        }
    }
}

/* Roster names are the only place a ghost's name can come from — SNAPSHOT
 * carries ids only, so that a busy map's position feed stays small. */
static void ShNetW_NameGhosts(void)
{
    int i, j;
    for (i = 0; i < s_sh.ghostCount; i++)
    {
        for (j = 0; j < s_sh.peerCount; j++)
        {
            if (s_sh.peers[j].playerId == s_sh.ghosts[i].playerId)
            {
                SDL_strlcpy(s_sh.ghosts[i].name, s_sh.peers[j].name, SHNET_NAME_MAX);
                break;
            }
        }
    }
}

static void ShNetW_HandleWelcome(ShNetWorker* w, const unsigned char* p, int len)
{
    int off = 0;
    if (len < 4 + 4 + 2 + 2 + 2 + 2 + SHNET_SERVER_MAX + SHNET_MOTD_MAX)
    {
        return;
    }
    w->session     = ShnGetU32(p, &off);
    s_sh.selfId    = ShnGetU32(p, &off);
    (void)ShnGetU16(p, &off);
    s_sh.tickMs    = (int)ShnGetU16(p, &off);
    (void)ShnGetU16(p, &off);
    s_sh.serverFlags = (int)ShnGetU16(p, &off);
    ShnGetStr(p, &off, SHNET_SERVER_MAX, s_sh.serverName, sizeof(s_sh.serverName));
    ShnGetStr(p, &off, SHNET_MOTD_MAX, s_sh.motd, sizeof(s_sh.motd));

    if (s_sh.tickMs < 33)  s_sh.tickMs = 33;
    if (s_sh.tickMs > 1000) s_sh.tickMs = 1000;

    ShNet_SetStatus(SHNET_ST_CONNECTED, s_sh.serverName);
    w->helloTries = 0;
    SH_DBG("[NET] connected to \"%s\" as id %u (tick %dms, flags 0x%X)",
           s_sh.serverName, s_sh.selfId, s_sh.tickMs, s_sh.serverFlags);
    if (s_sh.motd[0])
    {
        SH_DBG("[NET] motd: %s", s_sh.motd);
    }

    /* A reconnect must not inherit the old map's markers or a roster of
     * players who have since left. */
    s_sh.ghostCount = 0;
    s_sh.memoCount  = 0;
    s_sh.memoMap    = -1;
    s_sh.peerCount  = 0;
    s_sh.wantMemos  = 1;
    s_sh.wantRoster = 1;
}

static void ShNetW_HandleSnapshot(const unsigned char* p, int len, unsigned int now)
{
    int off = 0;
    int count;
    int i;

    if (len < 8)
    {
        return;
    }
    (void)ShnGetU32(p, &off); /* server clock, reserved for lag compensation */
    count = (int)ShnGetU16(p, &off);
    (void)ShnGetU16(p, &off);

    if (count < 0 || count > SHNET_SNAP_MAX ||
        8 + count * SHNET_SNAP_ENTRY > len)
    {
        return;
    }

    for (i = 0; i < count; i++)
    {
        unsigned int   id    = ShnGetU32(p, &off);
        int            chara = (int)ShnGetU8(p, &off);
        int            flags = (int)ShnGetU8(p, &off);
        int            x     = ShnGetS32(p, &off);
        int            y     = ShnGetS32(p, &off);
        int            z     = ShnGetS32(p, &off);
        short          rotY  = ShnGetS16(p, &off);
        unsigned short anim  = ShnGetU16(p, &off);
        unsigned short frame = ShnGetU16(p, &off);

        if (id == s_sh.selfId)
        {
            continue;
        }
        ShNetW_GhostUpdate(id, chara, flags, x, y, z, rotY, anim, frame, now);
    }
    ShNetW_GhostExpire(now);
    ShNetW_NameGhosts();
}

static void ShNetW_HandleRoster(ShNetWorker* w, const unsigned char* p, int len)
{
    int off = 0;
    int total, count, first, i;

    if (len < 8)
    {
        return;
    }
    total = (int)ShnGetU16(p, &off);
    count = (int)ShnGetU16(p, &off);
    first = (int)ShnGetU16(p, &off);
    (void)ShnGetU16(p, &off);

    if (count < 0 || count > SHNET_ROSTER_MAX ||
        8 + count * SHNET_ROSTER_ENTRY > len)
    {
        return;
    }

    s_sh.peerTotal = total;
    /* first == 0 starts a fresh listing; later pages append. */
    if (first == 0)
    {
        s_sh.peerCount = 0;
    }

    for (i = 0; i < count; i++)
    {
        ShNetPeer tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.playerId  = ShnGetU32(p, &off);
        tmp.mapIdx    = (int)ShnGetU8(p, &off);
        tmp.charaId   = (int)ShnGetU8(p, &off);
        tmp.flags     = (int)ShnGetU8(p, &off);
        (void)ShnGetU8(p, &off);
        tmp.pingMs    = (int)ShnGetU16(p, &off);
        tmp.uptimeMin = (int)ShnGetU16(p, &off);
        ShnGetStr(p, &off, SHNET_NAME_MAX, tmp.name, SHNET_NAME_MAX);
        ShNet_SanitizeName(tmp.name);

        if (s_sh.peerCount < SHNET_MAX_PEERS)
        {
            s_sh.peers[s_sh.peerCount++] = tmp;
        }
    }
    w->rosterPending = 0;
    ShNetW_NameGhosts();
}

static void ShNetW_HandleMemoList(ShNetWorker* w, const unsigned char* p, int len)
{
    int off = 0;
    int mapIdx, count, i;

    if (len < 8)
    {
        return;
    }
    mapIdx = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    count = (int)ShnGetU16(p, &off);
    (void)ShnGetU32(p, &off);

    if (count < 0 || count > SHNET_MEMO_MAX ||
        8 + count * SHNET_MEMO_ENTRY > len)
    {
        return;
    }

    /* A list for a map the player has already left is stale by definition. */
    if (mapIdx != s_sh.localMap)
    {
        w->memoPending = 0;
        return;
    }

    if (s_sh.memoMap != mapIdx)
    {
        s_sh.memoCount = 0;
        s_sh.memoMap   = mapIdx;
    }

    for (i = 0; i < count; i++)
    {
        ShNetMemo m;
        int       j;
        int       dup = 0;

        memset(&m, 0, sizeof(m));
        m.memoId  = ShnGetU32(p, &off);
        m.ownerId = ShnGetU32(p, &off);
        m.kind    = (int)ShnGetU8(p, &off);
        m.mapIdx  = (int)ShnGetU8(p, &off);
        m.rating  = ShnGetS16(p, &off);
        m.phraseA = ShnGetU16(p, &off);
        m.wordA   = ShnGetU16(p, &off);
        m.phraseB = ShnGetU16(p, &off);
        m.wordB   = ShnGetU16(p, &off);
        m.x       = ShnGetS32(p, &off);
        m.y       = ShnGetS32(p, &off);
        m.z       = ShnGetS32(p, &off);
        m.rotY    = ShnGetS16(p, &off);

        for (j = 0; j < s_sh.memoCount; j++)
        {
            if (s_sh.memos[j].memoId == m.memoId)
            {
                s_sh.memos[j] = m;
                dup = 1;
                break;
            }
        }
        if (!dup && s_sh.memoCount < SHNET_MAX_MEMOS)
        {
            s_sh.memos[s_sh.memoCount++] = m;
        }
    }
    w->memoPending = 0;
}

static void ShNetW_HandleEvent(const unsigned char* p, int len)
{
    int  off = 0;
    char text[SHNET_EVENT_MAX];
    int  kind;

    if (len < 8 + SHNET_EVENT_MAX)
    {
        return;
    }
    kind = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    (void)ShnGetU16(p, &off);
    (void)ShnGetU32(p, &off);
    ShnGetStr(p, &off, SHNET_EVENT_MAX, text, sizeof(text));
    ShNet_SanitizeName(text);
    (void)kind;
    ShNet_PushEvent(text);
}

static void ShNetW_HandleReject(const unsigned char* p, int len)
{
    int  off = 0;
    char text[SHNET_REJECT_MAX];
    int  reason;

    if (len < 4 + SHNET_REJECT_MAX)
    {
        return;
    }
    reason = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    (void)ShnGetU16(p, &off);
    ShnGetStr(p, &off, SHNET_REJECT_MAX, text, sizeof(text));
    ShNet_SanitizeName(text);

    ShNet_SetStatus(SHNET_ST_REJECTED, text[0] ? text : "Refused by server");
    SH_DBG("[NET] rejected (reason %d): %s", reason, text);
}

static void ShNetW_Receive(ShNetWorker* w, unsigned int now)
{
    unsigned char buf[SHNET_MTU];
    ShNetAddr     from;
    int           budget = 64;

    while (budget-- > 0)
    {
        shn_u8       type;
        shn_u16      payLen;
        shn_u32      session;
        const unsigned char* pay;
        int          n = ShNetPlat_Recv(w->sock, &from, buf, (int)sizeof(buf));

        if (n <= 0)
        {
            break;
        }
        if (!ShnParseHeader(buf, n, &type, &payLen, &session))
        {
            continue;
        }
        /* Everything must come from the server this client is talking to.
         * Without this a third party who guesses the ephemeral port could
         * inject ghosts and memos. */
        if (!ShNetPlat_AddrEqual(&from, &w->server))
        {
            continue;
        }

        pay        = buf + SHNET_HDR_SIZE;
        w->lastRxMs = now;

        SDL_LockMutex(s_lock);
        switch (type)
        {
        case SHNET_MSG_WELCOME:
            ShNetW_HandleWelcome(w, pay, (int)payLen);
            break;
        case SHNET_MSG_REJECT:
            ShNetW_HandleReject(pay, (int)payLen);
            break;
        case SHNET_MSG_PONG:
            if (payLen >= 8)
            {
                int off = 0;
                unsigned int echoed = ShnGetU32(pay, &off);
                s_sh.pingMs = (int)(now - echoed);
                if (s_sh.pingMs < 0)   s_sh.pingMs = 0;
                if (s_sh.pingMs > 9999) s_sh.pingMs = 9999;
            }
            break;
        case SHNET_MSG_SNAPSHOT:
            ShNetW_HandleSnapshot(pay, (int)payLen, now);
            break;
        case SHNET_MSG_ROSTER:
            ShNetW_HandleRoster(w, pay, (int)payLen);
            break;
        case SHNET_MSG_MEMO_LIST:
            ShNetW_HandleMemoList(w, pay, (int)payLen);
            break;
        case SHNET_MSG_MEMO_ACK:
            /* The placement landed; the next MEMO_LIST carries it back with a
             * server-assigned id, so nothing to do but stop retrying. */
            s_sh.wantMemos = 1;
            break;
        case SHNET_MSG_EVENT:
            ShNetW_HandleEvent(pay, (int)payLen);
            break;
        default:
            break;
        }
        SDL_UnlockMutex(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Worker loop                                                         */
/* ------------------------------------------------------------------ */

static int SDLCALL ShNet_Worker(void* unused)
{
    ShNetWorker w;
    int         resolved = 0;

    (void)unused;
    memset(&w, 0, sizeof(w));

    if (!ShNetPlat_Init())
    {
        SDL_LockMutex(s_lock);
        ShNet_SetStatus(SHNET_ST_REJECTED, "Socket layer unavailable");
        SDL_UnlockMutex(s_lock);
        return 0;
    }

    if (s_masterOn)
    {
        w.sock = ShNetPlat_Open();
        if (!w.sock)
        {
            SDL_LockMutex(s_lock);
            ShNet_SetStatus(SHNET_ST_REJECTED, "Could not open a UDP socket");
            SDL_UnlockMutex(s_lock);
            ShNetPlat_Shutdown();
            return 0;
        }
    }

    /* Steam is brought up on THIS thread and used from nowhere else. */
    ShSession_Init();

    while (!s_quit)
    {
        unsigned int now = ShNetPlat_Millis();
        int          st;
        int          reconnect;
        int          chara;

        SDL_LockMutex(s_lock);
        reconnect = s_sh.wantReconnect;
        s_sh.wantReconnect = 0;
        if (reconnect)
        {
            ShNet_SetStatus(SHNET_ST_RESOLVING, "Looking up server...");
        }
        chara = s_sh.localChara;
        SDL_UnlockMutex(s_lock);

        if (reconnect)
        {
            resolved     = 0;
            w.session    = 0;
            w.helloTries = 0;
        }

        if (!s_masterOn)
        {
            /* Steam-session-only: no master server to talk to, but the session
             * still needs its callbacks pumped and its pings sent. */
            ShSession_Tick(now);
            SDL_Delay(5);
            continue;
        }

        if (!resolved)
        {
            char text[128];
            if (ShNetPlat_Resolve(s_host, s_port, &w.server))
            {
                char addr[64];
                ShNetPlat_AddrToString(&w.server, addr, (int)sizeof(addr));
                resolved     = 1;
                w.helloTries = 0;
                w.lastHelloMs = 0;
                SDL_LockMutex(s_lock);
                SDL_snprintf(text, sizeof(text), "Connecting to %s...", addr);
                ShNet_SetStatus(SHNET_ST_CONNECTING, text);
                SDL_UnlockMutex(s_lock);
                SH_DBG("[NET] %s resolved to %s", s_host, addr);
            }
            else
            {
                SDL_LockMutex(s_lock);
                SDL_snprintf(text, sizeof(text), "Cannot resolve \"%s\"", s_host);
                ShNet_SetStatus(SHNET_ST_LOST, text);
                SDL_UnlockMutex(s_lock);
                /* A failed lookup is usually "no network yet"; retrying every
                 * few seconds costs nothing and recovers on its own. */
                SDL_Delay(4000);
                continue;
            }
        }

        SDL_LockMutex(s_lock);
        st = s_sh.status;
        SDL_UnlockMutex(s_lock);

        if (st == SHNET_ST_CONNECTING || st == SHNET_ST_LOST)
        {
            if (now - w.lastHelloMs >= SHNET_HELLO_MS)
            {
                w.lastHelloMs = now;
                if (w.helloTries < SHNET_HELLO_MAX)
                {
                    w.helloTries++;
                    ShNetW_SendHello(&w, chara);
                }
                else
                {
                    SDL_LockMutex(s_lock);
                    ShNet_SetStatus(SHNET_ST_LOST, "No reply from server");
                    SDL_UnlockMutex(s_lock);
                    /* Back off rather than spinning HELLOs at a dead host.
                     * `continue` because `now` is five seconds stale after the
                     * sleep, and handing that to ShNetW_Receive would stamp a
                     * freshly-arrived packet as five seconds old -- which is
                     * most of the way to a spurious timeout. */
                    w.helloTries = 0;
                    SDL_Delay(5000);
                    continue;
                }
            }
        }
        else if (st == SHNET_ST_CONNECTED)
        {
            int  localValid;
            int  tick;
            int  wantRoster, wantMemos, memoMap;
            ShNetCmd pending[SHNET_CMD_RING];
            int  pendingCount = 0;

            SDL_LockMutex(s_lock);
            localValid = s_sh.localValid;
            tick       = s_sh.tickMs ? s_sh.tickMs : SHNET_STATE_MS;
            wantRoster = s_sh.wantRoster;
            wantMemos  = s_sh.wantMemos;
            memoMap    = s_sh.localMap;
            s_sh.wantRoster = 0;
            s_sh.wantMemos  = 0;
            while (s_sh.cmdTail != s_sh.cmdHead && pendingCount < SHNET_CMD_RING)
            {
                pending[pendingCount++] = s_sh.cmds[s_sh.cmdTail];
                s_sh.cmdTail = (s_sh.cmdTail + 1) % SHNET_CMD_RING;
            }
            SDL_UnlockMutex(s_lock);

            if (localValid && now - w.lastStateMs >= (unsigned int)tick)
            {
                w.lastStateMs = now;
                SDL_LockMutex(s_lock);
                ShNetW_SendState(&w);
                SDL_UnlockMutex(s_lock);
            }

            if (now - w.lastPingMs >= SHNET_PING_MS)
            {
                w.lastPingMs = now;
                ShNetW_SendPing(&w, now);
            }

            {
                int i;
                for (i = 0; i < pendingCount; i++)
                {
                    ShNetW_SendCmd(&w, &pending[i]);
                }
            }

            if (wantRoster)
            {
                w.rosterPending = 1;
                w.rosterTries   = 0;
                w.lastRosterMs  = 0;
            }
            if (w.rosterPending && now - w.lastRosterMs >= SHNET_RETRY_MS)
            {
                w.lastRosterMs = now;
                if (++w.rosterTries > SHNET_REQ_TRIES)
                {
                    w.rosterPending = 0;
                }
                else
                {
                    ShNetW_SendSimple(&w, SHNET_MSG_ROSTER_REQ);
                }
            }

            if (wantMemos)
            {
                w.memoPending    = 1;
                w.memoTries      = 0;
                w.memoPendingMap = memoMap;
                w.lastMemoMs     = 0;
            }
            if (w.memoPending && now - w.lastMemoMs >= SHNET_RETRY_MS)
            {
                w.lastMemoMs = now;
                if (++w.memoTries > SHNET_REQ_TRIES)
                {
                    w.memoPending = 0;
                }
                else
                {
                    ShNetW_SendMemoQuery(&w, w.memoPendingMap);
                }
            }

            if (now - w.lastRxMs > SHNET_TIMEOUT_MS)
            {
                SDL_LockMutex(s_lock);
                ShNet_SetStatus(SHNET_ST_LOST, "Lost contact, reconnecting...");
                s_sh.ghostCount = 0;
                SDL_UnlockMutex(s_lock);
                w.helloTries  = 0;
                w.lastHelloMs = 0;
                SH_DBG("[NET] timed out after %u ms of silence", now - w.lastRxMs);
            }
        }

        ShNetW_Receive(&w, now);
        ShSession_Tick(now);

        /* Expire ghosts even when SNAPSHOTs have stopped, or a server that
         * went quiet would leave everyone frozen in place forever. */
        SDL_LockMutex(s_lock);
        ShNetW_GhostExpire(now);
        SDL_UnlockMutex(s_lock);

        SDL_Delay(5);
    }

    ShSession_Shutdown();
    if (w.session)
    {
        ShNetW_SendSimple(&w, SHNET_MSG_BYE);
    }
    ShNetPlat_Close(w.sock);
    ShNetPlat_Shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ShNet_Init(void)
{
    if (s_thread)
    {
        return;
    }

    memset(&s_sh, 0, sizeof(s_sh));
    memset(&s_gt, 0, sizeof(s_gt));
    s_sh.memoMap  = -1;
    s_sh.localMap = -1;
    s_sh.tickMs   = SHNET_STATE_MS;

    s_masterOn = g_PcConfig.onlineEnabled;
    if (!s_masterOn && !g_PcConfig.onlineSteam)
    {
        s_enabled = 0;
        ShNet_SetStatus(SHNET_ST_OFF, "Online disabled");
        s_gt.status = SHNET_ST_OFF;
        SDL_strlcpy(s_gt.statusText, "Online disabled", sizeof(s_gt.statusText));
        return;
    }

    SDL_strlcpy(s_host, g_PcConfig.onlineServer[0] ? g_PcConfig.onlineServer : "127.0.0.1",
                sizeof(s_host));
    s_port = (unsigned short)(g_PcConfig.onlinePort > 0 ? g_PcConfig.onlinePort
                                                        : SHNET_DEFAULT_PORT);
    SDL_strlcpy(s_name, g_PcConfig.onlineName[0] ? g_PcConfig.onlineName : "Wanderer",
                sizeof(s_name));
    ShNet_SanitizeName(s_name);
    s_passHash = g_PcConfig.onlinePassword[0] ? ShnHashPassword(g_PcConfig.onlinePassword) : 0;

    s_lock = SDL_CreateMutex();
    if (!s_lock)
    {
        SH_DBG("[NET] SDL_CreateMutex failed: %s", SDL_GetError());
        return;
    }

    s_quit    = 0;
    s_enabled = 1;
    if (s_masterOn)
    {
        ShNet_SetStatus(SHNET_ST_RESOLVING, "Looking up server...");
        s_gt.status = SHNET_ST_RESOLVING;
    }
    else
    {
        ShNet_SetStatus(SHNET_ST_OFF, "Steam session only");
        s_gt.status = SHNET_ST_OFF;
    }

    s_thread = SDL_CreateThread(ShNet_Worker, "SH_NET", NULL);
    if (!s_thread)
    {
        SH_DBG("[NET] SDL_CreateThread failed: %s", SDL_GetError());
        SDL_DestroyMutex(s_lock);
        s_lock    = NULL;
        s_enabled = 0;
        return;
    }

    SH_DBG("[NET] online client started: %s:%u as \"%s\"", s_host, (unsigned)s_port, s_name);
}

void ShNet_Shutdown(void)
{
    if (!s_thread)
    {
        return;
    }
    s_quit = 1;
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    if (s_lock)
    {
        SDL_DestroyMutex(s_lock);
        s_lock = NULL;
    }
    s_enabled = 0;
}

void ShNet_Reconnect(void)
{
    if (!s_enabled || !s_lock)
    {
        return;
    }
    SDL_LockMutex(s_lock);
    s_sh.wantReconnect = 1;
    SDL_UnlockMutex(s_lock);
}

/* ------------------------------------------------------------------ */
/* Game-thread interface                                               */
/* ------------------------------------------------------------------ */

void ShNet_PublishLocal(int valid, int mapIdx, int charaId, int flags,
                        int x, int y, int z, short rotY, short health,
                        unsigned short animIdx, unsigned short animFrame)
{
    if (!s_enabled || !s_lock)
    {
        return;
    }
    SDL_LockMutex(s_lock);
    /* A map change invalidates this map's marker list; ask for the new one. */
    if (valid && mapIdx != s_sh.localMap)
    {
        s_sh.memoCount  = 0;
        s_sh.memoMap    = -1;
        s_sh.ghostCount = 0;
        s_sh.wantMemos  = 1;
    }
    s_sh.localValid  = valid;
    s_sh.localMap    = mapIdx;
    s_sh.localChara  = charaId;
    s_sh.localFlags  = flags;
    s_sh.localX      = x;
    s_sh.localY      = y;
    s_sh.localZ      = z;
    s_sh.localRotY   = rotY;
    s_sh.localHealth = health;
    s_sh.localAnim   = animIdx;
    s_sh.localFrame  = animFrame;
    SDL_UnlockMutex(s_lock);
}

void ShNet_PumpToGameThread(void)
{
    if (!s_enabled || !s_lock)
    {
        return;
    }
    SDL_LockMutex(s_lock);
    s_gt.status      = s_sh.status;
    s_gt.selfId      = s_sh.selfId;
    s_gt.pingMs      = s_sh.pingMs;
    s_gt.serverFlags = s_sh.serverFlags;
    memcpy(s_gt.statusText, s_sh.statusText, sizeof(s_gt.statusText));
    memcpy(s_gt.serverName, s_sh.serverName, sizeof(s_gt.serverName));
    memcpy(s_gt.motd, s_sh.motd, sizeof(s_gt.motd));

    s_gt.ghostCount = s_sh.ghostCount;
    if (s_gt.ghostCount > 0)
    {
        memcpy(s_gt.ghosts, s_sh.ghosts, sizeof(ShNetGhost) * (size_t)s_gt.ghostCount);
    }
    s_gt.memoCount = s_sh.memoCount;
    if (s_gt.memoCount > 0)
    {
        memcpy(s_gt.memos, s_sh.memos, sizeof(ShNetMemo) * (size_t)s_gt.memoCount);
    }
    s_gt.peerCount = s_sh.peerCount;
    s_gt.peerTotal = s_sh.peerTotal;
    if (s_gt.peerCount > 0)
    {
        memcpy(s_gt.peers, s_sh.peers, sizeof(ShNetPeer) * (size_t)s_gt.peerCount);
    }
    SDL_UnlockMutex(s_lock);
}

/* ------------------------------------------------------------------ */
/* Accessors (game thread, lock-free against s_gt)                     */
/* ------------------------------------------------------------------ */

int ShNet_Status(void)
{
    return s_gt.status;
}

int ShNet_Enabled(void)
{
    /* Written once on the game thread in ShNet_Init and never again, so a
     * plain read is the whole synchronisation. Distinct from ShNet_Status:
     * a Steam-session-only build has no master-server status but is very
     * much running. */
    return s_enabled;
}

const char* ShNet_StatusText(void)
{
    return s_gt.statusText;
}

const char* ShNet_ServerName(void)
{
    return s_gt.serverName;
}

const char* ShNet_Motd(void)
{
    return s_gt.motd;
}

int ShNet_PingMs(void)
{
    return s_gt.pingMs;
}

unsigned int ShNet_SelfId(void)
{
    return s_gt.selfId;
}

int ShNet_ServerFlags(void)
{
    return s_gt.serverFlags;
}

int ShNet_GhostCount(void)
{
    return s_gt.ghostCount;
}

const ShNetGhost* ShNet_Ghost(int i)
{
    if (i < 0 || i >= s_gt.ghostCount)
    {
        return NULL;
    }
    return &s_gt.ghosts[i];
}

int ShNet_GhostInterp(int i, int* outX, int* outY, int* outZ, short* outRotY)
{
    const ShNetGhost* g;
    unsigned int      now;
    unsigned int      span;
    unsigned int      age;
    int               t; /* Q12 blend factor */

    if (i < 0 || i >= s_gt.ghostCount)
    {
        return 0;
    }
    g = &s_gt.ghosts[i];

    if ((g->flags & SHNET_PF_CUTSCENE) || (g->flags & SHNET_PF_MENU))
    {
        return 0;
    }

    now = ShNetPlat_Millis();
    age = now - g->curMs;
    if (age > SHNET_GHOST_STALE_MS)
    {
        return 0;
    }

    /* Play the samples one interval BEHIND the newest one. Rendering at the
     * newest sample means every frame between two packets has to extrapolate,
     * which overshoots on every direction change; a fixed delay turns the same
     * problem into plain interpolation between two known points. */
    span = (g->curMs > g->prevMs) ? (g->curMs - g->prevMs) : 1u;
    if (span > 1000u)
    {
        /* A long gap means the ghost was not being updated (map change,
         * packet loss). Snap rather than sliding across the room. */
        t = 4096;
    }
    else
    {
        t = (int)((age * 4096u) / span);
        if (t > 4096) t = 4096;
        if (t < 0)    t = 0;
        /* age is measured from the NEWEST sample, so it walks prev->cur as it
         * grows; that is the one-interval delay described above. */
    }

    if (outX) *outX = g->prevX + (int)(((long long)(g->curX - g->prevX) * t) >> 12);
    if (outY) *outY = g->prevY + (int)(((long long)(g->curY - g->prevY) * t) >> 12);
    if (outZ) *outZ = g->prevZ + (int)(((long long)(g->curZ - g->prevZ) * t) >> 12);

    if (outRotY)
    {
        /* Q3.12 angles wrap at 4096; blending 4000 -> 100 the short way round
         * means folding the difference into [-2048, 2048) first. */
        int d = (int)((unsigned short)g->curRotY - (unsigned short)g->prevRotY) & 0xFFF;
        if (d > 2048)
        {
            d -= 4096;
        }
        *outRotY = (short)((((unsigned short)g->prevRotY) + ((d * t) >> 12)) & 0xFFF);
    }
    return 1;
}

int ShNet_MemoCount(void)
{
    return s_gt.memoCount;
}

const ShNetMemo* ShNet_Memo(int i)
{
    if (i < 0 || i >= s_gt.memoCount)
    {
        return NULL;
    }
    return &s_gt.memos[i];
}

int ShNet_PeerCount(void)
{
    return s_gt.peerCount;
}

const ShNetPeer* ShNet_Peer(int i)
{
    if (i < 0 || i >= s_gt.peerCount)
    {
        return NULL;
    }
    return &s_gt.peers[i];
}

int ShNet_PeerTotal(void)
{
    return s_gt.peerTotal;
}

/* ------------------------------------------------------------------ */
/* Commands out                                                        */
/* ------------------------------------------------------------------ */

static int ShNet_QueueCmd(const ShNetCmd* c)
{
    int next;
    if (!s_enabled || !s_lock)
    {
        return 0;
    }
    SDL_LockMutex(s_lock);
    next = (s_sh.cmdHead + 1) % SHNET_CMD_RING;
    if (next == s_sh.cmdTail)
    {
        SDL_UnlockMutex(s_lock);
        return 0;
    }
    s_sh.cmds[s_sh.cmdHead] = *c;
    s_sh.cmdHead            = next;
    SDL_UnlockMutex(s_lock);
    return 1;
}

int ShNet_PlaceMarker(int kind, int x, int y, int z, short rotY,
                      unsigned short phraseA, unsigned short wordA,
                      unsigned short phraseB, unsigned short wordB)
{
    ShNetCmd c;

    if (!s_enabled || !s_lock)
    {
        return 0;
    }
    memset(&c, 0, sizeof(c));
    c.kind = SHNET_MSG_MEMO_PLACE;
    c.a    = kind;
    SDL_LockMutex(s_lock);
    c.b = s_sh.localMap;
    SDL_UnlockMutex(s_lock);
    c.c    = x;
    c.d    = y;
    c.u0   = (unsigned int)z;
    c.s0   = rotY;
    c.w0   = phraseA;
    c.w1   = wordA;
    c.w2   = phraseB;
    c.w3   = wordB;
    return ShNet_QueueCmd(&c);
}

void ShNet_ReportDeath(int x, int y, int z, short rotY)
{
    ShNet_PlaceMarker(SHNET_MARK_DEATH, x, y, z, rotY, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);
}

void ShNet_RateMemo(unsigned int memoId, int delta)
{
    ShNetCmd c;
    memset(&c, 0, sizeof(c));
    c.kind = SHNET_MSG_MEMO_RATE;
    c.u0   = memoId;
    c.a    = (delta >= 0) ? 1 : -1;
    ShNet_QueueCmd(&c);
}

void ShNet_RequestMemos(void)
{
    if (!s_enabled || !s_lock)
    {
        return;
    }
    SDL_LockMutex(s_lock);
    s_sh.wantMemos = 1;
    SDL_UnlockMutex(s_lock);
}

void ShNet_RequestRoster(void)
{
    if (!s_enabled || !s_lock)
    {
        return;
    }
    SDL_LockMutex(s_lock);
    s_sh.wantRoster = 1;
    SDL_UnlockMutex(s_lock);
}

int ShNet_PopEvent(char* out, int cap)
{
    int got = 0;
    if (!s_enabled || !s_lock || !out || cap <= 0)
    {
        return 0;
    }
    SDL_LockMutex(s_lock);
    if (s_sh.eventTail != s_sh.eventHead)
    {
        SDL_strlcpy(out, s_sh.events[s_sh.eventTail], (size_t)cap);
        s_sh.eventTail = (s_sh.eventTail + 1) % SHNET_MAX_EVENTS;
        got = 1;
    }
    SDL_UnlockMutex(s_lock);
    return got;
}
