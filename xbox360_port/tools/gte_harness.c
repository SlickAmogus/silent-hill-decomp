/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gte_harness.c - run the software GTE on the desktop, both endiannesses.
 *
 * The 360's first full-game boot logged:
 *   [GTE] RotTransPers v(64,32,512) -> sx=303 sy=447 sz=0 flag=0x80021000
 *                                      (expect sx=352 sy=256)
 * sz=0 with the divide-overflow flag set means the transform collapsed, so
 * nothing downstream can be trusted. This reproduces that on the desktop -- the
 * GTE is pure fixed-point C with no platform dependency, so building it for
 * powerpc-linux-gnu and running it under qemu-ppc exercises the same
 * big-endian behaviour without spending a BadUpdate run per attempt.
 *
 * Build native (little-endian) and cross (big-endian) from the same sources and
 * diff: any field that differs is an endian bug, and the intermediates say which.
 */
#include <stdio.h>

#include <libgte.h>
#include <gtereg.h>   /* declares GTERegisters gteRegs */

/* PsyX_GTE.cpp references the PGXP/renderer globals even with PGXP off. Defined
 * here so the harness links standalone; all zero, which is PGXP disabled. */
int g_PsxUsePgxp;
int g_PsyX_UsePerPixelFlashlight;
int g_PsxWholeMapFar;
int g_PsxWholeMapLastSz;
int g_PgxpUseUnquantizedDepth;
int g_PsxWholeMapChunkSz;
int g_PsxWorldVShift;
float g_PgxpGteOfx, g_PgxpGteOfy, g_PgxpGteH;
/* g_PsyX_RtpSz is NOT defined here -- libgte.c already owns it. */

void Shadow_Store(void* addr, float x, float y, float w, unsigned value)
{ (void)addr; (void)x; (void)y; (void)w; (void)value; }
void VShadow_Store(void* addr, float x, float y, float z)
{ (void)addr; (void)x; (void)y; (void)z; }

int main(void)
{
    MATRIX rot = { { { 4096, 0, 0 }, { 0, 4096, 0 }, { 0, 0, 4096 } }, { 0, 0, 0 } };
    SVECTOR v  = { 64, 32, 512, 0 };
    int  sxy   = 0;
    long p     = 0;
    long flag  = 0;
    int  sz;

    printf("endian: %s\n",
           (*(const unsigned char*)(const unsigned int[]){1}) ? "little" : "BIG");
    printf("sizeof MATRIX=%d SVECTOR=%d short=%d int=%d long=%d ptr=%d\n",
           (int)sizeof(MATRIX), (int)sizeof(SVECTOR), (int)sizeof(short),
           (int)sizeof(int), (int)sizeof(long), (int)sizeof(void*));

    InitGeom();
    SetGeomOffset(320, 240);
    SetGeomScreen(256);
    SetRotMatrix(&rot);
    SetTransMatrix(&rot);

    /* What did the GTE actually receive? The rotation matrix reaches it as five
     * packed 32-bit control registers, so this says whether the packing survived
     * the trip. Identity should read 4096 0 0 / 0 4096 0 / 0 0 4096. */
    {
        int i;
        printf("CP2C raw:");
        for (i = 0; i < 8; i++)
            printf(" [%d]=%08x", i, (unsigned)gteRegs.CP2C.p[i].d);
        printf("\n");
        printf("matrix as GTE sees it: %d %d %d / %d %d %d / %d %d %d\n",
               gteRegs.CP2C.p[0].sw.l, gteRegs.CP2C.p[0].sw.h, gteRegs.CP2C.p[1].sw.l,
               gteRegs.CP2C.p[1].sw.h, gteRegs.CP2C.p[2].sw.l, gteRegs.CP2C.p[2].sw.h,
               gteRegs.CP2C.p[3].sw.l, gteRegs.CP2C.p[3].sw.h, gteRegs.CP2C.p[4].sw.l);
        printf("OFX=%d OFY=%d H=%d (expect 320<<16, 240<<16, 256)\n",
               gteRegs.CP2C.p[24].sd, gteRegs.CP2C.p[25].sd, gteRegs.CP2C.p[26].sw.l);
    }

    sz = RotTransPers(&v, &sxy, &p, &flag);

    /* sz is OTZ, i.e. SZ3>>2 (gte_stszotz), so 512>>2 = 128 -- not 512.
     * flag 0x1000 is IR0-saturated and benign; the error bit (31) must be clear. */
    printf("RotTransPers -> sx=%d sy=%d sz=%d flag=0x%lx  (expect sx=352 sy=256 sz=128)\n",
           (int)(short)(sxy & 0xFFFF), (int)(short)((sxy >> 16) & 0xFFFF), sz, flag);

    /* Intermediates: which stage first goes wrong. */
    printf("  raw sxy word = 0x%08x\n", (unsigned)sxy);
    return 0;
}
