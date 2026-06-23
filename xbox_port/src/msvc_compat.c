/* msvc_compat.c — MSVC C runtime symbols needed by dsound.lib
 *
 * dsound.lib was compiled with MSVC and references compiler intrinsics
 * and runtime helpers.  We provide minimal implementations here to avoid
 * linking the full libcmt.lib (which cascades into hundreds of duplicate
 * symbol conflicts with nxdk's pdclib).
 *
 * Symbols provided:
 *   __ftol2            — float-to-long truncation (x87)
 *   __CIpow            — intrinsic pow() via x87 FYL2X/F2XM1
 *   __CIsinh           — intrinsic sinh() via x87 exponentials
 *   __fpclass          — IEEE 754 double classification
 *   __except_handler3  — SEH handler stub (kept for safety)
 */

#include <string.h>

/* ========================================================================
 * __except_handler3 — SEH exception dispatcher
 *
 * Called by __SEH_prolog (from sehprolg.obj) to register in the FS:[0]
 * exception chain.  On Xbox, structured exceptions from dsound code
 * should never actually fire, so we just return EXCEPTION_CONTINUE_SEARCH
 * to let the chain propagate (which will hit the kernel's default handler).
 * ======================================================================== */
int __cdecl _except_handler3(
    void *ExceptionRecord,
    void *EstablisherFrame,
    void *ContextRecord,
    void *DispatcherContext)
{
    (void)ExceptionRecord;
    (void)EstablisherFrame;
    (void)ContextRecord;
    (void)DispatcherContext;
    return 1; /* EXCEPTION_CONTINUE_SEARCH */
}


/* ========================================================================
 * __ftol2 — Convert x87 FPU top-of-stack to 64-bit integer (truncation)
 *
 * Called by MSVC-compiled code for (long) and (int) casts from float/double.
 * Reads ST(0), returns result in EDX:EAX.  Uses truncation (round toward 0)
 * by temporarily setting the x87 rounding mode.
 *
 * Pure x87 — works on Pentium III (Xbox CPU).
 * ======================================================================== */
__attribute__((naked)) void _ftol2(void)
{
    __asm__(
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "subl $16, %esp\n\t"
        "fnstcw -4(%ebp)\n\t"       /* save current control word       */
        "movw -4(%ebp), %ax\n\t"
        "orw $0x0C00, %ax\n\t"      /* RC=11: truncation (toward zero) */
        "movw %ax, -8(%ebp)\n\t"
        "fldcw -8(%ebp)\n\t"        /* load truncation control word    */
        "fistpll -16(%ebp)\n\t"     /* ST(0) -> 64-bit int at [ebp-16] */
        "fldcw -4(%ebp)\n\t"        /* restore original control word   */
        "movl -16(%ebp), %eax\n\t"  /* low 32 bits of result           */
        "movl -12(%ebp), %edx\n\t"  /* high 32 bits of result          */
        "leave\n\t"
        "ret\n"
    );
}


/* ========================================================================
 * _fpclass — IEEE 754 double classification
 *
 * Returns a bitmask identifying the float class (NaN, Inf, normal, etc.).
 * dsound.lib uses this for volume/parameter validation.
 * ======================================================================== */
#define MY_FPCLASS_SNAN   0x0001
#define MY_FPCLASS_QNAN   0x0002
#define MY_FPCLASS_NINF   0x0004
#define MY_FPCLASS_NN     0x0008
#define MY_FPCLASS_ND     0x0010
#define MY_FPCLASS_NZ     0x0020
#define MY_FPCLASS_PZ     0x0040
#define MY_FPCLASS_PD     0x0080
#define MY_FPCLASS_PN     0x0100
#define MY_FPCLASS_PINF   0x0200

int __cdecl _fpclass(double x)
{
    unsigned long long bits;
    int sign, exp;
    unsigned long long mant;

    memcpy(&bits, &x, sizeof(bits));
    sign = (int)(bits >> 63);
    exp  = (int)((bits >> 52) & 0x7FF);
    mant = bits & 0x000FFFFFFFFFFFFFull;

    if (exp == 0x7FF) {
        if (mant == 0)
            return sign ? MY_FPCLASS_NINF : MY_FPCLASS_PINF;
        /* Quiet NaN: bit 51 (MSB of mantissa) is set */
        return (mant & 0x0008000000000000ull) ? MY_FPCLASS_QNAN : MY_FPCLASS_SNAN;
    }
    if (exp == 0) {
        if (mant == 0)
            return sign ? MY_FPCLASS_NZ : MY_FPCLASS_PZ;
        return sign ? MY_FPCLASS_ND : MY_FPCLASS_PD;
    }
    return sign ? MY_FPCLASS_NN : MY_FPCLASS_PN;
}


/* ========================================================================
 * __CIpow — MSVC compiler intrinsic for pow(base, exponent)
 *
 * Calling convention: base in ST(1), exponent in ST(0).
 * Returns result in ST(0).
 *
 * Algorithm: x^y = 2^(y * log2(x))
 *   1. FYL2X computes y * log2(x)
 *   2. Split result into integer and fractional parts
 *   3. F2XM1 computes 2^frac - 1 (valid for |frac| <= 1)
 *   4. FSCALE multiplies by 2^int
 *
 * Pure x87 — no C library dependency.
 * ======================================================================== */
__attribute__((naked)) void _CIpow(void)
{
    __asm__(
        "fxch %st(1)\n\t"           /* ST0=base, ST1=exp              */
        "fyl2x\n\t"                 /* ST0 = exp * log2(base)         */
        "fld %st(0)\n\t"            /* dup                            */
        "frndint\n\t"               /* ST0=int_part, ST1=full_value   */
        "fxch %st(1)\n\t"           /* ST0=full, ST1=int              */
        "fsub %st(1)\n\t"           /* ST0=frac (full - int)          */
        "f2xm1\n\t"                 /* ST0=2^frac - 1                 */
        "fld1\n\t"                  /* ST0=1                          */
        "faddp\n\t"                 /* ST0=2^frac  (1 + (2^f - 1))   */
        "fscale\n\t"                /* ST0=2^frac * 2^int = result    */
        "fstp %st(1)\n\t"           /* pop int_part, leave result     */
        "ret\n"
    );
}


/* ========================================================================
 * __CIsinh — MSVC compiler intrinsic for sinh(x)
 *
 * Calling convention: argument in ST(0).  Returns result in ST(0).
 *
 * Algorithm: sinh(x) = (e^x - e^(-x)) / 2
 * Each e^v is computed as 2^(v * log2(e)) using the same FYL2X/F2XM1
 * technique as __CIpow.
 *
 * Pure x87 — no C library dependency.
 * ======================================================================== */
__attribute__((naked)) void _CIsinh(void)
{
    __asm__(
        /* --- Compute e^x --- */
        "fld %st(0)\n\t"            /* dup x: ST0=x, ST1=x            */
        "fldl2e\n\t"                /* ST0=log2(e), ST1=x, ST2=x      */
        "fmulp\n\t"                 /* ST0=x*log2(e), ST1=x            */
        "fld %st(0)\n\t"            /* dup                             */
        "frndint\n\t"               /* ST0=int, ST1=val, ST2=x         */
        "fxch %st(1)\n\t"           /* ST0=val, ST1=int                */
        "fsub %st(1)\n\t"           /* ST0=frac                        */
        "f2xm1\n\t"                 /* ST0=2^frac-1                    */
        "fld1\n\t"
        "faddp\n\t"                 /* ST0=2^frac                      */
        "fscale\n\t"                /* ST0=e^x                         */
        "fstp %st(1)\n\t"           /* ST0=e^x, ST1=x                 */

        /* --- Compute e^(-x) --- */
        "fxch %st(1)\n\t"           /* ST0=x, ST1=e^x                  */
        "fchs\n\t"                  /* ST0=-x                          */
        "fldl2e\n\t"                /* ST0=log2(e), ST1=-x             */
        "fmulp\n\t"                 /* ST0=-x*log2(e)                  */
        "fld %st(0)\n\t"
        "frndint\n\t"
        "fxch %st(1)\n\t"
        "fsub %st(1)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %st(1)\n\t"           /* ST0=e^(-x), ST1=e^x            */

        /* --- sinh = (e^x - e^(-x)) / 2 --- */
        "fsubp\n\t"                 /* ST0 = e^x - e^(-x)             */
                                    /* (fsubp: ST1 -= ST0, pop)        */

        /* Divide by 2 via FSCALE with exponent -1 */
        "fld1\n\t"                  /* ST0=1, ST1=diff                 */
        "fchs\n\t"                  /* ST0=-1                          */
        "fxch %st(1)\n\t"           /* ST0=diff, ST1=-1                */
        "fscale\n\t"                /* ST0 = diff * 2^(-1) = diff/2    */
        "fstp %st(1)\n\t"           /* pop -1, leave sinh(x) in ST0    */
        "ret\n"
    );
}
