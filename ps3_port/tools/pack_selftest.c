/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pack_selftest.c - prove the PSX_ST_* macros put every field where it belongs.
 *
 * The type-pun sweep converted ~280 stores that compose several sub-word fields
 * into one wide store. Those sites live in gameplay drawing paths, so a title
 * screen boot exercises almost none of them -- "the game still runs" is evidence
 * of no regression and NOT evidence of correctness. This is the missing half:
 * build a primitive through each macro with known values, then read the fields
 * back and check them.
 *
 * Runs as its own EBOOT under RPCS3, so it tests the real compiler, the real
 * struct layout and the real endianness rather than a desktop approximation.
 *
 *   ppu-gcc ... ps3_port/tools/pack_selftest.c -o pack.elf && make_self
 *   ps3_port/tools/rpcs3_smoke.sh -m "PACK" pack_EBOOT.BIN
 */
#include <stdio.h>
#include <string.h>

#include "psx/libgte.h"
#include "psx/libgpu.h"
#include "psx_pack.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "maps/shared.h"
#include "lib_unk.h"

static int s_fail;

static void chk(const char* what, int got, int want)
{
    if (got != want) {
        printf("[PACK] FAIL %-14s got=0x%X want=0x%X\n", what, got, want);
        s_fail++;
    } else {
        printf("[PACK] ok   %-14s 0x%X\n", what, got);
    }
}

int main(void)
{
    static POLY_FT4 p;
    static POLY_GT4 g;   /* gouraud: the only shape with r1/g1/b1/pad1 */

    printf("[PACK] endian=%s sizeof(POLY_FT4)=%d\n",
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
           "big",
#else
           "little",
#endif
           (int)sizeof(POLY_FT4));

    /* RGBC: the quartet that carries the GPU command byte.
     * Composed exactly as the decomp does it: r | g<<8 | b<<16 | code<<24. */
    PSX_ST_RGBC(&p, 0x11u | (0x22u << 8) | (0x33u << 16) | (0x2Cu << 24));
    chk("r0",   p.r0,   0x11);
    chk("g0",   p.g0,   0x22);
    chk("b0",   p.b0,   0x33);
    chk("code", p.code, 0x2C);

    /* RGB: a gouraud vertex colour + pad, not reversed on big-endian. */
    PSX_ST_RGB(&g, r1, 0x44u | (0x55u << 8) | (0x66u << 16) | (0x77u << 24));
    chk("r1",   g.r1,   0x44);
    chk("g1",   g.g1,   0x55);
    chk("b1",   g.b1,   0x66);
    chk("p1",   g.p1,   0x77);

    /* UV: (u8 u, u8 v, u16 half). The half is the one a plain byte-reverse
     * would get wrong, which is the whole reason this macro exists. */
    PSX_ST_UV(&p, u0, 0x12u | (0x34u << 8) | (0xABCDu << 16));
    chk("u0",   p.u0,   0x12);
    chk("v0",   p.v0,   0x34);
    chk("clut", p.clut, 0xABCD);

    PSX_ST_UV(&p, u1, 0x56u | (0x78u << 8) | (0x1F3u << 16));
    chk("u1",    p.u1,    0x56);
    chk("v1",    p.v1,    0x78);
    chk("tpage", p.tpage, 0x1F3);

    /* XY: two s16 coordinates packed low-x, high-y. */
    PSX_ST_XY(&p, x0, (0x0123u & 0xFFFFu) | (0x0456u << 16));
    chk("x0",   p.x0,   0x0123);
    chk("y0",   p.y0,   0x0456);

    /* Negative y, because these are SIGNED and off-screen geometry is normal. */
    PSX_ST_XY(&p, x2, (0x0011u & 0xFFFFu) | ((unsigned)((int)-5 & 0xFFFF) << 16));
    chk("x2",   p.x2,   0x0011);
    chk("y2 -5", p.y2,  -5);

    /* ---- hazard 2: bitfield allocation order -------------------------------
     * These names encode their own bit positions -- field_14C_0 is bit 0 of the
     * aliased `flags` word, field_14C_1 is bit 1 -- so the layout is
     * machine-checkable rather than a matter of opinion. On little-endian the
     * first-declared bitfield takes the LOW bits and the names are true; on
     * big-endian it takes the HIGH bits and every one of them means its
     * opposite, silently, with sizeof unchanged. */
    {
        s_sharedData_800E21D0_0_s01 sd;
        memset(&sd, 0, sizeof(sd));
        sd.field_14C.bits32.field_14C_0 = 1;
        chk("bf flags b0", (int)sd.field_14C.flags, 1);
        memset(&sd, 0, sizeof(sd));
        sd.field_14C.bits32.field_14C_1 = 1;
        chk("bf flags b1", (int)sd.field_14C.flags, 2);
        memset(&sd, 0, sizeof(sd));
        sd.field_14C.bits32.field_14C_3 = 1;
        chk("bf flags b3", (int)sd.field_14C.flags, 8);
        memset(&sd, 0, sizeof(sd));
        sd.field_14C.flags = 1u;
        chk("bf b0 from flags", (int)sd.field_14C.bits32.field_14C_0, 1);
    }

    /* The other aliasing union: lib_unk.h s_8002AC04.field_4, where a plain
     * u8 field_5 in one variant shares storage with a 7+1 bitfield split in
     * another, and a u32 field_C shares with a 16/12/3/1 split. Same naming
     * convention, so same machine-check. */
    {
        s_8002AC04 u;
        memset(&u, 0, sizeof(u));
        u.field_4.func_80089BB8_data.field_5_7 = 1;
        chk("bf f5_7", (int)u.field_4.func_8008973C_data.field_5, 0x80);
        memset(&u, 0, sizeof(u));
        u.field_4.func_80089BB8_data.field_5 = 0x7F;
        chk("bf f5_lo", (int)u.field_4.func_8008973C_data.field_5, 0x7F);
        /* NOT cross-checked against func_8008973C_data.field_C, and the reason
         * is worth recording: func_80089DF0_data declares `u8* field_8` where
         * its sibling declares `s32 field_8`. On PSX both were 4 bytes and the
         * variants aliased exactly; on ANY LP64 build the pointer is 8, so
         * everything after it shifts and the two field_C members no longer
         * overlap at all. That is a 64-bit divergence rather than an endian
         * one -- it is equally true of the PC port -- so a cross-variant
         * assertion here would be testing a layout the language never
         * promised. The field_C_* bit order is still reversed for big-endian
         * above; it is simply not observable through the sibling. */
    }

    printf("[PACK] %s (%d failures)\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
