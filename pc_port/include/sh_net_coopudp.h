/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_coopudp.h - co-op over a relay server (the sh_master rooms).
 *
 * The transport the co-op session rides when it is NOT using Steam P2P. It owns
 * a UDP socket to the server, does the same HELLO/WELCOME handshake the living
 * world uses, then creates or joins a ROOM and relays the session's S_* messages
 * through the server to the rest of the room. It exposes the same handful of
 * transport primitives the session needs (members, self id, send, recv), so the
 * session logic above it is identical to the Steam path.
 *
 * THREADING: every function here is called only from the net worker (from
 * ShSession_Tick), never the game thread, so it holds no locks of its own. The
 * game thread reaches it only through the session's published snapshot.
 */
#ifndef SH_NET_COOPUDP_H
#define SH_NET_COOPUDP_H

#ifdef __cplusplus
extern "C" {
#endif

void CoopUdp_Init(void);
void CoopUdp_Shutdown(void);
void CoopUdp_Tick(unsigned int nowMs); /* pump: resolve, hello, ping, drain recv */

/* Point at a server (host name or dotted IP) and arm the connect. Returns 0 if
 * the address will not resolve. Switching servers drops any current room. */
int  CoopUdp_SetServer(const char* host, unsigned short port);

/* Unified transport: ride the living-world client's existing connection instead
 * of opening our own socket (used when co-op is on the same server the living
 * world is connected to). The living-world worker feeds inbound room/relay
 * packets here. Both run on that worker thread. */
void CoopUdp_UseWorldLink(void);
void CoopUdp_OnWorldPacket(int type, const unsigned char* p, int len);

/* Host co-op on THIS PC: start the relay in-process (no separate server) and
 * connect to it on loopback. Guests reach it at this machine's own address. */
void CoopUdp_HostListen(unsigned short port, int maxPlayers);

void CoopUdp_CreateRoom(int maxPlayers, int hidden); /* host a room (hidden = not listed, code only) */
void CoopUdp_JoinRoom(unsigned short code);  /* join a room by its shareable code */
void CoopUdp_RequestRoomList(void);          /* ask the server for open rooms */
void CoopUdp_Leave(void);                    /* leave the room (stays connected) */
void CoopUdp_Disconnect(void);               /* leave the room AND drop the server link */

int                CoopUdp_Connected(void);  /* WELCOME received from the server */
int                CoopUdp_Active(void);     /* in a room */
int                CoopUdp_IsHost(void);
unsigned long long CoopUdp_SelfId(void);
unsigned long long CoopUdp_RoomId(void);     /* the room code, 0 if none */
int                CoopUdp_MemberCount(void);
unsigned long long CoopUdp_Member(int i);
const char*        CoopUdp_NameOf(unsigned long long id);
void               CoopUdp_StatusLine(char* out, int cap);

/* Relay one of the session's S_* packets to a room member (to == 0 = all). */
int  CoopUdp_Send(unsigned long long to, const unsigned char* buf, int len);
/* Pop one relayed packet; returns its length and the sender in *from, or 0. */
int  CoopUdp_Recv(unsigned long long* from, unsigned char* buf, int cap);

/* Open-room browser results from the last RequestRoomList. */
int  CoopUdp_RoomListCount(void);
int  CoopUdp_RoomListGet(int i, unsigned short* code, int* players, int* max,
                         char* host, int hostCap);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_COOPUDP_H */
