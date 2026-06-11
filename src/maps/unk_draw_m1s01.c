#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "main/rng.h"

#if defined(MAP1_S01)
#include "maps/map1/map1_s01.h" // For `sharedData_800DFB7C_0_s00` size.
#elif defined(MAP6_S04)
#include "maps/map6/map6_s04.h" // For `sharedData_800DFB7C_0_s00` size.
#endif

// Unknown drawing code included in M1S01 and M6S04.
// Called by cutscene event code? Might be related to smoke/steam particle effects.
// TODO: Make this separate split in each map instead of `#include`.

void sharedFunc_800CB7F4_1_s01(void)
{
    s32 count;
    s32 i;

    count = sharedData_800DEE50_1_s01.field_4;

#ifdef SH_PC_PORT
    /* x86 idiv faults on the zero divisors MIPS tolerates; an empty/unset
     * smoke config means there is nothing to spawn. */
    if (sharedData_800DEE50_1_s01.field_4 == 0 || sharedData_800DEE50_1_s01.field_C == 0)
    {
        return;
    }
#endif

    for (i = 0; i < ARRAY_SIZE(sharedData_800DFB7C_0_s00); i++)
    {
        if (sharedData_800DFB7C_0_s00[i].field_A != 0)
        {
            continue;
        }

        sharedData_800DFB7C_0_s00[i].field_A         = 13;
        sharedData_800DFB7C_0_s00[i].field_C.field_0 = ((TO_FIXED(sharedData_800DEE50_1_s01.field_6 - sharedData_800DEE50_1_s01.field_8, Q12_SHIFT) / sharedData_800DEE50_1_s01.field_C) * count) / sharedData_800DEE50_1_s01.field_4;

        count--;
        if (count == 0)
        {
            break;
        }
    }

    sharedData_800DEE50_1_s01.field_10 = 0;
    D_800C4414                        |= 1 << 1;
}

void sharedFunc_800CB8A0_1_s01(s32 idx)
{
    s32    rngB;
    q19_12 angleZ;
    s16    rngX;

    if (sharedData_800DEE50_1_s01.field_2 == 0)
    {
        sharedData_800DFB7C_0_s00[idx].field_A = 14;
    }

#ifdef SH_PC_PORT
    /* x86 rem-by-zero faults; spawn radius 0 just means spawn at origin. */
    if (sharedData_800DEE50_1_s01.field_A == 0)
    {
        sharedData_800DFB7C_0_s00[idx].field_0.vx_0 = 0;
        sharedData_800DFB7C_0_s00[idx].field_4.vz_4 = 0;
    }
    else
#endif
    if (sharedData_800DEE50_1_s01.field_0 == 0)
    {
        rngX                                = (Rng_Rand16() % sharedData_800DEE50_1_s01.field_A);
        angleZ                              = Rng_GenerateUInt(0, 4095);
        sharedData_800DFB7C_0_s00[idx].field_0.vx_0 = (s32)(rngX * Math_Cos(angleZ)) >> Q12_SHIFT;
        sharedData_800DFB7C_0_s00[idx].field_4.vz_4 = (s32)(rngX * Math_Sin(angleZ)) >> Q12_SHIFT;
    }
    else
    {
        sharedData_800DFB7C_0_s00[idx].field_0.vx_0 = (Rng_Rand16() % (sharedData_800DEE50_1_s01.field_A * 2)) - sharedData_800DEE50_1_s01.field_A;
        sharedData_800DFB7C_0_s00[idx].field_4.vz_4 = (Rng_Rand16() % (sharedData_800DEE50_1_s01.field_A * 2)) - sharedData_800DEE50_1_s01.field_A;
    }

    sharedData_800DFB7C_0_s00[idx].vy_8            = sharedData_800DEE50_1_s01.field_8;
    sharedData_800DFB7C_0_s00[idx].field_B         = Rng_Rand16() % 3;
    sharedData_800DFB7C_0_s00[idx].field_C.field_0 = 0;
}

bool sharedFunc_800CBA38_1_s01(s32 idx)
{
    sharedData_800DFB7C_0_s00[idx].field_C.field_0 += Q12_MULT_PRECISE(g_DeltaTime, ((Rng_Rand16() % Q12_ANGLE(144.0f)) + Q12_ANGLE(288.0f)));

#ifdef SH_PC_PORT
    /* x86 idiv faults on zero; field_C == 0 means no rise speed, so the
     * lifetime threshold is unreachable — keep the particle as-is. */
    if (sharedData_800DEE50_1_s01.field_C == 0)
    {
        return false;
    }
#endif

    if (Q12_DIV(sharedData_800DEE50_1_s01.field_6 - sharedData_800DEE50_1_s01.field_8, sharedData_800DEE50_1_s01.field_C) < sharedData_800DFB7C_0_s00[idx].field_C.field_0)
    {
        sharedFunc_800CB8A0_1_s01(idx);
        return true;
    }

    return false;
}

bool sharedFunc_800CBB30_1_s01(POLY_FT4** poly, s32 idx)
{
    typedef struct
    {
        s_func_8005E89C field_0;
        SVECTOR         field_12C;
        s32             field_134;
        DVECTOR         field_138;
    } s_func_800CBB30;

    q19_12           temp_a0;
    s32              temp_s1;
    s32              temp_v0_2;
    s32              temp_v0_3;
    s32              temp_v0_4;
    s32              temp_v0_5;
    s32              temp_v1_2;
    s32              temp_v1_3;
    s32              var_a1;
    s_func_800CBB30* ptr;

    ptr = PSX_SCRATCH;

    *(s32*)&(*poly)->u0 = (((sharedData_800DFB7C_0_s00[idx].field_B << 5) + 160) << 8) + 0x011300E0;
    *(s32*)&(*poly)->u1 = (((sharedData_800DFB7C_0_s00[idx].field_B << 5) + 160) << 8) + 0x2B00FF;
    *(u16*)&(*poly)->u2 = (((sharedData_800DFB7C_0_s00[idx].field_B << 5) + 191) << 8) + 0xE0;
    *(u16*)&(*poly)->u3 = (((sharedData_800DFB7C_0_s00[idx].field_B << 5) + 191) << 8) + 0xFF;

    temp_v1_2 = 0x40 - (sharedData_800DFB7C_0_s00[idx].field_C.field_0 >> 6);
    if (temp_v1_2 >= 0)
    {
        var_a1 = Q12_MULT_PRECISE(temp_v1_2, (0x400 - MIN(sharedData_800DEE50_1_s01.field_10 * 2, 0x400)) * 4);
    }
    else
    {
        var_a1 = 0;
    }

    setRGBC0(*poly, var_a1, var_a1, var_a1, PRIM_POLY | RECT_BLEND | RECT_TEXTURE | RECT_SIZE_1);

    temp_v0_2 = sharedData_800DFB7C_0_s00[idx].field_0.vx_0 >> 6;
    temp_v0_3 = sharedData_800DFB7C_0_s00[idx].field_4.vz_4 >> 6;

    sharedData_800DFB7C_0_s00[idx].vy_8 += Q12_MULT_PRECISE(g_DeltaTime, sharedData_800DEE50_1_s01.field_C);

    temp_v0_4 = SquareRoot0(SQUARE(temp_v0_2) + SQUARE(temp_v0_3)) << 10;
    temp_s1   = Q12_MULT_PRECISE(temp_v0_4, sharedData_800DEE50_1_s01.field_A);

#ifdef SH_PC_PORT
    /* PSX MIPS rem-by-zero returns garbage without trapping; x86 idiv raises
     * #DE. A freshly-spawned smoke particle has ~zero velocity, so temp_s1 (its
     * speed) is 0 and `% temp_s1` crashes (chemical-on-hand cutscene). Skip the
     * random kick that frame — the PSX result was garbage anyway. */
    temp_v1_3 = (temp_s1 != 0) ? (Rng_Rand16() % temp_s1) - (temp_s1 >> 2) : 0;
#else
    temp_v1_3 = (Rng_Rand16() % temp_s1) - (temp_s1 >> 2);
#endif

    temp_s1   = Q12_MULT_PRECISE(g_DeltaTime, temp_v1_3) >> 1;
    temp_v0_5 = ratan2(sharedData_800DFB7C_0_s00[idx].field_0.vx_0, sharedData_800DFB7C_0_s00[idx].field_4.vz_4);

    sharedData_800DFB7C_0_s00[idx].field_0.vx_0    += Q12_MULT(temp_s1, Math_Cos(temp_v0_5));
    sharedData_800DFB7C_0_s00[idx].field_4.vz_4    += Q12_MULT(temp_s1, Math_Sin(temp_v0_5));
    sharedData_800DFB7C_0_s00[idx].field_C.field_0 += MAX(temp_s1, 0);

    *(s32*)&ptr->field_12C = ((((sharedData_800DFB7C_0_s00[idx].field_0.vx_0 + sharedData_800DEE50_1_s01.field_14) >> 4) - (u16)ptr->field_0.field_0.vx) & 0xFFFF) +
                             (((sharedData_800DFB7C_0_s00[idx].vy_8 >> 4) - ptr->field_0.field_0.vy) << 16);

    ptr->field_12C.vz = ((sharedData_800DFB7C_0_s00[idx].field_4.vz_4 + sharedData_800DEE50_1_s01.field_18) >> 4) - ptr->field_0.field_0.vz;

#ifdef SH_PC_PORT
    {
        /* Same PSX div-by-zero guard: this denominator reaches 0 as field_10
         * (the cos input) sweeps. Skip the growth term that frame, don't #DE. */
        s32 _denom = Q12_MULT(Math_Cos(sharedData_800DEE50_1_s01.field_10), sharedData_800DEE50_1_s01.field_6) -
                     sharedData_800DEE50_1_s01.field_8;
        if (_denom != 0) {
            sharedData_800DFB7C_0_s00[idx].field_C.field_0 += CLAMP_LOW(
                TO_FIXED(Q12_MULT_PRECISE(g_DeltaTime, sharedData_800DEE50_1_s01.field_C), Q12_SHIFT) / _denom, 0);
        }
    }
#else
    sharedData_800DFB7C_0_s00[idx].field_C.field_0 += CLAMP_LOW(TO_FIXED(Q12_MULT_PRECISE(g_DeltaTime, sharedData_800DEE50_1_s01.field_C), Q12_SHIFT) /
                                                                (Q12_MULT(Math_Cos(sharedData_800DEE50_1_s01.field_10), sharedData_800DEE50_1_s01.field_6) -
                                                                 sharedData_800DEE50_1_s01.field_8),
                                                                0);
#endif

    if (sharedData_800DFB7C_0_s00[idx].field_C.field_0 > Q12(1.0f) || sharedData_800DEE50_1_s01.field_10 == Q12(0.25f))
    {
        if (sharedData_800DEE50_1_s01.field_1C == 0 && Math_Cos(sharedData_800DEE50_1_s01.field_10) < Rng_GenerateUInt(0, Q12_ANGLE(360.0f) - 1))
        {
            sharedData_800DFB7C_0_s00[idx].field_A = 0;
        }
        else
        {
            sharedFunc_800CB8A0_1_s01(idx);
        }

        return false;
    }

    gte_ldv0(&ptr->field_12C);
    gte_rtps();
    gte_stsxy(&ptr->field_138);
    gte_stsz(&ptr->field_134);

    if (ptr->field_134 <= 0 || (ptr->field_134 >> 3) >= ORDERING_TABLE_SIZE)
    {
        return false;
    }

    if (ABS(ptr->field_138.vx) > 200)
    {
        return false;
    }

    if (ABS(ptr->field_138.vy) > 160)
    {
        return false;
    }

    temp_a0 = sharedData_800DFB7C_0_s00[idx].field_C.field_0;
    var_a1  = sharedData_800DEE50_1_s01.field_1 * ptr->field_0.field_2C / ptr->field_134;

    if (temp_a0 < 0x200)
    {
        var_a1 = Q12_MULT_PRECISE(var_a1, (temp_a0 * 4) + 0x800);
    }

    if (var_a1 != 0)
    {
        setXY0Fast(*poly, (u16)ptr->field_138.vx - var_a1, ptr->field_138.vy - var_a1);
        setXY1Fast(*poly, (u16)ptr->field_138.vx + var_a1, ptr->field_138.vy - var_a1);
        setXY2Fast(*poly, (u16)ptr->field_138.vx - var_a1, ptr->field_138.vy + var_a1);
        setXY3Fast(*poly, (u16)ptr->field_138.vx + var_a1, ptr->field_138.vy + var_a1);
    }
    else
    {
        setXY0Fast(*poly, ptr->field_138.vx, ptr->field_138.vy);
        setXY1Fast(*poly, (u16)ptr->field_138.vx + 1, ptr->field_138.vy);
        setXY2Fast(*poly, ptr->field_138.vx, ptr->field_138.vy + 1);
        setXY3Fast(*poly, (u16)ptr->field_138.vx + 1, ptr->field_138.vy + 1);
    }

#ifdef SH_PC_PORT
    /* field_134 passed the `<= 0` check above but the OT bias can push it
     * negative, indexing org[] before the array — clamp instead. */
    ptr->field_134 = CLAMP_LOW(ptr->field_134 - sharedData_800DEE50_1_s01.field_12, 0);
#else
    ptr->field_134 -= sharedData_800DEE50_1_s01.field_12;
#endif
    addPrimFast(&g_OrderingTable0[g_ActiveBufferIdx].org[ptr->field_134 >> 3], *poly, 9);
    *poly += 1;

    return true;
}
