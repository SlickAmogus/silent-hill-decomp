/*
 * gte_selftest.c - milestone-2b proof that the reused PsyCross software GTE
 * (src/gte/PsyX_GTE.cpp + src/psx/inline_c.c + libgte.c) links and computes
 * under nxdk. Transforms a known vertex with an identity rotation and a fixed
 * perspective, and logs the screen-space result.
 *
 * Expected: v=(64,32,512), OFX=320 OFY=240 h=256, identity rot, zero trans ->
 *   sx = 320 + 256*64/512 = 352
 *   sy = 240 + 256*32/512 = 256
 */
#include <libgte.h>

#include "sh_log.h"

void Gte_SelfTest(void)
{
    MATRIX rot = { { { 4096, 0, 0 }, { 0, 4096, 0 }, { 0, 0, 4096 } }, { 0, 0, 0 } }; /* identity (1.0 == 4096) */
    SVECTOR v  = { 64, 32, 512, 0 };
    int  sxy   = 0;
    long p     = 0;
    long flag  = 0;
    int  sz;

    InitGeom();
    SetGeomOffset(320, 240);
    SetGeomScreen(256);
    SetRotMatrix(&rot);
    SetTransMatrix(&rot);

    sz = RotTransPers(&v, &sxy, &p, &flag);

    SH_DBG("[GTE] RotTransPers v(64,32,512) -> sx=%d sy=%d sz=%d flag=0x%lx (expect sx=352 sy=256)",
           (int)(short)(sxy & 0xFFFF), (int)(short)((sxy >> 16) & 0xFFFF), sz, flag);
}
