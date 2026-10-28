/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * xdk_crt.c - the C99 CRT pieces MSVC 2010 lacks, for xdk_compat.h.
 *
 * _snprintf and _vsnprintf return -1 and leave the buffer UNTERMINATED when
 * the output does not fit. The tree was written to the C99 contract (always
 * terminated, returns the length that would have been written), and several
 * callers build paths into fixed buffers on exactly that assumption.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#undef snprintf
#undef vsnprintf

int sh_xdk_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap)
{
    int r;
    if (!buf || n == 0)
        return _vscprintf(fmt, ap);
    r = _vsnprintf(buf, n, fmt, ap);
    if (r < 0 || (size_t)r >= n) {
        buf[n - 1] = '\0';
        return _vscprintf(fmt, ap);
    }
    return r;
}

int sh_xdk_snprintf(char* buf, size_t n, const char* fmt, ...)
{
    va_list ap;
    int     r;
    va_start(ap, fmt);
    r = sh_xdk_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* No environment on a console. fsqueue_3.c asks for one PC-only override. */
char* getenv(const char* name)
{
    (void)name;
    return 0;
}

/* va_arg for transpiled code, which only knows the argument's size. The 360
 * ABI homes every variadic argument in an 8-byte slot; a scalar narrower than
 * the slot sits right-justified (big-endian), anything else left-justified,
 * which is what the XDK's own va_arg macro computes from the type. */
void* sh_xdk_va_arg_p(va_list* ap, size_t n)
{
    char*  slot = (char*)(((unsigned)*ap + 7u) & ~7u);
    size_t sz   = (n + 7u) & ~7u;
    int    left = (n > 8) || ((n & (n - 1)) != 0);
    *ap = slot + sz;
    return left ? slot : slot + 8 - n;
}

/* Incremented by every transpiler-generated .CRT$XCU initialiser (the stores
 * C89 cannot spell). main logs it: zero means the CRT never walked them. */
int sh_c89_init_count = 0;

/* The table of transpiler-generated initialisers. Each generated TU drops a
 * function pointer into .shinit$m; the linker sorts $a < $m < $z into one
 * contiguous run, so walking from the begin marker to the end marker visits
 * every entry. NULL slots (alignment padding) are skipped. */
#pragma section(".shinit$a", read)
#pragma section(".shinit$z", read)
__declspec(allocate(".shinit$a")) void (*sh_c89_init_begin)(void) = 0;
__declspec(allocate(".shinit$z")) void (*sh_c89_init_end)(void) = 0;

void Sh_RunC89Inits(void)
{
    void (**p)(void);
    for (p = &sh_c89_init_begin + 1; p < &sh_c89_init_end; p++)
        if (*p)
            (*p)();
}
