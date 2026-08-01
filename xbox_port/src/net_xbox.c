/*
 * net_xbox.c - Xbox networking + blocking plaintext-HTTP transport.
 *
 * The transport for the RetroAchievements client. RA.org serves plain HTTP (no
 * TLS), so this is a minimal HTTP/1.0 GET/POST over lwIP BSD sockets: DNS-resolve
 * the host, connect (bounded), send the request, read the whole response until
 * the server closes (Connection: close) or a recv times out, then split off the
 * body.
 *
 * ISOLATION (deliberate, matches pc_ra_http.c on PC): this file includes ONLY
 * the lwIP stack + libc + the stdio-only sh_log.h. It pulls in NOTHING from the
 * game/PSX/decomp headers -- lwIP collides with them (htons, RECT, `byte`,
 * network-order struct sockaddr). The RA-client side owns those headers and
 * talks to us solely through net_xbox.h (plain C prototypes).
 */
#include <string.h>
#include <stdlib.h>

#include <nxdk/net.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "sh_log.h"
#include "net_xbox.h"

/* nxNetInit() stores the active netif here (declared in nxdk/net.c, no public
 * header exposes it -- the httpd_bsd sample externs it the same way). */
extern struct netif *g_pnetif;

/* LWIP_SO_RCVTIMEO defaults to 0 in this fork's lwipopts, so SO_RCVTIMEO is a
 * no-op here -- every blocking wait is bounded with select() instead. */
#define NET_CONNECT_TIMEOUT_MS 4000
#define NET_RECV_TIMEOUT_MS    8000
/* After a DNS/connect failure, fast-fail every request for this long instead of
 * paying the multi-second resolve/connect timeout again. A console that drops its
 * network mid-session was stalling the frame ~8s per RA request, over and over
 * (log 018 = the "menus get terribly slow" report). net_xbox does NOT pump VSync
 * while blocking, so each stall is a hard freeze -- the backoff turns a continuous
 * stall into one probe every 20s. sys_now() is lwip's ms-since-boot clock. */
#define NET_BACKOFF_MS         20000u
extern unsigned sys_now(void);   /* lwip ms-since-boot (u32_t == unsigned on 32-bit) */

/* Hard cap on a single response so a misbehaving/slow server can't exhaust the
 * 64 MB console. RA payloads (patch data, session responses) sit far below this. */
#define NET_MAX_RESP (8 * 1024 * 1024)

static int s_attempted = 0;   /* nxNetInit was called (success or fail) */
static int s_up        = 0;   /* nxNetInit succeeded */

int Net_XboxIsUp(void)
{
    return s_up;
}

int Net_XboxBringUp(void)
{
    int rc;

    if (s_up)        return 0;    /* idempotent: already up */
    if (s_attempted) return -1;   /* one shot: nxNetInit must not run twice */
    s_attempted = 1;

    rc = nxNetInit(NULL);         /* NULL = auto (EEPROM config, DHCP), blocks <=10s */
    if (rc != 0) {
        SH_DBG("[NET] bring-up FAILED rc=%d (%s)", rc,
               (rc == -2) ? "DHCP timeout" : "no/invalid config");
        return rc;
    }

    s_up = 1;
    if (g_pnetif != NULL) {
        /* lwIP stores the address in network byte order; on little-endian x86
         * byte 0 is the first octet. Integer-only (nxdk printf drops %f/%s use
         * is fine but keep octets numeric to match the port's logging rule). */
        unsigned ip = (unsigned)ip4_addr_get_u32(netif_ip4_addr(g_pnetif));
        SH_DBG("[NET] up, DHCP IP %u.%u.%u.%u",
               (ip      ) & 0xff,
               (ip >>  8) & 0xff,
               (ip >> 16) & 0xff,
               (ip >> 24) & 0xff);
    } else {
        SH_DBG("[NET] up (no netif handle)");
    }
    return 0;
}

/* Wait until a socket is readable/writable, or the timeout elapses.
 * Returns >0 ready, 0 timeout, <0 error. */
static int Net_WaitFd(int fd, int forWrite, int ms)
{
    fd_set         fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    if (forWrite)
        return select(fd + 1, NULL, &fds, NULL, &tv);
    return select(fd + 1, &fds, NULL, NULL, &tv);
}

/* Connect to one resolved address with a bounded (non-blocking connect +
 * select) handshake. Returns a connected, blocking socket fd, or -1. */
static int Net_ConnectBounded(struct addrinfo *ai)
{
    int       s;
    int       fl;
    int       soErr;
    socklen_t soLen;

    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0)
        return -1;

    /* Non-blocking so a dead host can't stall on the full SYN-retransmit
     * budget; select() caps the handshake at NET_CONNECT_TIMEOUT_MS. */
    fl = lwip_fcntl(s, F_GETFL, 0);
    lwip_fcntl(s, F_SETFL, fl | O_NONBLOCK);

    if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
        /* In progress (or an immediate error) -- let select decide, then read
         * the pending socket error. Avoids depending on errno/EINPROGRESS,
         * which pdclib may not define. */
        if (Net_WaitFd(s, 1 /*write*/, NET_CONNECT_TIMEOUT_MS) <= 0) {
            close(s);
            return -1;
        }
        soErr = 0;
        soLen = (socklen_t)sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &soLen) != 0 || soErr != 0) {
            close(s);
            return -1;
        }
    }

    /* Back to blocking: the request is small, and each recv is gated by a
     * readable-select first, so blocking calls never stall unbounded. */
    lwip_fcntl(s, F_SETFL, fl);
    return s;
}

static int Net_SendAll(int fd, const char *p, int n)
{
    int sent = 0;
    while (sent < n) {
        int w = send(fd, p + sent, n - sent, 0);
        if (w <= 0)
            return -1;
        sent += w;
    }
    return 0;
}

int Net_XboxHttpRequest(const char* url, const char* post, char** out_body, int* out_len)
{
    const char     *p;
    const char     *pathPtr;
    char            host[256];
    char            portStr[8];
    int             hi;
    int             isPost;
    int             postLen;
    int             hdrCap;
    int             hlen;
    char           *hdr;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int             s = -1;
    char           *buf = NULL;
    size_t          cap = 0;
    size_t          len = 0;
    int             status = 0;
    size_t          i;
    char           *bodyStart = NULL;
    size_t          bodyLen = 0;

    if (out_body) *out_body = NULL;
    if (out_len)  *out_len  = 0;

    if (!url || !url[0] || !out_body || !out_len)
        return 0;
    if (!s_up)
        return 0;   /* caller must Net_XboxBringUp() during init */

    /* --- Parse "http://host[:port]/path" ------------------------------------ */
    if (strncmp(url, "http://", 7) != 0) {
        SH_DBG("[NET] non-http URL rejected (no TLS on this transport): %.80s", url);
        return 0;   /* https:// / other schemes: no TLS on this transport */
    }
    p = url + 7;

    hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < (int)sizeof(host) - 1)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0)
        return 0;

    strcpy(portStr, "80");
    if (*p == ':') {
        int pi = 0;
        p++;
        while (*p >= '0' && *p <= '9' && pi < (int)sizeof(portStr) - 1)
            portStr[pi++] = *p++;
        portStr[pi] = '\0';
        if (pi == 0)
            strcpy(portStr, "80");
    }
    while (*p && *p != '/')   /* skip any stray host remainder */
        p++;
    pathPtr = (*p == '/') ? p : "/";

    /* Network-down backoff (see NET_BACKOFF_MS): if we recently failed to resolve
     * or connect, don't block on it again -- fast-fail until the window elapses. */
    static unsigned s_netFailAtMs = 0;
    if (s_netFailAtMs != 0 && (unsigned)(sys_now() - s_netFailAtMs) < NET_BACKOFF_MS)
        return 0;

    /* --- DNS resolve -------------------------------------------------------- */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      /* IPv4 only: keep the connect path simple */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || res == NULL) {
        SH_DBG("[NET] DNS fail host=%s", host);
        s_netFailAtMs = sys_now();
        if (s_netFailAtMs == 0) s_netFailAtMs = 1;   /* 0 means "no failure" */
        return 0;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = Net_ConnectBounded(ai);
        if (s >= 0)
            break;
    }
    freeaddrinfo(res);
    if (s < 0) {
        SH_DBG("[NET] connect fail host=%s:%s", host, portStr);
        s_netFailAtMs = sys_now();
        if (s_netFailAtMs == 0) s_netFailAtMs = 1;
        return 0;
    }
    s_netFailAtMs = 0;   /* connected -> network is up, clear the backoff */

    /* --- Build + send the request ------------------------------------------ */
    isPost  = (post != NULL && post[0] != '\0');
    postLen = isPost ? (int)strlen(post) : 0;

    hdrCap = 320 + (int)strlen(pathPtr) + (int)strlen(host);
    hdr    = (char *)malloc((size_t)hdrCap);
    if (!hdr) {
        close(s);
        return 0;
    }

    if (isPost) {
        hlen = snprintf(hdr, (size_t)hdrCap,
                        "POST %s HTTP/1.0\r\n"
                        "Host: %s\r\n"
                        "User-Agent: SilentHillXbox/1.0\r\n"
                        "Accept: */*\r\n"
                        "Content-Type: application/x-www-form-urlencoded\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        pathPtr, host, postLen);
    } else {
        hlen = snprintf(hdr, (size_t)hdrCap,
                        "GET %s HTTP/1.0\r\n"
                        "Host: %s\r\n"
                        "User-Agent: SilentHillXbox/1.0\r\n"
                        "Accept: */*\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        pathPtr, host);
    }
    if (hlen < 0 || hlen >= hdrCap) {   /* truncated -> bail */
        free(hdr);
        close(s);
        return 0;
    }

    if (Net_SendAll(s, hdr, hlen) != 0 ||
        (isPost && Net_SendAll(s, post, postLen) != 0)) {
        free(hdr);
        close(s);
        SH_DBG("[NET] send fail host=%s", host);
        return 0;
    }
    free(hdr);

    /* --- Read the full response (until server close or timeout) ------------- */
    for (;;) {
        int got;

        if (Net_WaitFd(s, 0 /*read*/, NET_RECV_TIMEOUT_MS) <= 0)
            break;   /* timeout or select error -> stop */

        if (len + 4096 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 16384;
            char  *ng;
            if (ncap > (size_t)NET_MAX_RESP + 4096)
                ncap = (size_t)NET_MAX_RESP + 4096;
            if (ncap <= len + 1)
                break;   /* cap reached */
            ng = (char *)realloc(buf, ncap);
            if (!ng)
                break;
            buf = ng;
            cap = ncap;
        }

        got = recv(s, buf + len, (int)(cap - len - 1), 0);
        if (got <= 0)
            break;   /* 0 = server closed (normal end), <0 = error */
        len += (size_t)got;
        if (len >= (size_t)NET_MAX_RESP)
            break;
    }
    close(s);

    if (!buf || len == 0) {
        free(buf);
        SH_DBG("[NET] empty response host=%s", host);
        return 0;
    }
    buf[len] = '\0';

    /* --- Parse the status line: "HTTP/1.x SSS ..." ------------------------- */
    {
        const char *sp = (const char *)memchr(buf, ' ', len);
        if (sp) {
            const char *c = sp + 1;
            while (c < buf + len && *c == ' ')
                c++;
            while (c < buf + len && *c >= '0' && *c <= '9')
                status = status * 10 + (*c++ - '0');
        }
    }

    /* --- Split off the body at the first CRLFCRLF, shift it to the front --- */
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            bodyStart = buf + i + 4;
            bodyLen   = len - (i + 4);
            break;
        }
    }

    if (bodyStart) {
        memmove(buf, bodyStart, bodyLen);   /* overlapping: memmove, not memcpy */
        buf[bodyLen] = '\0';
        *out_body = buf;
        *out_len  = (int)bodyLen;
    } else {
        /* No header terminator (truncated). Keep the status, drop the buffer. */
        free(buf);
        *out_body = NULL;
        *out_len  = 0;
    }

    SH_DBG("[NET] %s %s -> %d (%d bytes)",
           isPost ? "POST" : "GET", host, status, (int)bodyLen);
    return status;
}
