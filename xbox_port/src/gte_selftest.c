/*
 * gte_selftest.c - boot-time proof that the reused PsyCross software GTE
 * (src/gte/PsyX_GTE.cpp + src/psx/inline_c.c + libgte.c) links and computes.
 *
 * Part 1 (milestone 2b): identity rotation, fixed perspective, one vertex.
 *   v=(64,32,512), OFX=320 OFY=240 h=256 -> sx=352 sy=256.
 * Part 2: the exact macro path the world drawers use (func_8005A900 /
 * func_8005AC50: gte_LoadVector0_1_2_XYZ, gte_rtpt, gte_FetchScreen0_1_2_XYZ,
 * gte_ldsxy3 / gte_nclip / gte_stopz, gte_NormalClip) on a real in-game matrix
 * with near, behind-camera and far vertices. The numbers have no "expected"
 * value; they are compared BETWEEN the console log and the Xenia log, because
 * the console drew an alley the emulator drew in full (hardware run 021) and
 * an arithmetic divergence in this path would show here before anything else.
 */
#include "game.h"
#include "inline_no_dmpsx.h"
#include "gpu_gte_pc.h"
#include <psyq/gtemac.h>
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

    /* Read the pair BY FIELD, as every game caller does: gte_stsxy stores the
     * raw register word, which is memory order on both byte orders. A shift
     * decode here is only right on little-endian. */
    SH_DBG("[GTE] RotTransPers v(64,32,512) -> sx=%d sy=%d sz=%d flag=0x%lx (expect sx=352 sy=256)",
           (int)((const DVECTOR*)&sxy)->vx, (int)((const DVECTOR*)&sxy)->vy, sz, flag);

    {
        /* Rotation rows from an [A900] probe in the alley, translation too. */
        MATRIX  m = { { { 3897, -111, 891 }, { -200, 2900, 1100 }, { -696, -2881, 2594 } }, { 27, 114, 368 } };
        DVECTOR inXy[6] = { { 10, 10 }, { 16, 29 }, { -25, -7 }, { 300, -200 }, { -600, 50 }, { 2000, 2000 } };
        s16     inZ[6]  = { -17, -15, -6, 900, -900, 2000 };
        DVECTOR outXy[6];
        u16     outZ[6];
        s32     ncl = 0, ncl2 = 0;
        u32     fl  = 0;
        int     g;

        SetRotMatrix(&m);
        SetTransMatrix(&m);
        for (g = 0; g < 6; g += 3)
        {
            gte_LoadVector0_1_2_XYZ(&inXy[g], &inZ[g]);
            gte_rtpt();
            gte_FetchScreen0_1_2_XYZ(&outXy[g], &outZ[g]);
            gte_stflg(&fl);
            SH_DBG("[GTE2] rtpt in=(%d,%d,%d)(%d,%d,%d)(%d,%d,%d) out=(%d,%d,%u)(%d,%d,%u)(%d,%d,%u) flag=0x%08x",
                   (int)inXy[g].vx, (int)inXy[g].vy, (int)inZ[g],
                   (int)inXy[g + 1].vx, (int)inXy[g + 1].vy, (int)inZ[g + 1],
                   (int)inXy[g + 2].vx, (int)inXy[g + 2].vy, (int)inZ[g + 2],
                   (int)outXy[g].vx, (int)outXy[g].vy, (unsigned)outZ[g],
                   (int)outXy[g + 1].vx, (int)outXy[g + 1].vy, (unsigned)outZ[g + 1],
                   (int)outXy[g + 2].vx, (int)outXy[g + 2].vy, (unsigned)outZ[g + 2],
                   (unsigned)fl);
            gte_ldsxy3(*(s32*)&outXy[g], *(s32*)&outXy[g + 1], *(s32*)&outXy[g + 2]);
            gte_nclip();
            gte_stopz(&ncl);
            gte_NormalClip(*(s32*)&outXy[g], *(s32*)&outXy[g + 1], *(s32*)&outXy[g + 2], &ncl2);
            SH_DBG("[GTE2] words=%08x %08x %08x nclip=%d NormalClip=%d avgZ=%d",
                   (unsigned)*(u32*)&outXy[g], (unsigned)*(u32*)&outXy[g + 1], (unsigned)*(u32*)&outXy[g + 2],
                   (int)ncl, (int)ncl2,
                   (int)((outZ[g] + outZ[g + 1] + outZ[g + 2] + outZ[g + 2]) >> 2));
        }
    }

    /* Part 3: the MATRIX MULTIPLY and the PACKED vertex path. Harry renders
     * right while world pieces come out rotated (hardware 021/022, Xenia
     * alike), and the view matrix logged for world meshes is not orthonormal
     * (row lengths 4089 / 3240 / 3963 instead of 4096). Every placement goes
     * through Vw_MultiplyAndTransformMatrix (gte_ldclmv / gte_rtir /
     * gte_stclmv) and MulMatrix0 (gte_ldv0 / gte_rtv0 / gte_stsv); the world
     * drawer alone projects through func_80057B7C's packed MATRIX trick
     * (gte_ldv3c / gte_rtpt / gte_stsxy3c / gte_stsz3). Each is checked
     * against plain C here; "ref" is the C result. */
    {
        extern void Vw_MultiplyAndTransformMatrix(MATRIX* transformMat, MATRIX* inMat, MATRIX* outMat);
        MATRIX a = { { { 3897, -111, 891 }, { -200, 2900, 1100 }, { -696, -2881, 2594 } }, { 27, 114, 368 } };
        MATRIX b = { { { 3547, 0, 2048 }, { 0, 4096, 0 }, { -2048, 0, 3547 } }, { 100, 200, 300 } };
        MATRIX ref, o1, o2;
        int i, j, k;
        for (i = 0; i < 3; i++) {
            long t = 0;
            for (j = 0; j < 3; j++) {
                long acc = 0;
                for (k = 0; k < 3; k++) acc += (long)a.m[i][k] * b.m[k][j];
                ref.m[i][j] = (short)(acc >> 12);
            }
            for (k = 0; k < 3; k++) t += (long)a.m[i][k] * b.t[k];
            ref.t[i] = (t >> 12) + a.t[i];
        }
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) { o1.m[i][j] = -1; o2.m[i][j] = -1; }
        o1.t[0] = o1.t[1] = o1.t[2] = -1; o2.t[0] = o2.t[1] = o2.t[2] = -1;
        MulMatrix0(&a, &b, &o1);
        Vw_MultiplyAndTransformMatrix(&a, &b, &o2);
        SH_DBG("[GTE3] ref  R=[%d %d %d][%d %d %d][%d %d %d] T=[%d %d %d]",
               ref.m[0][0], ref.m[0][1], ref.m[0][2], ref.m[1][0], ref.m[1][1], ref.m[1][2],
               ref.m[2][0], ref.m[2][1], ref.m[2][2], (int)ref.t[0], (int)ref.t[1], (int)ref.t[2]);
        SH_DBG("[GTE3] Mul0 R=[%d %d %d][%d %d %d][%d %d %d]",
               o1.m[0][0], o1.m[0][1], o1.m[0][2], o1.m[1][0], o1.m[1][1], o1.m[1][2],
               o1.m[2][0], o1.m[2][1], o1.m[2][2]);
        SH_DBG("[GTE3] VwMT R=[%d %d %d][%d %d %d][%d %d %d] T=[%d %d %d]",
               o2.m[0][0], o2.m[0][1], o2.m[0][2], o2.m[1][0], o2.m[1][1], o2.m[1][2],
               o2.m[2][0], o2.m[2][1], o2.m[2][2], (int)o2.t[0], (int)o2.t[1], (int)o2.t[2]);

        /* Packed path, same matrix and vertices as [GTE2] group 1, so the
         * expected screen result is the [GTE2] line: (343,325,348)
         * (349,338,335) (321,314,373). */
        {
            MATRIX  pk;
            DVECTOR xy[3];
            s16     z[3];
            xy[0].vx = 10;  xy[0].vy = 10;  z[0] = -17;
            xy[1].vx = 16;  xy[1].vy = 29;  z[1] = -15;
            xy[2].vx = -25; xy[2].vy = -7;  z[2] = -6;
            SetRotMatrix(&a);
            SetTransMatrix(&a);
            *(s32*)&pk.m[0][0] = *(s32*)&xy[0];
            *(s32*)&pk.m[1][1] = *(s32*)&xy[1];
            *(s32*)&pk.m[2][2] = *(s32*)&xy[2];
            pk.m[0][2]        = z[0];
            pk.m[2][0]        = z[1];
            *(s16*)&pk.t[0]   = z[2];
            gte_ldv3c(&pk);
            gte_rtpt();
            gte_stsxy3c(xy);
            gte_stsz3(&pk.m[0][2], &pk.m[2][0], &pk.t[0]);
            SH_DBG("[GTE3] packed out=(%d,%d)(%d,%d)(%d,%d) z=(%d,%d,%d) expect (343,325,348)(349,338,335)(321,314,373)",
                   xy[0].vx, xy[0].vy, xy[1].vx, xy[1].vy, xy[2].vx, xy[2].vy,
                   (int)pk.m[0][2], (int)pk.m[2][0], (int)(s16)pk.t[0]);
        }
    }
}
