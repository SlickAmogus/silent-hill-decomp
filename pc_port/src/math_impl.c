/*
 * math_impl.c - Implementations of PSX math functions for PC port
 *
 * On PSX these were hand-optimized MIPS assembly. On PC we implement them
 * using standard C math, matching the Q19.12 fixed-point conventions:
 *   - Angles: Q19.12 where 4096 = full rotation (360 degrees)
 *   - Values: Q19.12 where 4096 = 1.0
 */
#include <math.h>
#include "game.h"
#include "bodyprog/math/math.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Math_Sin - Compute sine in Q19.12 fixed-point
 *
 * angle: Q19.12, where 4096 = full rotation (360 degrees)
 * returns: Q19.12, where 4096 = 1.0
 */
q19_12 Math_Sin(q19_12 angle)
{
    double rad = ((double)angle / 4096.0) * 2.0 * M_PI;
    return (q19_12)(sin(rad) * 4096.0);
}

/*
 * Math_Cos - Compute cosine in Q19.12 fixed-point
 *
 * angle: Q19.12, where 4096 = full rotation (360 degrees)
 * returns: Q19.12, where 4096 = 1.0
 */
q19_12 Math_Cos(q19_12 angle)
{
    double rad = ((double)angle / 4096.0) * 2.0 * M_PI;
    return (q19_12)(cos(rad) * 4096.0);
}

/*
 * SquareRoot12 - Integer square root with Q19.12 output
 *
 * Takes a Q19.12 value and returns its square root in Q19.12.
 */
s32 SquareRoot12(s32 val)
{
    if (val <= 0) return 0;
    return (s32)(sqrt((double)val) * 64.0);  /* sqrt(Q12) = sqrt(val/4096)*4096 = sqrt(val)*64 */
}

/* SquareRoot0 is provided by PsyCross (libgte) */

/*
 * Math_RotMatrixZxyNeg - Build rotation matrix from ZXY Euler angles (negated)
 */
void Math_RotMatrixZxyNeg(SVECTOR* rot, MATRIX* mat)
{
    double rx = ((double)rot->vx / 4096.0) * 2.0 * M_PI;
    double ry = ((double)rot->vy / 4096.0) * 2.0 * M_PI;
    double rz = ((double)rot->vz / 4096.0) * 2.0 * M_PI;

    double sx = sin(rx), cx = cos(rx);
    double sy = sin(ry), cy = cos(ry);
    double sz = sin(rz), cz = cos(rz);

    /* R = Ry * Rx * Rz. Verified element-for-element against real PSX (USA)
     * vcWork.cam_mat at the opening street: e.g. m[1][2] = -sx is POSITIVE
     * (+1562 hw) for a downward-pitched -Z-facing cam.
     *
     * The previous PC form negated all three angles AND used the transposed
     * layout. That produced m[1][2] = sy*sz + cy*sx*cz, which entangles pitch
     * with yaw: it matches Ry*Rx*Rz only when cos(yaw)=1, and FLIPS the pitch
     * sign when cos(yaw)=-1 (facing -Z). Result: every -Z-facing road/chase
     * camera pitched up into the sky instead of down at Harry, while models
     * (which sit at pitch ~0, where the entangled term vanishes) looked fine.
     * This is the matrix vwMatrixToAngleYXZ inverts, so angle->matrix->angle
     * now round-trips identically, matching the PSX camera pipeline. */
    mat->m[0][0] = (short)(( cy*cz + sy*sx*sz) * 4096.0);
    mat->m[0][1] = (short)((-cy*sz + sy*sx*cz) * 4096.0);
    mat->m[0][2] = (short)(( sy*cx) * 4096.0);

    mat->m[1][0] = (short)(( cx*sz) * 4096.0);
    mat->m[1][1] = (short)(( cx*cz) * 4096.0);
    mat->m[1][2] = (short)((-sx) * 4096.0);

    mat->m[2][0] = (short)((-sy*cz + cy*sx*sz) * 4096.0);
    mat->m[2][1] = (short)(( sy*sz + cy*sx*cz) * 4096.0);
    mat->m[2][2] = (short)(( cy*cx) * 4096.0);

    {
        const double exact[9] = {
             cy*cz + sy*sx*sz, -cy*sz + sy*sx*cz,  sy*cx,
             cx*sz,             cx*cz,            -sx,
            -sy*cz + cy*sx*sz,  sy*sz + cy*sx*cz,  cy*cx
        };
        PGXP_MatrixRegister(mat, exact);
    }
}

/*
 * Math_RotMatrixZxyNegGte - Same as above but may also set GTE rotation matrix
 * For PC, same implementation as Math_RotMatrixZxyNeg.
 */
void Math_RotMatrixZxyNegGte(SVECTOR* rot, MATRIX* mat)
{
    Math_RotMatrixZxyNeg(rot, mat);
}

/*
 * Math_RotMatrixXyz - Build rotation matrix from XYZ Euler angles
 */
void Math_RotMatrixXyz(SVECTOR* rot, MATRIX* mat)
{
    double rx = ((double)rot->vx / 4096.0) * 2.0 * M_PI;
    double ry = ((double)rot->vy / 4096.0) * 2.0 * M_PI;
    double rz = ((double)rot->vz / 4096.0) * 2.0 * M_PI;

    double sx = sin(rx), cx = cos(rx);
    double sy = sin(ry), cy = cos(ry);
    double sz = sin(rz), cz = cos(rz);

    /* XYZ rotation order: Rz * Ry * Rx */
    mat->m[0][0] = (short)(( cy*cz) * 4096.0);
    mat->m[0][1] = (short)((-cy*sz) * 4096.0);
    mat->m[0][2] = (short)(( sy) * 4096.0);

    mat->m[1][0] = (short)(( sx*sy*cz + cx*sz) * 4096.0);
    mat->m[1][1] = (short)((-sx*sy*sz + cx*cz) * 4096.0);
    mat->m[1][2] = (short)((-sx*cy) * 4096.0);

    mat->m[2][0] = (short)((-cx*sy*cz + sx*sz) * 4096.0);
    mat->m[2][1] = (short)(( cx*sy*sz + sx*cz) * 4096.0);
    mat->m[2][2] = (short)(( cx*cy) * 4096.0);

    {
        const double exact[9] = {
             cy*cz,             -cy*sz,             sy,
             sx*sy*cz + cx*sz, -sx*sy*sz + cx*cz, -sx*cy,
            -cx*sy*cz + sx*sz,  cx*sy*sz + sx*cz,  cx*cy
        };
        PGXP_MatrixRegister(mat, exact);
    }
}

/*
 * Math_RotMatrixZxy - Build rotation matrix from ZXY Euler angles
 */
void Math_RotMatrixZxy(SVECTOR* rot, MATRIX* mat)
{
    double rx = ((double)rot->vx / 4096.0) * 2.0 * M_PI;
    double ry = ((double)rot->vy / 4096.0) * 2.0 * M_PI;
    double rz = ((double)rot->vz / 4096.0) * 2.0 * M_PI;

    double sx = sin(rx), cx = cos(rx);
    double sy = sin(ry), cy = cos(ry);
    double sz = sin(rz), cz = cos(rz);

    /* ZXY rotation order: Ry * Rx * Rz */
    mat->m[0][0] = (short)(( cy*cz + sy*sx*sz) * 4096.0);
    mat->m[0][1] = (short)(( cx*sz) * 4096.0);
    mat->m[0][2] = (short)((-sy*cz + cy*sx*sz) * 4096.0);

    mat->m[1][0] = (short)((-cy*sz + sy*sx*cz) * 4096.0);
    mat->m[1][1] = (short)(( cx*cz) * 4096.0);
    mat->m[1][2] = (short)(( sy*sz + cy*sx*cz) * 4096.0);

    mat->m[2][0] = (short)(( sy*cx) * 4096.0);
    mat->m[2][1] = (short)((-sx) * 4096.0);
    mat->m[2][2] = (short)(( cy*cx) * 4096.0);

    {
        const double exact[9] = {
             cy*cz + sy*sx*sz,  cx*sz, -sy*cz + cy*sx*sz,
            -cy*sz + sy*sx*cz,  cx*cz,  sy*sz + cy*sx*cz,
             sy*cx,             -sx,     cy*cx
        };
        PGXP_MatrixRegister(mat, exact);
    }
}

/*
 * Math_RotMatrixXyxGte - Build rotation matrix from XYX Euler angles (GTE variant)
 * For PC, just builds the matrix without GTE hardware interaction.
 */
void Math_RotMatrixXyxGte(SVECTOR* rot, MATRIX* mat)
{
    Math_RotMatrixXyz(rot, mat);
}

/*
 * Math_RotMatrixZ - Build rotation matrix for Z-axis rotation only
 */
MATRIX* Math_RotMatrixZ(s32 angle, MATRIX* mat)
{
    double rad = ((double)angle / 4096.0) * 2.0 * M_PI;
    double s = sin(rad), c = cos(rad);

    mat->m[0][0] = (short)( c * 4096.0);
    mat->m[0][1] = (short)(-s * 4096.0);
    mat->m[0][2] = 0;

    mat->m[1][0] = (short)( s * 4096.0);
    mat->m[1][1] = (short)( c * 4096.0);
    mat->m[1][2] = 0;

    mat->m[2][0] = 0;
    mat->m[2][1] = 0;
    mat->m[2][2] = 4096;

    {
        const double exact[9] = {
             c, -s, 0.0,
             s,  c, 0.0,
             0.0, 0.0, 1.0
        };
        PGXP_MatrixRegister(mat, exact);
    }

    return mat;
}

/*
 * Math_MatrixTransform - Transform a vector by a matrix
 * Multiplies mat * vec, result in Q19.12
 */
void Math_MatrixVectorMul(MATRIX* mat, VECTOR3* in, VECTOR3* out)
{
    s32 x = in->vx, y = in->vy, z = in->vz;

    out->vx = ((s32)mat->m[0][0] * x + (s32)mat->m[0][1] * y + (s32)mat->m[0][2] * z) >> 12;
    out->vy = ((s32)mat->m[1][0] * x + (s32)mat->m[1][1] * y + (s32)mat->m[1][2] * z) >> 12;
    out->vz = ((s32)mat->m[2][0] * x + (s32)mat->m[2][1] * y + (s32)mat->m[2][2] * z) >> 12;

    out->vx += mat->t[0];
    out->vy += mat->t[1];
    out->vz += mat->t[2];
}

/*
 * ReadGeomOffset / ReadGeomScreen - GTE geometry parameter access.
 * Must return the LIVE GTE control registers: the game varies H
 * (projection distance) with camera FOV and uses it to scale screen-space
 * effect sizes. The old hardcoded 256/0 defaults silently skewed every
 * caller (flare depth gate, effect quad sizing).
 * gte_ReadGeomScreen(out) is a macro in inline_no_dmpsx.h; the old void()
 * fallback that ignored its out pointer is gone on purpose so stragglers
 * fail at link time instead of reading stale memory.
 */
extern unsigned int CFC2(int reg); /* PsyCross GTE control reg read */

void ReadGeomOffset(s32* ofx, s32* ofy)
{
    if (ofx) *ofx = (s32)CFC2(24) >> 16;
    if (ofy) *ofy = (s32)CFC2(25) >> 16;
}

s32 ReadGeomScreen(void)
{
    return (s32)(s16)CFC2(26);
}
