/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net.h - game-facing API for Silent Hill Online.
 *
 * THREADING. A worker thread owns the socket and the whole protocol state
 * machine, for the same reason the RetroAchievements client uses one: a server
 * that has gone away must never be able to stall a frame, and DNS resolution
 * blocks for seconds on a bad hostname.
 *
 * The game thread touches exactly one function per frame — ShNet_GameTick —
 * which takes the lock once to publish this player's position and to copy the
 * shared snapshot into a game-thread-private buffer. Every other reader
 * (ShNet_Ghost*, ShNet_Memo*, ShNet_Peer*) then works off that private copy
 * with no lock at all, which is what keeps the world-render path free of
 * synchronization.
 *
 * COORDINATES are the game's own: Q19.12 world space straight out of
 * g_SysWork.playerWork.player.position, and a Q3.12 heading. No conversion
 * happens anywhere in the stack, so a ghost stands exactly where the other
 * player stands.
 */
#ifndef SH_NET_H
#define SH_NET_H

#include "sh_net_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    SHNET_ST_OFF = 0,     /* disabled in config, or shut down */
    SHNET_ST_RESOLVING,   /* looking up the master server hostname */
    SHNET_ST_CONNECTING,  /* HELLO sent, waiting for WELCOME */
    SHNET_ST_CONNECTED,
    SHNET_ST_REJECTED,    /* server said no; ShNet_StatusText has the reason */
    SHNET_ST_LOST         /* was connected, stopped hearing back; retrying */
};

/* Ghost render styles (config: online_ghost_style). */
enum
{
    SHNET_GS_SILHOUETTE = 0, /* the hollow humanoid contour alone */
    SHNET_GS_FOOTPRINT  = 1, /* a ring on the floor where they stand */
    SHNET_GS_BOTH       = 2
};

#define SHNET_MAX_GHOSTS 32
#define SHNET_MAX_PEERS  128
#define SHNET_MAX_MEMOS  96
#define SHNET_MAX_EVENTS 8

typedef struct
{
    unsigned int   playerId;
    int            charaId;
    int            flags;      /* SHNET_PF_* */
    /* Two samples plus their arrival times: the server speaks at ~10Hz and the
     * game draws at 30-60, so a ghost that simply teleported to the newest
     * sample would visibly step. ShNet_GhostInterp walks between them. */
    int            curX, curY, curZ;
    int            prevX, prevY, prevZ;
    short          curRotY;
    short          prevRotY;
    unsigned int   curMs;
    unsigned int   prevMs;
    unsigned short animIdx;
    unsigned short animFrame;
    char           name[SHNET_NAME_MAX];
} ShNetGhost;

typedef struct
{
    unsigned int   memoId;
    unsigned int   ownerId;
    int            kind;       /* SHNET_MARK_* */
    int            mapIdx;
    int            x, y, z;
    short          rotY;
    short          rating;
    unsigned short phraseA, wordA, phraseB, wordB;
    char           owner[SHNET_NAME_MAX];
} ShNetMemo;

typedef struct
{
    unsigned int playerId;
    int          mapIdx;
    int          charaId;
    int          flags;
    int          pingMs;
    int          uptimeMin;
    char         name[SHNET_NAME_MAX];
} ShNetPeer;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Reads g_PcConfig and, when online_enabled is set, starts the worker thread.
 * Safe to call when disabled: it becomes a no-op and every accessor below
 * reports an empty world. */
void ShNet_Init(void);
void ShNet_Shutdown(void);

/* Once per frame from the game thread, after the frame's game state settles.
 * This is the ONLY function that takes the lock. */
void ShNet_GameTick(void);

/* Reconnect with the current config (used by the console command and by the
 * player-list panel's Reconnect row). */
void ShNet_Reconnect(void);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

/* 1 when the worker is running at all - the master server, a Steam session, or
 * both. ShNet_Status only describes the master-server half. */
int         ShNet_Enabled(void);

/* 1 when the world is shared or watched live -- co-op is active, or we are
 * connected to a master server. Pausing such a world is no longer a purely
 * local act, so the pause button and the console both consult this. */
int         ShNet_LiveWorld(void);
int         ShNet_Status(void);        /* SHNET_ST_* */
const char* ShNet_StatusText(void);    /* one short line for the UI */
const char* ShNet_ServerName(void);
const char* ShNet_Motd(void);
int         ShNet_PingMs(void);
unsigned int ShNet_SelfId(void);
int         ShNet_ServerFlags(void);   /* SHNET_SF_* the server admitted to */

/* ------------------------------------------------------------------ */
/* Ghosts — the other players in THIS map, already filtered by the server      */
/* ------------------------------------------------------------------ */

int               ShNet_GhostCount(void);
const ShNetGhost* ShNet_Ghost(int i);

/* Position at the current wall clock, interpolated between the ghost's two
 * newest samples. Returns 0 when the ghost should not be drawn this frame
 * (stale beyond the extrapolation window, in a cutscene, or in a menu). */
int ShNet_GhostInterp(int i, int* outX, int* outY, int* outZ, short* outRotY);

/* ------------------------------------------------------------------ */
/* Markers                                                             */
/* ------------------------------------------------------------------ */

int              ShNet_MemoCount(void);
const ShNetMemo* ShNet_Memo(int i);

/* Queue a marker for the server at the given world position. Returns 1 when
 * queued. phraseB/wordB may be 0xFFFF for a single-clause memo. */
int ShNet_PlaceMarker(int kind, int x, int y, int z, short rotY,
                      unsigned short phraseA, unsigned short wordA,
                      unsigned short phraseB, unsigned short wordB);

/* Called from the death path. Fire-and-forget; the server dedupes. */
void ShNet_ReportDeath(int x, int y, int z, short rotY);

void ShNet_RateMemo(unsigned int memoId, int delta);

/* Ask for this map's markers again (map load, or the player pressed refresh). */
void ShNet_RequestMemos(void);

/* ------------------------------------------------------------------ */
/* Roster                                                              */
/* ------------------------------------------------------------------ */

int              ShNet_PeerCount(void);
const ShNetPeer* ShNet_Peer(int i);
int              ShNet_PeerTotal(void); /* what the server says, may exceed PeerCount */
void             ShNet_RequestRoster(void);

/* ------------------------------------------------------------------ */
/* Event feed                                                          */
/* ------------------------------------------------------------------ */

/* Pops the oldest unread server event line into `out`, returning 1 when one
 * was waiting. Drives the toast. */
int ShNet_PopEvent(char* out, int cap);

/* ------------------------------------------------------------------ */
/* Chat                                                                */
/* ------------------------------------------------------------------ */

/* Queue a line for the server. scope is SHNET_CHAT_GLOBAL or SHNET_CHAT_GAME.
 * Fire-and-forget; drops silently if the queue is full or online is off. */
void ShNet_SendChat(int scope, const char* text);

/* Pop the oldest received chat line. Returns 1 when one was waiting. The chat
 * UI drains this each frame. */
int  ShNet_PopChat(int* outScope, unsigned int* outFromId, char* outName, int nameCap,
                   char* outText, int textCap);

/* ------------------------------------------------------------------ */
/* Hooks the rest of the port calls                                    */
/* ------------------------------------------------------------------ */

/* world_draw.c emits ghosts and markers into the world ordering table by
 * calling ShNet_DrawWorld(GsOT*). It is NOT declared here: GsOT is an
 * anonymous-struct typedef in libgs.h, so a `struct GsOT*` forward declaration
 * would silently be a different type. The call site declares it inline, the
 * way Pc_DecalsDraw is declared. */

/* Map init: drop this map's markers and re-query. */
void ShNet_OnMapChanged(int mapIdx);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_H */
