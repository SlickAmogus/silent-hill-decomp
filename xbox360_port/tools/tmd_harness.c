/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tmd_harness.c - does the TMD swapper give the drawers native values?
 *
 * Builds a synthetic TMD the way the disc holds one (little-endian bytes):
 * two objects, a shared normal pool (to prove the double-swap guard), and one
 * primitive of each layout the GsSortObject4J drawers dispatch -- including
 * both Silent Hill nonstandard layouts, which are the reason the swapper
 * cannot key on mode alone. Runs the swapper, then reads every field back the
 * way the consumers do: native u32 loads for the object table (as
 * GsMapModelingData), native u16 struct fields for indices/clut/tpage (as the
 * TMD_P_* overlays), native s16 for vertices. Also asserts idempotency: a
 * second call must change nothing, because the inventory re-registers the
 * same buffer every carousel open.
 *
 * The input is the &buf[1] form GsMapModelingData receives: flags, nobj,
 * object table, data. Offsets are relative to the object table.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void Tmd_SwapForBigEndian(void* tmdNoId);

static int failures;

static void Check(const char* what, long long got, long long want)
{
    int ok = (got == want);
    if (!ok)
        failures++;
    printf("  %-36s got=%-10lld want=%-10lld %s\n", what, got, want, ok ? "ok" : "FAIL");
}

static void PutLE16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void PutLE32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint16_t N16(const uint8_t* p) { return *(const uint16_t*)p; }
static uint32_t N32(const uint8_t* p) { return *(const uint32_t*)p; }

int main(void)
{
    static uint8_t tmd[4096];
    uint8_t* obj;    /* object table base: offsets are relative to this */
    uint8_t* d;
    uint32_t vertTop, normTop, primTop, vert2Top, prim2Top;

    printf("endian: %s\n",
           (*(const unsigned char*)(const unsigned int[]){1}) ? "little" : "BIG");
    memset(tmd, 0, sizeof(tmd));

    PutLE32(tmd + 0, 0);          /* flags (FIXP=0) */
    PutLE32(tmd + 4, 2);          /* nobj  */
    obj = tmd + 8;

    /* Data area starts after 2 * 28-byte object entries. */
    vertTop  = 2 * 28;                    /* obj0: 3 vertices             */
    normTop  = vertTop + 3 * 8;           /* shared: 2 normals            */
    primTop  = normTop + 2 * 8;           /* obj0: 4 prims                */
    /* obj0 entries */
    PutLE32(obj + 0,  vertTop); PutLE32(obj + 4,  3);
    PutLE32(obj + 8,  normTop); PutLE32(obj + 12, 2);
    PutLE32(obj + 16, primTop); PutLE32(obj + 20, 4);
    PutLE32(obj + 24, 0);

    /* vertices: SVECTOR x,y,z,pad as s16 */
    d = obj + vertTop;
    PutLE16(d + 0, 0x1122); PutLE16(d + 2, (uint16_t)-77); PutLE16(d + 4, 0x0304); PutLE16(d + 6, 0);
    PutLE16(d + 8, 100);    PutLE16(d + 10, 200);          PutLE16(d + 12, 300);   PutLE16(d + 14, 0);
    PutLE16(d + 16, 400);   PutLE16(d + 18, 500);          PutLE16(d + 20, 600);   PutLE16(d + 22, 0);

    /* normals (shared pool) */
    d = obj + normTop;
    PutLE16(d + 0, 0x1000); PutLE16(d + 2, 0); PutLE16(d + 4, 0); PutLE16(d + 6, 0);
    PutLE16(d + 8, 0);      PutLE16(d + 10, 0x1000); PutLE16(d + 12, 0); PutLE16(d + 14, 0);

    /* obj0 prims: F3(lit flat tri), TG3(lit tex gouraud tri),
     * NG3 SH-variant (ilen=4, mode 0x30), NTG3 SH-variant (ilen=6). */
    d = obj + primTop;
    /* F3: hdr(olen=4,ilen=3,flag=1,mode=0x20), rgbc, n0v0, v1v2 */
    d[0]=4; d[1]=3; d[2]=1; d[3]=0x20;
    d[4]=10; d[5]=20; d[6]=30; d[7]=0x20;
    PutLE16(d+8, 1);  PutLE16(d+10, 0);      /* n0=1  v0=0 */
    PutLE16(d+12, 1); PutLE16(d+14, 2);      /* v1=1  v2=2 */
    d += 16;
    /* TG3: hdr(ilen=6,flag=1,mode=0x34), uv0+clut, uv1+tpage, uv2+pad, n0v0,n1v1,n2v2 */
    d[0]=6; d[1]=6; d[2]=1; d[3]=0x34;
    d[4]=8; d[5]=16; PutLE16(d+6, 0x7ABC);   /* clut  */
    d[8]=24; d[9]=32; PutLE16(d+10, 0x01F3); /* tpage */
    d[12]=40; d[13]=48; PutLE16(d+14, 0);
    PutLE16(d+16, 0); PutLE16(d+18, 0);
    PutLE16(d+20, 1); PutLE16(d+22, 1);
    PutLE16(d+24, 1); PutLE16(d+26, 2);
    d += 28;
    /* NG3 SH variant: hdr(ilen=4,flag=0,mode=0x30), rgbc, n0v0,n1v1,n2v2 */
    d[0]=4; d[1]=4; d[2]=0; d[3]=0x30;
    d[4]=99; d[5]=88; d[6]=77; d[7]=0x30;
    PutLE16(d+8, 0);  PutLE16(d+10, 2);      /* n0=0 v0=2 */
    PutLE16(d+12, 1); PutLE16(d+14, 1);
    PutLE16(d+16, 0); PutLE16(d+18, 0);
    d += 20;
    /* NTG3 SH variant: hdr(ilen=6,flag=0,mode=0x35), 3 uv words, 3 (n,v) words */
    d[0]=6; d[1]=6; d[2]=0; d[3]=0x35;
    d[4]=1; d[5]=2; PutLE16(d+6, 0x1234);
    d[8]=3; d[9]=4; PutLE16(d+10, 0x0155);
    d[12]=5; d[13]=6; PutLE16(d+14, 0);
    PutLE16(d+16, 1); PutLE16(d+18, 2);
    PutLE16(d+20, 0); PutLE16(d+22, 0);
    PutLE16(d+24, 1); PutLE16(d+26, 1);
    d += 28;

    /* obj1: its own vertex pool, SHARES obj0's normal pool, one NF4 prim. */
    vert2Top = (uint32_t)(d - obj);
    PutLE16(d + 0, 7); PutLE16(d + 2, 8); PutLE16(d + 4, 9); PutLE16(d + 6, 0);
    d += 8;
    prim2Top = (uint32_t)(d - obj);
    d[0]=3; d[1]=3; d[2]=0; d[3]=0x29;       /* NF4: mode 0x29, ilen=3 */
    d[4]=11; d[5]=22; d[6]=33; d[7]=0x29;
    PutLE16(d+8, 0);  PutLE16(d+10, 0);
    PutLE16(d+12, 0); PutLE16(d+14, 0);
    PutLE32(obj + 28 + 0,  vert2Top); PutLE32(obj + 28 + 4,  1);
    PutLE32(obj + 28 + 8,  normTop);  PutLE32(obj + 28 + 12, 2);   /* shared! */
    PutLE32(obj + 28 + 16, prim2Top); PutLE32(obj + 28 + 20, 1);
    PutLE32(obj + 28 + 24, 0);

    Tmd_SwapForBigEndian(tmd);

    Check("nobj (native, as GsMapModelingData)", N32(tmd + 4), 2);
    Check("obj0 vert_top",  N32(obj + 0),  vertTop);
    Check("obj0 n_vert",    N32(obj + 4),  3);
    Check("obj0 n_prim",    N32(obj + 20), 4);
    Check("obj1 norm_top (shared)", N32(obj + 28 + 8), normTop);

    Check("vtx0.x", (int16_t)N16(obj + vertTop + 0), 0x1122);
    Check("vtx0.y", (int16_t)N16(obj + vertTop + 2), -77);
    Check("vtx2.z", (int16_t)N16(obj + vertTop + 20), 600);
    Check("shared normal[1].y", (int16_t)N16(obj + normTop + 10), 0x1000);
    Check("obj1 vtx0.x", (int16_t)N16(obj + vert2Top + 0), 7);

    d = obj + primTop;
    Check("F3.n0", N16(d + 8), 1);
    Check("F3.v2", N16(d + 14), 2);
    Check("F3.rgb bytes untouched", d[4] | (d[5]<<8) | (d[6]<<16), 10 | (20<<8) | (30<<16));
    d += 16;
    Check("TG3.clut",  N16(d + 6), 0x7ABC);
    Check("TG3.tpage", N16(d + 10), 0x01F3);
    Check("TG3.uv bytes untouched", d[4] | (d[5]<<8), 8 | (16<<8));
    Check("TG3.v2", N16(d + 26), 2);
    d += 28;
    Check("NG3sh.v0 (SH layout hit)", N16(d + 10), 2);
    d += 20;
    Check("NTG3sh.clut", N16(d + 6), 0x1234);
    Check("NTG3sh.n0",   N16(d + 16), 1);
    d += 28 + 8;
    Check("NF4.v0", N16(d + 8), 0);

    /* Idempotency: the second call must be a no-op. */
    Tmd_SwapForBigEndian(tmd);
    Check("idempotent: nobj",     N32(tmd + 4), 2);
    Check("idempotent: TG3.clut", N16(obj + primTop + 16 + 6), 0x7ABC);
    Check("idempotent: vtx0.y",   (int16_t)N16(obj + vertTop + 2), -77);

    printf(failures ? "FAILED (%d)\n" : "PASSED (0 failures)\n", failures);
    return failures != 0;
}
