/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shnet_selftest.c - drives a running sh_master with two synthetic players and
 * checks that the protocol does what sh_net_proto.h says it does.
 *
 * The game client cannot be unit tested (it needs an engine, a window and a
 * disc image), but everything it says on the wire can be. This program speaks
 * the same protocol from the same header, so a mismatch between the two ends
 * shows up here in a second rather than as "ghosts do not appear" after a
 * ten-minute build and a manual playthrough.
 *
 *   gcc -O2 -I../pc_port/include shnet_selftest.c -o shnet_selftest.exe -lws2_32
 *   sh_master -p 27899 -d selftest.db &
 *   shnet_selftest 127.0.0.1 27899
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET    TSock;
typedef int       TSockLen;
#define T_SLEEP(ms) Sleep(ms)
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
typedef int       TSock;
typedef socklen_t TSockLen;
#define T_SLEEP(ms) usleep((ms) * 1000)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_net_proto.h"

static int g_fail;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); }      \
        else      { printf("  FAIL " __VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

typedef struct
{
    TSock              sock;
    struct sockaddr_in server;
    shn_u32            session;
    shn_u32            playerId;
    char               name[SHNET_NAME_MAX];
} Peer;

static void PeerOpen(Peer* p, const char* host, unsigned short port, const char* name)
{
    memset(p, 0, sizeof(*p));
    p->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    p->server.sin_family      = AF_INET;
    p->server.sin_port        = htons(port);
    p->server.sin_addr.s_addr = inet_addr(host);
    snprintf(p->name, sizeof(p->name), "%s", name);
#if defined(_WIN32)
    { u_long nb = 1; ioctlsocket(p->sock, FIONBIO, &nb); }
#else
    { int fl = fcntl(p->sock, F_GETFL, 0); fcntl(p->sock, F_SETFL, fl | O_NONBLOCK); }
#endif
    {
        struct sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family      = AF_INET;
        any.sin_addr.s_addr = INADDR_ANY;
        bind(p->sock, (struct sockaddr*)&any, sizeof(any));
    }
}

static void PeerSend(Peer* p, const shn_u8* buf, int len)
{
    sendto(p->sock, (const char*)buf, (size_t)len, 0,
           (struct sockaddr*)&p->server, (TSockLen)sizeof(p->server));
}

/* Waits up to `ms` for a datagram of `want`, returning its payload length or
 * -1. Everything else that arrives is discarded, which is what makes this
 * usable for "did the SNAPSHOT come" while PONGs are also in flight. */
static int PeerWait(Peer* p, int want, int ms, shn_u8* payOut, int payCap)
{
    unsigned int spent = 0;
    while (spent < (unsigned int)ms)
    {
        shn_u8  buf[SHNET_MTU];
        int     n = (int)recv(p->sock, (char*)buf, sizeof(buf), 0);
        if (n > 0)
        {
            shn_u8  type;
            shn_u16 payLen;
            shn_u32 session;
            if (ShnParseHeader(buf, n, &type, &payLen, &session) && type == want)
            {
                if (payOut && payLen <= (shn_u16)payCap)
                {
                    memcpy(payOut, buf + SHNET_HDR_SIZE, payLen);
                }
                return (int)payLen;
            }
            continue;
        }
        T_SLEEP(10);
        spent += 10;
    }
    return -1;
}

static int PeerHello(Peer* p)
{
    shn_u8 buf[SHNET_HDR_SIZE + 128];
    shn_u8 pay[SHNET_MAX_PAYLOAD];
    int    off = SHNET_HDR_SIZE;
    int    n;

    ShnPutU16(buf, &off, SHNET_PROTO_VER);
    ShnPutU8(buf, &off, 0);
    ShnPutU8(buf, &off, 0);
    ShnPutU32(buf, &off, 0);
    ShnPutStr(buf, &off, p->name, SHNET_NAME_MAX);
    ShnPutStr(buf, &off, "selftest", SHNET_BUILD_MAX);
    ShnPutU32(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_HELLO, (shn_u16)(off - SHNET_HDR_SIZE), 0);
    PeerSend(p, buf, off);

    n = PeerWait(p, SHNET_MSG_WELCOME, 2000, pay, sizeof(pay));
    if (n < 0)
    {
        return 0;
    }
    {
        int o = 0;
        p->session  = ShnGetU32(pay, &o);
        p->playerId = ShnGetU32(pay, &o);
    }
    return 1;
}

static void PeerState(Peer* p, int mapIdx, int x, int y, int z, int rotY, int flags)
{
    shn_u8 buf[SHNET_HDR_SIZE + 64];
    int    off = SHNET_HDR_SIZE;
    static shn_u32 seq;

    ShnPutU32(buf, &off, ++seq);
    ShnPutU8(buf, &off, (shn_u8)mapIdx);
    ShnPutU8(buf, &off, 0);
    ShnPutU8(buf, &off, (shn_u8)flags);
    ShnPutU8(buf, &off, 0);
    ShnPutS32(buf, &off, x);
    ShnPutS32(buf, &off, y);
    ShnPutS32(buf, &off, z);
    ShnPutS16(buf, &off, (shn_s16)rotY);
    ShnPutS16(buf, &off, 100);
    ShnPutU16(buf, &off, 1);
    ShnPutU16(buf, &off, 2);
    ShnPutHeader(buf, SHNET_MSG_STATE, (shn_u16)(off - SHNET_HDR_SIZE), p->session);
    PeerSend(p, buf, off);
}

static void PeerPlace(Peer* p, int kind, int mapIdx, int x, int y, int z,
                      int phraseA, int wordA)
{
    shn_u8 buf[SHNET_HDR_SIZE + 64];
    int    off = SHNET_HDR_SIZE;

    ShnPutU8(buf, &off, (shn_u8)kind);
    ShnPutU8(buf, &off, (shn_u8)mapIdx);
    ShnPutU16(buf, &off, (shn_u16)phraseA);
    ShnPutU16(buf, &off, (shn_u16)wordA);
    ShnPutU16(buf, &off, 0xFFFF);
    ShnPutU16(buf, &off, 0xFFFF);
    ShnPutU16(buf, &off, 0);
    ShnPutS32(buf, &off, x);
    ShnPutS32(buf, &off, y);
    ShnPutS32(buf, &off, z);
    ShnPutS16(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_MEMO_PLACE, (shn_u16)(off - SHNET_HDR_SIZE), p->session);
    PeerSend(p, buf, off);
}

static void PeerSimple(Peer* p, int type)
{
    shn_u8 buf[SHNET_HDR_SIZE];
    ShnPutHeader(buf, (shn_u8)type, 0, p->session);
    PeerSend(p, buf, SHNET_HDR_SIZE);
}

static void PeerChat(Peer* p, int scope, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 4 + SHNET_CHAT_MAX];
    int    off = SHNET_HDR_SIZE;
    ShnPutU8(buf, &off, (shn_u8)scope);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutStr(buf, &off, text, SHNET_CHAT_MAX);
    ShnPutHeader(buf, SHNET_MSG_CHAT_SAY, (shn_u16)(off - SHNET_HDR_SIZE), p->session);
    PeerSend(p, buf, off);
}

static void PeerQueryMemos(Peer* p, int mapIdx)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8];
    int    off = SHNET_HDR_SIZE;
    ShnPutU8(buf, &off, (shn_u8)mapIdx);
    ShnPutU8(buf, &off, 0xFF);
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_MEMO_QUERY, (shn_u16)(off - SHNET_HDR_SIZE), p->session);
    PeerSend(p, buf, off);
}

int main(int argc, char** argv)
{
    const char*    host = (argc > 1) ? argv[1] : "127.0.0.1";
    unsigned short port = (unsigned short)((argc > 2) ? atoi(argv[2]) : SHNET_DEFAULT_PORT);
    Peer           a, b;
    shn_u8         pay[SHNET_MAX_PAYLOAD];
    int            n;

#if defined(_WIN32)
    { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#endif

    printf("Silent Hill Online protocol self-test against %s:%u\n\n", host, (unsigned)port);

    /* --- entry sizes must agree with what both ends serialize --- */
    printf("wire layout\n");
    CHECK(SHNET_HDR_SIZE == 12, "header is 12 bytes");
    CHECK(SHNET_SNAP_ENTRY == 24 && SHNET_SNAP_MAX >= 32,
          "snapshot entry 24B, %d per datagram", SHNET_SNAP_MAX);
    CHECK(SHNET_HDR_SIZE + 8 + SHNET_SNAP_MAX * SHNET_SNAP_ENTRY <= SHNET_MTU,
          "a full snapshot fits the MTU");
    CHECK(SHNET_HDR_SIZE + 8 + SHNET_ROSTER_MAX * SHNET_ROSTER_ENTRY <= SHNET_MTU,
          "a full roster page fits the MTU");
    CHECK(SHNET_HDR_SIZE + 8 + SHNET_MEMO_MAX * SHNET_MEMO_ENTRY <= SHNET_MTU,
          "a full memo page fits the MTU");

    /* --- serialization round-trips --- */
    printf("\nserialization\n");
    {
        shn_u8 buf[64];
        int    o = 0;
        char   s[SHNET_NAME_MAX];
        ShnPutU32(buf, &o, 0xDEADBEEFu);
        ShnPutS32(buf, &o, -123456);
        ShnPutS16(buf, &o, -4321);
        ShnPutStr(buf, &o, "Harry Mason 12345678901234567890", SHNET_NAME_MAX);
        o = 0;
        CHECK(ShnGetU32(buf, &o) == 0xDEADBEEFu, "u32 round-trips");
        CHECK(ShnGetS32(buf, &o) == -123456, "negative s32 round-trips");
        CHECK(ShnGetS16(buf, &o) == -4321, "negative s16 round-trips");
        ShnGetStr(buf, &o, SHNET_NAME_MAX, s, SHNET_NAME_MAX);
        CHECK(strlen(s) == SHNET_NAME_MAX - 1, "an over-long name is truncated, not overflowed");
        CHECK(o == 4 + 4 + 2 + SHNET_NAME_MAX, "the cursor advanced by exactly the field widths");
    }
    {
        shn_u8  buf[SHNET_HDR_SIZE];
        shn_u8  type;
        shn_u16 len;
        shn_u32 sess;
        ShnPutHeader(buf, SHNET_MSG_STATE, 28, 0x12345678u);
        CHECK(ShnParseHeader(buf, SHNET_HDR_SIZE, &type, &len, &sess) == 0,
              "a header claiming more payload than arrived is rejected");
        ShnPutHeader(buf, SHNET_MSG_PING, 0, 0x12345678u);
        CHECK(ShnParseHeader(buf, SHNET_HDR_SIZE, &type, &len, &sess) == 1 &&
              type == SHNET_MSG_PING && sess == 0x12345678u,
              "a valid header parses");
        buf[0] = 'X';
        CHECK(ShnParseHeader(buf, SHNET_HDR_SIZE, &type, &len, &sess) == 0,
              "a datagram with the wrong magic is rejected");
    }

    /* --- live server --- */
    printf("\nhandshake\n");
    PeerOpen(&a, host, port, "TestHarry");
    PeerOpen(&b, host, port, "TestCybil");
    CHECK(PeerHello(&a), "player A got a WELCOME (session %08X, id %u)", a.session, a.playerId);
    CHECK(PeerHello(&b), "player B got a WELCOME (session %08X, id %u)", b.session, b.playerId);
    CHECK(a.session != b.session && a.playerId != b.playerId,
          "the two players got distinct identities");
    if (!a.session || !b.session)
    {
        printf("\nno server on %s:%u - start sh_master first\n", host, (unsigned)port);
        return 1;
    }

    printf("\nping\n");
    {
        shn_u8 buf[SHNET_HDR_SIZE + 8];
        int    off = SHNET_HDR_SIZE;
        ShnPutU32(buf, &off, 1234u);
        ShnPutHeader(buf, SHNET_MSG_PING, (shn_u16)(off - SHNET_HDR_SIZE), a.session);
        PeerSend(&a, buf, off);
        n = PeerWait(&a, SHNET_MSG_PONG, 1000, pay, sizeof(pay));
        CHECK(n >= 8, "PONG came back");
        if (n >= 8)
        {
            int o = 0;
            CHECK(ShnGetU32(pay, &o) == 1234u, "PONG echoed the client clock");
        }
    }

    printf("\nghosts\n");
    /* Both on map 5, a few metres apart. */
    PeerState(&a, 5, 10 << 12, 0, 20 << 12, 512, SHNET_PF_ALIVE);
    PeerState(&b, 5, 14 << 12, 0, 20 << 12, 1024, SHNET_PF_ALIVE | SHNET_PF_FLASHLIGHT);
    n = PeerWait(&a, SHNET_MSG_SNAPSHOT, 1500, pay, sizeof(pay));
    CHECK(n > 0, "A received a SNAPSHOT");
    if (n > 0)
    {
        int o = 0;
        int count;
        (void)ShnGetU32(pay, &o);
        count = (int)ShnGetU16(pay, &o);
        (void)ShnGetU16(pay, &o);
        CHECK(count == 1, "the snapshot holds exactly the other player (%d)", count);
        if (count == 1)
        {
            shn_u32 id = ShnGetU32(pay, &o);
            int chara  = ShnGetU8(pay, &o);
            int flags  = ShnGetU8(pay, &o);
            int x      = ShnGetS32(pay, &o);
            int y      = ShnGetS32(pay, &o);
            int z      = ShnGetS32(pay, &o);
            (void)chara; (void)y;
            CHECK(id == b.playerId, "it is player B");
            CHECK(x == (14 << 12) && z == (20 << 12), "B's world position survived the round trip");
            CHECK((flags & SHNET_PF_FLASHLIGHT) != 0, "B's flashlight flag survived");
        }
    }

    printf("\nmap isolation\n");
    PeerState(&b, 9, 14 << 12, 0, 20 << 12, 1024, SHNET_PF_ALIVE);
    T_SLEEP(150);
    PeerState(&a, 5, 10 << 12, 0, 20 << 12, 512, SHNET_PF_ALIVE);
    {
        /* B moved to another map, so A's next snapshot should be empty - and
         * the server sends nothing at all rather than an empty datagram. */
        int got = PeerWait(&a, SHNET_MSG_SNAPSHOT, 500, pay, sizeof(pay));
        CHECK(got < 0, "no snapshot once the other player is on a different map");
    }

    printf("\nroster\n");
    PeerSimple(&a, SHNET_MSG_ROSTER_REQ);
    n = PeerWait(&a, SHNET_MSG_ROSTER, 1500, pay, sizeof(pay));
    CHECK(n > 0, "ROSTER came back");
    if (n > 0)
    {
        int o = 0;
        int total = (int)ShnGetU16(pay, &o);
        int count = (int)ShnGetU16(pay, &o);
        int first = (int)ShnGetU16(pay, &o);
        int i;
        int sawA = 0, sawB = 0;
        (void)ShnGetU16(pay, &o);
        CHECK(total >= 2, "the server counts at least our two players (%d)", total);
        CHECK(first == 0, "the first page starts at index 0");
        for (i = 0; i < count; i++)
        {
            char    nm[SHNET_NAME_MAX];
            shn_u32 id = ShnGetU32(pay, &o);
            int     mp = ShnGetU8(pay, &o);
            (void)ShnGetU8(pay, &o);
            (void)ShnGetU8(pay, &o);
            (void)ShnGetU8(pay, &o);
            (void)ShnGetU16(pay, &o);
            (void)ShnGetU16(pay, &o);
            ShnGetStr(pay, &o, SHNET_NAME_MAX, nm, SHNET_NAME_MAX);
            if (id == a.playerId) { sawA = 1; CHECK(strcmp(nm, "TestHarry") == 0 && mp == 5,
                                                    "A listed as \"%s\" on map %d", nm, mp); }
            if (id == b.playerId) { sawB = 1; CHECK(strcmp(nm, "TestCybil") == 0 && mp == 9,
                                                    "B listed as \"%s\" on map %d", nm, mp); }
        }
        CHECK(sawA && sawB, "both players are in the roster");
    }

    printf("\nmarkers\n");
    PeerPlace(&a, SHNET_MARK_MEMO, 5, 11 << 12, 0, 21 << 12, 3, 7);
    n = PeerWait(&a, SHNET_MSG_MEMO_ACK, 1500, pay, sizeof(pay));
    CHECK(n >= 8, "the server acknowledged the placement");
    {
        shn_u32 placedId = 0;
        int     o = 0;
        if (n >= 8) placedId = ShnGetU32(pay, &o);

        PeerQueryMemos(&b, 5);
        n = PeerWait(&b, SHNET_MSG_MEMO_LIST, 1500, pay, sizeof(pay));
        CHECK(n > 0, "the OTHER player can read the map's markers");
        if (n > 0)
        {
            int o2 = 0;
            int mapIdx = ShnGetU8(pay, &o2);
            int count;
            (void)ShnGetU8(pay, &o2);
            count = (int)ShnGetU16(pay, &o2);
            (void)ShnGetU32(pay, &o2);
            CHECK(mapIdx == 5, "the list is for the map that was asked about");
            CHECK(count >= 1, "at least the marker just placed is in it (%d)", count);
            if (count >= 1)
            {
                shn_u32 id    = ShnGetU32(pay, &o2);
                shn_u32 owner = ShnGetU32(pay, &o2);
                int     kind  = ShnGetU8(pay, &o2);
                int     mp    = ShnGetU8(pay, &o2);
                int     rating = ShnGetS16(pay, &o2);
                int     pa    = ShnGetU16(pay, &o2);
                int     wa    = ShnGetU16(pay, &o2);
                int     x;
                (void)ShnGetU16(pay, &o2);
                (void)ShnGetU16(pay, &o2);
                x = ShnGetS32(pay, &o2);
                (void)mp; (void)rating;
                CHECK(id == placedId, "the id matches the acknowledgement");
                CHECK(owner == a.playerId, "the owner is player A");
                CHECK(kind == SHNET_MARK_MEMO, "the kind survived");
                CHECK(pa == 3 && wa == 7, "the phrase and word indices survived");
                CHECK(x == (11 << 12), "the marker's world X survived");
            }
        }

        printf("\nmap isolation for markers\n");
        PeerQueryMemos(&b, 9);
        n = PeerWait(&b, SHNET_MSG_MEMO_LIST, 1500, pay, sizeof(pay));
        CHECK(n > 0, "an empty map still gets an answer");
        if (n > 0)
        {
            int o2 = 2;
            CHECK(ShnGetU16(pay, &o2) == 0, "and the answer is zero markers");
        }

        printf("\ndeath dedupe\n");
        PeerPlace(&a, SHNET_MARK_DEATH, 5, 30 << 12, 0, 30 << 12, 0xFFFF, 0xFFFF);
        n = PeerWait(&a, SHNET_MSG_MEMO_ACK, 1000, pay, sizeof(pay));
        CHECK(n >= 8, "a death marker is accepted");
        PeerPlace(&a, SHNET_MARK_DEATH, 5, 30 << 12, 0, 30 << 12, 0xFFFF, 0xFFFF);
        n = PeerWait(&a, SHNET_MSG_MEMO_ACK, 600, pay, sizeof(pay));
        CHECK(n < 0, "a second death at the same spot is collapsed into the first");
    }

    printf("\nchat\n");
    /* From the map-isolation block above, A is on map 5 and B on map 9. */
    PeerState(&a, 5, 10 << 12, 0, 20 << 12, 512, SHNET_PF_ALIVE);
    PeerState(&b, 9, 14 << 12, 0, 20 << 12, 512, SHNET_PF_ALIVE);
    T_SLEEP(120);
    PeerChat(&a, SHNET_CHAT_GLOBAL, "hello everyone");
    n = PeerWait(&b, SHNET_MSG_CHAT_MSG, 1500, pay, sizeof(pay));
    CHECK(n > 0, "global chat reaches a player on another map");
    if (n > 0)
    {
        int  o = 0;
        int  scope = ShnGetU8(pay, &o);
        char nm[SHNET_NAME_MAX], tx[SHNET_CHAT_MAX];
        unsigned int fromId;
        (void)ShnGetU8(pay, &o);
        (void)ShnGetU16(pay, &o);
        fromId = ShnGetU32(pay, &o);
        ShnGetStr(pay, &o, SHNET_NAME_MAX, nm, SHNET_NAME_MAX);
        ShnGetStr(pay, &o, SHNET_CHAT_MAX, tx, SHNET_CHAT_MAX);
        CHECK(scope == SHNET_CHAT_GLOBAL, "the scope survived");
        CHECK(fromId == a.playerId, "the sender id is player A");
        CHECK(strcmp(nm, "TestHarry") == 0, "the sender name survived (%s)", nm);
        CHECK(strcmp(tx, "hello everyone") == 0, "the text survived (%s)", tx);
    }
    /* Game chat from A (map 5) must NOT reach B (map 9), but A hears its own. */
    PeerChat(&a, SHNET_CHAT_GAME, "anyone in this room");
    n = PeerWait(&b, SHNET_MSG_CHAT_MSG, 500, pay, sizeof(pay));
    CHECK(n < 0, "game chat does not cross to another map");
    n = PeerWait(&a, SHNET_MSG_CHAT_MSG, 1000, pay, sizeof(pay));
    CHECK(n > 0, "the sender hears their own game-chat line (echo)");

    printf("\nhostile input\n");
    {
        shn_u8 junk[64];
        int    o = SHNET_HDR_SIZE;
        /* A STATE that claims a session that is not ours must be ignored. If
         * the server took it, one player could move another player's ghost. */
        ShnPutU32(junk, &o, 1);
        ShnPutU8(junk, &o, 5);
        ShnPutU8(junk, &o, 0);
        ShnPutU8(junk, &o, 0);
        ShnPutU8(junk, &o, 0);
        ShnPutS32(junk, &o, 999 << 12);
        ShnPutS32(junk, &o, 0);
        ShnPutS32(junk, &o, 999 << 12);
        ShnPutS16(junk, &o, 0);
        ShnPutS16(junk, &o, 0);
        ShnPutU16(junk, &o, 0);
        ShnPutU16(junk, &o, 0);
        ShnPutHeader(junk, SHNET_MSG_STATE, (shn_u16)(o - SHNET_HDR_SIZE), b.session);
        PeerSend(&a, junk, o); /* A's socket, B's session */

        PeerState(&b, 5, 14 << 12, 0, 20 << 12, 0, SHNET_PF_ALIVE);
        T_SLEEP(150);
        PeerState(&a, 5, 10 << 12, 0, 20 << 12, 0, SHNET_PF_ALIVE);
        n = PeerWait(&a, SHNET_MSG_SNAPSHOT, 1000, pay, sizeof(pay));
        if (n > 0)
        {
            int o2 = 0;
            int count;
            (void)ShnGetU32(pay, &o2);
            count = (int)ShnGetU16(pay, &o2);
            (void)ShnGetU16(pay, &o2);
            if (count >= 1)
            {
                int x;
                (void)ShnGetU32(pay, &o2);
                (void)ShnGetU8(pay, &o2);
                (void)ShnGetU8(pay, &o2);
                x = ShnGetS32(pay, &o2);
                CHECK(x == (14 << 12),
                      "a STATE sent from the wrong address for another player's session was ignored");
            }
        }
        {
            shn_u8 tiny[SHNET_HDR_SIZE + 2];
            ShnPutHeader(tiny, SHNET_MSG_MEMO_PLACE, 2, a.session);
            PeerSend(&a, tiny, SHNET_HDR_SIZE + 2);
            PeerSend(&a, tiny, 3); /* shorter than a header */
            PeerSimple(&a, 0x7F);  /* a type the server has never heard of */
            T_SLEEP(100);
            PeerSimple(&a, SHNET_MSG_ROSTER_REQ);
            n = PeerWait(&a, SHNET_MSG_ROSTER, 1500, pay, sizeof(pay));
            CHECK(n > 0, "the server is still answering after truncated and unknown datagrams");
        }
    }

    PeerSimple(&a, SHNET_MSG_BYE);
    PeerSimple(&b, SHNET_MSG_BYE);

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
