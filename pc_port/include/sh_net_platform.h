/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_platform.h - UDP sockets for the online client, with the platform
 * stack kept behind an opaque wall.
 *
 * Same isolation contract as pc_ra_http.c: <winsock2.h> drags in <windows.h>,
 * whose RECT16 / `byte` / EnterCriticalSection cannot coexist with the PSX
 * headers, so sh_net_platform.c includes the platform stack and NOTHING from
 * the game. Everything here is expressed in plain C types.
 *
 * ShNetAddr is a byte blob rather than a sockaddr because that is the whole
 * point: a caller can store, copy and compare one without ever naming a system
 * type. 128 bytes is sockaddr_storage on both Windows and Linux, which makes
 * it large enough for IPv6 with room to spare.
 */
#ifndef SH_NET_PLATFORM_H
#define SH_NET_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

#define SHNET_ADDR_BYTES 128

typedef struct
{
    unsigned char bytes[SHNET_ADDR_BYTES];
    int           len; /* 0 = unset */
} ShNetAddr;

typedef struct ShNetSock ShNetSock;

/* Process-wide startup/teardown (WSAStartup on Windows, a no-op elsewhere).
 * Refcounted, so a second Init is free. Returns 1 on success. */
int  ShNetPlat_Init(void);
void ShNetPlat_Shutdown(void);

/* Non-blocking UDP socket bound to an ephemeral port. NULL on failure. */
ShNetSock* ShNetPlat_Open(void);
void       ShNetPlat_Close(ShNetSock* s);

/* BLOCKS on DNS — call from the network thread only. Returns 1 on success.
 * Accepts a hostname, an IPv4 literal or an IPv6 literal. */
int ShNetPlat_Resolve(const char* host, unsigned short port, ShNetAddr* out);

/* Returns bytes sent, or -1. A -1 here is not fatal: UDP send failures are
 * routine on a network that has just changed state. */
int ShNetPlat_Send(ShNetSock* s, const ShNetAddr* to, const void* buf, int len);

/* Returns bytes received, 0 when nothing is queued, or -1 on a real error.
 * Never blocks. `from` may be NULL. */
int ShNetPlat_Recv(ShNetSock* s, ShNetAddr* from, void* buf, int cap);

/* Monotonic milliseconds since ShNetPlat_Init. Not wall clock: it must not
 * jump when the system clock is adjusted mid-session. */
unsigned int ShNetPlat_Millis(void);

/* "1.2.3.4:27888" into `out`, for logs and the connection UI. */
void ShNetPlat_AddrToString(const ShNetAddr* a, char* out, int cap);

/* 1 when both name the same host AND port. */
int ShNetPlat_AddrEqual(const ShNetAddr* a, const ShNetAddr* b);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_PLATFORM_H */
