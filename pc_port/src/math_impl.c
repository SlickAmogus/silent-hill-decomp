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

    /* Negate angles */
    double sx = sin(-rx), cx = cos(-rx);
    double sy = sin(-ry), cy = cos(-ry);
    double sz = sin(-rz), cz = cos(-rz);

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
 * ReadGeomOffset / ReadGeomScreen - GTE geometry parameter access
 * These read GTE registers. On PC, return reasonable defaults.
 */
void ReadGeomOffset(s32* ofx, s32* ofy)
{
    if (ofx) *ofx = 0;
    if (ofy) *ofy = 0;
}

s32 ReadGeomScreen(void)
{
    return 256; /* Default projection distance */
}

s32 gte_ReadGeomScreen(void)
{
    return 256;
}
