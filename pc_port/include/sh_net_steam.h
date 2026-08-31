/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_steam.h - Steam lobbies, invites, presence and peer-to-peer.
 *
 * WHY THIS EXISTS. The master server (sh_master.c) is the right tool for the
 * ambient world — ghosts and messages from strangers, no accounts, anyone can
 * host one. It is the wrong tool for a co-op session between two friends, which
 * wants an invite you click in the Steam overlay, a friends list, and a
 * connection that works through a NAT nobody configured. Steam does all four
 * and does them better than we ever would.
 *
 * WHY IT LOADS AT RUNTIME. The Steamworks SDK is not redistributable in a
 * GPL-3 source tree, and a hard link against it would mean the port stops
 * building for anyone who has not fetched it. So this file resolves
 * steam_api64.dll with GetProcAddress and declares the FLAT C API itself. The
 * consequences are all good ones: no SDK in the repo, no build dependency, no
 * licensing question, and a build with no Steam at all just logs one line and
 * stays dormant.
 *
 * WHY THE APP ID IS 480. 480 is Spacewar, Valve's public test application.
 * Every Steam account owns it, so an unshipped game can use lobbies, P2P and
 * the overlay without an AppID of its own. It is what every fan port and
 * recompilation project does, and it is what `online_steam_appid` defaults to.
 * A project that later gets a real AppID changes one config line.
 *
 * THREADING. Steam is called from ONE thread — the network worker in
 * sh_net_client.c — and every function here is worker-thread-only unless its
 * comment says otherwise. The game thread asks for things by setting a flag
 * the worker services, exactly as it already does for the roster and markers.
 * The few read-only accessors marked SAFE ANYWHERE return a value the worker
 * published under the client's lock.
 */
#ifndef SH_NET_STEAM_H
#define SH_NET_STEAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spacewar. See the file header. */
#define SHSTEAM_DEFAULT_APPID 480

/* Channel the port's own datagrams travel on, so a future co-op session and
 * anything else sharing the link cannot be confused for one another. */
#define SHSTEAM_CHANNEL 41

#define SHSTEAM_NAME_MAX  40
#define SHSTEAM_MAX_MEMBERS 8

enum
{
    SHSTEAM_LOBBY_NONE = 0,
    SHSTEAM_LOBBY_CREATING,
    SHSTEAM_LOBBY_JOINING,
    SHSTEAM_LOBBY_IN,
    SHSTEAM_LOBBY_FAILED
};

/* Lobby visibility, matching ELobbyType. */
enum
{
    SHSTEAM_LOBBY_PRIVATE     = 0,
    SHSTEAM_LOBBY_FRIENDSONLY = 1,
    SHSTEAM_LOBBY_PUBLIC      = 2,
    SHSTEAM_LOBBY_INVISIBLE   = 3
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Resolves steam_api64.dll and calls SteamAPI_Init. Returns 1 when Steam is
 * usable. Every failure is soft: no DLL, Steam not running, a DLL too old to
 * export what is needed — each logs once and leaves the layer dormant, and
 * every other function here then does nothing. */
int  ShSteam_Init(unsigned int appId);
void ShSteam_Shutdown(void);

/* 1 once Init has succeeded. SAFE ANYWHERE. */
int  ShSteam_Available(void);

/* Pump Steam's callback queue. Must be called regularly from the worker or
 * nothing asynchronous — lobby creation, joins, invites — ever completes. */
void ShSteam_RunCallbacks(void);

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

unsigned long long ShSteam_SelfId(void);
const char*        ShSteam_PersonaName(void);
/* Display name for any Steam user; "" when Steam has not cached one yet. */
const char*        ShSteam_NameOf(unsigned long long steamId);

/* ------------------------------------------------------------------ */
/* Lobby                                                               */
/* ------------------------------------------------------------------ */

int                ShSteam_LobbyState(void);   /* SHSTEAM_LOBBY_* */
unsigned long long ShSteam_LobbyId(void);
unsigned long long ShSteam_LobbyOwner(void);
int                ShSteam_IsLobbyOwner(void);
int                ShSteam_MemberCount(void);
unsigned long long ShSteam_Member(int i);

/* Both are asynchronous: the state moves to CREATING/JOINING and reaches IN
 * (or FAILED) a few RunCallbacks later. */
void ShSteam_CreateLobby(int lobbyType, int maxMembers);
void ShSteam_JoinLobby(unsigned long long lobbyId);
void ShSteam_LeaveLobby(void);

/* Lobby metadata, which is how a joiner learns what it is joining before any
 * of the port's own protocol has run. Owner-only for Set. */
void        ShSteam_SetLobbyData(const char* key, const char* value);
const char* ShSteam_GetLobbyData(const char* key);

/* Opens the Steam overlay's friend-invite dialog for the current lobby. Does
 * nothing when not in a lobby, or when the overlay is unavailable (which it is
 * for a game launched outside Steam). */
void ShSteam_OpenInviteOverlay(void);

/* A lobby id the player asked to join from OUTSIDE the game: the overlay's
 * "Join game", a friends-list invite, or the +connect_lobby command line.
 * Returns 0 when there is none, and CONSUMES it, so a caller that returns
 * non-zero owns the join. */
unsigned long long ShSteam_TakePendingJoin(void);

/* Seed a +connect_lobby id parsed from argv, before Init. */
void ShSteam_SetCommandLineLobby(unsigned long long lobbyId);

/* ------------------------------------------------------------------ */
/* Rich presence                                                       */
/* ------------------------------------------------------------------ */

/* "status" is the line friends see. Passing NULL clears everything. */
void ShSteam_SetRichPresence(const char* key, const char* value);

/* ------------------------------------------------------------------ */
/* Peer to peer                                                        */
/* ------------------------------------------------------------------ */

/* Datagrams to another Steam user, over Valve's relay network: no port
 * forwarding, and NAT traversal is Steam's problem rather than ours. The
 * payload is whatever the caller wants — for the port that means the same
 * sh_net_proto.h datagrams the UDP transport carries.
 *
 * Returns 1 when handed to Steam. `reliable` picks between the
 * fire-and-forget and the retransmitting send. */
int ShSteam_Send(unsigned long long steamId, const void* buf, int len, int reliable);

/* Next queued datagram, or 0 when none. Never blocks. */
int ShSteam_Recv(unsigned long long* outFrom, void* buf, int cap);

/* Accept an incoming session from a lobby member. Called by the dispatch loop
 * itself; exposed for a caller that wants to allow someone outside the lobby. */
void ShSteam_AcceptSession(unsigned long long steamId);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* One line for the console and the player list, e.g.
 * "Steam: in a lobby of 2 as Harry (76561198…)". SAFE ANYWHERE. */
void ShSteam_StatusLine(char* out, int cap);

/* Which of the interfaces this layer wants actually resolved, as a bitmask, so
 * a version mismatch in someone's steam_api64.dll is diagnosable rather than
 * just "it does not work". */
enum
{
    SHSTEAM_IF_USER      = 1 << 0,
    SHSTEAM_IF_FRIENDS   = 1 << 1,
    SHSTEAM_IF_MATCHMAKE = 1 << 2,
    SHSTEAM_IF_UTILS     = 1 << 3,
    SHSTEAM_IF_MESSAGES  = 1 << 4
};
int ShSteam_Interfaces(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_STEAM_H */
