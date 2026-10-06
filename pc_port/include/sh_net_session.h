/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_session.h - the Steam co-op session, and the seam co-op plugs into.
 *
 * A SESSION is a Steam lobby whose members are in direct peer-to-peer contact
 * over Valve's relay. It is a different thing from the master-server world in
 * sh_net.h, and deliberately so:
 *
 *   master server   strangers, ambient, no accounts, anyone can host one,
 *                   ghosts and messages only
 *   session         friends, invited through the Steam overlay, direct P2P,
 *                   and the place actual co-op will live
 *
 * They coexist. A player can be wandering a master server's fogged town and be
 * in a two-person session at the same time; the two systems share only the
 * datagram format.
 *
 * WHAT WORKS TODAY: create or join a lobby, invite through the overlay, join
 * from an invite or from +connect_lobby, see who is in the session, and a live
 * round-trip measurement to each member over the real P2P link.
 *
 * WHAT IT IS FOR: that last part is the point. The link check is not a
 * diagnostic bolted on the side, it is the co-op transport running end to end
 * with one trivial message type on it. Adding "here is where I am and what I
 * am doing" is another type in the same range in sh_net_proto.h, sent from the
 * same place, parsed in the same switch.
 *
 * THREADING: everything here runs on the network worker thread, which is the
 * one thread Steam is called from. The accessors are read from the game thread
 * against a copy the worker publishes, the same arrangement the rest of the
 * client uses.
 */
#ifndef SH_NET_SESSION_H
#define SH_NET_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define SHSESSION_MAX_MEMBERS 8
#define SHSESSION_NAME_MAX    40

typedef struct
{
    unsigned long long steamId;
    char               name[SHSESSION_NAME_MAX];
    int                pingMs;     /* -1 until a pong comes back */
    int                linked;     /* 1 once they have answered us */
    int                mapIdx;     /* -1 unknown */

    /* Co-op presence: this member's last reported pose (SHNET_MSG_S_POS). Valid
     * only while poseMs is recent and the member is on our map. */
    int                charaId;
    int                flags;      /* SHNET_PF_* */
    int                x, y, z;    /* Q19.12 world position */
    short              rotY;       /* Q3.12 */
    unsigned short     anim;
    unsigned short     frame;
    unsigned int       poseMs;     /* wall clock of the last pose, 0 = none */
} ShSessionMember;

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

/* Brings Steam up when online_steam is set. Safe and cheap when it is not. */
void ShSession_Init(void);
void ShSession_Shutdown(void);

/* Once per worker iteration: pump Steam, service what the game thread asked
 * for, exchange session traffic, and refresh the published copy. */
void ShSession_Tick(unsigned int nowMs);

/* ------------------------------------------------------------------ */
/* Game thread                                                         */
/* ------------------------------------------------------------------ */

/* Requests. Each sets a flag the worker services on its next tick, because
 * Steam must only ever be called from that thread. */
void ShSession_RequestHost(void);
void ShSession_RequestJoin(unsigned long long lobbyId);

/* Co-op over a relay server (vs Steam). Host opens a room, join enters one by its
 * code, list asks the server for open rooms. host is an IP or name; port 0 means
 * the default. These switch the session to the UDP backend. */
void ShSession_RequestHostServer(const char* host, int port, int maxPlayers);
void ShSession_RequestJoinServer(const char* host, int port, int code);
void ShSession_RequestListServer(const char* host, int port);
void ShSession_RequestLeave(void);
void ShSession_RequestInvite(void);

/* Published state, safe to read from the game thread. */
int                ShSession_Active(void);      /* 1 while in a lobby */
int                ShSession_MemberCount(void);
const ShSessionMember* ShSession_Member(int i);
unsigned long long ShSession_LobbyId(void);
int                ShSession_IsHost(void);
/* One line for the console and the player list panel. */
void               ShSession_StatusLine(char* out, int cap);

/* The area name the session advertises as rich presence, so friends see
 * "Alchemilla Hospital" rather than "In Game". Called from the game thread with
 * whatever the player is looking at; the worker forwards it to Steam. */
void ShSession_PublishPresence(const char* area, int mapIdx);

/* Co-op presence. The game thread publishes the local player's pose once a
 * frame; the worker broadcasts it to the other members as SHNET_MSG_S_POS, and
 * each member's incoming pose lands in its ShSessionMember for the renderer to
 * read. map < 0 means "not in a map" (menus) -- no pose is sent. */
void ShSession_PublishLocalPos(int mapIdx, int charaId, int flags,
                               int x, int y, int z, short rotY,
                               unsigned short anim, unsigned short frame);

/* Host -> guests: everyone boot into this map for a co-op game. Sent when the
 * host presses Start Game. */
void ShSession_RequestWorld(int mapIdx);

/* Guest poll: a map the host told us to boot into since the last call, or -1.
 * Clears the request. The game thread consumes this to launch the co-op game. */
int  ShSession_TakeWorldRequest(void);

/* Co-op shared items. The game thread notices a local pickup and queues it here;
 * the worker broadcasts it (reliable) to every other member. On the other end the
 * worker queues what it receives, and the game thread drains it with TakeItem and
 * grants it to the local inventory -- so each player keeps their own pool and
 * everyone ends up with what anyone picked up. */
void ShSession_QueueItem(int itemId, int count);
int  ShSession_TakeItem(int* itemId, int* count);

/* Co-op debug/cheat permission for JOINED players. The host sets it (off by
 * default); it rides the Steam lobby data so every guest reads the same answer.
 * The host's own debug controls are never gated by this -- only guests'. */
void ShSession_SetGuestDebug(int on);
int  ShSession_GuestDebugGranted(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_SESSION_H */
