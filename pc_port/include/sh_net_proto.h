/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_proto.h - Silent Hill Online wire protocol, version 1.
 *
 * THE CONTRACT between the game client (pc_port/src/net) and the master server
 * (online_server/sh_master.c). Both compile this file verbatim; it is the one
 * place either side may learn the byte layout, so it deliberately includes
 * nothing at all — not even <stdint.h>, which cannot be pulled into a
 * translation unit that also sees the decomp's own u8/u32 typedefs.
 *
 * Transport is UDP. Every field is written through the ShnPut / ShnGet helpers
 * below rather than by casting a struct over the buffer: the client is built by
 * MinGW for x86-64 and the server may be built by anything, and a struct laid
 * over a datagram is a promise about padding and alignment that neither
 * compiler made.
 *
 * Byte order is LITTLE-ENDIAN on the wire, chosen because every machine that
 * will ever run either end is little-endian and the helpers make the choice
 * free. The helpers are endian-agnostic, so a big-endian build (the port has
 * PS3/360/N64 branches) interoperates without a #ifdef.
 *
 * Datagrams are capped at SHNET_MTU so nothing this protocol emits can be
 * fragmented by a home router; anything larger (a full roster, a busy map's
 * memo list) is paged across several datagrams instead.
 *
 * RELIABILITY: there is none, by design. STATE and SNAPSHOT are pure
 * fire-and-forget — a dropped position update is corrected 100ms later. The
 * handful of messages that DO matter (HELLO, MEMO_PLACE, MEMO_QUERY,
 * ROSTER_REQ) are re-sent by the client on a timer until the matching reply
 * arrives, which is the whole reliability layer and is enough for a protocol
 * with no ordering requirements.
 */
#ifndef SH_NET_PROTO_H
#define SH_NET_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width aliases. Spelled out rather than taken from <stdint.h> so this
 * header stays includable from a decomp TU (whose own u8/u16/u32 would collide
 * with a system header's). The static assert below is the only guarantee
 * needed: every target is a 8/16/32 machine for these three widths. */
typedef unsigned char  shn_u8;
typedef unsigned short shn_u16;
typedef unsigned int   shn_u32;
typedef signed char    shn_s8;
typedef short          shn_s16;
typedef int            shn_s32;

/* GCC's -Wunused-function fires on a static helper no TU happens to call, and
 * every TU that includes this header uses a different subset. `static inline`
 * with the unused attribute keeps internal linkage without the noise. */
#if defined(__GNUC__)
#define SHN_FN static __inline__ __attribute__((unused))
#else
#define SHN_FN static
#endif

typedef char shn_static_assert_widths[
    (sizeof(shn_u8) == 1 && sizeof(shn_u16) == 2 && sizeof(shn_u32) == 4) ? 1 : -1];

#define SHNET_PROTO_VER   1
#define SHNET_MAGIC0      'S'
#define SHNET_MAGIC1      'H'
#define SHNET_MAGIC2      'O'
#define SHNET_MAGIC3      '1'

/* Payload cap. 1200 keeps the IP datagram under the 1280-byte floor every
 * IPv6 path must carry and well under a 1500-byte Ethernet MTU with room for
 * a PPPoE or VPN header, so nothing here is ever fragmented. */
#define SHNET_MTU         1200
#define SHNET_HDR_SIZE    12
#define SHNET_MAX_PAYLOAD (SHNET_MTU - SHNET_HDR_SIZE)

#define SHNET_DEFAULT_PORT 27888

#define SHNET_NAME_MAX     24   /* including the NUL */
#define SHNET_BUILD_MAX    24
#define SHNET_SERVER_MAX   32
#define SHNET_MOTD_MAX     128
#define SHNET_REJECT_MAX   96
#define SHNET_EVENT_MAX    96

/* ------------------------------------------------------------------ */
/* Message types                                                       */
/* ------------------------------------------------------------------ */
enum
{
    SHNET_MSG_HELLO      = 0x01, /* C->S  join / reconnect */
    SHNET_MSG_WELCOME    = 0x02, /* S->C  accepted, here is your session */
    SHNET_MSG_PING       = 0x03, /* C->S */
    SHNET_MSG_PONG       = 0x04, /* S->C */
    SHNET_MSG_BYE        = 0x05, /* C->S  clean disconnect */
    SHNET_MSG_REJECT     = 0x06, /* S->C  refused, with a reason */

    SHNET_MSG_STATE      = 0x10, /* C->S  this player's position, ~10Hz */
    SHNET_MSG_SNAPSHOT   = 0x11, /* S->C  everyone else on this map, ~10Hz */

    SHNET_MSG_ROSTER_REQ = 0x20, /* C->S  who is online (for the player list) */
    SHNET_MSG_ROSTER     = 0x21, /* S->C  one page of the roster */

    SHNET_MSG_MEMO_PLACE = 0x30, /* C->S  leave a message / death marker */
    SHNET_MSG_MEMO_QUERY = 0x31, /* C->S  what is on this map */
    SHNET_MSG_MEMO_LIST  = 0x32, /* S->C  one page of markers */
    SHNET_MSG_MEMO_RATE  = 0x33, /* C->S  +1 / -1 on someone's memo */
    SHNET_MSG_MEMO_ACK   = 0x34, /* S->C  your placement landed, here is its id */

    SHNET_MSG_CHAT_SAY   = 0x35, /* C->S  a line the player typed (scope + text) */
    SHNET_MSG_CHAT_MSG   = 0x36, /* S->C  a line to show (scope + sender + text) */

    SHNET_MSG_EVENT      = 0x40, /* S->C  a line for the toast feed */

    /* ---- Session messages ----
     * Peer to peer between the members of a Steam lobby, NOT to the master
     * server. They travel on the Steam relay (sh_net_steam.c) rather than the
     * UDP socket, but they carry the same header and go through the same
     * ShnPut/ShnGet helpers, so one parser covers both and a co-op message
     * added later is just another type in this range. */
    SHNET_MSG_S_HELLO    = 0x50, /* P2P  I am here, this is my name and map */
    SHNET_MSG_S_PING     = 0x51,
    SHNET_MSG_S_PONG     = 0x52,
    SHNET_MSG_S_BYE      = 0x53
};

/* Marker kinds. */
enum
{
    SHNET_MARK_MEMO  = 0, /* templated player message */
    SHNET_MARK_DEATH = 1, /* someone died here */
    SHNET_MARK_SAVE  = 2  /* someone saved here (a lit spot in the fog) */
};

/* STATE / SNAPSHOT player flag bits. */
enum
{
    SHNET_PF_ALIVE      = 1 << 0,
    SHNET_PF_FLASHLIGHT = 1 << 1,
    SHNET_PF_RUNNING    = 1 << 2,
    SHNET_PF_CUTSCENE   = 1 << 3, /* ghost hides: the player is not really "there" */
    SHNET_PF_AIMING     = 1 << 4,
    SHNET_PF_MENU       = 1 << 5,
    SHNET_PF_OTHERWORLD = 1 << 6  /* reserved for a future fog/otherworld split */
};

/* WELCOME feature bits — a server can host ghosts without memos, etc. */
enum
{
    SHNET_SF_GHOSTS = 1 << 0,
    SHNET_SF_MEMOS  = 1 << 1,
    SHNET_SF_DEATHS = 1 << 2,
    SHNET_SF_EVENTS = 1 << 3
};

/* REJECT reasons. */
enum
{
    SHNET_REJ_VERSION  = 1,
    SHNET_REJ_FULL     = 2,
    SHNET_REJ_PASSWORD = 3,
    SHNET_REJ_BANNED   = 4,
    SHNET_REJ_NAME     = 5
};

/* Chat scope. Also the value the U key cycles through. */
enum
{
    SHNET_CHAT_GLOBAL = 0, /* everyone connected to the server */
    SHNET_CHAT_GAME   = 1  /* everyone on the same map (a "game", for now) */
};

#define SHNET_CHAT_MAX 160   /* max UTF-8 bytes of one chat line, incl. NUL */

/* EVENT kinds. */
enum
{
    SHNET_EV_JOIN   = 0,
    SHNET_EV_LEAVE  = 1,
    SHNET_EV_DEATH  = 2,
    SHNET_EV_SERVER = 3,
    SHNET_EV_MEMO   = 4
};

/* Entry sizes, so both ends page identically. */
#define SHNET_SNAP_ENTRY   24
#define SHNET_ROSTER_ENTRY 36
#define SHNET_MEMO_ENTRY   34

#define SHNET_SNAP_MAX   ((SHNET_MAX_PAYLOAD - 8) / SHNET_SNAP_ENTRY)
#define SHNET_ROSTER_MAX ((SHNET_MAX_PAYLOAD - 8) / SHNET_ROSTER_ENTRY)
#define SHNET_MEMO_MAX   ((SHNET_MAX_PAYLOAD - 8) / SHNET_MEMO_ENTRY)

/* ------------------------------------------------------------------ */
/* Serialization primitives                                            */
/* ------------------------------------------------------------------ */
/* Cursor-style: every helper advances *off. The caller checks the final
 * offset against the buffer size once, rather than at every field. Callers
 * MUST size the buffer for the worst case before writing. */

SHN_FN void ShnPutU8(shn_u8* b, int* off, shn_u8 v)
{
    b[(*off)++] = v;
}

SHN_FN void ShnPutU16(shn_u8* b, int* off, shn_u16 v)
{
    b[(*off)++] = (shn_u8)(v & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 8) & 0xFF);
}

SHN_FN void ShnPutU32(shn_u8* b, int* off, shn_u32 v)
{
    b[(*off)++] = (shn_u8)(v & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 8) & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 16) & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 24) & 0xFF);
}

SHN_FN void ShnPutS32(shn_u8* b, int* off, shn_s32 v)
{
    ShnPutU32(b, off, (shn_u32)v);
}

SHN_FN void ShnPutS16(shn_u8* b, int* off, shn_s16 v)
{
    ShnPutU16(b, off, (shn_u16)v);
}

/* Fixed-width field, always exactly `cap` bytes on the wire, NUL padded and
 * NUL terminated. A short string does not shorten the datagram: fixed offsets
 * are what let a reader skip a field it does not care about. */
SHN_FN void ShnPutStr(shn_u8* b, int* off, const char* s, int cap)
{
    int i = 0;
    while (i < cap - 1 && s != 0 && s[i] != '\0')
    {
        b[*off + i] = (shn_u8)s[i];
        i++;
    }
    while (i < cap)
    {
        b[*off + i] = 0;
        i++;
    }
    *off += cap;
}

SHN_FN shn_u8 ShnGetU8(const shn_u8* b, int* off)
{
    return b[(*off)++];
}

SHN_FN shn_u16 ShnGetU16(const shn_u8* b, int* off)
{
    shn_u16 v = (shn_u16)(b[*off] | ((shn_u16)b[*off + 1] << 8));
    *off += 2;
    return v;
}

SHN_FN shn_u32 ShnGetU32(const shn_u8* b, int* off)
{
    shn_u32 v = (shn_u32)b[*off] | ((shn_u32)b[*off + 1] << 8) |
                ((shn_u32)b[*off + 2] << 16) | ((shn_u32)b[*off + 3] << 24);
    *off += 4;
    return v;
}

SHN_FN shn_s32 ShnGetS32(const shn_u8* b, int* off)
{
    return (shn_s32)ShnGetU32(b, off);
}

SHN_FN shn_s16 ShnGetS16(const shn_u8* b, int* off)
{
    return (shn_s16)ShnGetU16(b, off);
}

/* Reads `cap` wire bytes into a caller buffer of `outCap`, always NUL
 * terminating. The wire field is consumed in full even when the destination is
 * smaller, or every field after it would be read at the wrong offset. */
SHN_FN void ShnGetStr(const shn_u8* b, int* off, int cap, char* out, int outCap)
{
    int i;
    for (i = 0; i < cap; i++)
    {
        if (i < outCap - 1)
        {
            out[i] = (char)b[*off + i];
        }
    }
    if (outCap > 0)
    {
        out[(cap < outCap - 1) ? cap : outCap - 1] = '\0';
    }
    *off += cap;
}

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

SHN_FN void ShnPutHeader(shn_u8* b, shn_u8 type, shn_u16 payloadLen, shn_u32 session)
{
    int off = 0;
    ShnPutU8(b, &off, SHNET_MAGIC0);
    ShnPutU8(b, &off, SHNET_MAGIC1);
    ShnPutU8(b, &off, SHNET_MAGIC2);
    ShnPutU8(b, &off, SHNET_MAGIC3);
    ShnPutU8(b, &off, SHNET_PROTO_VER);
    ShnPutU8(b, &off, type);
    ShnPutU16(b, &off, payloadLen);
    ShnPutU32(b, &off, session);
}

/* Returns 1 and fills the out params when `b` is a well-formed datagram of
 * `len` bytes whose declared payload actually fits; 0 otherwise. A caller that
 * gets 0 must drop the packet without looking at any of it — this is the only
 * place a hostile datagram is filtered. */
SHN_FN int ShnParseHeader(const shn_u8* b, int len, shn_u8* outType,
                          shn_u16* outPayloadLen, shn_u32* outSession)
{
    int off = 4;
    if (len < SHNET_HDR_SIZE)
    {
        return 0;
    }
    if (b[0] != SHNET_MAGIC0 || b[1] != SHNET_MAGIC1 ||
        b[2] != SHNET_MAGIC2 || b[3] != SHNET_MAGIC3)
    {
        return 0;
    }
    if (ShnGetU8(b, &off) != SHNET_PROTO_VER)
    {
        return 0;
    }
    *outType       = ShnGetU8(b, &off);
    *outPayloadLen = ShnGetU16(b, &off);
    *outSession    = ShnGetU32(b, &off);
    if (*outPayloadLen > SHNET_MAX_PAYLOAD ||
        SHNET_HDR_SIZE + (int)*outPayloadLen > len)
    {
        return 0;
    }
    return 1;
}

/* Non-cryptographic. This exists so a private server can require a shared word
 * without either end linking a hash library, and to keep the plaintext off the
 * wire — it is NOT a defence against anyone who can watch the traffic. */
SHN_FN shn_u32 ShnHashPassword(const char* s)
{
    shn_u32 h = 2166136261u;
    if (s == 0)
    {
        return 0;
    }
    while (*s)
    {
        h ^= (shn_u8)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_PROTO_H */
