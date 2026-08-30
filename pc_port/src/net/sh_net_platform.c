/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_platform.c - UDP transport for the online client.
 *
 * Deliberately isolated from the rest of the port, exactly as pc_ra_http.c is:
 * this file includes the platform networking stack and NOTHING from the game.
 * <winsock2.h> pulls in <windows.h>, and RECT16 / `byte` / the CRITICAL_SECTION
 * family collide with the PSX headers the moment both land in one TU.
 *
 * Winsock2 on Windows, BSD sockets elsewhere.
 */
#if defined(_WIN32)
/* Ahead of any other header: <windows.h> arriving first would drag in the
 * winsock1 declarations and every winsock2 symbol would then redefine them. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET  ShnRawSock;
#define SHN_INVALID_SOCK INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
typedef int ShnRawSock;
#define SHN_INVALID_SOCK (-1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_net_platform.h"

struct ShNetSock
{
    ShnRawSock fd;
};

static int          s_initRefs;
static unsigned int s_epochMs;

#if defined(_WIN32)
static unsigned int ShnRawMillis(void)
{
    return (unsigned int)GetTickCount();
}
#else
static unsigned int ShnRawMillis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

int ShNetPlat_Init(void)
{
    if (s_initRefs > 0)
    {
        s_initRefs++;
        return 1;
    }
#if defined(_WIN32)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            return 0;
        }
    }
#endif
    s_epochMs  = ShnRawMillis();
    s_initRefs = 1;
    return 1;
}

void ShNetPlat_Shutdown(void)
{
    if (s_initRefs <= 0)
    {
        return;
    }
    if (--s_initRefs == 0)
    {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
}

unsigned int ShNetPlat_Millis(void)
{
    return ShnRawMillis() - s_epochMs;
}

ShNetSock* ShNetPlat_Open(void)
{
    ShNetSock* s = (ShNetSock*)calloc(1, sizeof(ShNetSock));
    if (!s)
    {
        return NULL;
    }

    /* AF_INET rather than a dual-stack AF_INET6: the resolver below asks for
     * IPv4 too, and a v6 socket that has to reach a v4-only server needs
     * v4-mapped addresses working, which is off by default on some Windows
     * configurations. One family, no surprises. */
    s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd == SHN_INVALID_SOCK)
    {
        free(s);
        return NULL;
    }

#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(s->fd, FIONBIO, &nb);
    }
    {
        /* Without this, a single ICMP port-unreachable (server not running
         * yet) latches the socket into a permanent WSAECONNRESET on every
         * subsequent recvfrom, and the client can never reconnect without
         * reopening. Windows-only misfeature; BSD sockets do not do it. */
        DWORD  off = 0;
        DWORD  ret = 0;
        BOOL   b   = FALSE;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        (void)off;
        WSAIoctl(s->fd, SIO_UDP_CONNRESET, &b, sizeof(b), NULL, 0, &ret, NULL, NULL);
    }
#else
    {
        int fl = fcntl(s->fd, F_GETFL, 0);
        fcntl(s->fd, F_SETFL, fl | O_NONBLOCK);
    }
#endif

    {
        struct sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family      = AF_INET;
        any.sin_addr.s_addr = INADDR_ANY;
        any.sin_port        = 0;
        if (bind(s->fd, (struct sockaddr*)&any, sizeof(any)) != 0)
        {
            ShNetPlat_Close(s);
            return NULL;
        }
    }
    return s;
}

void ShNetPlat_Close(ShNetSock* s)
{
    if (!s)
    {
        return;
    }
    if (s->fd != SHN_INVALID_SOCK)
    {
#if defined(_WIN32)
        closesocket(s->fd);
#else
        close(s->fd);
#endif
    }
    free(s);
}

int ShNetPlat_Resolve(const char* host, unsigned short port, ShNetAddr* out)
{
    struct addrinfo  hints;
    struct addrinfo* res = NULL;
    char             portStr[16];

    if (!host || !host[0] || !out)
    {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res)
    {
        return 0;
    }
    if (res->ai_addrlen > (unsigned)SHNET_ADDR_BYTES)
    {
        freeaddrinfo(res);
        return 0;
    }
    memcpy(out->bytes, res->ai_addr, res->ai_addrlen);
    out->len = (int)res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

int ShNetPlat_Send(ShNetSock* s, const ShNetAddr* to, const void* buf, int len)
{
    int n;
    if (!s || !to || to->len <= 0 || !buf || len <= 0)
    {
        return -1;
    }
    n = (int)sendto(s->fd, (const char*)buf, (size_t)len, 0,
                    (const struct sockaddr*)to->bytes, (socklen_t)to->len);
    return n;
}

int ShNetPlat_Recv(ShNetSock* s, ShNetAddr* from, void* buf, int cap)
{
    unsigned char     stor[SHNET_ADDR_BYTES];
    socklen_t         slen = (socklen_t)sizeof(stor);
    int               n;

    if (!s || !buf || cap <= 0)
    {
        return -1;
    }

    n = (int)recvfrom(s->fd, (char*)buf, (size_t)cap, 0,
                      (struct sockaddr*)stor, &slen);
    if (n < 0)
    {
#if defined(_WIN32)
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAECONNRESET)
        {
            return 0;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED)
        {
            return 0;
        }
#endif
        return -1;
    }

    if (from)
    {
        memset(from, 0, sizeof(*from));
        if (slen > 0 && slen <= (socklen_t)SHNET_ADDR_BYTES)
        {
            memcpy(from->bytes, stor, (size_t)slen);
            from->len = (int)slen;
        }
    }
    return n;
}

void ShNetPlat_AddrToString(const ShNetAddr* a, char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (!a || a->len < (int)sizeof(struct sockaddr_in))
    {
        snprintf(out, (size_t)cap, "(unset)");
        return;
    }
    {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)(const void*)a->bytes;
        unsigned int              ip  = ntohl(sin->sin_addr.s_addr);
        snprintf(out, (size_t)cap, "%u.%u.%u.%u:%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                 (unsigned)ntohs(sin->sin_port));
    }
}

int ShNetPlat_AddrEqual(const ShNetAddr* a, const ShNetAddr* b)
{
    if (!a || !b || a->len <= 0 || a->len != b->len)
    {
        return 0;
    }
    return memcmp(a->bytes, b->bytes, (size_t)a->len) == 0;
}
