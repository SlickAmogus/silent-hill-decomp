/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tmd_endian.c - byte-swap a TMD model file in place for big-endian hosts.
 *
 * TMD is a raw disc format with no reformat walker: GsMapModelingData reads
 * nobj and the 7-u32 object entries as native loads, GsSortObject4J's drawers
 * overlay TMD_P_* structs whose u16 fields (vertex/normal indices, clut,
 * tpage) come straight off the file, and the vertex/normal pools are SVECTOR
 * s16 triples. All of it reads byte-swapped on big-endian. Xbox 360 log 031
 * showed the first symptom: [ITEMPICK] nobj=570425344, which is 0x22000000 --
 * 34 item models, read the wrong way round.
 *
 * WHAT DOES AND DOES NOT MOVE. The 4-byte primitive header (olen, ilen, flag,
 * mode) is consumed with BYTE reads in GsSortObject4J, and bytes have no
 * order, so it is left alone. Every 32-bit payload word of every primitive is
 * one of exactly three shapes:
 *     B  four u8   (colour r,g,b,code / padding)      -> nothing moves
 *     C  u8,u8,u16 (tu,tv, then clut or tpage)        -> swap the u16 only
 *     H  u16,u16   (indices / packed pairs)           -> swap both
 * so a primitive's swap recipe is a short shape string, keyed by the same
 * dispatch GsSortObject4J uses (flag bit 0 = lit, mode bits TME/IIP/QD) plus
 * ilen -- ilen is what distinguishes Silent Hill's two nonstandard layouts
 * (mode 0x30 with ilen 4 is TMD_P_G3-shaped, NTG3 with ilen 6 is
 * TMD_P_TG3-shaped: the drawers special-case both, so this file must too).
 * A combination not in the table is left unswapped and reported once, which
 * renders wrongly but corrupts nothing and names the missing layout.
 *
 * IDEMPOTENCY BY VALIDATION, the ANM/DAT technique: nobj read little-endian
 * and read natively cannot BOTH be in the valid range unless the file is
 * degenerate, so the readable direction says whether the buffer has been
 * converted. That matters because the inventory re-registers the same
 * resident buffer on every carousel open.
 *
 * OVERLAPPING POOLS. Objects in one file may in principle share vertex or
 * normal pools. Each pool's start offset is remembered and an exact repeat is
 * skipped, so a shared pool cannot be double-swapped (which would restore
 * foreign order and corrupt every other user).
 */
#include <stdint.h>

#include "tmd_endian.h"
#include "sh_log.h"

#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#define TMD_MAX_OBJS 96          /* sanity cap, comfortably above SH packs */
#define TMD_MAX_POOLS (TMD_MAX_OBJS * 3)

static uint32_t Rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void Fix32(uint8_t* p) { *(uint32_t*)p = Rd32(p); }
static void Fix16(uint8_t* p) { *(uint16_t*)p = (uint16_t)(p[0] | (p[1] << 8)); }

/* Swap recipe per (lit, tme, iip, qd); a second candidate where Silent Hill
 * uses a nonstandard layout, distinguished by ilen. Shape strings cover
 * PAYLOAD words only, so their length equals ilen by the packet format's own
 * rule (total bytes = (1+ilen)*4). */
typedef struct { const char* a; const char* b; } ShapePair;
static const ShapePair SHAPES[16] = {
    /* index = lit<<3 | tme<<2 | iip<<1 | qd */
    /* 0000 NF3  */ { "BHH",        0        },
    /* 0001 NF4  */ { "BHH",        0        },
    /* 0010 NG3  */ { "BBBHH",      "BHHH"   },  /* SH variant = G3 layout  */
    /* 0011 NG4  */ { "BBBBHH",     0        },
    /* 0100 NTF3 */ { "CCCBHH",     0        },
    /* 0101 NTF4 */ { "CCCCBHH",    0        },
    /* 0110 NTG3 */ { "CCCBBBHH",   "CCCHHH" },  /* SH variant = TG3 layout */
    /* 0111 NTG4 */ { "CCCCBBBBHH", 0        },
    /* 1000 F3   */ { "BHH",        0        },
    /* 1001 F4   */ { "BHHH",       0        },
    /* 1010 G3   */ { "BHHH",       0        },
    /* 1011 G4   */ { "BHHHH",      0        },
    /* 1100 TF3  */ { "CCCHH",      0        },
    /* 1101 TF4  */ { "CCCCHHH",    0        },
    /* 1110 TG3  */ { "CCCHHH",     0        },
    /* 1111 TG4  */ { "CCCCHHHH",   0        },
};

static int ShapeLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void SwapPrims(uint8_t* pp, uint32_t nprim)
{
    static int s_unknownLogged = 0;
    uint32_t   i;

    for (i = 0; i < nprim; i++) {
        uint8_t ilen = pp[1];
        uint8_t flag = pp[2];
        uint8_t mode = pp[3];
        int     key  = ((flag & 1) << 3) | (((mode >> 2) & 1) << 2) |
                       (((mode >> 4) & 1) << 1) | ((mode >> 3) & 1);
        const char* shape = 0;
        int         w;

        if (ilen == 0)
            return;             /* mirror the walker's own bail-out */
        if (ShapeLen(SHAPES[key].a) == ilen)      shape = SHAPES[key].a;
        else if (ShapeLen(SHAPES[key].b) == ilen) shape = SHAPES[key].b;

        if (shape) {
            for (w = 0; w < ilen; w++) {
                uint8_t* word = pp + 4 + w * 4;
                switch (shape[w]) {
                case 'C': Fix16(word + 2); break;
                case 'H': Fix16(word); Fix16(word + 2); break;
                default: break;   /* B: bytes stay bytes */
                }
            }
        } else if (s_unknownLogged < 8) {
            s_unknownLogged++;
            SH_DBG("[TMD-SWAP] unknown prim layout mode=0x%02x flag=0x%02x ilen=%d - left unswapped",
                   mode, flag, ilen);
        }
        pp += (1 + (uint32_t)ilen) * 4;
    }
}

void Tmd_SwapForBigEndian(void* tmdNoId)
{
    uint8_t*  p = (uint8_t*)tmdNoId;
    uint32_t  nobjLe, nobjNat, i;
    uint8_t*  objTable;
    uint32_t  seen[TMD_MAX_POOLS];
    int       seenN = 0;

    if (!p)
        return;

    nobjLe  = Rd32(p + 4);
    nobjNat = *(uint32_t*)(p + 4);
    if (nobjNat >= 1 && nobjNat <= TMD_MAX_OBJS)
        return;                          /* native already readable: converted */
    if (nobjLe < 1 || nobjLe > TMD_MAX_OBJS)
        return;                          /* neither direction sane: not a TMD  */

    Fix32(p);                            /* flags */
    Fix32(p + 4);                        /* nobj  */
    objTable = p + 8;

    for (i = 0; i < nobjLe; i++) {
        uint8_t* e        = objTable + i * 28;
        uint32_t vertTop  = Rd32(e + 0);
        uint32_t nVert    = Rd32(e + 4);
        uint32_t normTop  = Rd32(e + 8);
        uint32_t nNorm    = Rd32(e + 12);
        uint32_t primTop  = Rd32(e + 16);
        uint32_t nPrim    = Rd32(e + 20);
        int      k, dup;
        uint32_t j;

        /* The 7 entry words themselves. */
        for (k = 0; k < 7; k++)
            Fix32(e + k * 4);

        /* Pools, deduplicated by start offset so a shared pool cannot be
         * swapped back to foreign order by its second owner. */
        dup = 0;
        for (k = 0; k < seenN; k++) if (seen[k] == vertTop) { dup = 1; break; }
        if (!dup) {
            if (seenN < TMD_MAX_POOLS) seen[seenN++] = vertTop;
            for (j = 0; j < nVert * 4; j++)
                Fix16(objTable + vertTop + j * 2);
        }

        dup = 0;
        for (k = 0; k < seenN; k++) if (seen[k] == normTop) { dup = 1; break; }
        if (!dup) {
            if (seenN < TMD_MAX_POOLS) seen[seenN++] = normTop;
            for (j = 0; j < nNorm * 4; j++)
                Fix16(objTable + normTop + j * 2);
        }

        dup = 0;
        for (k = 0; k < seenN; k++) if (seen[k] == primTop) { dup = 1; break; }
        if (!dup) {
            if (seenN < TMD_MAX_POOLS) seen[seenN++] = primTop;
            SwapPrims(objTable + primTop, nPrim);
        }
    }
}

#else

void Tmd_SwapForBigEndian(void* tmdNoId) { (void)tmdNoId; }

#endif
