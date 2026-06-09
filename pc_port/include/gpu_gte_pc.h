/*
 * gpu_gte_pc.h - PC replacements for Silent Hill's custom GTE inline assembly macros
 *
 * The original gpu.h defines many MIPS inline assembly macros for GTE operations.
 * This header provides C implementations that use PsyCross's GTE emulation.
 *
 * These replace the __asm__ volatile GTE macros from the decomp's gpu.h.
 */
#ifndef _GPU_GTE_PC_H
#define _GPU_GTE_PC_H

#include <libgte.h>
#include <inline_c.h>

#ifdef SH_PC_PORT

/*
 * PsyCross inline_c.h provides MTC2/CTC2/MFC2/CFC2 as C functions.
 * We redefine the Silent Hill custom GTE macros to use those.
 */

/* gte_lddqa - Load DQA register (COP2 control reg 27) */
#undef gte_lddqa
#define gte_lddqa(r0) CTC2(r0, 27)

/* gte_lddqb_0 - Zero DQB register (COP2 control reg 28) */
#undef gte_lddqb_0
#define gte_lddqb_0() CTC2(0, 28)

/* gte_ldtr_0 - Zero transfer vector (COP2 control regs 5,6,7) */
#undef gte_ldtr_0
#define gte_ldtr_0() do { CTC2(0, 5); CTC2(0, 6); CTC2(0, 7); } while(0)

/* gte_SetLightSVector - Load SVECTOR into light matrix rows */
#undef gte_SetLightSVector
#define gte_SetLightSVector(p) do { \
    SVECTOR *_sv = (SVECTOR*)(p); \
    unsigned int _xy = *(unsigned int*)&_sv->vx; \
    unsigned int _z = (unsigned int)_sv->vz; \
    CTC2(_xy, 8); \
    CTC2(_z, 9); \
    CTC2(0, 10); \
    CTC2(0, 11); \
    CTC2(0, 12); \
} while(0)

/* gte_ldsv_ - Broadcast single value to universal vector (regs 9,10,11) */
#undef gte_ldsv_
#define gte_ldsv_(val) do { \
    int _v = (int)(val); \
    MTC2(_v, 9); MTC2(_v, 10); MTC2(_v, 11); \
} while(0)

/* gte_stIR1 - Read IR1 register (COP2 data reg 9) */
#undef gte_stIR1
static inline unsigned int gte_stIR1_func(void) { return MFC2(9); }
#define gte_stIR1() gte_stIR1_func()

/* gte_SetVector0 - Load SVECTOR into Vector 0 */
#undef gte_SetVector0
#define gte_SetVector0(p) do { \
    SVECTOR *_sv = (SVECTOR*)(p); \
    unsigned int _xy = *(unsigned int*)&_sv->vx; \
    unsigned int _z = (unsigned int)_sv->vz; \
    MTC2(_xy, 0); \
    MTC2(_z, 1); \
} while(0)

/* gte_SetLightSourceXY - Set light source X,Y (COP2 control reg 8) */
#undef gte_SetLightSourceXY
#define gte_SetLightSourceXY(x, y) do { \
    unsigned int _val = ((unsigned int)(x) & 0xFFFF) | ((unsigned int)(y) << 16); \
    CTC2(_val, 8); \
} while(0)

/* gte_SetLightSourceZ - Set light source Z (COP2 control reg 9) + clear 10-12 */
#undef gte_SetLightSourceZ
#define gte_SetLightSourceZ(z) do { \
    CTC2((z) & 0xFFFF, 9); \
    CTC2(0, 10); \
    CTC2(0, 11); \
    CTC2(0, 12); \
} while(0)

/* gte_SetRotMatrix_Row0_1 - Load rotation matrix rows 0 and 1 */
#undef gte_SetRotMatrix_Row0_1
#define gte_SetRotMatrix_Row0_1(r0) do { \
    unsigned int *_p = (unsigned int*)(r0); \
    CTC2(_p[0], 0); \
    CTC2(_p[1], 1); \
    CTC2(_p[2], 2); \
} while(0)

/* gte_SetRotMatrix_Row2 - Load rotation matrix row 2 */
#undef gte_SetRotMatrix_Row2
#define gte_SetRotMatrix_Row2(r0) do { \
    unsigned int *_p = (unsigned int*)(r0); \
    CTC2(_p[3], 3); \
    CTC2(_p[4], 4); \
} while(0)

/* gte_LoadVector0_1_2_XYZ - Load 3 vectors from xy/z arrays */
#undef gte_LoadVector0_1_2_XYZ
#define gte_LoadVector0_1_2_XYZ(xy, z) do { \
    unsigned int *_xy = (unsigned int*)(xy); \
    unsigned short *_z = (unsigned short*)(z); \
    MTC2(_xy[0], 0); MTC2((unsigned int)_z[0], 1); \
    MTC2(_xy[1], 2); MTC2((unsigned int)_z[1], 3); \
    MTC2(_xy[2], 4); MTC2((unsigned int)_z[2], 5); \
} while(0)

/* gte_FetchScreen0_1_2_XYZ - Fetch 3 screen vertices and Z values */
#undef gte_FetchScreen0_1_2_XYZ
#define gte_FetchScreen0_1_2_XYZ(xy, z) do { \
    unsigned int *_xy = (unsigned int*)(xy); \
    unsigned short *_z = (unsigned short*)(z); \
    _z[0] = (unsigned short)MFC2(17); \
    _xy[0] = MFC2(12); \
    _z[1] = (unsigned short)MFC2(18); \
    _xy[1] = MFC2(13); \
    _z[2] = (unsigned short)MFC2(19); \
    _xy[2] = MFC2(14); \
} while(0)

/* gte_SetRotMatrix_custom - Custom rotation matrix load */
#undef gte_SetRotMatrix_custom
#define gte_SetRotMatrix_custom(mat) do { \
    short *_m = (short*)(mat); \
    CTC2(((unsigned int)(unsigned short)_m[3] << 16) | (unsigned short)_m[0], 0); \
    CTC2(((unsigned int)(unsigned short)_m[7] << 16) | (unsigned short)_m[6], 1); \
    CTC2(((unsigned int)(unsigned short)_m[5] << 16) | (unsigned short)_m[2], 1); \
    CTC2(((unsigned int)(unsigned short)_m[1] << 16) | (unsigned short)_m[6], 1); \
    CTC2((unsigned int)(unsigned short)_m[8], 4); \
} while(0)

/* gte_LoadVector0_XYZ - Load individual x,y,z into Vector 0 */
#undef gte_LoadVector0_XYZ
#define gte_LoadVector0_XYZ(x, y, z) do { \
    unsigned int _val = ((unsigned int)(x) & 0xFFFF) | ((unsigned int)(y) << 16); \
    MTC2(_val, 0); \
    MTC2((int)(z), 1); \
} while(0)

/* gte_ldvxy0_Zero / gte_ldvxy0 - Load VXY0 register */
#undef gte_ldvxy0_Zero
#define gte_ldvxy0_Zero() MTC2(0, 0)

#undef gte_ldvxy0
#define gte_ldvxy0(val) MTC2(val, 0)

/* gte_ldvz0 - Sets the GTE vz0 register to 0 (upstream renamed from gte_gte_ldvz0). */
#undef gte_ldvz0
#define gte_ldvz0() MTC2(0, 1)

/* gte_stSZ3 - Read SZ3 register */
#undef gte_stSZ3
static inline unsigned int gte_stSZ3_func(void) { return MFC2(19); }
#define gte_stSZ3() gte_stSZ3_func()

/* gte_stMAC0/1/2 - Read MAC registers */
#undef gte_stMAC0
static inline int gte_stMAC0_func(void) { return (int)MFC2(24); }
#define gte_stMAC0() gte_stMAC0_func()

#undef gte_stMAC1
static inline unsigned int gte_stMAC1_func(void) { return MFC2(25); }
#define gte_stMAC1() gte_stMAC1_func()

#undef gte_stMAC2
static inline unsigned int gte_stMAC2_func(void) { return MFC2(26); }
#define gte_stMAC2() gte_stMAC2_func()

/* gte_stMAC12 - Store MAC1 and MAC2 to short array */
#undef gte_stMAC12
#define gte_stMAC12(r0) do { \
    short *_p = (short*)(r0); \
    _p[0] = (short)MFC2(25); \
    _p[1] = (short)MFC2(26); \
} while(0)

/* gte_ldsv3_ - Load 3 values to universal vector */
#undef gte_ldsv3_
#define gte_ldsv3_(x, y, z) do { \
    MTC2((int)(x), 9); MTC2((int)(y), 10); MTC2((int)(z), 11); \
} while(0)

/* gte_ldR13R21 - Load negated value to control reg 1 */
#undef gte_ldR13R21
#define gte_ldR13R21(val) CTC2(-(int)(val), 1)

/* gte_ldR11R12 - Load value to control regs 0 and 2 */
#undef gte_ldR11R12
#define gte_ldR11R12(r0) do { CTC2(r0, 0); CTC2(r0, 2); } while(0)

/* gte_IsDisabled - Always returns 0 (GTE is always available on PC) */
#undef gte_IsDisabled
#define gte_IsDisabled() (0)

/* gte_stlzcr - Read flag register (leading zero count result) */
#undef gte_stlzcr
#define gte_stlzcr(dst) do { (dst) = CFC2(31); } while(0)

/* gte_ldir_stbk - Copy background color (BK R/G/B) into IR1/IR2/IR3 */
#undef gte_ldir_stbk
#define gte_ldir_stbk() do { \
    MTC2(CFC2(13), 9);  \
    MTC2(CFC2(14), 10); \
    MTC2(CFC2(15), 11); \
} while(0)

/* gte_ldmac_stir - Copy MAC1/MAC2/MAC3 into background color (BK R/G/B) */
#undef gte_ldmac_stir
#define gte_ldmac_stir() do { \
    CTC2(MFC2(25), 13); \
    CTC2(MFC2(26), 14); \
    CTC2(MFC2(27), 15); \
} while(0)

#endif /* SH_PC_PORT */

#endif /* _GPU_GTE_PC_H */
